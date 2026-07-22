#include "rtp_pacer.h"
#include <iostream>

void RtpPacer::setInitialBitrate(uint32_t bitrateBps)
{
    bitrateController_.setInitialBitrate(bitrateBps);
    lastBitrateDecision_.targetBitrateBps = bitrateBps;
    lastBitrateDecision_.shouldChangeEncoder = false;
}

void RtpPacer::updateNetworkFeedback(const NetworkFeedback& feedback)
{
    feedback_ = feedback;
    lastBitrateDecision_ = bitrateController_.update(feedback_);
}

NetworkFeedback RtpPacer::networkFeedback() const
{
    return feedback_;
}

BitrateDecision RtpPacer::bitrateDecision() const
{
    return lastBitrateDecision_;
}

bool RtpPacer::shouldUseAdaptiveVideo() const
{
    if (!feedback_.hasFeedback) {
        adaptiveVideoActive_ = false;
        return false;
    }

    const bool forceOn =
        feedback_.packetLossRatio >= 0.01 ||
        feedback_.jitterMs >= 120 ||
        feedback_.score < 8;

    const bool keepOn =
        feedback_.jitterMs >= 90 &&
        feedback_.score >= 8;

    if (forceOn) {
        adaptiveVideoActive_ = true;
    } else if (!keepOn) {
        adaptiveVideoActive_ = false;
    }

    return adaptiveVideoActive_;
}

bool RtpPacer::isAdaptiveActive() const
{
    return shouldUseAdaptiveVideo();
}

uint32_t RtpPacer::adaptiveExtraUs(bool isVideo) const
{
    if (!isVideo || !shouldUseAdaptiveVideo()) {
        return 0;
    }

    if (
        feedback_.packetLossRatio >= 0.01 ||
        feedback_.jitterMs >= 250 ||
        feedback_.score < 8
    ) {
        return 20;
    }

    if (feedback_.jitterMs >= 120) {
        return 10;
    }

    return 0;
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

    while (
        shouldRun.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < nextSendTime
    ) {
        Sleep(0);
    }

    const auto now = std::chrono::steady_clock::now();

    if (nextSendTime < now - std::chrono::milliseconds(5)) {
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
              << " feedback=" << (feedback_.hasFeedback ? "yes" : "no")
              << " loss=" << feedback_.packetLossRatio
              << " jitterMs=" << feedback_.jitterMs
              << " rttMs=" << feedback_.rttMs
              << " score=" << feedback_.score
              << " bitrate=" << feedback_.bitrateBps
              << " packetCount=" << feedback_.packetCount
              << " byteCount=" << feedback_.byteCount
              << "\n";
} 
