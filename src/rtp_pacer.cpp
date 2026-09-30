#include "rtp_pacer.h"
#include <iostream>

#include <thread>
#include <algorithm>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace {

inline void schedulerYield()
{
#if defined(_WIN32)
    Sleep(0);
#else
    std::this_thread::yield();
#endif
}

inline void cpuRelax()
{
#if defined(_WIN32)
    YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#else
    std::this_thread::yield();
#endif
}

} // namespace

void RtpPacer::resetTelemetry()
{
    telemetry_ = {};
}

RtpPacerTelemetry RtpPacer::telemetry() const
{
    return telemetry_;
}

void RtpPacer::resetRateStateLocked(uint32_t bitrateBps)
{
    targetBitrateBps_.store(
        bitrateBps,
        std::memory_order_release
    );

    bucketInitialized_ = false;
    bucketCreditBytes_ = 0.0;
    bucketMinCreditBytes_ = 0.0;
    bucketMaxCreditBytes_ = 0.0;
    bucketRateBps_ = bitrateBps;
    bucketLastRefill_ = std::chrono::steady_clock::time_point{};

    peakBucketInitialized_ = false;
    peakBucketCreditBytes_ = 0.0;
    peakBucketMinCreditBytes_ = 0.0;
    peakBucketLastRefill_ = std::chrono::steady_clock::time_point{};

    peakDebtActive_ = false;
    peakDebtStart_ = std::chrono::steady_clock::time_point{};
    peakMaxDebtDurationMs_ = 0.0;

    peakVirtualInitialized_ = false;
    peakVirtualTat_ = std::chrono::steady_clock::time_point{};
    peakVirtualMaxRequiredWaitMs_ = 0.0;
    peakVirtualWaitEvents_ = 0;
}

void RtpPacer::setInitialBitrate(uint32_t bitrateBps)
{
    std::lock_guard<std::mutex> lock(stateMutex_);

    resetRateStateLocked(bitrateBps);
    
    std::cerr << "[RtpPacer] initial target bitrate="
          << bitrateBps
          << " bps\n";

    bitrateController_.setInitialBitrate(bitrateBps);

    lastBitrateDecision_.targetBitrateBps = bitrateBps;
    lastBitrateDecision_.shouldChangeEncoder = false;
}

void RtpPacer::setAppliedBitrate(uint32_t bitrateBps)
{
    if (bitrateBps == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(stateMutex_);

    const uint32_t previousBitrateBps =
        targetBitrateBps_.load(
            std::memory_order_acquire
        );

    if (previousBitrateBps == bitrateBps) {
        return;
    }

    resetRateStateLocked(bitrateBps);

    bitrateController_.setAppliedBitrate(
        bitrateBps
    );

    lastBitrateDecision_.targetBitrateBps =
        bitrateBps;

    lastBitrateDecision_.shouldChangeEncoder =
        false;

    lastBitrateDecision_.stableFeedbackCount =
        0;

    std::cerr
        << "[RtpPacer] applied target bitrate updated"
        << " previous=" << previousBitrateBps
        << " applied=" << bitrateBps
        << " bps\n";
}

void RtpPacer::updateNetworkFeedback(
    const NetworkFeedback& feedback
)
{
    {
        std::lock_guard<std::mutex> lock(stateMutex_);

        feedback_ = feedback;

        lastBitrateDecision_ =
            bitrateController_.update(feedback_);
    }

    const bool previousAdaptiveActive =
        adaptiveVideoActive_.load(
            std::memory_order_relaxed
        );

    const uint32_t previousAdaptiveExtraUs =
        adaptiveVideoExtraUs_.load(
            std::memory_order_relaxed
        );

    bool adaptiveActive =
        previousAdaptiveActive;

    if (!feedback.hasFeedback) {
        adaptiveActive = false;
    } else {
        const bool forceOn =
            feedback.packetLossRatio >= 0.01 ||
            feedback.jitterMs >= 120 ||
            feedback.score < 8;

        const bool keepOn =
            feedback.jitterMs >= 90 &&
            feedback.score >= 8;

        if (forceOn) {
            adaptiveActive = true;
        } else if (!keepOn) {
            adaptiveActive = false;
        }
    }

    uint32_t adaptiveExtraUs = 0;

    if (adaptiveActive) {
        if (
            feedback.packetLossRatio >= 0.01 ||
            feedback.jitterMs >= 250 ||
            feedback.score < 8
        ) {
            adaptiveExtraUs = 20;
        } else if (feedback.jitterMs >= 120) {
            adaptiveExtraUs = 10;
        }
    }

    if (
        adaptiveActive != previousAdaptiveActive ||
        adaptiveExtraUs != previousAdaptiveExtraUs
    ) {
        std::cerr
            << "[RtpPacer] adaptive pacing updated"
            << " active=" << (adaptiveActive ? "yes" : "no")
            << " extraUs=" << adaptiveExtraUs
            << " loss=" << feedback.packetLossRatio
            << " jitterMs=" << feedback.jitterMs
            << " hasRtt=" << (feedback.hasRtt ? "yes" : "no")
            << " rttMs=" << feedback.rttMs
            << " score=" << feedback.score
            << "\n";
    }

    adaptiveVideoActive_.store(
        adaptiveActive,
        std::memory_order_release
    );

    adaptiveVideoExtraUs_.store(
        adaptiveExtraUs,
        std::memory_order_release
    );
}

NetworkFeedback RtpPacer::networkFeedback() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);

    return feedback_;
}

