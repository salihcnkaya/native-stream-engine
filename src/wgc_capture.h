#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct MonitorInfo {
    int index = 0;
    std::string name;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool primary = false;
};

std::vector<MonitorInfo> listMonitors();

bool wgcGetCaptureSize(uintptr_t hwnd, int delayMs, int& width, int& height);

