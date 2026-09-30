#pragma once

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
        uint32_t packetBytes,
        std::chrono::steady_clock::time_point& nextSendTime,
        bool accountSustained = true
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
    void setAppliedBitrate(uint32_t bitrateBps);

    void resetTelemetry();

    RtpPacerTelemetry telemetry() const;

private:
    RtpPacerConfig config_{};

    mutable std::mutex stateMutex_;
    
    NetworkFeedback feedback_{};
    BitrateController bitrateController_;
    BitrateDecision lastBitrateDecision_{};

    std::atomic<uint32_t> targetBitrateBps_{ 0 };

    mutable bool bucketInitialized_{ false };

    mutable std::chrono::steady_clock::time_point bucketLastRefill_{};

    mutable double bucketCreditBytes_{ 0.0 };
    
    mutable double bucketMinCreditBytes_{ 0.0 };

    mutable double bucketMaxCreditBytes_{ 0.0 };

    mutable uint32_t bucketRateBps_{ 0 };

    mutable bool peakBucketInitialized_{ false };
    mutable std::chrono::steady_clock::time_point peakBucketLastRefill_{};
    mutable double peakBucketCreditBytes_{ 0.0 };
    mutable double peakBucketMinCreditBytes_{ 0.0 };

    mutable bool peakDebtActive_{ false };
    mutable std::chrono::steady_clock::time_point peakDebtStart_{};
    mutable double peakMaxDebtDurationMs_{ 0.0 };

    mutable bool peakVirtualInitialized_{ false };

    mutable std::chrono::steady_clock::time_point
        peakVirtualTat_{};

    mutable double peakVirtualMaxRequiredWaitMs_{ 0.0 };

    mutable uint64_t peakVirtualWaitEvents_{ 0 };
    
    std::atomic<bool> adaptiveVideoActive_{ false };
    std::atomic<uint32_t> adaptiveVideoExtraUs_{ 0 };
    mutable RtpPacerTelemetry telemetry_{};

    void refillBucketLocked(
        std::chrono::steady_clock::time_point now
    ) const;

    void refillPeakBucketLocked(
        std::chrono::steady_clock::time_point now
    ) const;

    void resetRateStateLocked(uint32_t bitrateBps);
};
