#include "realtime_rtp_sender.h"

#include <algorithm>
#include <chrono>
#include <iostream>

#if defined(_WIN32)

#pragma comment(lib, "Ws2_32.lib")

#else

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#endif

namespace {

bool initializeSocketPlatform()
{
#if defined(_WIN32)
    WSADATA wsaData {};
    return WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
#else
    return true;
#endif
}

void cleanupSocketPlatform()
{
#if defined(_WIN32)
    WSACleanup();
#endif
}

void closeSocketPlatform(SOCKET socket)
{
    if (socket == INVALID_SOCKET) {
        return;
    }

#if defined(_WIN32)
    closesocket(socket);
#else
    ::close(socket);
#endif
}

int lastSocketError()
{
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool socketWouldBlock(int error)
{
#if defined(_WIN32)
    return error == WSAEWOULDBLOCK;
#else
    return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

bool setSocketNonBlocking(SOCKET socket)
{
#if defined(_WIN32)

    u_long nonBlocking = 1;

    return ioctlsocket(
        socket,
        FIONBIO,
        &nonBlocking
    ) == 0;

#else

    const int flags = fcntl(socket, F_GETFL, 0);

    if (flags < 0) {
        return false;
    }

    return fcntl(
        socket,
        F_SETFL,
        flags | O_NONBLOCK
    ) == 0;

#endif
}

} // namespace

static constexpr bool STREAM_DEBUG_RTP_STATS = false;
static constexpr bool STREAM_DEBUG_RTCP_NACK = false;
static constexpr bool STREAM_DEBUG_RTCP_TWCC = false;

RealtimeRtpSender::RealtimeRtpSender() = default;

RealtimeRtpSender::~RealtimeRtpSender()
{
    stop();
}

void RealtimeRtpSender::setLabel(const std::string& label)
{
    label_ = label.empty() ? "rtp" : label;
}

void RealtimeRtpSender::setInitialBitrate(uint32_t bitrateBps)
{
    pacer_.setInitialBitrate(bitrateBps);
}

void RealtimeRtpSender::setAppliedBitrate(uint32_t bitrateBps)
{
    pacer_.setAppliedBitrate(bitrateBps);
}

BitrateDecision RealtimeRtpSender::bitrateDecision() const
{
    return pacer_.bitrateDecision();
}

void RealtimeRtpSender::resetStats()
{
    packetsQueued_.store(
        0,
        std::memory_order_relaxed
    );

    packetsSent_.store(
        0,
        std::memory_order_relaxed
    );

    packetsDropped_.store(
        0,
        std::memory_order_relaxed
    );

    bytesSent_.store(
        0,
        std::memory_order_relaxed
    );

    wirePacketsSent_.store(
        0,
        std::memory_order_relaxed
    );

    wireBytesSent_.store(
        0,
        std::memory_order_relaxed
    );

    maxQueueSeen_.store(
        0,
        std::memory_order_relaxed
    );

    sendFailures_.store(
        0,
        std::memory_order_relaxed
    );

    partialSends_.store(
        0,
        std::memory_order_relaxed
    );

    totalQueueLatencyMs_.store(
        0,
        std::memory_order_relaxed
    );

    maxQueueLatencyMs_.store(
        0,
        std::memory_order_relaxed
    );

    queueLatencySamples_.store(
        0,
        std::memory_order_relaxed
    );

    historyPacketsStored_.store(
        0,
        std::memory_order_relaxed
    );

    historyPacketsRetransmitted_.store(
        0,
        std::memory_order_relaxed
    );

    historyPacketsMissed_.store(
        0,
        std::memory_order_relaxed
    );

    lastRetransmitWriteIndex_.store(
        0,
        std::memory_order_relaxed
    );

    retransmitsThisSecond_.store(
        0,
        std::memory_order_relaxed
    );

    retransmitWindowStartedAt_ =
        std::chrono::steady_clock::time_point{};

    retransmitPublishSequence_.store(
        0,
        std::memory_order_relaxed
    );

    retransmitConsumeSequence_.store(
        0,
        std::memory_order_relaxed
    );

    retransmitRequests_.store(
        0,
        std::memory_order_relaxed
    );

    retransmitQueued_.store(
        0,
        std::memory_order_relaxed
    );

    retransmitQueueDropped_.store(
        0,
        std::memory_order_relaxed
    );

    retransmitPacketsSent_.store(
        0,
        std::memory_order_relaxed
    );

    retransmitSendFailures_.store(
        0,
        std::memory_order_relaxed
    );

    maxRetransmitQueueSeen_.store(
        0,
        std::memory_order_relaxed
    );

    maxRetransmitQueueLatencyMs_.store(
        0,
        std::memory_order_relaxed
    );

    retransmitRateLimited_.store(
        0,
        std::memory_order_relaxed
    );

    controlPacketsReceived_.store(
        0,
        std::memory_order_relaxed
    );

    controlReceiveErrors_.store(
        0,
        std::memory_order_relaxed
    );

    fragmentedNalAdmissionDrops_.store(
        0,
        std::memory_order_relaxed
    );

    for (auto& pending : pendingRetransmits_) {
        pending.rtpSequenceNumber = 0;
        pending.sequence = 0;
        pending.enqueuedAt = std::chrono::steady_clock::time_point{};
    }

    senderLoopIterations_.store(
        0,
        std::memory_order_relaxed
    );

    emptyQueueWaits_.store(
        0,
        std::memory_order_relaxed
    );

    emptyQueueSignals_.store(
        0,
        std::memory_order_relaxed
    );

    emptyQueueTimeouts_.store(
        0,
        std::memory_order_relaxed
    );

    emptyQueueWaitErrors_.store(
        0,
        std::memory_order_relaxed
    );

    pacingCalls_.store(
        0,
        std::memory_order_relaxed
    );

    packetsProcessedByLoop_.store(
        0,
        std::memory_order_relaxed
    );

    pacer_.resetTelemetry();

    for (size_t i = 0; i < lastRetransmitSeqs_.size(); ++i) {
        lastRetransmitSeqs_[i] = 0xffff;
        lastRetransmitTimes_[i] =
            std::chrono::steady_clock::time_point{};
    }

    for (auto& packet : history_) {
        packet.valid = false;
        packet.size = 0;
        packet.rtpSequenceNumber = 0;
    }
}

bool RealtimeRtpSender::start(
    const std::string& ip,
    uint16_t port,
    uint8_t payloadType,
    uint32_t ssrc,
    uint32_t rtxSsrc,
    uint8_t rtxPayloadType
)
{
    if (running_) {
        stop();
    }

    ip_ = ip;
    port_ = port;
    payloadType_ = payloadType;
    ssrc_ = ssrc;

    rtxEnabled_ = rtxSsrc != 0 && rtxPayloadType != 0;
    rtxSsrc_ = rtxSsrc;
    rtxPayloadType_ = rtxPayloadType;
    rtxSequenceNumber_ = 1;

    std::cerr << "[Realtime RTP Sender:" << label_
              << "] rtx " << (rtxEnabled_ ? "enabled" : "disabled")
              << " rtxSsrc=" << rtxSsrc_
              << " rtxPayloadType=" << static_cast<int>(rtxPayloadType_)
              << "\n";

    resetStats();

    publishSequence_ = 0;
    consumeSequence_ = 0;
    sequenceNumber_ = 1;
    twccSequenceNumber_ = 1;

    if (!initializeSocketPlatform()) {
        std::cerr
            << "[Realtime RTP Sender:"
            << label_
            << "] socket platform initialization failed\n";

        return false;
    }

    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (socket_ == INVALID_SOCKET) {
        std::cerr << "[Realtime RTP Sender:" << label_
                  << "] socket failed\n";
        cleanupSocketPlatform();
        return false;
    }

    int sendBufferSize = 4 * 1024 * 1024;
    setsockopt(
        socket_,
        SOL_SOCKET,
        SO_SNDBUF,
        #if defined(_WIN32)
            reinterpret_cast<const char*>(&sendBufferSize),
        #else
            &sendBufferSize,
        #endif
        sizeof(sendBufferSize)
    );

    sockaddr_in remoteAddr {};
    remoteAddr.sin_family = AF_INET;
    remoteAddr.sin_port = htons(port_);

    if (inet_pton(AF_INET, ip_.c_str(), &remoteAddr.sin_addr) != 1) {
        std::cerr << "[Realtime RTP Sender:" << label_
                  << "] invalid ip: " << ip_ << "\n";

        closeSocketPlatform(socket_);
        socket_ = INVALID_SOCKET;
        cleanupSocketPlatform();

        return false;
    }

    if (connect(
            socket_,
            reinterpret_cast<const sockaddr*>(&remoteAddr),
            sizeof(remoteAddr)
        ) == SOCKET_ERROR) {
        std::cerr << "[Realtime RTP Sender:" << label_
                << "] UDP connect failed: "
                << lastSocketError()
                << "\n";

        closeSocketPlatform(socket_);
        socket_ = INVALID_SOCKET;
        cleanupSocketPlatform();

        return false;
    }

    if (!setSocketNonBlocking(socket_)) {
        std::cerr
            << "[Realtime RTP Sender:"
            << label_
            << "] failed to enable non-blocking socket, error="
            << lastSocketError()
            << "\n";

        closeSocketPlatform(socket_);
        socket_ = INVALID_SOCKET;
        cleanupSocketPlatform();

        return false;
    }

    #if defined(_WIN32)

    packetAvailableEvent_ = CreateEventW(
        nullptr,
        FALSE,
        FALSE,
        nullptr
    );

    if (!packetAvailableEvent_) {
        std::cerr
            << "[Realtime RTP Sender:"
            << label_
            << "] CreateEventW failed, error="
            << GetLastError()
            << "\n";

        closeSocketPlatform(socket_);
        socket_ = INVALID_SOCKET;
        cleanupSocketPlatform();

        return false;
    }

    if (label_ == "video") {
        socketReadEvent_ = WSACreateEvent();

        if (socketReadEvent_ == WSA_INVALID_EVENT) {
            std::cerr
                << "[Realtime RTP Sender:"
                << label_
                << "] WSACreateEvent failed, error="
                << lastSocketError()
                << "\n";

            CloseHandle(packetAvailableEvent_);
            packetAvailableEvent_ = nullptr;

            closeSocketPlatform(socket_);
            socket_ = INVALID_SOCKET;

            cleanupSocketPlatform();
            return false;
        }

        if (
            WSAEventSelect(
                socket_,
                socketReadEvent_,
                FD_READ
            ) == SOCKET_ERROR
        ) {
            std::cerr
                << "[Realtime RTP Sender:"
                << label_
                << "] WSAEventSelect failed, error="
                << lastSocketError()
                << "\n";

            WSACloseEvent(socketReadEvent_);
            socketReadEvent_ = WSA_INVALID_EVENT;

            CloseHandle(packetAvailableEvent_);
            packetAvailableEvent_ = nullptr;

            closeSocketPlatform(socket_);
            socket_ = INVALID_SOCKET;

            cleanupSocketPlatform();
            return false;
        }
    }

    #else

        packetAvailableEventFd_ = eventfd(
        0,
        EFD_NONBLOCK | EFD_CLOEXEC
    );

    if (packetAvailableEventFd_ < 0) {
        std::cerr
            << "[Realtime RTP Sender:"
            << label_
            << "] eventfd creation failed, error="
            << errno
            << "\n";

        closeSocketPlatform(socket_);
        socket_ = INVALID_SOCKET;
        cleanupSocketPlatform();

        return false;
    }

    #endif
    
    running_ = true;
    senderThreadRunning_ = true;
    senderThread_ = std::thread(&RealtimeRtpSender::senderLoop, this);

    std::cerr << "[Realtime RTP Sender:" << label_
              << "] started "
              << ip_ << ":" << port_
              << " pt=" << static_cast<int>(payloadType_)
              << " ssrc=" << ssrc_
              << "\n";

    return true;
}

void RealtimeRtpSender::stop()
{
    if (!running_) return;

    std::cerr << "[Realtime RTP Sender:" << label_
              << "] stopping\n";

    const auto stopDrainStartedAt =
        std::chrono::steady_clock::now();

    const uint64_t stopPendingMedia =
        publishSequence_.load(std::memory_order_acquire) -
        consumeSequence_.load(std::memory_order_acquire);

    const uint64_t stopPendingRtx =
        retransmitPublishSequence_.load(std::memory_order_acquire) -
        retransmitConsumeSequence_.load(std::memory_order_acquire);

    senderThreadRunning_ = false;

    #if defined(_WIN32)

    if (packetAvailableEvent_) {
        SetEvent(packetAvailableEvent_);
    }

    #else

    if (packetAvailableEventFd_ >= 0) {
        const uint64_t signal = 1;

        const ssize_t written = ::write(
            packetAvailableEventFd_,
            &signal,
            sizeof(signal)
        );

        (void)written;
    }

    #endif

    if (senderThread_.joinable()) {
        senderThread_.join();
    }

    const uint64_t stopDrainMs =
        static_cast<uint64_t>(
            std::chrono::duration_cast<
                std::chrono::milliseconds
            >(
                std::chrono::steady_clock::now() -
                stopDrainStartedAt
            ).count()
        );

    #if defined(_WIN32)

    if (socketReadEvent_ != WSA_INVALID_EVENT) {
        WSACloseEvent(socketReadEvent_);
        socketReadEvent_ = WSA_INVALID_EVENT;
    }

    if (packetAvailableEvent_) {
        CloseHandle(packetAvailableEvent_);
        packetAvailableEvent_ = nullptr;
    }

    #else

    if (packetAvailableEventFd_ >= 0) {
        ::close(packetAvailableEventFd_);
        packetAvailableEventFd_ = -1;
    }

    #endif

    if (socket_ != INVALID_SOCKET) {
        closeSocketPlatform(socket_);
        socket_ = INVALID_SOCKET;
    }

    cleanupSocketPlatform();

    running_ = false;

    const uint64_t latencySamples =
        queueLatencySamples_.load(std::memory_order_relaxed);

    const uint64_t averageQueueLatencyMs =
        latencySamples > 0
            ? totalQueueLatencyMs_.load(std::memory_order_relaxed) /
                latencySamples
            : 0;
    
    const auto pacerTelemetry =
        pacer_.telemetry();

    const uint64_t averagePacingWaitUs =
        pacerTelemetry.waitCalls > 0
            ? pacerTelemetry.totalWaitUs /
                pacerTelemetry.waitCalls
            : 0;

    const uint64_t averageYieldsPerWait =
        pacerTelemetry.waitCalls > 0
            ? pacerTelemetry.yieldIterations /
                pacerTelemetry.waitCalls
            : 0;

    std::cerr
        << "[Realtime RTP Sender:"
        << label_
        << "] stopped"
        << " queued="
        << packetsQueued_.load(std::memory_order_relaxed)
        << " sent="
        << packetsSent_.load(std::memory_order_relaxed)
        << " dropped="
        << packetsDropped_.load(std::memory_order_relaxed)
        << " nalAdmissionDrop="
        << fragmentedNalAdmissionDrops_.load(
            std::memory_order_relaxed
        )
        << " bytes="
        << bytesSent_.load(std::memory_order_relaxed)
        << " wireSent="
        << wirePacketsSent_.load(std::memory_order_relaxed)
        << " wireBytes="
        << wireBytesSent_.load(std::memory_order_relaxed)
        << " maxQueue="
        << maxQueueSeen_.load(std::memory_order_relaxed)
        << " avgQueueLatencyMs="
        << averageQueueLatencyMs
        << " maxQueueLatencyMs="
        << maxQueueLatencyMs_.load(std::memory_order_relaxed)
        << " sendFailures="
        << sendFailures_.load(std::memory_order_relaxed)
        << " partialSends="
        << partialSends_.load(
            std::memory_order_relaxed
        )
        << " loopIterations="
        << senderLoopIterations_.load(
            std::memory_order_relaxed
        )
        << " packetsProcessed="
        << packetsProcessedByLoop_.load(
            std::memory_order_relaxed
        )
        << " pacingCalls="
        << pacingCalls_.load(
            std::memory_order_relaxed
        )
        << " emptyWaits="
        << emptyQueueWaits_.load(
            std::memory_order_relaxed
        )
        << " emptySignals="
        << emptyQueueSignals_.load(
            std::memory_order_relaxed
        )
        << " emptyTimeouts="
        << emptyQueueTimeouts_.load(
            std::memory_order_relaxed
        )
        << " emptyWaitErrors="
        << emptyQueueWaitErrors_.load(
            std::memory_order_relaxed
        )
        << " pacerWaitCalls="
        << pacerTelemetry.waitCalls
        << " pacerYieldIterations="
        << pacerTelemetry.yieldIterations
        << " pacerSpinIterations="
        << pacerTelemetry.spinIterations
        << " avgPacerWaitUs="
        << averagePacingWaitUs
        << " maxPacerWaitUs="
        << pacerTelemetry.maxWaitUs
        << " avgYieldsPerWait="
        << averageYieldsPerWait
        << " pacerLateResets="
        << pacerTelemetry.lateResets
        << " stopPendingMedia="
        << stopPendingMedia
        << " stopPendingRtx="
        << stopPendingRtx
        << " stopDrainMs="
        << stopDrainMs
        << " ctrlRx="
        << controlPacketsReceived_.load(
            std::memory_order_relaxed
        )
        << " ctrlRxErr="
        << controlReceiveErrors_.load(
            std::memory_order_relaxed
        )
        << " rtxReq="
        << retransmitRequests_.load(std::memory_order_relaxed)
        << " rtxQueued="
        << retransmitQueued_.load(std::memory_order_relaxed)
        << " rtxQueueDrop="
        << retransmitQueueDropped_.load(std::memory_order_relaxed)
        << " rtxRateLimited="
        << retransmitRateLimited_.load(std::memory_order_relaxed)
        << " rtxSent="
        << retransmitPacketsSent_.load(std::memory_order_relaxed)
        << " rtxSendFail="
        << retransmitSendFailures_.load(std::memory_order_relaxed)
        << " rtxMaxQueue="
        << maxRetransmitQueueSeen_.load(std::memory_order_relaxed)
        << " rtxMaxQueueLatencyMs="
        << maxRetransmitQueueLatencyMs_.load(std::memory_order_relaxed)
        << "\n";
}

bool RealtimeRtpSender::isRunning() const
{
    return running_;
}

void RealtimeRtpSender::updateNetworkFeedback(
    const NetworkFeedback& feedback
)
{
    pacer_.updateNetworkFeedback(feedback);
}

void RealtimeRtpSender::drainIncomingControlPackets()
{
    if (!running_ || socket_ == INVALID_SOCKET || label_ != "video") {
        return;
    }

    for (int i = 0; i < 8; i++) {
        uint8_t buffer[1500] {};
        sockaddr_in from {};
        
        #if defined(_WIN32)
        int fromLen = sizeof(from);
        #else
        socklen_t fromLen = sizeof(from);
        #endif

        const int received = recvfrom(
            socket_,
            reinterpret_cast<char*>(buffer),
            sizeof(buffer),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &fromLen
        );

        if (received <= 0) {
            const int error = lastSocketError();

            if (socketWouldBlock(error)) {
                return;
            }

            controlReceiveErrors_.fetch_add(
                1,
                std::memory_order_relaxed
            );

            return;
        }

        controlPacketsReceived_.fetch_add(
            1,
            std::memory_order_relaxed
        );

        handleIncomingControlPacket(
            buffer,
            received
        );
    }
}

void RealtimeRtpSender::handleIncomingControlPacket(
    const uint8_t* data,
    int size
)
{
    if (!data || size < 4) {
        return;
    }

    int offset = 0;

    while (offset + 4 <= size) {
        const uint8_t* packet = data + offset;
        const int remaining = size - offset;

        const uint8_t version = packet[0] >> 6;
        const uint8_t fmt = packet[0] & 0x1f;
        const uint8_t packetType = packet[1];

        if (version != 2) {
            return;
        }

        const uint16_t rtcpLengthWords =
            static_cast<uint16_t>((packet[2] << 8) | packet[3]);

        const int packetSize =
            static_cast<int>((rtcpLengthWords + 1) * 4);

        if (packetSize < 4 || packetSize > remaining) {
            return;
        }

        // RTCP Transport Feedback, Generic NACK
        if (packetType == 205 && fmt == 1) {
            handleRtcpGenericNack(packet, packetSize);
        }

        // RTCP Transport Feedback, Transport-Wide CC
        if (packetType == 205 && fmt == 15) {
            handleRtcpTransportWideFeedback(packet, packetSize);
        }

        offset += packetSize;
    }
}

void RealtimeRtpSender::handleRtcpGenericNack(
    const uint8_t* data,
    int size
)
{
    if (!data || size < 16) {
        return;
    }

    const uint16_t rtcpLengthWords =
        static_cast<uint16_t>((data[2] << 8) | data[3]);

    const int packetSize = static_cast<int>((rtcpLengthWords + 1) * 4);

    if (packetSize > size || packetSize < 4) {
        return;
    }

    int effectivePacketSize = packetSize;

    if ((data[0] & 0x20) != 0) {
        const uint8_t paddingBytes = data[packetSize - 1];

        if (paddingBytes == 0 ||
            paddingBytes > packetSize - 4) {
            return;
        }

        effectivePacketSize -= paddingBytes;
    }

    if (effectivePacketSize < 16) {
        return;
    }

    const uint32_t mediaSsrc =
        (static_cast<uint32_t>(data[8]) << 24) |
        (static_cast<uint32_t>(data[9]) << 16) |
        (static_cast<uint32_t>(data[10]) << 8) |
        static_cast<uint32_t>(data[11]);

    if (mediaSsrc != ssrc_) {
        return;
    }

    int offset = 12;

    while (offset + 4 <= effectivePacketSize) {
        const uint16_t pid =
            static_cast<uint16_t>((data[offset] << 8) | data[offset + 1]);

        const uint16_t blp =
            static_cast<uint16_t>((data[offset + 2] << 8) | data[offset + 3]);

        if (STREAM_DEBUG_RTCP_NACK) {
            std::cerr << "[RTCP] Generic NACK"
                    << " seq=" << pid
                    << "\n";
        }

        retransmitPacket(pid);        

        for (int bit = 0; bit < 16; bit++) {
            if ((blp & (1 << bit)) == 0) {
                continue;
            }

            const uint16_t seq = static_cast<uint16_t>(pid + bit + 1);

            if (STREAM_DEBUG_RTCP_NACK) {
                std::cerr << "[RTCP] Generic NACK"
                        << " seq=" << seq
                        << "\n";
            }

            retransmitPacket(seq);
        }

        offset += 4;
    }
}

void RealtimeRtpSender::handleRtcpTransportWideFeedback(
    const uint8_t* data,
    int size
)
{
    if (!data || size < 20) {
        return;
    }

    const uint16_t rtcpLengthWords =
        static_cast<uint16_t>((data[2] << 8) | data[3]);

    const int packetSize = static_cast<int>((rtcpLengthWords + 1) * 4);

    if (packetSize > size || packetSize < 4) {
        return;
    }

    int effectivePacketSize = packetSize;

    if ((data[0] & 0x20) != 0) {
        const uint8_t paddingBytes = data[packetSize - 1];

        if (paddingBytes == 0 ||
            paddingBytes > packetSize - 4) {
            return;
        }

        effectivePacketSize -= paddingBytes;
    }

    if (effectivePacketSize < 20) {
        return;
    }

    const uint32_t mediaSsrc =
        (static_cast<uint32_t>(data[8]) << 24) |
        (static_cast<uint32_t>(data[9]) << 16) |
        (static_cast<uint32_t>(data[10]) << 8) |
        static_cast<uint32_t>(data[11]);

    if (mediaSsrc != ssrc_) {
        return;
    }

    const uint16_t baseSequence =
        static_cast<uint16_t>((data[12] << 8) | data[13]);

    const uint16_t packetStatusCount =
        static_cast<uint16_t>((data[14] << 8) | data[15]);

    if (STREAM_DEBUG_RTCP_TWCC) {
        std::cerr << "[RTCP] TWCC feedback"
                << " baseSeq=" << baseSequence
                << " statusCount=" << packetStatusCount
                << " size=" << packetSize
                << "\n";
    }
}

bool RealtimeRtpSender::writeRtpPacketToSlot(
    RtpPacket& slot,
    const uint8_t* payload,
    size_t payloadSize,
    uint32_t timestamp,
    bool marker
)
{
    if (!payload || payloadSize == 0) return false;

    const bool useTwcc = label_ == "video";
    const size_t rtpHeaderSize = useTwcc ? RTP_TWCC_HEADER_SIZE : RTP_HEADER_SIZE;

    if (payloadSize + rtpHeaderSize > MAX_RTP_PACKET_SIZE) {
        return false;
    }

    const uint16_t sequenceNumber = sequenceNumber_.fetch_add(1);

    slot.size = static_cast<uint16_t>(rtpHeaderSize + payloadSize);
    slot.timestamp = timestamp;
    slot.marker = marker;
    slot.enqueuedAt = std::chrono::steady_clock::now();

    slot.data[0] = useTwcc ? 0x90 : 0x80;
    slot.data[1] = static_cast<uint8_t>((marker ? 0x80 : 0x00) | payloadType_);

    slot.data[2] = static_cast<uint8_t>((sequenceNumber >> 8) & 0xff);
    slot.data[3] = static_cast<uint8_t>(sequenceNumber & 0xff);

    slot.data[4] = static_cast<uint8_t>((timestamp >> 24) & 0xff);
    slot.data[5] = static_cast<uint8_t>((timestamp >> 16) & 0xff);
    slot.data[6] = static_cast<uint8_t>((timestamp >> 8) & 0xff);
    slot.data[7] = static_cast<uint8_t>(timestamp & 0xff);

    slot.data[8] = static_cast<uint8_t>((ssrc_ >> 24) & 0xff);
    slot.data[9] = static_cast<uint8_t>((ssrc_ >> 16) & 0xff);
    slot.data[10] = static_cast<uint8_t>((ssrc_ >> 8) & 0xff);
    slot.data[11] = static_cast<uint8_t>(ssrc_ & 0xff);

    if (useTwcc) {
        // RTP one-byte header extension profile: 0xBEDE.
        // The transport-wide sequence value itself is assigned by the
        // sender thread immediately before send so media and future RTX
        // packets share one wire-order sequence space.
        slot.data[12] = 0xBE;
        slot.data[13] = 0xDE;

        // Extension length in 32-bit words. We use 1 word = 4 bytes.
        slot.data[14] = 0x00;
        slot.data[15] = 0x01;

        // One-byte extension header:
        // high 4 bits = extension id, low 4 bits = len - 1.
        // TWCC payload is 2 bytes, so len - 1 = 1.
        slot.data[16] = static_cast<uint8_t>((TWCC_EXTENSION_ID << 4) | 0x01);

        // Placeholder; senderLoop assigns the real transport sequence.
        slot.data[17] = 0x00;
        slot.data[18] = 0x00;

        // Padding to complete 32-bit extension word.
        slot.data[19] = 0x00;
    }

    std::copy(
        payload,
        payload + payloadSize,
        slot.data.data() + rtpHeaderSize
    );

    return true;
}

bool RealtimeRtpSender::enqueueRtpPacket(
    const uint8_t* payload,
    size_t payloadSize,
    uint32_t timestamp,
    bool marker
)
{
    if (
        !running_ ||
        socket_ == INVALID_SOCKET ||
        !payload ||
        payloadSize == 0
    ) {
        return false;
    }


    const uint64_t writeSeq =
        publishSequence_.load(std::memory_order_relaxed);

    const uint64_t readSeq =
        consumeSequence_.load(std::memory_order_acquire);

    const uint64_t queueSize = writeSeq - readSeq;

    if (queueSize >= RING_SIZE - 1) {
        packetsDropped_.fetch_add(
            1,
            std::memory_order_relaxed
        );

        static thread_local uint64_t ringFullLogCounter = 0;
        ringFullLogCounter++;

        if (ringFullLogCounter % 100 == 1) {
            std::cerr
                << "[Realtime RTP Sender:"
                << label_
                << "] ring full, dropping incoming packet"
                << " queue=" << queueSize
                << " capacity=" << RING_SIZE
                << " timestamp=" << timestamp
                << " marker=" << (marker ? "yes" : "no")
                << " pt=" << static_cast<int>(payloadType_)
                << " port=" << port_
                << "\n";
        }

        return false;
    }

    RtpPacket& slot = ring_[writeSeq % RING_SIZE];

    if (
        !writeRtpPacketToSlot(
            slot,
            payload,
            payloadSize,
            timestamp,
            marker
        )
    ) {
        packetsDropped_.fetch_add(
            1,
            std::memory_order_relaxed
        );

        return false;
    }

    slot.sequence = writeSeq;

    publishSequence_.store(
        writeSeq + 1,
        std::memory_order_release
    );

    packetsQueued_.fetch_add(
        1,
        std::memory_order_relaxed
    );

    const uint32_t currentQueue =
        static_cast<uint32_t>(queueSize + 1);

    uint32_t previousMax =
        maxQueueSeen_.load(std::memory_order_relaxed);

    while (
        currentQueue > previousMax &&
        !maxQueueSeen_.compare_exchange_weak(
            previousMax,
            currentQueue,
            std::memory_order_relaxed
        )
    ) {
    }

    signalSender();

    return true;
}

bool RealtimeRtpSender::sendEncodedPayload(
    const uint8_t* data,
    size_t size,
    uint32_t timestamp,
    bool marker
)
{
    if (!data || size == 0) return false;

    return enqueueRtpPacket(data, size, timestamp, marker);
}

bool RealtimeRtpSender::sendH264Nal(
    const uint8_t* data,
    size_t size,
    uint32_t timestamp,
    bool marker
)
{
    if (!data || size == 0) return false;
    
    const bool useTwcc = label_ == "video";
    const size_t rtpHeaderSize = useTwcc ? RTP_TWCC_HEADER_SIZE : RTP_HEADER_SIZE;
    const size_t retransmitHeadroom = useTwcc ? RTX_OSN_SIZE : 0;
    const size_t maxPayloadSize =
        MAX_RTP_PACKET_SIZE - rtpHeaderSize - retransmitHeadroom;

    if (size <= maxPayloadSize) {
        return enqueueRtpPacket(data, size, timestamp, marker);
    }

    const size_t fragmentPayloadSize =
        maxPayloadSize - 2;

    const size_t payloadBytesToFragment =
        size - 1;

    const uint64_t requiredFragments =
        static_cast<uint64_t>(
            (
                payloadBytesToFragment +
                fragmentPayloadSize - 1
            ) /
            fragmentPayloadSize
        );

    const uint64_t writeSeq =
        publishSequence_.load(
            std::memory_order_relaxed
        );

    const uint64_t readSeq =
        consumeSequence_.load(
            std::memory_order_acquire
        );

    const uint64_t queueSize =
        writeSeq - readSeq;

    const uint64_t usableCapacity =
        RING_SIZE - 1;

    const uint64_t freeSlots =
        usableCapacity > queueSize
            ? usableCapacity - queueSize
            : 0;

    if (requiredFragments > freeSlots) {
        fragmentedNalAdmissionDrops_.fetch_add(
            1,
            std::memory_order_relaxed
        );

        static thread_local uint64_t nalAdmissionDropLogCounter = 0;
        nalAdmissionDropLogCounter++;

        if (nalAdmissionDropLogCounter % 100 == 1) {
            std::cerr
                << "[Realtime RTP Sender:"
                << label_
                << "] fragmented NAL rejected before enqueue"
                << " requiredFragments=" << requiredFragments
                << " freeSlots=" << freeSlots
                << " queue=" << queueSize
                << " capacity=" << usableCapacity
                << " nalBytes=" << size
                << " timestamp=" << timestamp
                << "\n";
        }

        return false;
    }

    const uint8_t nalHeader = data[0];
    const uint8_t nalF = nalHeader & 0x80;
    const uint8_t nalNri = nalHeader & 0x60;
    const uint8_t fragmentedNalType  = nalHeader & 0x1f;

    const uint8_t fuIndicator = nalF | nalNri | 28;
    const uint8_t fuHeaderStart = 0x80 | fragmentedNalType ;
    const uint8_t fuHeaderMiddle = fragmentedNalType ;
    const uint8_t fuHeaderEnd = 0x40 | fragmentedNalType ;

    size_t offset = 1;
    bool first = true;

    while (offset < size) {
        const size_t remaining = size - offset;
        const size_t chunkSize = (std::min)(remaining, maxPayloadSize - 2);
        const bool last = (offset + chunkSize) >= size;

        uint8_t fuPayload[MAX_RTP_PACKET_SIZE] {};
        fuPayload[0] = fuIndicator;

        if (first) {
            fuPayload[1] = fuHeaderStart;
        } else if (last) {
            fuPayload[1] = fuHeaderEnd;
        } else {
            fuPayload[1] = fuHeaderMiddle;
        }

        std::copy(
            data + offset,
            data + offset + chunkSize,
            fuPayload + 2
        );

        if (!enqueueRtpPacket(
                fuPayload,
                chunkSize + 2,
                timestamp,
                marker && last
            )) {
            return false;
        }

        offset += chunkSize;
        first = false;
    }

    return true;
}

void RealtimeRtpSender::storeHistoryPacket(const RtpPacket& packet)
{
    if (packet.size == 0) return;

    const uint16_t rtpSequenceNumber =
        static_cast<uint16_t>((packet.data[2] << 8) | packet.data[3]);

    HistoryPacket& slot = history_[rtpSequenceNumber % HISTORY_SIZE];

    std::copy(
        packet.data.begin(),
        packet.data.begin() + packet.size,
        slot.data.begin()
    );

    slot.size = packet.size;
    slot.rtpSequenceNumber = rtpSequenceNumber;
    slot.valid = true;

    historyPacketsStored_.fetch_add(1, std::memory_order_relaxed);
}

bool RealtimeRtpSender::isRetransmitSuppressed(
    uint16_t rtpSequenceNumber,
    std::chrono::steady_clock::time_point now
) const
{
    for (size_t i = 0; i < lastRetransmitSeqs_.size(); ++i) {
        if (lastRetransmitSeqs_[i] != rtpSequenceNumber) {
            continue;
        }

        const auto previous = lastRetransmitTimes_[i];

        return (
            previous.time_since_epoch().count() != 0 &&
            now - previous < RTX_SUPPRESSION_WINDOW
        );
    }

    return false;
}

void RealtimeRtpSender::rememberRetransmit(
    uint16_t rtpSequenceNumber,
    std::chrono::steady_clock::time_point now
)
{
    for (size_t i = 0; i < lastRetransmitSeqs_.size(); ++i) {
        if (lastRetransmitSeqs_[i] == rtpSequenceNumber) {
            lastRetransmitTimes_[i] = now;
            return;
        }
    }

    const uint32_t index = lastRetransmitWriteIndex_.fetch_add(
        1,
        std::memory_order_relaxed
    );

    const size_t slot = index % RTX_SUPPRESSION_SIZE;

    lastRetransmitSeqs_[slot] = rtpSequenceNumber;
    lastRetransmitTimes_[slot] = now;
}

void RealtimeRtpSender::signalSender()
{
    #if defined(_WIN32)

    if (packetAvailableEvent_) {
        SetEvent(packetAvailableEvent_);
    }

    #else

    if (packetAvailableEventFd_ >= 0) {
        const uint64_t signal = 1;

        const ssize_t written = ::write(
            packetAvailableEventFd_,
            &signal,
            sizeof(signal)
        );

        (void)written;
    }

    #endif
}

bool RealtimeRtpSender::enqueueRetransmitRequest(
    uint16_t rtpSequenceNumber
)
{
    const uint64_t writeSeq =
        retransmitPublishSequence_.load(std::memory_order_relaxed);

    const uint64_t readSeq =
        retransmitConsumeSequence_.load(std::memory_order_acquire);

    const uint64_t queueSize = writeSeq - readSeq;

    if (queueSize >= RETRANSMIT_PENDING_SIZE - 1) {
        retransmitQueueDropped_.fetch_add(
            1,
            std::memory_order_relaxed
        );

        return false;
    }
    PendingRetransmit& pending =
        pendingRetransmits_[writeSeq % RETRANSMIT_PENDING_SIZE];

    pending.rtpSequenceNumber = rtpSequenceNumber;
    pending.sequence = writeSeq;
    pending.enqueuedAt = std::chrono::steady_clock::now();

    retransmitPublishSequence_.store(
        writeSeq + 1,
        std::memory_order_release
    );

    retransmitQueued_.fetch_add(1, std::memory_order_relaxed);

    const uint32_t currentQueue =
        static_cast<uint32_t>(queueSize + 1);

    uint32_t previousMax =
        maxRetransmitQueueSeen_.load(std::memory_order_relaxed);

    while (
        currentQueue > previousMax &&
        !maxRetransmitQueueSeen_.compare_exchange_weak(
            previousMax,
            currentQueue,
            std::memory_order_relaxed
        )
    ) {
    }

    signalSender();
    return true;
}

bool RealtimeRtpSender::serviceOnePendingRetransmit(
    std::chrono::steady_clock::time_point& nextSendTime,
    uint32_t mediaQueueDepth
)
{
    using clock = std::chrono::steady_clock;

    const uint64_t readSeq =
        retransmitConsumeSequence_.load(std::memory_order_relaxed);

    const uint64_t writeSeq =
        retransmitPublishSequence_.load(std::memory_order_acquire);

    if (readSeq == writeSeq) {
        return false;
    }

    const PendingRetransmit& pending =
        pendingRetransmits_[readSeq % RETRANSMIT_PENDING_SIZE];

    bool sendAttempted = false;
    bool sent = false;
    bool historyMissed = false;

    if (pending.sequence == readSeq) {
        const auto latencyMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                clock::now() - pending.enqueuedAt
            ).count();

        if (latencyMs >= 0) {
            const uint32_t latency = static_cast<uint32_t>(latencyMs);
            uint32_t previousMax =
                maxRetransmitQueueLatencyMs_.load(std::memory_order_relaxed);

            while (
                latency > previousMax &&
                !maxRetransmitQueueLatencyMs_.compare_exchange_weak(
                    previousMax,
                    latency,
                    std::memory_order_relaxed
                )
            ) {
            }
        }

        const uint16_t rtpSequenceNumber = pending.rtpSequenceNumber;
        const HistoryPacket& slot =
            history_[rtpSequenceNumber % HISTORY_SIZE];

        if (
            slot.valid &&
            slot.rtpSequenceNumber == rtpSequenceNumber &&
            slot.size > 0
        ) {
            RtpPacket packet {};

            if (buildRetransmitPacket(slot, packet)) {
                const uint32_t queueDepth = static_cast<uint32_t>(
                    (writeSeq - readSeq) + mediaQueueDepth
                );

                pacingCalls_.fetch_add(1, std::memory_order_relaxed);

                // RTX consumes the same active peak / virtual-departure
                // budget as media. Sustained accounting remains media-only.
                pacer_.pace(
                    senderThreadRunning_,
                    true,
                    queueDepth,
                    packet.size,
                    nextSendTime,
                    false
                );

                // TWCC belongs to the actual transport send order. Both
                // fresh media and retransmissions consume the same sequence
                // space on the video socket.
                assignTransportWideSequenceNumber(packet);

                sendAttempted = true;
                sent = sendRawPacketInternal(packet, false);
            }
        } else {
            historyMissed = true;
        }
    }

    retransmitConsumeSequence_.store(
        readSeq + 1,
        std::memory_order_release
    );

    if (sent) {
        retransmitPacketsSent_.fetch_add(1, std::memory_order_relaxed);
        historyPacketsRetransmitted_.fetch_add(1, std::memory_order_relaxed);
    } else {
        if (sendAttempted) {
            retransmitSendFailures_.fetch_add(1, std::memory_order_relaxed);
        }

        if (historyMissed) {
            historyPacketsMissed_.fetch_add(1, std::memory_order_relaxed);
        }

    }

    return true;
}

bool RealtimeRtpSender::consumeRetransmitBudget(
    std::chrono::steady_clock::time_point now
)
{
    if (
        retransmitWindowStartedAt_.time_since_epoch().count() == 0 ||
        now - retransmitWindowStartedAt_ >= std::chrono::seconds(1)
    ) {
        retransmitWindowStartedAt_ = now;

        retransmitsThisSecond_.store(
            0,
            std::memory_order_relaxed
        );
    }

    constexpr uint32_t MAX_RETRANSMITS_PER_SECOND = 300;

    if (
        retransmitsThisSecond_.fetch_add(
            1,
            std::memory_order_relaxed
        ) >= MAX_RETRANSMITS_PER_SECOND
    ) {
        retransmitRateLimited_.fetch_add(
            1,
            std::memory_order_relaxed
        );

        return false;
    }

    return true;
}

bool RealtimeRtpSender::retransmitPacket(uint16_t rtpSequenceNumber)
{
    retransmitRequests_.fetch_add(1, std::memory_order_relaxed);

    if (!running_ || socket_ == INVALID_SOCKET) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();

    if (isRetransmitSuppressed(rtpSequenceNumber, now)) {
        return false;
    }

    const HistoryPacket& slot =
        history_[rtpSequenceNumber % HISTORY_SIZE];

    if (
        !slot.valid ||
        slot.rtpSequenceNumber != rtpSequenceNumber ||
        slot.size == 0
    ) {
        historyPacketsMissed_.fetch_add(1, std::memory_order_relaxed);
        return false;
    } 

    if (!consumeRetransmitBudget(now)) {
        return false;
    }

    if (!enqueueRetransmitRequest(rtpSequenceNumber)) {
        return false;
    }

    rememberRetransmit(rtpSequenceNumber, now);

    return true;
}

bool RealtimeRtpSender::buildRetransmitPacket(
    const HistoryPacket& slot,
    RtpPacket& packet
)
{
    if (!rtxEnabled_) {
        if (slot.size == 0 || slot.size > MAX_RTP_PACKET_SIZE) {
            return false;
        }

        std::copy(
            slot.data.begin(),
            slot.data.begin() + slot.size,
            packet.data.begin()
        );

        packet.size = slot.size;
        return true;
    }

    const bool useTwcc = label_ == "video";
    const size_t originalRtpHeaderSize =
        useTwcc ? RTP_TWCC_HEADER_SIZE : RTP_HEADER_SIZE;
    const size_t rtxRtpHeaderSize =
        useTwcc ? RTP_TWCC_HEADER_SIZE : RTP_HEADER_SIZE;

    if (slot.size <= originalRtpHeaderSize) {
        return false;
    }

    const size_t originalPayloadSize =
        static_cast<size_t>(slot.size) - originalRtpHeaderSize;

    const size_t rtxPacketSize =
        rtxRtpHeaderSize + RTX_OSN_SIZE + originalPayloadSize;

    if (rtxPacketSize > MAX_RTP_PACKET_SIZE) {
        return false;
    }

    const bool originalMarker =
        (slot.data[1] & 0x80) != 0;

    const uint32_t originalTimestamp =
        (static_cast<uint32_t>(slot.data[4]) << 24) |
        (static_cast<uint32_t>(slot.data[5]) << 16) |
        (static_cast<uint32_t>(slot.data[6]) << 8) |
        static_cast<uint32_t>(slot.data[7]);

    packet = {};
    packet.size = static_cast<uint16_t>(rtxPacketSize);

    const uint16_t rtxSequenceNumber =
        rtxSequenceNumber_.fetch_add(1);

    packet.data[0] = useTwcc ? 0x90 : 0x80;
    packet.data[1] = static_cast<uint8_t>(
        (originalMarker ? 0x80 : 0x00) | rtxPayloadType_
    );

    packet.data[2] = static_cast<uint8_t>((rtxSequenceNumber >> 8) & 0xff);
    packet.data[3] = static_cast<uint8_t>(rtxSequenceNumber & 0xff);

    packet.data[4] = static_cast<uint8_t>((originalTimestamp >> 24) & 0xff);
    packet.data[5] = static_cast<uint8_t>((originalTimestamp >> 16) & 0xff);
    packet.data[6] = static_cast<uint8_t>((originalTimestamp >> 8) & 0xff);
    packet.data[7] = static_cast<uint8_t>(originalTimestamp & 0xff);

    packet.data[8] = static_cast<uint8_t>((rtxSsrc_ >> 24) & 0xff);
    packet.data[9] = static_cast<uint8_t>((rtxSsrc_ >> 16) & 0xff);
    packet.data[10] = static_cast<uint8_t>((rtxSsrc_ >> 8) & 0xff);
    packet.data[11] = static_cast<uint8_t>(rtxSsrc_ & 0xff);

    if (useTwcc) {
        packet.data[12] = 0xBE;
        packet.data[13] = 0xDE;
        packet.data[14] = 0x00;
        packet.data[15] = 0x01;
        packet.data[16] = static_cast<uint8_t>(
            (TWCC_EXTENSION_ID << 4) | 0x01
        );

        // Placeholder. serviceOnePendingRetransmit() assigns a fresh
        // transport-wide sequence immediately before the socket send.
        packet.data[17] = 0x00;
        packet.data[18] = 0x00;
        packet.data[19] = 0x00;
    }

    // OSN (Original Sequence Number), big-endian. It lives at the start of
    // the RTX payload, after any RTP header extensions.
    packet.data[rtxRtpHeaderSize] = static_cast<uint8_t>(
        (slot.rtpSequenceNumber >> 8) & 0xff
    );
    packet.data[rtxRtpHeaderSize + 1] = static_cast<uint8_t>(
        slot.rtpSequenceNumber & 0xff
    );

    std::copy(
        slot.data.begin() + originalRtpHeaderSize,
        slot.data.begin() + slot.size,
        packet.data.begin() + rtxRtpHeaderSize + RTX_OSN_SIZE
    );

    return true;
}

void RealtimeRtpSender::assignTransportWideSequenceNumber(
    RtpPacket& packet
)
{
    if (
        label_ != "video" ||
        packet.size < RTP_TWCC_HEADER_SIZE ||
        (packet.data[0] & 0x10) == 0
    ) {
        return;
    }

    const uint16_t twccSequenceNumber =
        twccSequenceNumber_.fetch_add(1, std::memory_order_relaxed);

    packet.data[17] = static_cast<uint8_t>(
        (twccSequenceNumber >> 8) & 0xff
    );
    packet.data[18] = static_cast<uint8_t>(
        twccSequenceNumber & 0xff
    );
}

bool RealtimeRtpSender::sendRawPacketInternal(
    const RtpPacket& packet,
    bool storeHistory
)
{
    if (!running_ || socket_ == INVALID_SOCKET || packet.size == 0) {
        return false;
    }

    #if defined(_WIN32)

    WSABUF buffer {};
    buffer.buf = reinterpret_cast<char*>(
        const_cast<uint8_t*>(packet.data.data())
    );
    buffer.len = static_cast<ULONG>(packet.size);

    DWORD bytesSent = 0;
    DWORD flags = 0;

    const int result = WSASend(
        socket_,
        &buffer,
        1,
        &bytesSent,
        flags,
        nullptr,
        nullptr
    );

    if (result == SOCKET_ERROR) {
        sendFailures_.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[Realtime RTP Sender:" << label_
                  << "] WSASend failed: "
                  << lastSocketError()
                  << "\n";
        return false;
    }

    #else

        const ssize_t sendResult = ::send(
        socket_,
        packet.data.data(),
        packet.size,
        0
    );

    if (sendResult < 0) {
        sendFailures_.fetch_add(
            1,
            std::memory_order_relaxed
        );

        std::cerr
            << "[Realtime RTP Sender:"
            << label_
            << "] send failed: "
            << lastSocketError()
            << "\n";

        return false;
    }

    const size_t bytesSent =
        static_cast<size_t>(sendResult);

    #endif

    if (bytesSent != packet.size) {
        partialSends_.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[Realtime RTP Sender:" << label_
                  << "] partial WSASend sent="
                  << bytesSent
                  << " expected=" << packet.size
                  << "\n";
        return false;
    }

    wirePacketsSent_.fetch_add(1, std::memory_order_relaxed);
    wireBytesSent_.fetch_add(packet.size, std::memory_order_relaxed);

    if (storeHistory) {
        packetsSent_.fetch_add(1, std::memory_order_relaxed);
        bytesSent_.fetch_add(packet.size, std::memory_order_relaxed);
    }

    if (label_ == "video") {
        static std::atomic<int64_t> windowBytes{ 0 };
        static auto windowStart = std::chrono::steady_clock::now();
        static constexpr int64_t kWindowMs = 100;
        
        windowBytes.fetch_add(packet.size, std::memory_order_relaxed);

        const auto now = std::chrono::steady_clock::now();
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                now - windowStart
            ).count();

        if (elapsedMs >= kWindowMs) {
            const int64_t actualBytes =
                windowBytes.exchange(0, std::memory_order_relaxed);

            windowStart = now;

            const BitrateDecision bitrateDecision =
                pacer_.bitrateDecision();

            const uint32_t targetBitrateBps =
                bitrateDecision.targetBitrateBps;

            if (targetBitrateBps > 0) {
                const uint64_t expectedBytes =
                    (static_cast<uint64_t>(targetBitrateBps) *
                    static_cast<uint64_t>(elapsedMs)) /
                    8000ULL;

                if (
                    static_cast<uint64_t>(actualBytes) >
                    expectedBytes * 2ULL
                ) {
                    const double instantMbps =
                        (static_cast<double>(actualBytes) * 8.0) /
                        (static_cast<double>(elapsedMs) / 1000.0) /
                        1000000.0;

                    std::cerr
                        << "[Realtime RTP Sender:video] burst-check"
                        << " windowMs=" << elapsedMs
                        << " bytes=" << actualBytes
                        << " expectedBytes=" << expectedBytes
                        << " targetMbps="
                        << (static_cast<double>(targetBitrateBps) /
                            1000000.0)
                        << " instantMbps=" << instantMbps
                        << "\n";
                }
            }
        }
    }

    if (storeHistory && label_ == "video") {
        storeHistoryPacket(packet);
    }

    return true;
}

bool RealtimeRtpSender::sendRawPacket(const RtpPacket& packet)
{
    return sendRawPacketInternal(packet, true);
}

void RealtimeRtpSender::flushPacketBatch(const PacketBatch& batch)
{
    if (batch.empty()) {
        return;
    }

    for (uint32_t i = 0; i < batch.count; i++) {
        const RtpPacket* packet = batch.packets[i];

        if (!packet || packet->size == 0) {
            continue;
        }

        sendRawPacket(*packet);
    }
}

void RealtimeRtpSender::senderLoop()
{
    #if defined(_WIN32)

    DWORD priority = THREAD_PRIORITY_ABOVE_NORMAL;

    if (!SetThreadPriority(GetCurrentThread(), priority)) {
        std::cerr << "[Realtime RTP Sender:" << label_
                  << "] failed to set thread priority, error="
                  << GetLastError()
                  << "\n";
    } else {
        std::cerr << "[Realtime RTP Sender:" << label_
                  << "] thread priority set="
                  << priority
                  << "\n";
    }

    #endif

    using clock = std::chrono::steady_clock;

    auto nextStatsLog = clock::now() + std::chrono::seconds(10);
    auto nextSendTime = clock::now();

    auto lastBatchWindowStart = clock::now();
    uint32_t packetsSentInWindow = 0;

    constexpr uint32_t MAX_BATCH_PACKETS = 1;
    constexpr uint32_t VIDEO_SOFT_BATCH_PACKETS = 1;
    constexpr uint32_t AUDIO_SOFT_BATCH_PACKETS = 1;
    
    pacer_.logBaseline(label_, MAX_BATCH_PACKETS);

    while (true) {
        senderLoopIterations_.fetch_add(
            1,
            std::memory_order_relaxed
        );
        drainIncomingControlPackets();
        const uint64_t readSeq = consumeSequence_.load(std::memory_order_relaxed);
        const uint64_t writeSeq = publishSequence_.load(std::memory_order_acquire);
        const uint64_t rtxReadSeq =
            retransmitConsumeSequence_.load(std::memory_order_relaxed);
        const uint64_t rtxWriteSeq =
            retransmitPublishSequence_.load(std::memory_order_acquire);

        const bool hasMedia = readSeq != writeSeq;
        const bool hasPendingRetransmit = rtxReadSeq != rtxWriteSeq;

        const bool shouldContinueRunning =
            senderThreadRunning_.load(
                std::memory_order_acquire
            );

        if (
            !shouldContinueRunning &&
            !hasMedia &&
            !hasPendingRetransmit
        ) {
            break;
        }

        if (!hasMedia && !hasPendingRetransmit) {
            emptyQueueWaits_.fetch_add(
                1,
                std::memory_order_relaxed
            );

            #if defined(_WIN32)

            if (
                label_ == "video" &&
                packetAvailableEvent_ &&
                socketReadEvent_ != WSA_INVALID_EVENT
            ) {
                HANDLE waitHandles[2] = {
                    packetAvailableEvent_,
                    socketReadEvent_
                };

                const DWORD waitResult =
                    WaitForMultipleObjects(
                        2,
                        waitHandles,
                        FALSE,
                        INFINITE
                    );

                if (waitResult == WAIT_OBJECT_0) {
                    emptyQueueSignals_.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
                } else if (
                    waitResult == WAIT_OBJECT_0 + 1
                ) {
                    WSANETWORKEVENTS networkEvents{};

                    if (
                        WSAEnumNetworkEvents(
                            socket_,
                            socketReadEvent_,
                            &networkEvents
                        ) == SOCKET_ERROR
                    ) {
                        emptyQueueWaitErrors_.fetch_add(
                            1,
                            std::memory_order_relaxed
                        );
                    } else if (
                        networkEvents.lNetworkEvents &
                        FD_READ
                    ) {
                        drainIncomingControlPackets();
                    }
                } else {
                    emptyQueueWaitErrors_.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
                }
            } else if (packetAvailableEvent_) {
                const DWORD waitResult =
                    WaitForSingleObject(
                        packetAvailableEvent_,
                        INFINITE
                    );

                if (waitResult == WAIT_OBJECT_0) {
                    emptyQueueSignals_.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
                } else {
                    emptyQueueWaitErrors_.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
                }
            } else {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(2)
                );

                emptyQueueTimeouts_.fetch_add(
                    1,
                    std::memory_order_relaxed
                );
            }

            #else

            if (packetAvailableEventFd_ < 0) {
                emptyQueueWaitErrors_.fetch_add(
                    1,
                    std::memory_order_relaxed
                );

                std::this_thread::yield();

                nextSendTime = clock::now();
                continue;
            }

            struct pollfd fds[2] {};
            nfds_t fdCount = 0;

            const nfds_t queueEventIndex = fdCount;

            fds[fdCount].fd =
                packetAvailableEventFd_;

            fds[fdCount].events =
                POLLIN;

            fdCount++;

            nfds_t socketIndex = 0;
            bool watchSocket = false;

            if (
                label_ == "video" &&
                socket_ != INVALID_SOCKET
            ) {
                socketIndex = fdCount;

                fds[fdCount].fd =
                    socket_;

                fds[fdCount].events =
                    POLLIN;

                fdCount++;

                watchSocket = true;
            }

            const int pollResult =
                ::poll(
                    fds,
                    fdCount,
                    -1
                );

            if (pollResult > 0) {

                if (
                    fds[queueEventIndex].revents &
                    POLLIN
                ) {
                    uint64_t signalCount = 0;

                    const ssize_t readResult =
                        ::read(
                            packetAvailableEventFd_,
                            &signalCount,
                            sizeof(signalCount)
                        );

                    if (
                        readResult ==
                        static_cast<ssize_t>(
                            sizeof(signalCount)
                        )
                    ) {
                        emptyQueueSignals_.fetch_add(
                            1,
                            std::memory_order_relaxed
                        );
                    }
                }

                if (
                    watchSocket &&
                    (
                        fds[socketIndex].revents &
                        POLLIN
                    )
                ) {
                    drainIncomingControlPackets();
                }

                bool pollFdError = false;

                if (
                    fds[queueEventIndex].revents &
                    (POLLERR | POLLHUP | POLLNVAL)
                ) {
                    pollFdError = true;
                }

                if (
                    watchSocket &&
                    (
                        fds[socketIndex].revents &
                        (POLLERR | POLLHUP | POLLNVAL)
                    )
                ) {
                    pollFdError = true;
                }

                if (pollFdError) {
                    emptyQueueWaitErrors_.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
                }

            } else if (pollResult < 0) {
                if (errno != EINTR) {
                    emptyQueueWaitErrors_.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );
                }
            }

            #endif

            nextSendTime = clock::now();

            continue;
        }

        const uint32_t queueSizeBeforeSend = static_cast<uint32_t>(writeSeq - readSeq);

        // Retransmissions get first service opportunity, but only one per
        // loop. If fresh media is also queued, it is serviced immediately
        // afterwards, preventing strict-priority RTX starvation.
        if (hasPendingRetransmit) {
            serviceOnePendingRetransmit(
                nextSendTime,
                queueSizeBeforeSend
            );

            if (!hasMedia) {
                continue;
            }
        }

        const auto batchNow = clock::now();

        if (batchNow - lastBatchWindowStart >= std::chrono::milliseconds(1)) {
            lastBatchWindowStart = batchNow;
            packetsSentInWindow = 0;
        }

        const uint64_t available = writeSeq - readSeq;

        uint32_t dynamicBatchLimit = MAX_BATCH_PACKETS;

        const bool isVideoBatch = label_ == "video";
        const uint32_t perMsPacketBudget = isVideoBatch ? 16 : 4;

        if (isVideoBatch && available > 180 && packetsSentInWindow < perMsPacketBudget) {
            dynamicBatchLimit = VIDEO_SOFT_BATCH_PACKETS;
        } else if (!isVideoBatch) {
            dynamicBatchLimit = AUDIO_SOFT_BATCH_PACKETS;
        }

        const uint32_t remainingBudget =
            packetsSentInWindow < perMsPacketBudget
                ? perMsPacketBudget - packetsSentInWindow
                : 1;

        dynamicBatchLimit = (std::min)(dynamicBatchLimit, remainingBudget);

        const uint32_t batchCount = static_cast<uint32_t>(
            (std::min<uint64_t>)(available, dynamicBatchLimit)
        );

        PacketBatch batch;
        uint64_t currentReadSeq = readSeq;

        for (uint32_t i = 0; i < batchCount; i++) {

            RtpPacket& packet = ring_[currentReadSeq % RING_SIZE];

            if (packet.sequence == currentReadSeq) {
                const auto latencyMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    clock::now() - packet.enqueuedAt
                ).count();

                
                if (latencyMs >= 0) {
                    const uint32_t latency =
                        static_cast<uint32_t>(latencyMs);

                    totalQueueLatencyMs_.fetch_add(
                        latency,
                        std::memory_order_relaxed
                    );

                    queueLatencySamples_.fetch_add(
                        1,
                        std::memory_order_relaxed
                    );

                    uint32_t previousMax =
                        maxQueueLatencyMs_.load(
                            std::memory_order_relaxed
                        );

                    while (
                        latency > previousMax &&
                        !maxQueueLatencyMs_.compare_exchange_weak(
                            previousMax,
                            latency,
                            std::memory_order_relaxed
                        )
                    ) {
                    }

                    if (label_ == "video" && latency >= 50) {
                        static thread_local auto lastHighLatencyLogAt =
                            clock::time_point{};

                        const auto now = clock::now();

                        if (
                            lastHighLatencyLogAt.time_since_epoch().count() == 0 ||
                            now - lastHighLatencyLogAt >= std::chrono::seconds(1)
                        ) {
                            std::cerr
                                << "[Realtime RTP Sender:"
                                << label_
                                << "] queue latency warning"
                                << " latencyMs=" << latency
                                << " packetTimestamp=" << packet.timestamp
                                << " marker="
                                << (packet.marker ? "yes" : "no")
                                << " sequence=" << packet.sequence
                                << " queueBeforeSend="
                                << queueSizeBeforeSend
                                << "\n";

                            lastHighLatencyLogAt = now;
                        }
                    }
                }

                batch.push(&packet);
                packetsProcessedByLoop_.fetch_add(
                    1,
                    std::memory_order_relaxed
                );
            } else {
                packetsDropped_.fetch_add(1, std::memory_order_relaxed);
            }

            currentReadSeq++;
        }

