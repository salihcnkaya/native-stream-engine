#include "realtime_rtp_sender.h"

#include <algorithm>
#include <chrono>
#include <iostream>

#pragma comment(lib, "Ws2_32.lib")

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

BitrateDecision RealtimeRtpSender::bitrateDecision() const
{
    return pacer_.bitrateDecision();
}

void RealtimeRtpSender::resetStats()
{
    packetsQueued_ = 0;
    packetsSent_ = 0;
    packetsDropped_ = 0;
    bytesSent_ = 0;
    maxQueueSeen_ = 0;
    sendFailures_ = 0;
    partialSends_ = 0;
    totalQueueLatencyMs_ = 0;
    maxQueueLatencyMs_ = 0;
    queueLatencySamples_ = 0;
    historyPacketsStored_ = 0;
    historyPacketsRetransmitted_ = 0;
    historyPacketsMissed_ = 0;
    lastRetransmitWriteIndex_ = 0;
    retransmitsThisSecond_ = 0;
    retransmitWindowStartedAt_ = std::chrono::steady_clock::time_point{};

    for (auto& seq : lastRetransmitSeqs_) {
        seq = 0xffff;
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
    uint32_t ssrc
)
{
    if (running_) {
        stop();
    }

    ip_ = ip;
    port_ = port;
    payloadType_ = payloadType;
    ssrc_ = ssrc;

    resetStats();

    publishSequence_ = 0;
    consumeSequence_ = 0;
    sequenceNumber_ = 1;
    twccSequenceNumber_ = 1;

    WSADATA wsaData {};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "[Realtime RTP Sender:" << label_
                  << "] WSAStartup failed\n";
        return false;
    }

    socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (socket_ == INVALID_SOCKET) {
        std::cerr << "[Realtime RTP Sender:" << label_
                  << "] socket failed\n";
        WSACleanup();
        return false;
    }

    int sendBufferSize = 4 * 1024 * 1024;
    setsockopt(
        socket_,
        SOL_SOCKET,
        SO_SNDBUF,
        reinterpret_cast<const char*>(&sendBufferSize),
        sizeof(sendBufferSize)
    );

    sockaddr_in remoteAddr {};
    remoteAddr.sin_family = AF_INET;
    remoteAddr.sin_port = htons(port_);

    if (inet_pton(AF_INET, ip_.c_str(), &remoteAddr.sin_addr) != 1) {
        std::cerr << "[Realtime RTP Sender:" << label_
                  << "] invalid ip: " << ip_ << "\n";

        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        WSACleanup();

        return false;
    }

    if (connect(
            socket_,
            reinterpret_cast<const sockaddr*>(&remoteAddr),
            sizeof(remoteAddr)
        ) == SOCKET_ERROR) {
        std::cerr << "[Realtime RTP Sender:" << label_
                << "] UDP connect failed: "
                << WSAGetLastError()
                << "\n";

        closesocket(socket_);
        socket_ = INVALID_SOCKET;
        WSACleanup();

        return false;
    }

    u_long nonBlocking = 1;
    ioctlsocket(socket_, FIONBIO, &nonBlocking);
    
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

    senderThreadRunning_ = false;

    if (senderThread_.joinable()) {
        senderThread_.join();
    }

    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }

    WSACleanup();

    running_ = false;

    const uint64_t latencySamples =
        queueLatencySamples_.load(std::memory_order_relaxed);

    const uint64_t averageQueueLatencyMs =
        latencySamples > 0
            ? totalQueueLatencyMs_.load(std::memory_order_relaxed) /
                latencySamples
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
        << " bytes="
        << bytesSent_.load(std::memory_order_relaxed)
        << " maxQueue="
        << maxQueueSeen_.load(std::memory_order_relaxed)
        << " avgQueueLatencyMs="
        << averageQueueLatencyMs
        << " maxQueueLatencyMs="
        << maxQueueLatencyMs_.load(std::memory_order_relaxed)
        << " sendFailures="
        << sendFailures_.load(std::memory_order_relaxed)
        << " partialSends="
        << partialSends_.load(std::memory_order_relaxed)
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
        int fromLen = sizeof(from);

        const int received = recvfrom(
            socket_,
            reinterpret_cast<char*>(buffer),
            sizeof(buffer),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &fromLen
        );

        if (received <= 0) {
            const int error = WSAGetLastError();

            if (error == WSAEWOULDBLOCK) {
                return;
            }

            return;
        }

        handleIncomingControlPacket(buffer, received);
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

    const uint8_t version = data[0] >> 6;
    const uint8_t fmt = data[0] & 0x1f;
    const uint8_t packetType = data[1];

    if (version != 2) {
        return;
    }

    // RTCP Transport Feedback, Generic NACK
    if (packetType == 205 && fmt == 1) {
        handleRtcpGenericNack(data, size);
        return;
    }

    // RTCP Transport Feedback, Transport-Wide CC
    if (packetType == 205 && fmt == 15) {
        handleRtcpTransportWideFeedback(data, size);
        return;
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

    if (packetSize > size || packetSize < 16) {
        return;
    }

    const uint32_t mediaSsrc =
        (static_cast<uint32_t>(data[12]) << 24) |
        (static_cast<uint32_t>(data[13]) << 16) |
        (static_cast<uint32_t>(data[14]) << 8) |
        static_cast<uint32_t>(data[15]);

    if (mediaSsrc != ssrc_) {
        return;
    }

    int offset = 16;

    while (offset + 4 <= packetSize) {
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

    if (packetSize > size || packetSize < 20) {
        return;
    }

    const uint32_t mediaSsrc =
        (static_cast<uint32_t>(data[12]) << 24) |
        (static_cast<uint32_t>(data[13]) << 16) |
        (static_cast<uint32_t>(data[14]) << 8) |
        static_cast<uint32_t>(data[15]);

    if (mediaSsrc != ssrc_) {
        return;
    }

    const uint16_t baseSequence =
        static_cast<uint16_t>((data[16] << 8) | data[17]);

    const uint16_t packetStatusCount =
        static_cast<uint16_t>((data[18] << 8) | data[19]);

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
        const uint16_t twccSequenceNumber = twccSequenceNumber_.fetch_add(1);

        // RTP one-byte header extension profile: 0xBEDE
        slot.data[12] = 0xBE;
        slot.data[13] = 0xDE;

        // Extension length in 32-bit words. We use 1 word = 4 bytes.
        slot.data[14] = 0x00;
        slot.data[15] = 0x01;

        // One-byte extension header:
        // high 4 bits = extension id, low 4 bits = len - 1.
        // TWCC payload is 2 bytes, so len - 1 = 1.
        slot.data[16] = static_cast<uint8_t>((TWCC_EXTENSION_ID << 4) | 0x01);

        slot.data[17] = static_cast<uint8_t>((twccSequenceNumber >> 8) & 0xff);
        slot.data[18] = static_cast<uint8_t>(twccSequenceNumber & 0xff);

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
    const size_t maxPayloadSize = MAX_RTP_PACKET_SIZE - rtpHeaderSize;

    if (size <= maxPayloadSize) {
        return enqueueRtpPacket(data, size, timestamp, marker);
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

bool RealtimeRtpSender::shouldSuppressRetransmit(
    uint16_t rtpSequenceNumber
)
{
    for (const uint16_t seq : lastRetransmitSeqs_) {
        if (seq == rtpSequenceNumber) {
            return true;
        }
    }

    const uint32_t index = lastRetransmitWriteIndex_.fetch_add(
        1,
        std::memory_order_relaxed
    );

    lastRetransmitSeqs_[index % RTX_SUPPRESSION_SIZE] = rtpSequenceNumber;

    return false;
}

bool RealtimeRtpSender::retransmitPacket(uint16_t rtpSequenceNumber)
{
    if (!running_ || socket_ == INVALID_SOCKET) {
        return false;
    }

    if (shouldSuppressRetransmit(rtpSequenceNumber)) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();

    if (
        retransmitWindowStartedAt_.time_since_epoch().count() == 0 ||
        now - retransmitWindowStartedAt_ >= std::chrono::seconds(1)
    ) {
        retransmitWindowStartedAt_ = now;
        retransmitsThisSecond_ = 0;
    }

    constexpr uint32_t MAX_RETRANSMITS_PER_SECOND = 300;

    if (retransmitsThisSecond_.fetch_add(1, std::memory_order_relaxed) >= MAX_RETRANSMITS_PER_SECOND) {
        historyPacketsMissed_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const HistoryPacket& slot = history_[rtpSequenceNumber % HISTORY_SIZE];

    if (
        !slot.valid ||
        slot.rtpSequenceNumber != rtpSequenceNumber ||
        slot.size == 0
    ) {
        historyPacketsMissed_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    RtpPacket packet {};
    std::copy(
        slot.data.begin(),
        slot.data.begin() + slot.size,
        packet.data.begin()
    );

    packet.size = slot.size;

    const bool ok = sendRawPacketInternal(packet, false);

    if (ok) {
        historyPacketsRetransmitted_.fetch_add(1, std::memory_order_relaxed);
    } else {
        historyPacketsMissed_.fetch_add(1, std::memory_order_relaxed);
    }

    return ok;
}

bool RealtimeRtpSender::sendRawPacketInternal(
    const RtpPacket& packet,
    bool storeHistory
)
{
    if (!running_ || socket_ == INVALID_SOCKET || packet.size == 0) {
        return false;
    }

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
                  << WSAGetLastError()
                  << "\n";
        return false;
    }

    if (bytesSent != packet.size) {
        partialSends_.fetch_add(1, std::memory_order_relaxed);
        std::cerr << "[Realtime RTP Sender:" << label_
                  << "] partial WSASend sent="
                  << bytesSent
                  << " expected=" << packet.size
                  << "\n";
        return false;
    }

    packetsSent_.fetch_add(1, std::memory_order_relaxed);
    bytesSent_.fetch_add(packet.size, std::memory_order_relaxed);

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

    using clock = std::chrono::steady_clock;

    auto nextStatsLog = clock::now() + std::chrono::seconds(10);
    auto nextSendTime = clock::now();

    auto lastBatchWindowStart = clock::now();
    uint32_t packetsSentInWindow = 0;

    constexpr uint32_t MAX_BATCH_PACKETS = 1;
    constexpr uint32_t VIDEO_SOFT_BATCH_PACKETS = 4;
    constexpr uint32_t AUDIO_SOFT_BATCH_PACKETS = 1;
    
    pacer_.logBaseline(label_, MAX_BATCH_PACKETS);

    while (senderThreadRunning_) {
        drainIncomingControlPackets();
        const uint64_t readSeq = consumeSequence_.load(std::memory_order_relaxed);
        const uint64_t writeSeq = publishSequence_.load(std::memory_order_acquire);

        if (readSeq == writeSeq) {
            nextSendTime = clock::now();
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }

        const uint32_t queueSizeBeforeSend = static_cast<uint32_t>(writeSeq - readSeq);

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

            const RtpPacket& packet = ring_[currentReadSeq % RING_SIZE];

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

                    /*
                    * Sadece video queue 120 ms veya daha fazla gerideyse
                    * saniyede en fazla bir kez canlÄ± uyarÄ± yaz.
                    */
                    if (label_ == "video" && latency >= 120) {
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
            } else {
                packetsDropped_.fetch_add(1, std::memory_order_relaxed);
            }

            currentReadSeq++;
        }

        flushPacketBatch(batch);

        packetsSentInWindow += batchCount;
        consumeSequence_.store(currentReadSeq, std::memory_order_release);
       
        const uint32_t queueSize = static_cast<uint32_t>(
            publishSequence_.load(std::memory_order_acquire) -
            consumeSequence_.load(std::memory_order_acquire)
        );

        pacer_.pace(
            senderThreadRunning_,
            label_ == "video",
            queueSizeBeforeSend,
            nextSendTime
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
