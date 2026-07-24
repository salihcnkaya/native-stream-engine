#include "rtp_pacer.h"
#include <iostream>

void RtpPacer::resetTelemetry()
{
    telemetry_ = {};
}

RtpPacerTelemetry RtpPacer::telemetry() const
{
    return telemetry_;
}

void RtpPacer::setInitialBitrate(uint32_t bitrateBps)
{
    std::lock_guard<std::mutex> lock(stateMutex_);

    bitrateController_.setInitialBitrate(bitrateBps);
    lastBitrateDecision_.targetBitrateBps = bitrateBps;
    lastBitrateDecision_.shouldChangeEncoder = false;
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

    bool adaptiveActive =
        adaptiveVideoActive_.load(
            std::memory_order_relaxed
        );

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
    nextSendTime += spacing;

    const auto waitStartedAt =
        std::chrono::steady_clock::now();

    uint64_t yieldIterations = 0;

    while (
        shouldRun.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < nextSendTime
    ) {
        Sleep(0);
        yieldIterations++;
    }

    const auto now = std::chrono::steady_clock::now();

    const auto waitedUs =
        std::chrono::duration_cast<
            std::chrono::microseconds
        >(
            now - waitStartedAt
        ).count();

    telemetry_.waitCalls++;
    telemetry_.yieldIterations +=
        yieldIterations;

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
    std::chrono::steady_clock::time_point& nextSendTime
) const
{
    const auto spacing = calculateSpacing(isVideo, queueDepth);

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
              << " rttMs=" << feedback.rttMs
              << " score=" << feedback.score
              << " bitrate=" << feedback.bitrateBps
              << " packetCount=" << feedback.packetCount
              << " byteCount=" << feedback.byteCount
              << "\n";
} 
