#pragma once

#include <cstdint>

struct NetworkFeedback {
    double packetLossRatio = 0.0;
    uint32_t jitterMs = 0;
    uint32_t rttMs = 0;
    uint32_t score = 10;

    uint32_t bitrateBps = 0;
    uint64_t packetCount = 0;
    uint64_t byteCount = 0;

    bool hasFeedback = false;
    uint32_t nackCount = 0;
    uint32_t nackPacketCount = 0;
    uint32_t pliCount = 0;
    uint32_t firCount = 0;
};
