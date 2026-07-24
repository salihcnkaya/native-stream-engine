#include "rtp_output.h"

#include <obs.h>

#include <atomic>
#include <cstdint>
#include <iostream>
#include <new>

namespace {

struct NativeRtpOutputContext {
    obs_output_t* output = nullptr;
    std::atomic<bool> active{ false };
    uint64_t packetCount = 0;
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
        << context->packetCount
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

    if (!g_packetHandler.load(
        std::memory_order_acquire
    )) {
        std::cerr
            << "[Native RTP Output] "
            << "start rejected"
            << " reason=no_packet_handler\n";

        return false;
    }

    context->packetCount = 0;
    context->active.store(
        true,
        std::memory_order_release
    );

    if (!obs_output_begin_data_capture(
        context->output,
        0
    )) {
        context->active.store(
            false,
            std::memory_order_release
        );

        std::cerr
            << "[Native RTP Output] "
            << "begin data capture failed\n";

        return false;
    }

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

    const bool wasActive =
        context->active.exchange(
            false,
            std::memory_order_acq_rel
        );

    if (!wasActive) {
        return;
    }

    obs_output_end_data_capture(
        context->output
    );

    std::cerr
        << "[Native RTP Output] stopped"
        << " packets="
        << context->packetCount
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
        !packet ||
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

    context->packetCount++;
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
    obs_output_info info{};

    info.id = "native_rtp_output";
    info.flags =
        OBS_OUTPUT_AV |
        OBS_OUTPUT_ENCODED;

    info.get_name = getOutputName;
    info.create = createOutput;
    info.destroy = destroyOutput;
    info.start = startOutput;
    info.stop = stopOutput;
    info.encoded_packet =
        receiveEncodedPacket;

    obs_register_output(&info);

    std::cerr
        << "[Native RTP Output] registered\n";
}