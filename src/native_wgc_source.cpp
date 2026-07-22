#include "native_wgc_source.h"

#include <obs.h>
#include <graphics/graphics.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <objbase.h>


class ThreadComApartment {
public:
    ThreadComApartment()
    {
        APTTYPE apartmentType{};
        APTTYPEQUALIFIER qualifier{};

        const HRESULT currentApartmentResult =
            CoGetApartmentType(
                &apartmentType,
                &qualifier
            );

        /*
         * Thread zaten COM apartment iÃ§indeyse tekrar farklÄ± bir
         * apartment modeli dayatmaya Ã§alÄ±ÅŸma.
         *
         * OBS veya baÅŸka bir Windows bileÅŸeni thread'i STA olarak
         * hazÄ±rlamÄ±ÅŸ olabilir. Bu durumda MTA istemek
         * RPC_E_CHANGED_MODE (0x80010106) Ã¼retir.
         */
        if (SUCCEEDED(currentApartmentResult)) {
            return;
        }

        const HRESULT initializeResult =
            CoInitializeEx(
                nullptr,
                COINIT_MULTITHREADED
            );

        if (
            initializeResult == S_OK ||
            initializeResult == S_FALSE
        ) {
            initializedByUs_ = true;
            return;
        }

        throw winrt::hresult_error(
            initializeResult,
            L"CoInitializeEx failed for WGC thread"
        );
    }

    ~ThreadComApartment()
    {
        if (initializedByUs_) {
            CoUninitialize();
        }
    }

    ThreadComApartment(const ThreadComApartment&) = delete;
    ThreadComApartment& operator=(
        const ThreadComApartment&
    ) = delete;

private:
    bool initializedByUs_ = false;
};

static void ensureCurrentThreadComApartment()
{
    /*
     * Her thread kendi guard'Ä±na sahip olur.
     * Destructor da aynÄ± thread sona erdiÄŸinde Ã§alÄ±ÅŸÄ±r.
     */
    thread_local ThreadComApartment apartment;
    (void)apartment;
}

struct WgcSource;

static WgcTargetType g_targetType = WgcTargetType::Window;
static uintptr_t g_targetHwnd = 0;
static int g_monitorIndex = 0;
static int g_captureDelayMs = 1000;
static bool g_debugFrames = false;
static constexpr auto kFramePoolResizeDebounce =
    std::chrono::milliseconds(150);

static constexpr auto kTargetWindowCheckInterval =
    std::chrono::milliseconds(250);

static constexpr uint32_t kDefaultCaptureWidth = 1280;
static constexpr uint32_t kDefaultCaptureHeight = 960;

static std::atomic<bool> g_borderlessAccessRequested = false;
static std::atomic<bool> g_borderlessAccessGranted = false;

static std::atomic<bool> g_activeTargetClosed = false;

static std::atomic<bool> g_frameReady = false;

static std::atomic<uint32_t> g_latestFrameWidth = 0;

static std::atomic<uint32_t> g_latestFrameHeight = 0;

static std::atomic<uint64_t> g_nextSourceGeneration = 1;

static std::atomic<uint64_t> g_activeSourceGeneration = 0;

static std::atomic<uint64_t> g_readyFrameGeneration = 0;

static std::mutex g_activeSourceMutex;

static WgcSource* g_activeSource = nullptr;

struct D3DDeviceBundle {
    winrt::com_ptr<ID3D11Device> device;
    winrt::com_ptr<ID3D11DeviceContext> context;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrtDevice{ nullptr };
};

struct WgcSource {
    uint64_t generation = 0;

    uintptr_t hwndValue = 0;
    DWORD targetProcessId = 0;
    WgcTargetType targetType = WgcTargetType::Window;

    std::chrono::steady_clock::time_point
        lastTargetWindowCheckAt{};
    uint32_t width = kDefaultCaptureWidth;
    uint32_t height = kDefaultCaptureHeight;

    uint32_t framePoolWidth = 0;
    uint32_t framePoolHeight = 0;

    uint32_t pendingFramePoolWidth = 0;
    uint32_t pendingFramePoolHeight = 0;

    bool framePoolResizePending = false;

    std::chrono::steady_clock::time_point
        framePoolResizeStableSince{};
    
    D3DDeviceBundle d3d;

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool framePool{ nullptr };
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session{ nullptr };
    winrt::event_token frameToken{};
    winrt::event_token itemClosedToken{};

    std::mutex sharedMutex;
    std::mutex framePoolMutex;
    winrt::com_ptr<ID3D11Texture2D> sharedTexture;
    HANDLE sharedHandle = nullptr;
    gs_texture_t* obsTexture = nullptr;

    std::atomic<int> frameCount = 0;
    std::atomic<bool> targetClosed = false;
    std::atomic<bool> destroying = false;
};

