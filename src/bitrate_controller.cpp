#include "bitrate_controller.h"
#include <algorithm>

void BitrateController::setInitialBitrate(uint32_t bitrateBps)
{
    currentTargetBitrateBps_ = bitrateBps;
    maxBitrateBps_ = bitrateBps;
    stableFeedbackCount_ = 0;
}

BitrateDecision BitrateController::update(const NetworkFeedback& feedback)
{
    BitrateDecision decision;
    decision.targetBitrateBps = currentTargetBitrateBps_;

    if (!feedback.hasFeedback || currentTargetBitrateBps_ == 0) {
        decision.stableFeedbackCount = stableFeedbackCount_;
        return decision;
    }

    const bool hasLoss = feedback.packetLossRatio >= 0.02;
    const bool mediumJitter = feedback.jitterMs >= 120;
    const bool veryHighJitter = feedback.jitterMs >= 250;
    const bool badScore = feedback.score < 8;

    if (mediumJitter && !veryHighJitter && !hasLoss && !badScore) {
        stableFeedbackCount_ = 0;
        decision.stableFeedbackCount = stableFeedbackCount_;
        return decision;
    }

    if (hasLoss || veryHighJitter || badScore) {
        stableFeedbackCount_ = 0;

        double reduceFactor = 0.90;

        if (feedback.packetLossRatio >= 0.08 || feedback.jitterMs >= 500 || feedback.score <= 5) {
            reduceFactor = 0.65;
        } else if (feedback.packetLossRatio >= 0.04 || feedback.jitterMs >= 350 || feedback.score <= 6) {
            reduceFactor = 0.80;
        }

        const uint32_t reduced = std::max(
            minBitrateBps_,
            static_cast<uint32_t>(currentTargetBitrateBps_ * reduceFactor)
        );

        if (reduced < currentTargetBitrateBps_) {
            currentTargetBitrateBps_ = reduced;
            decision.targetBitrateBps = currentTargetBitrateBps_;
            decision.shouldChangeEncoder = true;
        }

        decision.stableFeedbackCount = stableFeedbackCount_;
        return decision;
    }

    const bool goodNetwork =
        feedback.packetLossRatio == 0.0 &&
        feedback.jitterMs < 80 &&
        feedback.score >= 9;

    if (goodNetwork && currentTargetBitrateBps_ < maxBitrateBps_) {
        stableFeedbackCount_++;

        if (stableFeedbackCount_ >= 3) {
            stableFeedbackCount_ = 0;

            const uint32_t increased = std::min(
                maxBitrateBps_,
                static_cast<uint32_t>(currentTargetBitrateBps_ * 1.20)
            );

            if (increased > currentTargetBitrateBps_) {
                currentTargetBitrateBps_ = increased;
                decision.targetBitrateBps = currentTargetBitrateBps_;
                decision.shouldChangeEncoder = true;
            }
        }
    } else {
        stableFeedbackCount_ = 0;
    }

    decision.stableFeedbackCount = stableFeedbackCount_;
    return decision;
}
