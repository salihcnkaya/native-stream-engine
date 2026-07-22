#pragma once

#include <cstdint>
#include <chrono>

struct BitrateUpdateDecision {
    bool shouldApply = false;
    uint32_t bitrateBps = 0;
};

class BitrateUpdateScheduler {
public:
    BitrateUpdateDecision update(uint32_t targetBitrateBps);

    void reset();

private:
    uint32_t lastAppliedBitrateBps_ = 0;
    std::chrono::steady_clock::time_point lastAppliedAt_;
};
