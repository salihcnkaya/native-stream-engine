#pragma once

#include <cstdint>
#include <string>
#include <vector>


bool resizeBgraFrame(
    const std::vector<uint8_t>& sourcePixels,
    uint32_t sourceWidth,
    uint32_t sourceHeight,
    uint32_t maxWidth,
    uint32_t maxHeight,
    std::vector<uint8_t>& resizedPixels,
    uint32_t& resizedWidth,
    uint32_t& resizedHeight
);

bool encodeBgraFrameToPng(
    const std::vector<uint8_t>& pixels,
    uint32_t width,
    uint32_t height,
    std::vector<uint8_t>& pngBytes
);

bool saveBgraFrameAsPng(
    const std::vector<uint8_t>& pixels,
    uint32_t width,
    uint32_t height,
    const std::wstring& outputPath
);