static bool isCaptureInactive(
    const WgcSource* ctx
)
{
    return
        !ctx ||
        ctx->destroying.load(
            std::memory_order_acquire
        ) ||
        ctx->targetClosed.load(
            std::memory_order_acquire
        );
}

static bool markCaptureTargetClosed(
    WgcSource* ctx,
    const char* reason
)
{
    if (!ctx) {
        return false;
    }

    if (
        ctx->destroying.load(
            std::memory_order_acquire
        )
    ) {
        return false;
    }

    bool expected = false;

    if (
        !ctx->targetClosed.compare_exchange_strong(
            expected,
            true,
            std::memory_order_acq_rel
        )
    ) {
        /*
         * BaÅŸka bir yol hedefi zaten kapalÄ± olarak iÅŸaretledi.
         * AynÄ± logu ve cleanup davranÄ±ÅŸÄ±nÄ± tekrar Ã§alÄ±ÅŸtÄ±rma.
         */
        return false;
    }
  
    g_activeTargetClosed.store(
        true,
        std::memory_order_release
    );

    resetWgcFrameState();

    std::cerr
        << "[Native WGC Source] "
        << "capture target closed"
        << " reason="
        << (reason ? reason : "unknown")
        << "\n";

    return true;
}

bool isWgcTargetClosed()
{
    return g_activeTargetClosed.load(
        std::memory_order_acquire
    );
}

void resetWgcTargetClosed()
{
    g_activeTargetClosed.store(
        false,
        std::memory_order_release
    );
}

bool copyWgcFrameBgra(
    std::vector<uint8_t>& pixels,
    uint32_t& width,
    uint32_t& height
)
{
    pixels.clear();
    width = 0;
    height = 0;

    /*
     * Bu mutex yalnÄ±zca pointer'Ä± okumak iÃ§in deÄŸil,
     * bÃ¼tÃ¼n kopyalama iÅŸlemi boyunca tutuluyor.
     *
     * BÃ¶ylece destroy fonksiyonu g_activeSource'u
     * temizleyip ctx'yi delete edemez.
     */
    std::lock_guard<std::mutex> activeSourceLock(
        g_activeSourceMutex
    );

    WgcSource* ctx =
        g_activeSource;

    if (!ctx) {
        std::cerr
            << "[Native WGC Source] "
            << "frame copy failed"
            << " reason=no_active_source"
            << "\n";

        return false;
    }

    const uint64_t readyGeneration =
        g_readyFrameGeneration.load(
            std::memory_order_acquire
        );

    if (
        readyGeneration == 0 ||
        readyGeneration != ctx->generation
    ) {
        std::cerr
            << "[Native WGC Source] "
            << "frame copy failed"
            << " reason=stale_frame_generation"
            << " activeGeneration="
            << ctx->generation
            << " readyGeneration="
            << readyGeneration
            << "\n";

        return false;
    }

    if (
        ctx->destroying.load(
            std::memory_order_acquire
        ) ||
        ctx->targetClosed.load(
            std::memory_order_acquire
        )
    ) {
        std::cerr
            << "[Native WGC Source] "
            << "frame copy failed"
            << " reason=source_unavailable"
            << "\n";

        return false;
    }

    std::lock_guard<std::mutex> sharedLock(
        ctx->sharedMutex
    );

    if (
        !ctx->sharedTexture ||
        !ctx->d3d.device ||
        !ctx->d3d.context
    ) {
        std::cerr
            << "[Native WGC Source] "
            << "frame copy failed"
            << " reason=texture_unavailable"
            << "\n";

        return false;
    }

    D3D11_TEXTURE2D_DESC sourceDesc{};

    ctx->sharedTexture->GetDesc(
        &sourceDesc
    );

    if (
        sourceDesc.Width == 0 ||
        sourceDesc.Height == 0
    ) {
        std::cerr
            << "[Native WGC Source] "
            << "frame copy failed"
            << " reason=invalid_texture_size"
            << "\n";

        return false;
    }

    if (
        sourceDesc.Format !=
            DXGI_FORMAT_B8G8R8A8_UNORM &&
        sourceDesc.Format !=
            DXGI_FORMAT_B8G8R8A8_UNORM_SRGB
    ) {
        std::cerr
            << "[Native WGC Source] "
            << "frame copy failed"
            << " reason=unsupported_texture_format"
            << " format="
            << static_cast<int>(
                sourceDesc.Format
            )
            << "\n";

        return false;
    }

    D3D11_TEXTURE2D_DESC stagingDesc =
        sourceDesc;

    stagingDesc.Usage =
        D3D11_USAGE_STAGING;

    stagingDesc.BindFlags = 0;

    stagingDesc.CPUAccessFlags =
        D3D11_CPU_ACCESS_READ;

    stagingDesc.MiscFlags = 0;

    winrt::com_ptr<ID3D11Texture2D>
        stagingTexture;

    HRESULT result =
        ctx->d3d.device->CreateTexture2D(
            &stagingDesc,
            nullptr,
            stagingTexture.put()
        );

    if (FAILED(result)) {
        std::cerr
            << "[Native WGC Source] "
            << "frame copy failed"
            << " reason=create_staging_texture_failed"
            << " hr=0x"
            << std::hex
            << static_cast<uint32_t>(
                result
            )
            << std::dec
            << "\n";

        return false;
    }

    ctx->d3d.context->CopyResource(
        stagingTexture.get(),
        ctx->sharedTexture.get()
    );

    D3D11_MAPPED_SUBRESOURCE mapped{};

    result =
        ctx->d3d.context->Map(
            stagingTexture.get(),
            0,
            D3D11_MAP_READ,
            0,
            &mapped
        );

    if (FAILED(result)) {
        std::cerr
            << "[Native WGC Source] "
            << "frame copy failed"
            << " reason=map_staging_texture_failed"
            << " hr=0x"
            << std::hex
            << static_cast<uint32_t>(
                result
            )
            << std::dec
            << "\n";

        return false;
    }

    const uint32_t capturedWidth =
        sourceDesc.Width;

    const uint32_t capturedHeight =
        sourceDesc.Height;

    constexpr size_t bytesPerPixel = 4;

    const size_t destinationRowBytes =
        static_cast<size_t>(
            capturedWidth
        ) *
        bytesPerPixel;

    const size_t destinationSize =
        destinationRowBytes *
        static_cast<size_t>(
            capturedHeight
        );

    try {
        pixels.resize(
            destinationSize
        );
    } catch (...) {
        ctx->d3d.context->Unmap(
            stagingTexture.get(),
            0
        );

        pixels.clear();

        std::cerr
            << "[Native WGC Source] "
            << "frame copy failed"
            << " reason=pixel_buffer_allocation_failed"
            << "\n";

        return false;
    }

    const auto* sourceBytes =
        static_cast<const uint8_t*>(
            mapped.pData
        );

    for (
        uint32_t row = 0;
        row < capturedHeight;
        ++row
    ) {
        const uint8_t* sourceRow =
            sourceBytes +
            static_cast<size_t>(row) *
            mapped.RowPitch;

        uint8_t* destinationRow =
            pixels.data() +
            static_cast<size_t>(row) *
            destinationRowBytes;

        std::memcpy(
            destinationRow,
            sourceRow,
            destinationRowBytes
        );
    }

    ctx->d3d.context->Unmap(
        stagingTexture.get(),
        0
    );

    width = capturedWidth;
    height = capturedHeight;

    std::cerr
        << "[Native WGC Source] "
        << "frame copied to CPU "
        << width
        << "x"
        << height
        << " bytes="
        << pixels.size()
        << "\n";

    return true;
}

