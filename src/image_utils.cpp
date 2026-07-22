#include "image_utils.h"

#define NOMINMAX
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <iostream>
#include <limits>
#include <objidl.h>

namespace {

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

void logHresult(
    const char* operation,
    HRESULT result
)
{
    std::cerr
        << "[Image Utils] "
        << operation
        << " failed"
        << " hr=0x"
        << std::hex
        << static_cast<uint32_t>(result)
        << std::dec
        << "\n";
}

}

bool resizeBgraFrame(
    const std::vector<uint8_t>& sourcePixels,
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    uint32_t maxWidth,
    uint32_t maxHeight,
    std::vector<uint8_t>& resizedPixels,
    uint32_t& resizedWidth,
    uint32_t& resizedHeight
)
{
    resizedPixels.clear();
    resizedWidth = 0;
    resizedHeight = 0;

    if (
        sourcePixels.empty() ||
        sourceWidth == 0 ||
        sourceHeight == 0 ||
        maxWidth == 0 ||
        maxHeight == 0
    ) {
        std::cerr
            << "[Image Utils] "
            << "resize rejected"
            << " reason=invalid_arguments"
            << "\n";

        return false;
    }

    constexpr size_t bytesPerPixel = 4;

    const size_t sourceStride =
        static_cast<size_t>(sourceWidth) *
        bytesPerPixel;

    const size_t expectedSourceSize =
        sourceStride *
        static_cast<size_t>(sourceHeight);

    if (
        sourcePixels.size() !=
        expectedSourceSize
    ) {
        std::cerr
            << "[Image Utils] "
            << "resize rejected"
            << " reason=invalid_source_buffer"
            << " expected="
            << expectedSourceSize
            << " actual="
            << sourcePixels.size()
            << "\n";

        return false;
    }

    /*
     * KaynaÄŸÄ± maxWidth x maxHeight sÄ±nÄ±rlarÄ±nÄ±n iÃ§ine
     * en-boy oranÄ±nÄ± bozmadan sÄ±ÄŸdÄ±rÄ±yoruz.
     */
    const uint64_t widthComparison =
        static_cast<uint64_t>(sourceWidth) *
        static_cast<uint64_t>(maxHeight);

    const uint64_t heightComparison =
        static_cast<uint64_t>(sourceHeight) *
        static_cast<uint64_t>(maxWidth);

    if (widthComparison >= heightComparison) {
        resizedWidth = maxWidth;

        resizedHeight =
            static_cast<uint32_t>(
                (
                    static_cast<uint64_t>(
                        sourceHeight
                    ) *
                    static_cast<uint64_t>(
                        maxWidth
                    ) +
                    sourceWidth / 2
                ) /
                sourceWidth
            );
    } else {
        resizedHeight = maxHeight;

        resizedWidth =
            static_cast<uint32_t>(
                (
                    static_cast<uint64_t>(
                        sourceWidth
                    ) *
                    static_cast<uint64_t>(
                        maxHeight
                    ) +
                    sourceHeight / 2
                ) /
                sourceHeight
            );
    }

    if (resizedWidth == 0) {
        resizedWidth = 1;
    }

    if (resizedHeight == 0) {
        resizedHeight = 1;
    }

    const size_t resizedStride =
        static_cast<size_t>(resizedWidth) *
        bytesPerPixel;

    const size_t resizedBufferSize =
        resizedStride *
        static_cast<size_t>(resizedHeight);

    if (
        sourceStride >
            static_cast<size_t>(
                (std::numeric_limits<UINT>::max)()
            ) ||
        expectedSourceSize >
            static_cast<size_t>(
                (std::numeric_limits<UINT>::max)()
            ) ||
        resizedStride >
            static_cast<size_t>(
                (std::numeric_limits<UINT>::max)()
            ) ||
        resizedBufferSize >
            static_cast<size_t>(
                (std::numeric_limits<UINT>::max)()
            )
    ) {
        std::cerr
            << "[Image Utils] "
            << "resize rejected"
            << " reason=frame_too_large"
            << "\n";

        resizedWidth = 0;
        resizedHeight = 0;

        return false;
    }

    HRESULT comResult =
        CoInitializeEx(
            nullptr,
            COINIT_MULTITHREADED
        );

    const bool mustUninitialize =
        SUCCEEDED(comResult);

    if (
        FAILED(comResult) &&
        comResult != RPC_E_CHANGED_MODE
    ) {
        logHresult(
            "Resize CoInitializeEx",
            comResult
        );

        resizedWidth = 0;
        resizedHeight = 0;

        return false;
    }

    ComPtr<IWICImagingFactory> factory;

    HRESULT result =
        CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(
                factory.GetAddressOf()
            )
        );

    if (FAILED(result)) {
        logHresult(
            "Resize WIC factory",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        resizedWidth = 0;
        resizedHeight = 0;

        return false;
    }

    ComPtr<IWICBitmap> sourceBitmap;

    result =
        factory->CreateBitmapFromMemory(
            sourceWidth,
            sourceHeight,
            GUID_WICPixelFormat32bppBGRA,
            static_cast<UINT>(sourceStride),
            static_cast<UINT>(
                expectedSourceSize
            ),
            const_cast<BYTE*>(
                reinterpret_cast<const BYTE*>(
                    sourcePixels.data()
                )
            ),
            sourceBitmap.GetAddressOf()
        );

    if (FAILED(result)) {
        logHresult(
            "CreateBitmapFromMemory",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        resizedWidth = 0;
        resizedHeight = 0;

        return false;
    }

    ComPtr<IWICBitmapScaler> scaler;

    result =
        factory->CreateBitmapScaler(
            scaler.GetAddressOf()
        );

    if (FAILED(result)) {
        logHresult(
            "CreateBitmapScaler",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        resizedWidth = 0;
        resizedHeight = 0;

        return false;
    }

    result =
        scaler->Initialize(
            sourceBitmap.Get(),
            resizedWidth,
            resizedHeight,
            WICBitmapInterpolationModeFant
        );

    if (FAILED(result)) {
        logHresult(
            "BitmapScaler Initialize",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        resizedWidth = 0;
        resizedHeight = 0;

        return false;
    }

    resizedPixels.resize(
        resizedBufferSize
    );

    result =
        scaler->CopyPixels(
            nullptr,
            static_cast<UINT>(
                resizedStride
            ),
            static_cast<UINT>(
                resizedBufferSize
            ),
            reinterpret_cast<BYTE*>(
                resizedPixels.data()
            )
        );

    if (FAILED(result)) {
        logHresult(
            "BitmapScaler CopyPixels",
            result
        );

        resizedPixels.clear();
        resizedWidth = 0;
        resizedHeight = 0;

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    if (mustUninitialize) {
        CoUninitialize();
    }

    std::cerr
        << "[Image Utils] "
        << "frame resized "
        << sourceWidth
        << "x"
        << sourceHeight
        << " -> "
        << resizedWidth
        << "x"
        << resizedHeight
        << " bytes="
        << resizedPixels.size()
        << "\n";

    return true;
}

bool encodeBgraFrameToPng(
    const std::vector<uint8_t>& pixels,
    uint32_t width,
    uint32_t height,
    std::vector<uint8_t>& pngBytes
)
{
    pngBytes.clear();

    if (
        pixels.empty() ||
        width == 0 ||
        height == 0
    ) {
        std::cerr
            << "[Image Utils] "
            << "PNG memory encode rejected"
            << " reason=invalid_arguments"
            << "\n";

        return false;
    }

    constexpr size_t bytesPerPixel = 4;

    const size_t rowBytes =
        static_cast<size_t>(width) *
        bytesPerPixel;

    const size_t expectedSize =
        rowBytes *
        static_cast<size_t>(height);

    if (pixels.size() != expectedSize) {
        std::cerr
            << "[Image Utils] "
            << "PNG memory encode rejected"
            << " reason=invalid_buffer_size"
            << " expected="
            << expectedSize
            << " actual="
            << pixels.size()
            << "\n";

        return false;
    }

    if (
        rowBytes >
            static_cast<size_t>(
                (std::numeric_limits<UINT>::max)()
            ) ||
        expectedSize >
            static_cast<size_t>(
                (std::numeric_limits<UINT>::max)()
            )
    ) {
        std::cerr
            << "[Image Utils] "
            << "PNG memory encode rejected"
            << " reason=frame_too_large"
            << "\n";

        return false;
    }

    HRESULT comResult =
        CoInitializeEx(
            nullptr,
            COINIT_MULTITHREADED
        );

    const bool mustUninitialize =
        SUCCEEDED(comResult);

    if (
        FAILED(comResult) &&
        comResult != RPC_E_CHANGED_MODE
    ) {
        logHresult(
            "PNG memory CoInitializeEx",
            comResult
        );

        return false;
    }

    ComPtr<IWICImagingFactory> factory;

    HRESULT result =
        CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(
                factory.GetAddressOf()
            )
        );

    if (FAILED(result)) {
        logHresult(
            "PNG memory WIC factory",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    ComPtr<IStream> memoryStream;

    result =
        CreateStreamOnHGlobal(
            nullptr,
            TRUE,
            memoryStream.GetAddressOf()
        );

    if (FAILED(result)) {
        logHresult(
            "CreateStreamOnHGlobal",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;

    result =
        factory->CreateEncoder(
            GUID_ContainerFormatPng,
            nullptr,
            encoder.GetAddressOf()
        );

    if (FAILED(result)) {
        logHresult(
            "PNG memory CreateEncoder",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    result =
        encoder->Initialize(
            memoryStream.Get(),
            WICBitmapEncoderNoCache
        );

    if (FAILED(result)) {
        logHresult(
            "PNG memory Encoder Initialize",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;

    ComPtr<IPropertyBag2> properties;

    result =
        encoder->CreateNewFrame(
            frame.GetAddressOf(),
            properties.GetAddressOf()
        );

    if (FAILED(result)) {
        logHresult(
            "PNG memory CreateNewFrame",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    result =
        frame->Initialize(
            properties.Get()
        );

    if (FAILED(result)) {
        logHresult(
            "PNG memory Frame Initialize",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    result =
        frame->SetSize(
            width,
            height
        );

    if (FAILED(result)) {
        logHresult(
            "PNG memory SetSize",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    WICPixelFormatGUID pixelFormat =
        GUID_WICPixelFormat32bppBGRA;

    result =
        frame->SetPixelFormat(
            &pixelFormat
        );

    if (FAILED(result)) {
        logHresult(
            "PNG memory SetPixelFormat",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    if (
        pixelFormat !=
        GUID_WICPixelFormat32bppBGRA
    ) {
        std::cerr
            << "[Image Utils] "
            << "PNG memory encode failed"
            << " reason=unsupported_pixel_format"
            << "\n";

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    result =
        frame->WritePixels(
            height,
            static_cast<UINT>(rowBytes),
            static_cast<UINT>(expectedSize),
            const_cast<BYTE*>(
                reinterpret_cast<const BYTE*>(
                    pixels.data()
                )
            )
        );

    if (FAILED(result)) {
        logHresult(
            "PNG memory WritePixels",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    result = frame->Commit();

    if (FAILED(result)) {
        logHresult(
            "PNG memory Frame Commit",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    result = encoder->Commit();

    if (FAILED(result)) {
        logHresult(
            "PNG memory Encoder Commit",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    STATSTG streamStats{};

    result =
        memoryStream->Stat(
            &streamStats,
            STATFLAG_NONAME
        );

    if (FAILED(result)) {
        logHresult(
            "PNG memory Stream Stat",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    if (
        streamStats.cbSize.HighPart != 0 ||
        streamStats.cbSize.LowPart == 0
    ) {
        std::cerr
            << "[Image Utils] "
            << "PNG memory encode failed"
            << " reason=invalid_stream_size"
            << "\n";

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    const ULONG pngSize =
        streamStats.cbSize.LowPart;

    LARGE_INTEGER streamStart{};

    result =
        memoryStream->Seek(
            streamStart,
            STREAM_SEEK_SET,
            nullptr
        );

    if (FAILED(result)) {
        logHresult(
            "PNG memory Stream Seek",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    pngBytes.resize(
        static_cast<size_t>(pngSize)
    );

    ULONG bytesRead = 0;

    result =
        memoryStream->Read(
            pngBytes.data(),
            pngSize,
            &bytesRead
        );

    if (
        FAILED(result) ||
        bytesRead != pngSize
    ) {
        if (FAILED(result)) {
            logHresult(
                "PNG memory Stream Read",
                result
            );
        } else {
            std::cerr
                << "[Image Utils] "
                << "PNG memory Stream Read failed"
                << " expected="
                << pngSize
                << " actual="
                << bytesRead
                << "\n";
        }

        pngBytes.clear();

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    if (mustUninitialize) {
        CoUninitialize();
    }

    std::cerr
        << "[Image Utils] "
        << "PNG encoded to memory "
        << width
        << "x"
        << height
        << " bytes="
        << pngBytes.size()
        << "\n";

    return true;
}


bool saveBgraFrameAsPng(
    const std::vector<uint8_t>& pixels,
    uint32_t width,
    uint32_t height,
    const std::wstring& outputPath
)
{
    if (
        pixels.empty() ||
        width == 0 ||
        height == 0 ||
        outputPath.empty()
    ) {
        std::cerr
            << "[Image Utils] "
            << "PNG save rejected"
            << " reason=invalid_arguments"
            << "\n";

        return false;
    }

    constexpr size_t bytesPerPixel = 4;

    const size_t rowBytes =
        static_cast<size_t>(width) *
        bytesPerPixel;

    const size_t expectedSize =
        rowBytes *
        static_cast<size_t>(height);

    if (pixels.size() != expectedSize) {
        std::cerr
            << "[Image Utils] "
            << "PNG save rejected"
            << " reason=invalid_buffer_size"
            << " expected="
            << expectedSize
            << " actual="
            << pixels.size()
            << "\n";

        return false;
    }

    if (
        rowBytes >
            static_cast<size_t>(
                std::numeric_limits<UINT>::max()
            ) ||
        expectedSize >
            static_cast<size_t>(
                std::numeric_limits<UINT>::max()
            )
    ) {
        std::cerr
            << "[Image Utils] "
            << "PNG save rejected"
            << " reason=frame_too_large"
            << "\n";

        return false;
    }

    HRESULT comResult =
        CoInitializeEx(
            nullptr,
            COINIT_MULTITHREADED
        );

    const bool mustUninitialize =
        SUCCEEDED(comResult);

    if (
        FAILED(comResult) &&
        comResult != RPC_E_CHANGED_MODE
    ) {
        logHresult(
            "CoInitializeEx",
            comResult
        );

        return false;
    }

    ComPtr<IWICImagingFactory> factory;

    HRESULT result =
        CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(
                factory.GetAddressOf()
            )
        );

    if (FAILED(result)) {
        logHresult(
            "CoCreateInstance WIC factory",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    ComPtr<IWICStream> stream;

    result =
        factory->CreateStream(
            stream.GetAddressOf()
        );

    if (FAILED(result)) {
        logHresult(
            "CreateStream",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    result =
        stream->InitializeFromFilename(
            outputPath.c_str(),
            GENERIC_WRITE
        );

    if (FAILED(result)) {
        logHresult(
            "InitializeFromFilename",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    ComPtr<IWICBitmapEncoder> encoder;

    result =
        factory->CreateEncoder(
            GUID_ContainerFormatPng,
            nullptr,
            encoder.GetAddressOf()
        );

    if (FAILED(result)) {
        logHresult(
            "CreateEncoder",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    result =
        encoder->Initialize(
            stream.Get(),
            WICBitmapEncoderNoCache
        );

    if (FAILED(result)) {
        logHresult(
            "Encoder Initialize",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    ComPtr<IWICBitmapFrameEncode> frame;

    ComPtr<IPropertyBag2> properties;

    result =
        encoder->CreateNewFrame(
            frame.GetAddressOf(),
            properties.GetAddressOf()
        );

    if (FAILED(result)) {
        logHresult(
            "CreateNewFrame",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    result =
        frame->Initialize(
            properties.Get()
        );

    if (FAILED(result)) {
        logHresult(
            "Frame Initialize",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    result =
        frame->SetSize(
            width,
            height
        );

    if (FAILED(result)) {
        logHresult(
            "SetSize",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    WICPixelFormatGUID pixelFormat =
        GUID_WICPixelFormat32bppBGRA;

    result =
        frame->SetPixelFormat(
            &pixelFormat
        );

    if (FAILED(result)) {
        logHresult(
            "SetPixelFormat",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    if (
        pixelFormat !=
        GUID_WICPixelFormat32bppBGRA
    ) {
        std::cerr
            << "[Image Utils] "
            << "PNG save failed"
            << " reason=unsupported_pixel_format"
            << "\n";

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    result =
        frame->WritePixels(
            height,
            static_cast<UINT>(rowBytes),
            static_cast<UINT>(expectedSize),
            const_cast<BYTE*>(
                reinterpret_cast<const BYTE*>(
                    pixels.data()
                )
            )
        );

    if (FAILED(result)) {
        logHresult(
            "WritePixels",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    result =
        frame->Commit();

    if (FAILED(result)) {
        logHresult(
            "Frame Commit",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    result =
        encoder->Commit();

    if (FAILED(result)) {
        logHresult(
            "Encoder Commit",
            result
        );

        if (mustUninitialize) {
            CoUninitialize();
        }

        return false;
    }

    if (mustUninitialize) {
        CoUninitialize();
    }

    std::wcerr
        << L"[Image Utils] "
        << L"PNG saved "
        << width
        << L"x"
        << height
        << L" path="
        << outputPath
        << L"\n";

    return true;
}