BitrateDecision RtpPacer::bitrateDecision() const
{
    std::lock_guard<std::mutex> lock(stateMutex_);

    return lastBitrateDecision_;
}

bool RtpPacer::shouldUseAdaptiveVideo() const
{
    return adaptiveVideoActive_.load(
        std::memory_order_acquire
    );
}

bool RtpPacer::isAdaptiveActive() const
{
    return shouldUseAdaptiveVideo();
}

uint32_t RtpPacer::adaptiveExtraUs(
    bool isVideo
) const
{
    if (!isVideo) {
        return 0;
    }

    return adaptiveVideoExtraUs_.load(
        std::memory_order_acquire
    );
}

std::chrono::microseconds RtpPacer::calculateSpacing(
    bool isVideo,
    uint32_t queueDepth
) const
{
    std::chrono::microseconds spacing;

    if (isVideo) {
        if (queueDepth > 300) {
            spacing = config_.videoQ300;
        } else if (queueDepth > 120) {
            spacing = config_.videoQ120;
        } else if (queueDepth > 60) {
            spacing = config_.videoQ60;
        } else {
            spacing = config_.videoNormal;
        }

        spacing += std::chrono::microseconds(adaptiveExtraUs(true));

        const auto maxVideoSpacing = config_.videoNormal + std::chrono::microseconds(30);

        if (spacing > maxVideoSpacing) {
            spacing = maxVideoSpacing;
        }

        return spacing;
    }

    if (queueDepth > 300) return config_.audioQ300;
    if (queueDepth > 120) return config_.audioQ120;
    if (queueDepth > 60) return config_.audioQ60;

    return config_.audioNormal;
}

