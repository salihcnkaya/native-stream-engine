#include "bitrate_update_scheduler.h"

#include <algorithm>

BitrateUpdateDecision BitrateUpdateScheduler::update(uint32_t targetBitrateBps)
{
    BitrateUpdateDecision decision;
    decision.bitrateBps = targetBitrateBps;

    if (targetBitrateBps == 0) {
        return decision;
    }

    const auto now = std::chrono::steady_clock::now();

    if (lastAppliedBitrateBps_ == 0) {
        decision.shouldApply = true;
        return decision;
    }

    const uint32_t lower = std::min(lastAppliedBitrateBps_, targetBitrateBps);
    const uint32_t higher = std::max(lastAppliedBitrateBps_, targetBitrateBps);

    const double changeRatio =
        lower > 0 ? static_cast<double>(higher - lower) / lower : 1.0;

    if (changeRatio < 0.05) {
        return decision;
    }

    if (now - lastAppliedAt_ < std::chrono::seconds(3)) {
        return decision;
    }

    decision.shouldApply = true;

    return decision;
}

void BitrateUpdateScheduler::markApplied(uint32_t appliedBitrateBps)
{
    if (appliedBitrateBps == 0) {
        return;
    }

    lastAppliedBitrateBps_ = appliedBitrateBps;
    lastAppliedAt_ = std::chrono::steady_clock::now();
}

void BitrateUpdateScheduler::reset()
{
    lastAppliedBitrateBps_ = 0;
    lastAppliedAt_ = {};
}
