#include "rtp_output.h"

#include <obs.h>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <new>
#include <mutex>

namespace {

struct NativeRtpOutputContext {
    obs_output_t* output = nullptr;
    std::atomic<bool> active{ false };
    std::atomic<uint64_t> packetCount{ 0 };
    std::mutex callbackMutex;
};

std::atomic<NativeEncodedPacketHandler>
    g_packetHandler{ nullptr };

const char* getOutputName(void*)
{
    return "Native RTP Output";
}

void* createOutput(
    obs_data_t*,
    obs_output_t* output
)
{
    auto* context =
        new (std::nothrow)
            NativeRtpOutputContext();

    if (!context) {
        std::cerr
            << "[Native RTP Output] "
            << "context allocation failed\n";

        return nullptr;
    }

    context->output = output;

    std::cerr
        << "[Native RTP Output] created\n";

    return context;
}

void destroyOutput(void* data)
{
    auto* context =
        static_cast<NativeRtpOutputContext*>(
            data
        );

    if (!context) {
        return;
    }

    std::cerr
        << "[Native RTP Output] destroyed"
        << " packets="
        << context->packetCount.load(
            std::memory_order_relaxed
        )
        << "\n";

    delete context;
}

bool startOutput(void* data)
{
    auto* context =
        static_cast<NativeRtpOutputContext*>(
            data
        );

    if (!context || !context->output) {
        return false;
    }

    if (
        !g_packetHandler.load(
            std::memory_order_acquire
        )
    ) {
        std::cerr
            << "[Native RTP Output] "
            << "start rejected"
            << " reason=no_packet_handler\n";

        return false;
    }

    if (
        !obs_output_can_begin_data_capture(
            context->output,
            0
        )
    ) {
        std::cerr
            << "[Native RTP Output] "
            << "cannot begin data capture\n";

        return false;
    }

    if (
        !obs_output_initialize_encoders(
            context->output,
            0
        )
    ) {
        std::cerr
            << "[Native RTP Output] "
            << "encoder initialization failed\n";

        return false;
    }

    context->packetCount.store(
        0,
        std::memory_order_relaxed
    );

    if (
        !obs_output_begin_data_capture(
            context->output,
            0
        )
    ) {
        std::cerr
            << "[Native RTP Output] "
            << "begin data capture failed\n";

        return false;
    }

    context->active.store(
        true,
        std::memory_order_release
    );

    std::cerr
        << "[Native RTP Output] started\n";

    return true;
}

void stopOutput(
    void* data,
    uint64_t
)
{
    auto* context =
        static_cast<NativeRtpOutputContext*>(
            data
        );

    if (!context || !context->output) {
        return;
    }

    bool wasActive = false;

    {
        std::lock_guard<std::mutex> lock(
            context->callbackMutex
        );

        wasActive =
            context->active.exchange(
                false,
                std::memory_order_acq_rel
            );
    }

    if (!wasActive) {
        return;
    }

    obs_output_end_data_capture(
        context->output
    );

    std::cerr
        << "[Native RTP Output] stopped"
        << " packets="
        << context->packetCount.load(
                std::memory_order_relaxed
            )
        << "\n";
}

void receiveEncodedPacket(
    void* data,
    encoder_packet* packet
)
{
    auto* context =
        static_cast<NativeRtpOutputContext*>(
            data
        );

    if (
        !context ||
        !packet
    ) {
        return;
    }

    std::lock_guard<std::mutex> lock(
        context->callbackMutex
    );

    if (
        !context->active.load(
            std::memory_order_acquire
        )
    ) {
        return;
    }

    const auto handler =
        g_packetHandler.load(
            std::memory_order_acquire
        );

    if (!handler) {
        return;
    }

    context->packetCount.fetch_add(
        1,
        std::memory_order_relaxed
    );
    handler(packet);
}

} // namespace

void setNativeRtpOutputPacketHandler(
    NativeEncodedPacketHandler handler
)
{
    g_packetHandler.store(
        handler,
        std::memory_order_release
    );
}

void registerNativeRtpOutput()
{
    obs_output_info videoInfo{};

    videoInfo.id =
        "native_rtp_video_output";

    videoInfo.flags =
        OBS_OUTPUT_VIDEO |
        OBS_OUTPUT_ENCODED;

    videoInfo.get_name = getOutputName;
    videoInfo.create = createOutput;
    videoInfo.destroy = destroyOutput;
    videoInfo.start = startOutput;
    videoInfo.stop = stopOutput;
    videoInfo.encoded_packet =
        receiveEncodedPacket;

    obs_register_output(
        &videoInfo
    );

    obs_output_info avInfo{};

    avInfo.id =
        "native_rtp_av_output";

    avInfo.flags =
        OBS_OUTPUT_AV |
        OBS_OUTPUT_ENCODED;

    avInfo.get_name = getOutputName;
    avInfo.create = createOutput;
    avInfo.destroy = destroyOutput;
    avInfo.start = startOutput;
    avInfo.stop = stopOutput;
    avInfo.encoded_packet =
        receiveEncodedPacket;

    obs_register_output(
        &avInfo
    );

    std::cerr
        << "[Native RTP Output] registered"
        << " video=native_rtp_video_output"
        << " av=native_rtp_av_output"
        << "\n";
}