void RtpPacer::wait(
    const std::atomic<bool>& shouldRun,
    std::chrono::steady_clock::time_point& nextSendTime,
    std::chrono::microseconds spacing
) const
{
    using clock = std::chrono::steady_clock;

    nextSendTime += spacing;

    const auto waitStartedAt = clock::now();

    uint64_t yieldIterations = 0;
    uint64_t spinIterations = 0;

    bool schedulerYieldUsed = false;

    constexpr auto SPIN_THRESHOLD = std::chrono::microseconds(20);

    while (
        shouldRun.load(std::memory_order_acquire)
    ) {
        const auto now = clock::now();

        if (now >= nextSendTime) {
            break;
        }

        const auto remaining = nextSendTime - now;

        if (!schedulerYieldUsed && remaining > SPIN_THRESHOLD) {
            schedulerYield();

            schedulerYieldUsed = true;
            yieldIterations++;

            continue;
        }

        cpuRelax();
        spinIterations++;
    }

    const auto now = clock::now();

    const auto waitedUs =
        std::chrono::duration_cast<
            std::chrono::microseconds
        >(
            now - waitStartedAt
        ).count();

    telemetry_.waitCalls++;
    telemetry_.yieldIterations +=
        yieldIterations;
    
    telemetry_.spinIterations +=
        spinIterations;

    if (waitedUs > 0) {
        const uint64_t waitedUsUnsigned =
            static_cast<uint64_t>(waitedUs);

        telemetry_.totalWaitUs +=
            waitedUsUnsigned;

        if (
            waitedUsUnsigned >
            telemetry_.maxWaitUs
        ) {
            telemetry_.maxWaitUs =
                waitedUsUnsigned;
        }
    }

    if (nextSendTime < now - std::chrono::milliseconds(5)) {
        telemetry_.lateResets++;
        nextSendTime = now;
    }
}