bool isWgcFrameReady()
{
    if (
        !g_frameReady.load(
            std::memory_order_acquire
        )
    ) {
        return false;
    }

    const uint64_t activeGeneration =
        g_activeSourceGeneration.load(
            std::memory_order_acquire
        );

    const uint64_t readyGeneration =
        g_readyFrameGeneration.load(
            std::memory_order_acquire
        );

    return
        activeGeneration != 0 &&
        activeGeneration == readyGeneration;
}

uint32_t getWgcFrameWidth()
{
    return g_latestFrameWidth.load(
        std::memory_order_acquire
    );
}

uint32_t getWgcFrameHeight()
{
    return g_latestFrameHeight.load(
        std::memory_order_acquire
    );
}

void resetWgcFrameState()
{
    g_frameReady.store(
        false,
        std::memory_order_release
    );

    g_latestFrameWidth.store(
        0,
        std::memory_order_release
    );

    g_latestFrameHeight.store(
        0,
        std::memory_order_release
    );

    g_readyFrameGeneration.store(
        0,
        std::memory_order_release
    );
}

void setWgcConfig(
    WgcTargetType targetType,
    uintptr_t hwnd,
    int monitorIndex,
    int delayMs,
    bool debugFrames
)
{
    resetWgcTargetClosed();
    resetWgcFrameState();

    g_targetType = targetType;
    g_targetHwnd = hwnd;
    g_monitorIndex = monitorIndex;
    g_captureDelayMs =
        delayMs < 0
            ? 0
            : delayMs;

    g_debugFrames = debugFrames;
}

