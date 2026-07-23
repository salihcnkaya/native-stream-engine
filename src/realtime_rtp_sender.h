#pragma once

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>
#include "rtp_pacer.h"
#include <chrono>

class RealtimeRtpSender {
public:
    RealtimeRtpSender();
    ~RealtimeRtpSender();

    void setLabel(const std::string& label);

    bool start(
        const std::string& ip,
        uint16_t port,
        uint8_t payloadType,
        uint32_t ssrc
    );

    void stop();

    bool isRunning() const;

    bool sendEncodedPayload(
        const uint8_t* data,
        size_t size,
        uint32_t timestamp,
        bool marker
    );

    bool sendH264Nal(
        const uint8_t* data,
        size_t size,
        uint32_t timestamp,
        bool marker
    );

    void updateNetworkFeedback(const NetworkFeedback& feedback);
    void setInitialBitrate(uint32_t bitrateBps);
    BitrateDecision bitrateDecision() const;
    bool retransmitPacket(uint16_t rtpSequenceNumber);

private:
    static constexpr size_t MAX_RTP_PACKET_SIZE = 1200;
    static constexpr size_t RTP_HEADER_SIZE = 12;
    static constexpr size_t RING_SIZE = 2048;
    static constexpr size_t RTP_EXTENSION_HEADER_SIZE = 4;
    static constexpr size_t TWCC_EXTENSION_BLOCK_SIZE = 4;
    static constexpr size_t RTP_TWCC_HEADER_SIZE =
        RTP_HEADER_SIZE + RTP_EXTENSION_HEADER_SIZE + TWCC_EXTENSION_BLOCK_SIZE;

    static constexpr uint8_t TWCC_EXTENSION_ID = 5;

    std::atomic<uint16_t> twccSequenceNumber_{ 1 };

    struct RtpPacket {
        std::array<uint8_t, MAX_RTP_PACKET_SIZE> data {};
        uint16_t size = 0;
        uint32_t timestamp = 0;
        bool marker = false;
		uint64_t sequence = 0;
        std::chrono::steady_clock::time_point enqueuedAt {};
    };

    static constexpr size_t HISTORY_SIZE = 4096;

    struct HistoryPacket {
        std::array<uint8_t, MAX_RTP_PACKET_SIZE> data {};
        uint16_t size = 0;
        uint16_t rtpSequenceNumber = 0;
        bool valid = false;
    };

    struct PacketBatch {
        static constexpr uint32_t MAX_PACKETS = 64;

        std::array<const RtpPacket*, MAX_PACKETS> packets {};
        uint32_t count = 0;

        bool empty() const
        {
            return count == 0;
        }

        bool full() const
        {
            return count >= MAX_PACKETS;
        }

        bool push(const RtpPacket* packet)
        {
            if (!packet || full()) {
                return false;
            }

            packets[count++] = packet;
            return true;
        }
    };

    std::string label_ = "rtp";

    std::atomic<bool> running_{ false };
    std::atomic<bool> senderThreadRunning_{ false };

    std::string ip_;
    uint16_t port_ = 0;
    uint8_t payloadType_ = 102;
    uint32_t ssrc_ = 0;

    SOCKET socket_ = INVALID_SOCKET;

    std::atomic<uint16_t> sequenceNumber_{ 1 };

    std::array<RtpPacket, RING_SIZE> ring_ {};

    std::thread senderThread_;
    HANDLE packetAvailableEvent_ = nullptr;

    std::atomic<uint64_t> packetsQueued_{ 0 };
    std::atomic<uint64_t> packetsSent_{ 0 };
    std::atomic<uint64_t> packetsDropped_{ 0 };
    std::atomic<uint64_t> bytesSent_{ 0 };
    std::atomic<uint32_t> maxQueueSeen_{ 0 };

	std::atomic<uint64_t> publishSequence_{ 0 };
	std::atomic<uint64_t> consumeSequence_{ 0 };
    std::atomic<uint64_t> sendFailures_{ 0 };
    std::atomic<uint64_t> partialSends_{ 0 };
    std::atomic<uint64_t> totalQueueLatencyMs_{ 0 };
    std::atomic<uint32_t> maxQueueLatencyMs_{ 0 };
    std::atomic<uint64_t> queueLatencySamples_{ 0 };

    std::array<HistoryPacket, HISTORY_SIZE> history_ {};
    std::atomic<uint64_t> historyPacketsStored_{ 0 };
    std::atomic<uint64_t> historyPacketsRetransmitted_{ 0 };
    std::atomic<uint64_t> historyPacketsMissed_{ 0 };

    static constexpr size_t RTX_SUPPRESSION_SIZE = 256;

    std::array<uint16_t, RTX_SUPPRESSION_SIZE> lastRetransmitSeqs_ {};
    std::atomic<uint32_t> lastRetransmitWriteIndex_{ 0 };

    std::atomic<uint32_t> retransmitsThisSecond_{ 0 };
    std::chrono::steady_clock::time_point retransmitWindowStartedAt_{};

    bool enqueueRtpPacket(
        const uint8_t* payload,
        size_t payloadSize,
        uint32_t timestamp,
        bool marker
    );

    bool writeRtpPacketToSlot(
        RtpPacket& slot,
        const uint8_t* payload,
        size_t payloadSize,
        uint32_t timestamp,
        bool marker
    );

    bool sendRawPacket(const RtpPacket& packet);
    void flushPacketBatch(const PacketBatch& batch);
    RtpPacer pacer_;
    void storeHistoryPacket(const RtpPacket& packet);
    bool sendRawPacketInternal(const RtpPacket& packet, bool storeHistory);
    void drainIncomingControlPackets();
    void handleIncomingControlPacket(const uint8_t* data, int size);
    void handleRtcpGenericNack(const uint8_t* data, int size);
    bool shouldSuppressRetransmit(uint16_t rtpSequenceNumber);
    void handleRtcpTransportWideFeedback(const uint8_t* data, int size);
    
    void senderLoop();
    void resetStats();
};