        uint32_t pacedBytes = 0;

        for (uint32_t i = 0; i < batch.count; i++) {
            const RtpPacket* packet = batch.packets[i];

            if (packet) {
                pacedBytes += packet->size;
            }
        }

        pacingCalls_.fetch_add(
            1,
            std::memory_order_relaxed
        );

        pacer_.pace(
            senderThreadRunning_,
            label_ == "video",
            queueSizeBeforeSend,
            pacedBytes,
            nextSendTime
        );

        for (uint32_t i = 0; i < batch.count; i++) {
            RtpPacket* packet = batch.packets[i];
            if (packet) {
                assignTransportWideSequenceNumber(*packet);
            }
        }

        flushPacketBatch(batch);

        packetsSentInWindow += batchCount;

        consumeSequence_.store(
            currentReadSeq,
            std::memory_order_release
        );

        const uint32_t queueSize = static_cast<uint32_t>(
            publishSequence_.load(std::memory_order_acquire) -
            consumeSequence_.load(std::memory_order_acquire)
        );

        static thread_local uint64_t queueHighLogCounter = 0;

        if (queueSize > 100) {
            queueHighLogCounter++;

            if (queueHighLogCounter % 300 == 1) {
                std::cerr << "[Realtime RTP Sender:" << label_
                        << "] queue high="
                        << queueSize
                        << " pt=" << static_cast<int>(payloadType_)
                        << " port=" << port_
                        << "\n";
            }
        }