static bool requestBorderlessCaptureAccess()
{
    using namespace winrt::Windows::Graphics::Capture;

    if (g_borderlessAccessRequested.load(std::memory_order_acquire)) {
        return g_borderlessAccessGranted.load(std::memory_order_acquire);
    }

    g_borderlessAccessRequested.store(true, std::memory_order_release);

    try {
        const auto status =
            GraphicsCaptureAccess::RequestAccessAsync(
                GraphicsCaptureAccessKind::Borderless
            ).get();

        const bool granted =
            status ==
            winrt::Windows::Security::Authorization::AppCapabilityAccess::
                AppCapabilityAccessStatus::Allowed;

        g_borderlessAccessGranted.store(
            granted,
            std::memory_order_release
        );

        std::cerr
            << "[Native WGC Source] borderless access result="
            << static_cast<int>(status)
            << " granted="
            << (granted ? "yes" : "no")
            << "\n";

        return granted;
    } catch (const winrt::hresult_error& error) {
        g_borderlessAccessGranted.store(
            false,
            std::memory_order_release
        );

        std::wcerr
            << L"[Native WGC Source] borderless access request failed"
            << L" message=" << error.message().c_str()
            << L" hr=0x"
            << std::hex
            << static_cast<uint32_t>(error.code())
            << std::dec
            << L"\n";

        return false;
    }
}

static D3DDeviceBundle createD3DDeviceBundle()
{
    D3DDeviceBundle bundle;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };

    D3D_FEATURE_LEVEL usedLevel{};

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels,
        2,
        D3D11_SDK_VERSION,
        bundle.device.put(),
        &usedLevel,
        bundle.context.put()
    );

    if (FAILED(hr)) {
        throw winrt::hresult_error(hr, L"D3D11CreateDevice failed");
    }

    winrt::com_ptr<IDXGIDevice> dxgiDevice;
    bundle.device.as(dxgiDevice);

    winrt::com_ptr<::IInspectable> inspectable;

    hr = CreateDirect3D11DeviceFromDXGIDevice(
        dxgiDevice.get(),
        inspectable.put()
    );

    if (FAILED(hr)) {
        throw winrt::hresult_error(hr, L"CreateDirect3D11DeviceFromDXGIDevice failed");
    }

    bundle.winrtDevice =
        inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();

    return bundle;
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

struct MonitorEnumData {
    int targetIndex = 0;
    int currentIndex = 0;
    HMONITOR result = nullptr;
};

static BOOL CALLBACK enumMonitorCallback(
    HMONITOR monitor,
    HDC,
    LPRECT,
    LPARAM data
)
{
    auto* enumData = reinterpret_cast<MonitorEnumData*>(data);

    if (enumData->currentIndex == enumData->targetIndex) {
        enumData->result = monitor;
        return FALSE;
    }

    enumData->currentIndex++;
    return TRUE;
}

static HMONITOR getMonitorByIndex(int index)
{
    MonitorEnumData data;
    data.targetIndex = index;

    EnumDisplayMonitors(
        nullptr,
        nullptr,
        enumMonitorCallback,
        reinterpret_cast<LPARAM>(&data)
    );

    return data.result;
}

static winrt::Windows::Graphics::Capture::GraphicsCaptureItem createItemForMonitor(HMONITOR monitor)
{
    auto factory = winrt::get_activation_factory<
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
        IGraphicsCaptureItemInterop
    >();

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{ nullptr };

    HRESULT hr = factory->CreateForMonitor(
        monitor,
        winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
        winrt::put_abi(item)
    );

    if (FAILED(hr) || !item) {
        throw winrt::hresult_error(hr, L"CreateForMonitor failed");
    }

    return item;
}

static winrt::com_ptr<ID3D11Texture2D> getTextureFromSurface(
    const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface& surface
)
{
    auto access =
        surface.as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();

    winrt::com_ptr<ID3D11Texture2D> texture;

    HRESULT hr = access->GetInterface(
        __uuidof(ID3D11Texture2D),
        texture.put_void()
    );

    if (FAILED(hr)) {
        throw winrt::hresult_error(hr, L"GetInterface(ID3D11Texture2D) failed");
    }

    return texture;
}

static void destroyObsSharedTexture(
    WgcSource* ctx
)
{
    if (!ctx || !ctx->obsTexture) {
        return;
    }

    obs_enter_graphics();
    gs_texture_destroy(ctx->obsTexture);
    obs_leave_graphics();

    ctx->obsTexture = nullptr;
}

