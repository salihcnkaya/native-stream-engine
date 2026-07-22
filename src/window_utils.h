#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct WindowInfo {
    uintptr_t hwnd = 0;
    uint32_t pid = 0;
    std::string title;
    std::string className;
    std::string exeName;
};

std::vector<WindowInfo> listVisibleWindows();
void printVisibleWindows();
bool findWindowByHwnd(uintptr_t hwnd, WindowInfo& out);
std::string buildObsWindowString(const WindowInfo& info);