void RtpPacer::pace(
    const std::atomic<bool>& shouldRun,
    bool isVideo,
    uint32_t queueDepth,
    uint32_t packetBytes,
    std::chrono::steady_clock::time_point& nextSendTime,
    bool accountSustained
) const
{
    std::chrono::steady_clock::time_point activeVideoSendTime{};

    {
        std::lock_guard<std::mutex> lock(stateMutex_);  
        
        const auto now = std::chrono::steady_clock::now();

        refillBucketLocked(now);
        refillPeakBucketLocked(now);

        if (peakDebtActive_ && peakBucketCreditBytes_ >= 0.0) {
            const double debtDurationMs =
                std::chrono::duration<double, std::milli>(
                    now - peakDebtStart_
                ).count();

            if (debtDurationMs > peakMaxDebtDurationMs_) {
                peakMaxDebtDurationMs_ = debtDurationMs;
            }

            peakDebtActive_ = false;
        }

        if (accountSustained) {
            bucketCreditBytes_ -= static_cast<double>(packetBytes);
        }

        peakBucketCreditBytes_ -= static_cast<double>(packetBytes);

        if (peakBucketCreditBytes_ < peakBucketMinCreditBytes_) {
            peakBucketMinCreditBytes_ = peakBucketCreditBytes_;
        }

        if (peakBucketCreditBytes_ < 0.0 && !peakDebtActive_) {
            peakDebtActive_ = true;
            peakDebtStart_ = now;
        }

        if (bucketCreditBytes_ < bucketMinCreditBytes_) {
            bucketMinCreditBytes_ = bucketCreditBytes_;
        }

        if (bucketCreditBytes_ > bucketMaxCreditBytes_) {
            bucketMaxCreditBytes_ = bucketCreditBytes_;
        }

        double peakVirtualRequiredWaitMs = 0.0;

        if (
            isVideo &&
            packetBytes > 0 &&
            bucketRateBps_ > 0
        ) {
            const double peakRateBps =
                static_cast<double>(bucketRateBps_) * 2.75;

            const double packetIntervalSeconds =
                (static_cast<double>(packetBytes) * 8.0) /
                peakRateBps;

            const auto packetInterval =
                std::chrono::duration_cast<
                    std::chrono::steady_clock::duration
                >(
                    std::chrono::duration<double>(
                        packetIntervalSeconds
                    )
                );

            const auto burstTolerance =
                std::chrono::duration_cast<
                    std::chrono::steady_clock::duration
                >(
                    std::chrono::duration<double>(
                        20.0 / 1000.0
                    )
                );

            if (!peakVirtualInitialized_) {
                peakVirtualInitialized_ = true;
                peakVirtualTat_ = now;
            }

            const auto earliestAllowed =
                peakVirtualTat_ - burstTolerance;

            auto virtualSendTime = now;

            if (earliestAllowed > now) {
                virtualSendTime = earliestAllowed;

                peakVirtualRequiredWaitMs =
                    std::chrono::duration<double, std::milli>(
                        earliestAllowed - now
                    ).count();

                ++peakVirtualWaitEvents_;

                if (
                    peakVirtualRequiredWaitMs >
                    peakVirtualMaxRequiredWaitMs_
                ) {
                    peakVirtualMaxRequiredWaitMs_ =
                        peakVirtualRequiredWaitMs;
                }
            }

            activeVideoSendTime = virtualSendTime;

            const auto virtualBase =
                std::max(
                    peakVirtualTat_,
                    virtualSendTime
                );

            peakVirtualTat_ =
                virtualBase + packetInterval;
        }

        static thread_local uint64_t bucketDebugCalls = 0;

        if (isVideo && (++bucketDebugCalls % 10000) == 0) {
            const double bucketRateBytesPerSecond =
                (static_cast<double>(bucketRateBps_) * 1.03) / 8.0;

            const double maxCreditMs =
                bucketRateBytesPerSecond > 0.0
                    ? (bucketMaxCreditBytes_ / bucketRateBytesPerSecond) * 1000.0
                    : 0.0;

            const double minCreditMs =
                bucketRateBytesPerSecond > 0.0
                    ? (bucketMinCreditBytes_ / bucketRateBytesPerSecond) * 1000.0
                    : 0.0;

            const double peakRateBytesPerSecond =
                (static_cast<double>(bucketRateBps_) * 2.75) / 8.0;
            
            const double peakCreditMs =
                peakRateBytesPerSecond > 0.0
                    ? (peakBucketCreditBytes_ / peakRateBytesPerSecond) * 1000.0
                    : 0.0;

            const double peakMinCreditMs =
                peakRateBytesPerSecond > 0.0
                    ? (peakBucketMinCreditBytes_ / peakRateBytesPerSecond) * 1000.0
                    : 0.0;

            const double peakActiveDebtDurationMs =
                peakDebtActive_
                    ? std::chrono::duration<double, std::milli>(
                        now - peakDebtStart_
                    ).count()
                    : 0.0;

            std::cerr << "[RtpPacer] bucket-debug"
                    << " targetBps=" << bucketRateBps_
                    << " creditBytes=" << bucketCreditBytes_
                    << " minCreditBytes=" << bucketMinCreditBytes_
                    << " maxCreditBytes=" << bucketMaxCreditBytes_
                    << " maxCreditMs=" << maxCreditMs
                    << " minCreditMs=" << minCreditMs
                    << " peakCreditBytes=" << peakBucketCreditBytes_
                    << " peakMinCreditBytes=" << peakBucketMinCreditBytes_
                    << " peakMinCreditMs=" << peakMinCreditMs
                    << " peakCreditMs=" << peakCreditMs
                    << " peakDebtActive=" << (peakDebtActive_ ? 1 : 0)
                    << " peakActiveDebtDurationMs=" << peakActiveDebtDurationMs
                    << " peakMaxDebtDurationMs=" << peakMaxDebtDurationMs_
                    << " peakVirtualRequiredWaitMs="
                    << peakVirtualRequiredWaitMs
                    << " peakVirtualMaxRequiredWaitMs="
                    << peakVirtualMaxRequiredWaitMs_
                    << " peakVirtualWaitEvents="
                    << peakVirtualWaitEvents_
                    << " packetBytes=" << packetBytes
                    << " queueDepth=" << queueDepth
                    << '\n';
        }
    }

    if (
        isVideo &&
        activeVideoSendTime.time_since_epoch().count() != 0
    ) {
        nextSendTime = activeVideoSendTime;

        wait(
            shouldRun,
            nextSendTime,
            std::chrono::microseconds(0)
        );

        return;
    }

    const auto spacing =
        calculateSpacing(isVideo, queueDepth);

    wait(
        shouldRun,
        nextSendTime,
        spacing
    );
}