static bool createSharedTextureLocked(
    WgcSource* ctx,
    const D3D11_TEXTURE2D_DESC& sourceDesc
)
{
    D3D11_TEXTURE2D_DESC sharedDesc = sourceDesc;

    sharedDesc.Usage = D3D11_USAGE_DEFAULT;
    sharedDesc.BindFlags =
        D3D11_BIND_SHADER_RESOURCE |
        D3D11_BIND_RENDER_TARGET;
    sharedDesc.CPUAccessFlags = 0;
    sharedDesc.MiscFlags =
        D3D11_RESOURCE_MISC_SHARED;

    HRESULT result =
        ctx->d3d.device->CreateTexture2D(
            &sharedDesc,
            nullptr,
            ctx->sharedTexture.put()
        );

    if (FAILED(result)) {
        std::cerr
            << "[Native WGC Source] "
            << "CreateTexture2D shared failed: 0x"
            << std::hex
            << static_cast<uint32_t>(result)
            << std::dec
            << "\n";

        ctx->sharedTexture = nullptr;
        return false;
    }

    winrt::com_ptr<IDXGIResource> dxgiResource;
    ctx->sharedTexture.as(dxgiResource);

    result =
        dxgiResource->GetSharedHandle(
            &ctx->sharedHandle
        );

    if (
        FAILED(result) ||
        !ctx->sharedHandle
    ) {
        std::cerr
            << "[Native WGC Source] "
            << "GetSharedHandle failed: 0x"
            << std::hex
            << static_cast<uint32_t>(result)
            << std::dec
            << "\n";

        ctx->sharedTexture = nullptr;
        ctx->sharedHandle = nullptr;
        return false;
    }

    ctx->width = sharedDesc.Width;
    ctx->height = sharedDesc.Height;

    std::cerr
        << "[Native WGC Source] shared texture created "
        << sharedDesc.Width
        << "x"
        << sharedDesc.Height
        << " handle="
        << ctx->sharedHandle
        << "\n";

    return true;
}

static void resetSharedTextureLocked(
    WgcSource* ctx
)
{
    /*
     * Bu fonksiyon Ã§aÄŸrÄ±lÄ±rken sharedMutex kilitli olmalÄ±.
     */
    destroyObsSharedTexture(ctx);

    ctx->sharedTexture = nullptr;
    ctx->sharedHandle = nullptr;
}

static const char* wgc_get_name(void*)
{
    return "Native WGC Capture";
}

