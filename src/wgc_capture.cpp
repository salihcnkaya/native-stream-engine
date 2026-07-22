#include "wgc_capture.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <iostream>
#include <string>
#include <vector>

#include <winrt/base.h>

#include <winrt/Windows.Graphics.Capture.h>

#include <windows.graphics.capture.interop.h>

static std::string wideToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};

    int size = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );

    std::string result(size, 0);

    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        size,
        nullptr,
        nullptr
    );

    return result;
}


struct MonitorListEnumData {
    std::vector<MonitorInfo>* monitors = nullptr;
    int currentIndex = 0;
};

static BOOL CALLBACK listMonitorCallback(
    HMONITOR monitor,
    HDC,
    LPRECT,
    LPARAM data
)
{
    auto* enumData = reinterpret_cast<MonitorListEnumData*>(data);

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);

    if (!GetMonitorInfoW(monitor, &info)) {
        enumData->currentIndex++;
        return TRUE;
    }

    MonitorInfo item;
    item.index = enumData->currentIndex;
    item.name = wideToUtf8(info.szDevice);
    item.x = info.rcMonitor.left;
    item.y = info.rcMonitor.top;
    item.width = info.rcMonitor.right - info.rcMonitor.left;
    item.height = info.rcMonitor.bottom - info.rcMonitor.top;
    item.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

    enumData->monitors->push_back(item);
    enumData->currentIndex++;

    return TRUE;
}

std::vector<MonitorInfo> listMonitors()
{
    std::vector<MonitorInfo> monitors;

    MonitorListEnumData data;
    data.monitors = &monitors;

    EnumDisplayMonitors(
        nullptr,
        nullptr,
        listMonitorCallback,
        reinterpret_cast<LPARAM>(&data)
    );

    return monitors;
}

static winrt::Windows::Graphics::Capture::GraphicsCaptureItem createItemForHwnd(HWND hwnd)
{
    auto factory = winrt::get_activation_factory<
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
        IGraphicsCaptureItemInterop
    >();

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };

    HRESULT hr = factory->CreateForWindow(
        hwnd,
        winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
        winrt::put_abi(item)
    );

    if (FAILED(hr) || !item) {
        throw winrt::hresult_error(hr, L"CreateForWindow failed");
    }

    return item;
}

bool wgcGetCaptureSize(uintptr_t hwndValue, int delayMs, int& width, int& height)
{
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);

        HWND hwnd = reinterpret_cast<HWND>(hwndValue);

        if (!IsWindow(hwnd)) {
            std::cerr << "Invalid HWND: " << hwndValue << "\n";
            return false;
        }

        if (delayMs > 0) {
            std::cerr << "[Native Stream Engine] waiting before size probe: "
                      << delayMs << "ms\n";
            Sleep(delayMs);
        }

        auto item = createItemForHwnd(hwnd);
        auto size = item.Size();

        width = size.Width;
        height = size.Height;

        std::cerr << "[Native Stream Engine] detected capture size: "
                  << width << "x" << height << "\n";

        return width > 0 && height > 0;
    } catch (const winrt::hresult_error& e) {
        std::wcerr << L"WGC size probe failed: "
                   << e.message().c_str()
                   << L"\n";
        return false;
    }
}