void RtpPacer::logBaseline(
    const std::string& label,
    uint32_t maxBatch
) const
{
    const auto feedback = networkFeedback();

    std::cerr << "[Realtime RTP Sender:" << label
              << "] pacing baseline"
              << " maxBatch=" << maxBatch
              << " videoNormalUs=" << calculateSpacing(true, 0).count()
              << " videoQ60Us=" << calculateSpacing(true, 61).count()
              << " videoQ120Us=" << calculateSpacing(true, 121).count()
              << " videoQ300Us=" << calculateSpacing(true, 301).count()
              << " audioNormalUs=" << calculateSpacing(false, 0).count()
              << " audioQ60Us=" << calculateSpacing(false, 61).count()
              << " audioQ120Us=" << calculateSpacing(false, 121).count()
              << " audioQ300Us=" << calculateSpacing(false, 301).count()
              << " feedback=" << (feedback.hasFeedback ? "yes" : "no")
              << " loss=" << feedback.packetLossRatio
              << " jitterMs=" << feedback.jitterMs
              << " hasRtt=" << (feedback.hasRtt ? "yes" : "no")
              << " rttMs=" << feedback.rttMs
              << " score=" << feedback.score
              << " bitrate=" << feedback.bitrateBps
              << " packetCount=" << feedback.packetCount
              << " byteCount=" << feedback.byteCount
              << "\n";
} 

void RtpPacer::refillBucketLocked(
    std::chrono::steady_clock::time_point now
) const
{
    const uint32_t targetBps = targetBitrateBps_.load(
        std::memory_order_acquire
    );

    if (targetBps == 0) {
        bucketInitialized_ = false;
        bucketCreditBytes_ = 0.0;
        bucketRateBps_ = 0;
        bucketLastRefill_ = now;
        return;
    }

    if (!bucketInitialized_ || bucketRateBps_ != targetBps) {
        bucketInitialized_ = true;
        bucketCreditBytes_ = 0.0;
        bucketRateBps_ = targetBps;
        bucketLastRefill_ = now;
        return;
    }

    const auto elapsed =
        std::chrono::duration<double>(now - bucketLastRefill_);

    if (elapsed.count() <= 0.0) {
        return;
    }

    const double pacingRateBps =
        static_cast<double>(targetBps) * 1.03;

    const double bytesPerSecond =
        pacingRateBps / 8.0;

    bucketCreditBytes_ +=
        elapsed.count() * bytesPerSecond;

    const double maxCreditSeconds = 2.0 / 60.0;
    const double maxCreditBytes =
        bytesPerSecond * maxCreditSeconds;

    if (bucketCreditBytes_ > maxCreditBytes) {
        bucketCreditBytes_ = maxCreditBytes;
    }

    bucketLastRefill_ = now;
}

void RtpPacer::refillPeakBucketLocked(
    std::chrono::steady_clock::time_point now
) const
{
    const uint32_t targetBps = targetBitrateBps_.load(
        std::memory_order_acquire
    );

    if (targetBps == 0) {
        peakBucketInitialized_ = false;
        peakBucketCreditBytes_ = 0.0;
        peakBucketMinCreditBytes_ = 0.0;
        peakBucketLastRefill_ = now;
        return;
    }

    const double peakRateBps =
        static_cast<double>(targetBps) * 2.75;

    const double peakBytesPerSecond =
        peakRateBps / 8.0;

    if (!peakBucketInitialized_) {
        peakBucketInitialized_ = true;
        peakBucketCreditBytes_ = 0.0;
        peakBucketLastRefill_ = now;
        return;
    }

    const auto elapsed =
        std::chrono::duration<double>(now - peakBucketLastRefill_);

    if (elapsed.count() > 0.0) {
        peakBucketCreditBytes_ +=
            elapsed.count() * peakBytesPerSecond;

        const double maxPeakCreditSeconds = 20.0 / 1000.0;
        const double maxPeakCreditBytes =
            peakBytesPerSecond * maxPeakCreditSeconds;

        if (peakBucketCreditBytes_ > maxPeakCreditBytes) {
            peakBucketCreditBytes_ = maxPeakCreditBytes;
        }
    }

    peakBucketLastRefill_ = now;
}