static void* wgc_create(obs_data_t*, obs_source_t*)
{
    auto* ctx = new WgcSource();

    ctx->generation =
        g_nextSourceGeneration.fetch_add(
            1,
            std::memory_order_acq_rel
        );

    {
        std::lock_guard<std::mutex> lock(
            g_activeSourceMutex
        );

        g_activeSource = ctx;

        g_activeSourceGeneration.store(
            ctx->generation,
            std::memory_order_release
        );
    }

    resetWgcFrameState();

    ctx->targetType = g_targetType;
    ctx->hwndValue = g_targetHwnd;
    const int monitorIndex = g_monitorIndex;
    
    std::cerr
        << "[Native WGC Source] created"
        << " generation="
        << ctx->generation
        << " hwnd="
        << ctx->hwndValue
        << "\n";

    try {
        ensureCurrentThreadComApartment();

        std::cerr << "[Native WGC Source] waiting before capture: "
                << g_captureDelayMs
                << "ms\n";

        Sleep(g_captureDelayMs);

        if (ctx->targetType == WgcTargetType::Monitor) {
            HMONITOR monitor = getMonitorByIndex(monitorIndex);

            if (!monitor) {
                std::cerr << "[Native WGC Source] invalid monitor index: "
                        << monitorIndex << "\n";
                return ctx;
            }

            std::cerr << "[Native WGC Source] creating monitor item, index="
                    << monitorIndex << "\n";

            ctx->item = createItemForMonitor(monitor);
        } else {
            HWND hwnd = reinterpret_cast<HWND>(ctx->hwndValue);

            if (!IsWindow(hwnd)) {
                std::cerr << "[Native WGC Source] invalid hwnd\n";
                return ctx;
            }

            GetWindowThreadProcessId(
                hwnd,
                &ctx->targetProcessId
            );

            if (ctx->targetProcessId == 0) {
                std::cerr
                    << "[Native WGC Source] "
                    << "failed to resolve target process id\n";

                return ctx;
            }

            ctx->lastTargetWindowCheckAt =
                std::chrono::steady_clock::now();

            std::cout
                << "[Native WGC Source] "
                << "creating window item, hwnd="
                << ctx->hwndValue
                << " pid="
                << ctx->targetProcessId
                << "\n";

            ctx->item = createItemForHwnd(hwnd);
        }

        auto size = ctx->item.Size();
        ctx->width = static_cast<uint32_t>(size.Width);
        ctx->height = static_cast<uint32_t>(size.Height);

        ctx->framePoolWidth = ctx->width;
        ctx->framePoolHeight = ctx->height;

        ctx->itemClosedToken =
            ctx->item.Closed(
                [ctx](
                    auto const&,
                    auto const&
                ) {
                    markCaptureTargetClosed(
                        ctx,
                        "graphics-capture-item-closed"
                    );
                }
            );

        std::cerr << "[Native WGC Source] item size="
                  << ctx->width << "x" << ctx->height << "\n";

        ctx->d3d = createD3DDeviceBundle();

        ctx->framePool =
            winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
                ctx->d3d.winrtDevice,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                2,
                size
            );

        ctx->session = ctx->framePool.CreateCaptureSession(ctx->item);
        ctx->session.IsCursorCaptureEnabled(true);

        const bool borderlessGranted =
            requestBorderlessCaptureAccess();

        if (borderlessGranted) {
            try {
                ctx->session.IsBorderRequired(false);

                std::cerr
                    << "[Native WGC Source] capture border disabled\n";
            } catch (const winrt::hresult_error& error) {
                std::wcerr
                    << L"[Native WGC Source] failed to disable capture border"
                    << L" message=" << error.message().c_str()
                    << L" hr=0x"
                    << std::hex
                    << static_cast<uint32_t>(error.code())
                    << std::dec
                    << L"\n";
            }
        } else {
            std::cerr
                << "[Native WGC Source] borderless capture unavailable; "
                << "system border will remain enabled\n";
        }

        ctx->frameToken =
        ctx->framePool.FrameArrived(
            [ctx](
                auto const& sender,
                auto const&
            ) {
                if (isCaptureInactive(ctx)) {
                    return;
                }

                try {
                    auto frame =
                        sender.TryGetNextFrame();

                    if (!frame) {
                        return;
                    }

                    const auto contentSize =
                        frame.ContentSize();

                    if (
                        contentSize.Width <= 0 ||
                        contentSize.Height <= 0
                    ) {
                        return;
                    }

                    const uint32_t contentWidth =
                            static_cast<uint32_t>(
                                contentSize.Width
                            );

                        const uint32_t contentHeight =
                            static_cast<uint32_t>(
                                contentSize.Height
                            );

                        bool shouldRecreateFramePool = false;
                        uint32_t resizeWidth = 0;
                        uint32_t resizeHeight = 0;

                        {
                            std::lock_guard<std::mutex> poolLock(
                                ctx->framePoolMutex
                            );

                            const bool contentMatchesFramePool =
                                contentWidth == ctx->framePoolWidth &&
                                contentHeight == ctx->framePoolHeight;

                            if (contentMatchesFramePool) {
                                /*
                                * Boyut tekrar mevcut frame pool boyutuna dÃ¶ndÃ¼yse
                                * bekleyen resize iÅŸlemini iptal et.
                                */
                                ctx->framePoolResizePending = false;
                                ctx->pendingFramePoolWidth = 0;
                                ctx->pendingFramePoolHeight = 0;
                            } else {
                                const auto now =
                                    std::chrono::steady_clock::now();

                                const bool pendingSizeChanged =
                                    !ctx->framePoolResizePending ||
                                    contentWidth !=
                                        ctx->pendingFramePoolWidth ||
                                    contentHeight !=
                                        ctx->pendingFramePoolHeight;

                                if (pendingSizeChanged) {
                                    /*
                                    * Yeni bir ara boyut gÃ¶rdÃ¼k.
                                    * Debounce sÃ¼resini bu boyut iÃ§in yeniden baÅŸlat.
                                    */
                                    ctx->pendingFramePoolWidth =
                                        contentWidth;

                                    ctx->pendingFramePoolHeight =
                                        contentHeight;

                                    ctx->framePoolResizeStableSince =
                                        now;

                                    ctx->framePoolResizePending =
                                        true;
                                } else {
                                    const auto stableDuration =
                                        now -
                                        ctx->framePoolResizeStableSince;

                                    if (
                                        stableDuration >=
                                        kFramePoolResizeDebounce
                                    ) {
                                        shouldRecreateFramePool = true;

                                        resizeWidth =
                                            ctx->pendingFramePoolWidth;

                                        resizeHeight =
                                            ctx->pendingFramePoolHeight;
                                    }
                                }
                            }
                        }

                        if (shouldRecreateFramePool) {
                            /*
                            * Resize iÃ§in kullandÄ±ÄŸÄ±mÄ±z frame eski frame-pool
                            * texture boyutunda olabilir. Ã–nce frame'i bÄ±rak,
                            * ardÄ±ndan frame pool'u yeni sabit boyutla oluÅŸtur.
                            */
                            frame.Close();

                            std::lock_guard<std::mutex> poolLock(
                                ctx->framePoolMutex
                            );

                            if (
                                !ctx->destroying.load(
                                    std::memory_order_acquire
                                ) &&
                                ctx->framePool &&
                                ctx->framePoolResizePending &&
                                resizeWidth ==
                                    ctx->pendingFramePoolWidth &&
                                resizeHeight ==
                                    ctx->pendingFramePoolHeight
                            ) {
                                const winrt::Windows::Graphics::SizeInt32
                                    newFramePoolSize{
                                        static_cast<int32_t>(
                                            resizeWidth
                                        ),
                                        static_cast<int32_t>(
                                            resizeHeight
                                        )
                                    };

                                ctx->framePool.Recreate(
                                    ctx->d3d.winrtDevice,
                                    winrt::Windows::Graphics::
                                        DirectX::
                                        DirectXPixelFormat::
                                        B8G8R8A8UIntNormalized,
                                    2,
                                    newFramePoolSize
                                );

                                ctx->framePoolWidth =
                                    resizeWidth;

                                ctx->framePoolHeight =
                                    resizeHeight;

                                ctx->framePoolResizePending = false;
                                ctx->pendingFramePoolWidth = 0;
                                ctx->pendingFramePoolHeight = 0;

                                std::cerr
                                    << "[Native WGC Source] "
                                    << "frame pool resized "
                                    << resizeWidth
                                    << "x"
                                    << resizeHeight
                                    << "\n";
                            }

                            return;
                        }
                    auto texture =
                        getTextureFromSurface(
                            frame.Surface()
                        );

                    D3D11_TEXTURE2D_DESC sourceDesc{};
                    texture->GetDesc(&sourceDesc);

                    bool sizeChanged = false;

                    {
                        std::lock_guard<std::mutex> lock(
                            ctx->sharedMutex
                        );

                        if (ctx->sharedTexture) {
                            D3D11_TEXTURE2D_DESC currentDesc{};
                            ctx->sharedTexture->GetDesc(
                                &currentDesc
                            );

                            sizeChanged =
                                currentDesc.Width !=
                                    sourceDesc.Width ||
                                currentDesc.Height !=
                                    sourceDesc.Height ||
                                currentDesc.Format !=
                                    sourceDesc.Format;
                        }

                        if (
                            !ctx->sharedTexture ||
                            sizeChanged
                        ) {
                            if (sizeChanged) {
                                std::cerr
                                    << "[Native WGC Source] "
                                    << "capture size changed to "
                                    << sourceDesc.Width
                                    << "x"
                                    << sourceDesc.Height
                                    << "\n";
                            }

                            resetSharedTextureLocked(ctx);

                            if (
                                !createSharedTextureLocked(
                                    ctx,
                                    sourceDesc
                                )
                            ) {
                                return;
                            }
                        }

                        ctx->d3d.context->CopyResource(
                            ctx->sharedTexture.get(),
                            texture.get()
                        );

                        const uint64_t activeGeneration =
                            g_activeSourceGeneration.load(
                                std::memory_order_acquire
                            );

                        /*
                        * Eski bir source'un gecikmiÅŸ FrameArrived callback'i
                        * yeni preview state'ini kirletemez.
                        */
                        if (activeGeneration != ctx->generation) {
                            return;
                        }

                        g_latestFrameWidth.store(
                            sourceDesc.Width,
                            std::memory_order_release
                        );

                        g_latestFrameHeight.store(
                            sourceDesc.Height,
                            std::memory_order_release
                        );

                        g_readyFrameGeneration.store(
                            ctx->generation,
                            std::memory_order_release
                        );

                        g_frameReady.store(
                            true,
                            std::memory_order_release
                        );
                    }

                    const int count =
                        ++ctx->frameCount;

                    if (
                        g_debugFrames &&
                        count <= 5
                    ) {
                        std::cerr
                            << "[Native WGC Source] frame #"
                            << count
                            << " contentSize="
                            << contentSize.Width
                            << "x"
                            << contentSize.Height
                            << " texture="
                            << sourceDesc.Width
                            << "x"
                            << sourceDesc.Height
                            << " format="
                            << sourceDesc.Format
                            << "\n";
                    }

                    /*
                    * Frame artÄ±k kullanÄ±lmÄ±yor. Recreate iÅŸlemini
                    * frame kapandÄ±ktan sonra gerÃ§ekleÅŸtiriyoruz.
                    */
                    frame.Close();

                } catch (
                    const winrt::hresult_error& error
                ) {
                    if (
                        ctx->destroying.load(
                            std::memory_order_acquire
                        )
                    ) {
                        return;
                    }

                    std::wcerr
                        << L"[Native WGC Source] "
                        << L"frame processing failed"
                        << L" message="
                        << error.message().c_str()
                        << L" hr=0x"
                        << std::hex
                        << static_cast<uint32_t>(
                            error.code()
                        )
                        << std::dec
                        << L"\n";
                } catch (
                    const std::exception& error
                ) {
                    if (
                        ctx->destroying.load(
                            std::memory_order_acquire
                        )
                    ) {
                        return;
                    }

                    std::cerr
                        << "[Native WGC Source] "
                        << "frame processing failed: "
                        << error.what()
                        << "\n";
                }
            }
        );

        ctx->session.StartCapture();

        std::cerr << "[Native WGC Source] capture started\n";
    } catch (const winrt::hresult_error& e) {
        std::wcerr << L"[Native WGC Source] failed: "
                   << e.message().c_str()
                   << L" hr=0x"
                   << std::hex
                   << static_cast<uint32_t>(e.code())
                   << L"\n";
    }

    return ctx;
}

