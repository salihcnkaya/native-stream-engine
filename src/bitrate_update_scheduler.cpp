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
        lastAppliedBitrateBps_ = targetBitrateBps;
        lastAppliedAt_ = now;
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

    lastAppliedBitrateBps_ = targetBitrateBps;
    lastAppliedAt_ = now;
    decision.shouldApply = true;

    return decision;
}

void BitrateUpdateScheduler::reset()
{
    lastAppliedBitrateBps_ = 0;
    lastAppliedAt_ = {};
}
