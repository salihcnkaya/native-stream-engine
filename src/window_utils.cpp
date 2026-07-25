#include "window_utils.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <iostream>
#include <unordered_set>

static std::string wideToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};

    int size = WideCharToMultiByte(
        CP_UTF8, 0,
        value.data(), (int)value.size(),
        nullptr, 0,
        nullptr, nullptr
    );

    std::string result(size, 0);

    WideCharToMultiByte(
        CP_UTF8, 0,
        value.data(), (int)value.size(),
        result.data(), size,
        nullptr, nullptr
    );

    return result;
}

static std::string getWindowTextUtf8(HWND hwnd)
{
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};

    std::wstring buffer(len + 1, L'\0');
    GetWindowTextW(hwnd, buffer.data(), len + 1);
    buffer.resize(len);

    return wideToUtf8(buffer);
}

static std::string getClassNameUtf8(HWND hwnd)
{
    wchar_t buffer[256] = {};
    GetClassNameW(hwnd, buffer, 256);
    return wideToUtf8(buffer);
}

static std::string getExePathFromPid(DWORD pid)
{
    HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        pid
    );

    if (!process) {
        return {};
    }

    std::wstring path;
    path.resize(32768);

    DWORD size =
        static_cast<DWORD>(path.size());

    std::string result;

    if (
        QueryFullProcessImageNameW(
            process,
            0,
            path.data(),
            &size
        )
    ) {
        path.resize(size);
        result = wideToUtf8(path);
    }

    CloseHandle(process);

    return result;
}

static std::string getExeNameFromPath(
    const std::string& exePath
)
{
    if (exePath.empty()) {
        return {};
    }

    const size_t position =
        exePath.find_last_of("\\/");

    if (position == std::string::npos) {
        return exePath;
    }

    return exePath.substr(position + 1);
}

static std::string toLowerAscii(std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        }
    );

    return value;
}

static bool shouldHideWindowExe(const std::string& exeName)
{
    static const std::unordered_set<std::string> hiddenExecutables = {
        "systemsettings.exe",
        "textinputhost.exe",
        "taskmgr.exe",
        "searchhost.exe",
        "startmenuexperiencehost.exe",
        "shellexperiencehost.exe"
    };

    const std::string normalized =
        toLowerAscii(exeName);

    return hiddenExecutables.find(normalized) !=
           hiddenExecutables.end();
}

static BOOL CALLBACK enumWindowsProc(HWND hwnd, LPARAM lParam)
{
    auto* windows = reinterpret_cast<std::vector<WindowInfo>*>(lParam);

    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetAncestor(hwnd, GA_ROOT) != hwnd) return TRUE;

    RECT rect {};
    if (!GetWindowRect(hwnd, &rect)) return TRUE;

    int width = rect.right - rect.left;
    int height = rect.bottom - rect.top;

    if (width <= 0 || height <= 0) return TRUE;

    std::string title = getWindowTextUtf8(hwnd);
    if (title.empty()) return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    // const std::string exeName =
    //     getExeNameFromPid(pid);

    const std::string exePath =
        getExePathFromPid(pid);

    const std::string exeName =
        getExeNameFromPath(exePath);

    if (shouldHideWindowExe(exeName)) {
        return TRUE;
    }

    WindowInfo info;
    info.hwnd = reinterpret_cast<uintptr_t>(hwnd);
    info.pid = static_cast<uint32_t>(pid);
    info.title = title;
    info.className = getClassNameUtf8(hwnd);
    info.exeName = exeName;
    info.exePath = exePath;

    const std::string normalizedExe =
    toLowerAscii(info.exeName);

    const bool isProgramManager =
        info.className == "Progman" &&
        info.title == "Program Manager";

    const bool isWindowsSettings =
        normalizedExe == "applicationframehost.exe" &&
        info.className == "ApplicationFrameWindow" &&
        (
            info.title == "Ayarlar" ||
            info.title == "Settings"
        );

    if (isProgramManager || isWindowsSettings) {
        return TRUE;
    }
    
    windows->push_back(info);
    return TRUE;
}

std::vector<WindowInfo> listVisibleWindows()
{
    std::vector<WindowInfo> windows;
    EnumWindows(enumWindowsProc, reinterpret_cast<LPARAM>(&windows));
    return windows;
}

void printVisibleWindows()
{
    auto windows = listVisibleWindows();

    std::cout << "\nVisible windows:\n";

    for (const auto& w : windows) {
        std::cout
            << "hwnd=" << w.hwnd
            << " pid=" << w.pid
            << " exe=\"" << w.exeName << "\""
            << " class=\"" << w.className << "\""
            << " title=\"" << w.title << "\""
            << "\n";
    }
}

bool findWindowByHwnd(uintptr_t hwndValue, WindowInfo& out)
{
    HWND hwnd = reinterpret_cast<HWND>(hwndValue);

    if (!IsWindow(hwnd)) {
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);

    out.hwnd = hwndValue;
    out.pid = static_cast<uint32_t>(pid);
    out.title = getWindowTextUtf8(hwnd);
    out.className = getClassNameUtf8(hwnd);
    // out.exeName = getExeNameFromPid(pid);
    out.exePath = getExePathFromPid(pid);
    out.exeName = getExeNameFromPath(out.exePath);

    return !out.title.empty() && !out.className.empty();
}

std::string buildObsWindowString(const WindowInfo& info)
{
    return info.title + ":" + info.className + ":" + info.exeName;
}