static void wgc_destroy(void* data)
{
    auto* ctx =
        static_cast<WgcSource*>(data);

    if (!ctx) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(
            g_activeSourceMutex
        );

        if (g_activeSource == ctx) {
            g_activeSource = nullptr;

            g_activeSourceGeneration.store(
                0,
                std::memory_order_release
            );

            resetWgcFrameState();
        }
    }   

    ctx->destroying.store(
        true,
        std::memory_order_release
    );

    std::cerr
        << "[Native WGC Source] destroyed, frames="
        << ctx->frameCount.load()
        << "\n";

    try {
        if (ctx->item) {
            ctx->item.Closed(
                ctx->itemClosedToken
            );
        }
    } catch (...) {
    }

    try {
        std::lock_guard<std::mutex> poolLock(
            ctx->framePoolMutex
        );

        if (ctx->framePool) {
            ctx->framePool.FrameArrived(
                ctx->frameToken
            );
        }

        if (ctx->session) {
            ctx->session.Close();
            ctx->session = nullptr;
        }

        if (ctx->framePool) {
            ctx->framePool.Close();
            ctx->framePool = nullptr;
        }
    } catch (
        const winrt::hresult_error& error
    ) {
        std::wcerr
            << L"[Native WGC Source] cleanup warning"
            << L" message="
            << error.message().c_str()
            << L" hr=0x"
            << std::hex
            << static_cast<uint32_t>(
                error.code()
            )
            << std::dec
            << L"\n";
    } catch (...) {
    }

    {
        std::lock_guard<std::mutex> lock(
            ctx->sharedMutex
        );

        resetSharedTextureLocked(ctx);
    }

    ctx->item = nullptr;
    ctx->d3d.winrtDevice = nullptr;
    ctx->d3d.context = nullptr;
    ctx->d3d.device = nullptr;

    delete ctx;
}


