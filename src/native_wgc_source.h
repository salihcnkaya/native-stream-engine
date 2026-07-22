#pragma once

#include <cstdint>
#include <vector>

enum class WgcTargetType {
    Window,
    Monitor
};

bool registerWgcSource();

void setWgcConfig(
    WgcTargetType targetType,
    uintptr_t hwnd,
    int monitorIndex,
    int delayMs,
    bool debugFrames
);

bool isWgcTargetClosed();
void resetWgcTargetClosed();

bool isWgcFrameReady();

uint32_t getWgcFrameWidth();

uint32_t getWgcFrameHeight();

void resetWgcFrameState();

bool copyWgcFrameBgra(
    std::vector<uint8_t>& pixels,
    uint32_t& width,
    uint32_t& height
);
