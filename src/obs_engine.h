#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "native_wgc_source.h"
#include "bitrate_update_scheduler.h"
#include "network_feedback.h"

struct ObsVideoConfig {
    int baseWidth = 1920;
    int baseHeight = 1080;
    int outputWidth = 1920;
    int outputHeight = 1080;
    int fps = 60;
    std::string scaleFilter = "bicubic";
};

enum class CaptureType {
    Monitor,
    Window,
    Game,
    Wgc
};

class ObsEngine {
public:
    bool initialize(
        const std::filesystem::path& runtimeDir,
        const ObsVideoConfig& videoConfig
    );

    bool configureVideo(
        const ObsVideoConfig& videoConfig
    );

    bool createDesktopAudioSource();
    bool createCaptureScene(
        CaptureType captureType,
        uintptr_t hwnd,
        int delayMs,
        bool debugFrames,
        int monitorIndex = 0
    );
    bool createWgcScene(
        WgcTargetType targetType,
        uintptr_t hwnd,
        int monitorIndex,
        int delayMs,
        bool debugFrames
    );
    bool createProcessAudioSource(uintptr_t hwnd);

    bool startRtpStreaming(
        const std::string& rtpIp,
        uint16_t rtpPort,
        uint8_t payloadType,
        uint32_t ssrc,
        int bitrate,
        const std::string& encoder,
        const std::string& audioRtpIp = "",
        uint16_t audioRtpPort = 0,
        uint8_t audioPayloadType = 111,
        uint32_t audioSsrc = 0
    );

    bool waitForCaptureFrame(
        int timeoutMs,
        uint32_t& width,
        uint32_t& height
    ) const;

    void stopRtpStreaming();

    void clearCapture();
    void shutdown();
    
    bool setTargetBitrate(uint32_t targetBitrateBps);
    void updateNetworkFeedback(const NetworkFeedback& feedback);

    bool copyCaptureFrameBgra(
        std::vector<uint8_t>& pixels,
        uint32_t& width,
        uint32_t& height
    ) const;

private:
    std::string toUtf8Path(const std::filesystem::path& path);
    bool shutdownCalled_ = false;
    bool captureCleared_ = false;

    BitrateUpdateScheduler bitrateUpdateScheduler_;
};