static uint32_t wgc_get_width(void* data)
{
    auto* ctx =
        static_cast<WgcSource*>(data);

    return
        ctx
            ? ctx->width
            : kDefaultCaptureWidth;
}

static uint32_t wgc_get_height(void* data)
{
    auto* ctx =
        static_cast<WgcSource*>(data);

    return
        ctx
            ? ctx->height
            : kDefaultCaptureHeight;
}

static void wgc_video_tick(
    void* data,
    float
)
{
    auto* ctx =
        static_cast<WgcSource*>(
            data
        );

    if (
        isCaptureInactive(ctx) ||
        ctx->targetType !=
            WgcTargetType::Window ||
        ctx->hwndValue == 0
    ) {
        return;
    }

    const auto now =
        std::chrono::steady_clock::now();

    if (
        ctx->lastTargetWindowCheckAt
            .time_since_epoch()
            .count() != 0 &&
        now - ctx->lastTargetWindowCheckAt <
            kTargetWindowCheckInterval
    ) {
        return;
    }

    ctx->lastTargetWindowCheckAt = now;

    const HWND hwnd =
        reinterpret_cast<HWND>(
            ctx->hwndValue
        );

    if (!IsWindow(hwnd)) {
        markCaptureTargetClosed(
            ctx,
            "window-destroyed"
        );

        return;
    }

    DWORD currentProcessId = 0;

    GetWindowThreadProcessId(
        hwnd,
        &currentProcessId
    );

    if (
        currentProcessId == 0 ||
        currentProcessId !=
            ctx->targetProcessId
    ) {
        markCaptureTargetClosed(
            ctx,
            "window-handle-invalidated"
        );
    }
}

static void wgc_video_render(void* data, gs_effect_t*)
{
    auto* ctx = static_cast<WgcSource*>(data);

    if (isCaptureInactive(ctx)) {
        return;
    }


    std::lock_guard<std::mutex> lock(ctx->sharedMutex);

    if (!ctx->sharedHandle) {
        return;
    }

    if (!gs_shared_texture_available()) {
        static bool warned = false;
        if (!warned) {
            std::cerr << "[Native WGC Source] shared texture not available\n";
            warned = true;
        }
        return;
    }

    if (!ctx->obsTexture) {
        uint32_t handle32 = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(ctx->sharedHandle)
        );

        ctx->obsTexture = gs_texture_open_shared(handle32);

        if (!ctx->obsTexture) {
            static bool warned = false;
            if (!warned) {
                std::cout << "[Native WGC Source] gs_texture_open_shared failed\n";
                warned = true;
            }
            return;
        }

        std::cerr << "[Native WGC Source] OBS opened shared texture\n";
    }

    obs_source_draw(ctx->obsTexture, 0, 0, ctx->width, ctx->height, false);
}

bool registerWgcSource()
{
    obs_source_info info = {};
    info.id = "wgc_capture";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO;
    info.get_name = wgc_get_name;
    info.create = wgc_create;
    info.destroy = wgc_destroy;
    info.get_width = wgc_get_width;
    info.get_height = wgc_get_height;
    info.video_tick = wgc_video_tick;
    info.video_render = wgc_video_render;

    obs_register_source(&info);

    std::cerr << "[Native WGC Source] registered\n";

    return true;
}
