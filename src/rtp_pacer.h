#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <atomic>
#include <string>
#include <mutex>
#include "network_feedback.h"
#include "bitrate_controller.h"


struct RtpPacerConfig {
    std::chrono::microseconds videoNormal { 70 };
    std::chrono::microseconds videoQ60 { 55 };
    std::chrono::microseconds videoQ120 { 45 };
    std::chrono::microseconds videoQ300 { 35 };

    std::chrono::microseconds audioNormal { 20 };
    std::chrono::microseconds audioQ60 { 15 };
    std::chrono::microseconds audioQ120 { 10 };
    std::chrono::microseconds audioQ300 { 5 };
};

struct RtpPacerTelemetry {
    uint64_t waitCalls = 0;
    uint64_t yieldIterations = 0;
    uint64_t spinIterations = 0;
    uint64_t lateResets = 0;
    uint64_t totalWaitUs = 0;
    uint64_t maxWaitUs = 0;
};

class RtpPacer {
public:
    RtpPacer() = default;

    std::chrono::microseconds calculateSpacing(
        bool isVideo,
        uint32_t queueDepth
    ) const;

    void wait(
        const std::atomic<bool>& shouldRun,
        std::chrono::steady_clock::time_point& nextSendTime,
        std::chrono::microseconds spacing
    ) const;

    void pace(
        const std::atomic<bool>& shouldRun,
        bool isVideo,
        uint32_t queueDepth,
        std::chrono::steady_clock::time_point& nextSendTime
    ) const;

    void logBaseline(
        const std::string& label,
        uint32_t maxBatch
    ) const;

    void updateNetworkFeedback(const NetworkFeedback& feedback);
    NetworkFeedback networkFeedback() const;
    bool isAdaptiveActive() const;
    uint32_t adaptiveExtraUs(bool isVideo) const;
    bool shouldUseAdaptiveVideo() const;
    BitrateDecision bitrateDecision() const;
    void setInitialBitrate(uint32_t bitrateBps);

    void resetTelemetry();

    RtpPacerTelemetry telemetry() const;

private:
    RtpPacerConfig config_{};

    mutable std::mutex stateMutex_;
    
    NetworkFeedback feedback_{};
    BitrateController bitrateController_;
    BitrateDecision lastBitrateDecision_{};
    
    std::atomic<bool> adaptiveVideoActive_{ false };
    std::atomic<uint32_t> adaptiveVideoExtraUs_{ 0 };
    mutable RtpPacerTelemetry telemetry_{};
};
