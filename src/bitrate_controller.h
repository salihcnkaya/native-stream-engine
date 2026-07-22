#pragma once

#include "network_feedback.h"

#include <cstdint>

struct BitrateDecision {
    uint32_t targetBitrateBps = 0;
    bool shouldChangeEncoder = false;
    uint32_t stableFeedbackCount = 0;
};

class BitrateController {
public:
    void setInitialBitrate(uint32_t bitrateBps);
    BitrateDecision update(const NetworkFeedback& feedback);

private:
    uint32_t currentTargetBitrateBps_ = 0;
	uint32_t minBitrateBps_ = 800000;
    uint32_t maxBitrateBps_ = 2500000;
    uint32_t stableFeedbackCount_ = 0;
};
