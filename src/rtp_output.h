#pragma once

struct encoder_packet;

using NativeEncodedPacketHandler =
    void (*)(encoder_packet* packet);

void registerNativeRtpOutput();

void setNativeRtpOutputPacketHandler(
    NativeEncodedPacketHandler handler
);