        const auto now = clock::now();

        if (STREAM_DEBUG_RTP_STATS && now >= nextStatsLog) {
            const auto feedback = pacer_.networkFeedback();
            const bool isVideoStats = label_ == "video";
            std::cerr << "[Realtime RTP Sender:" << label_
                    << "] stats"
                    << " queued=" << packetsQueued_.load()
                    << " sent=" << packetsSent_.load()
                    << " dropped=" << packetsDropped_.load()
                    << " bytes=" << bytesSent_.load()
                    << " wireSent=" << wirePacketsSent_.load()
                    << " wireBytes=" << wireBytesSent_.load()
                    << " queue=" << queueSize
                    << " maxQueue=" << maxQueueSeen_.load()
                    << " avgQueueLatencyMs="
                    << (
                        queueLatencySamples_.load() > 0
                            ? totalQueueLatencyMs_.load() / queueLatencySamples_.load()
                            : 0
                    )
                    << " maxQueueLatencyMs=" << maxQueueLatencyMs_.load()
                    << " lastBatch=" << batchCount
                    << " sendFail=" << sendFailures_.load()
                    << " partialSend=" << partialSends_.load()
                    << " histStored=" << historyPacketsStored_.load()
                    << " histRtx=" << historyPacketsRetransmitted_.load()
                    << " histMiss=" << historyPacketsMissed_.load()
                    << " fbLoss=" << feedback.packetLossRatio
                    << " fbJitterMs=" << feedback.jitterMs
                    << " fbScore=" << feedback.score
                    << " fbBitrate=" << feedback.bitrateBps
                    << " fbPacketCount=" << feedback.packetCount
                    << " fbByteCount=" << feedback.byteCount
                    << " fbNackCount=" << feedback.nackCount
                    << " fbNackPacketCount=" << feedback.nackPacketCount
                    << " fbPliCount=" << feedback.pliCount
                    << " fbFirCount=" << feedback.firCount
                    << " bitrateTarget=" << pacer_.bitrateDecision().targetBitrateBps
                    << " bitrateChange=" << (pacer_.bitrateDecision().shouldChangeEncoder ? "yes" : "no")
                    << " bitrateStable=" << pacer_.bitrateDecision().stableFeedbackCount
                    << " fb=" << (feedback.hasFeedback ? "yes" : "no")
                    << " adaptiveHyst=" << (isVideoStats && pacer_.isAdaptiveActive() ? "yes" : "no")
                    << " adaptiveExtraUs=" << pacer_.adaptiveExtraUs(isVideoStats)
                    << "\n";

            nextStatsLog = now + std::chrono::seconds(10);
        }
    }
}
