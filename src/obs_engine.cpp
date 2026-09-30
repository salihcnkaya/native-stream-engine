#include "obs_engine.h"

#include <obs.h>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <iostream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <future>
#include "window_utils.h"
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <roapi.h>
#elif defined(__linux__)
#include <glib.h>
#include "linux_source_utils.h"
#endif
#include "native_wgc_source.h"
#include "realtime_rtp_sender.h"
#include <util/base.h>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include "rtp_output.h"

namespace fs = std::filesystem;

static obs_scene_t* g_scene = nullptr;
static obs_sceneitem_t* g_captureSceneItem = nullptr;
static obs_source_t* g_captureSource = nullptr;
#if defined(__linux__)
static std::string g_linuxSelectedAudioTarget;
#endif

static std::atomic<bool> g_obsOutputStateUnstable{ false };


static obs_source_t* g_audioSource = nullptr;
static obs_encoder_t* g_rtpVideoEncoder = nullptr;
static bool g_rtpStreaming = false;

static obs_output_t* g_rtpOutput = nullptr;
static obs_encoder_t* g_rtpAudioEncoder = nullptr;
static uint64_t g_rtpPacketCount = 0;
static int64_t g_firstVideoPts = 0;
static bool g_firstVideoPtsSet = false;
static std::vector<uint8_t> g_cachedSps;
static std::vector<uint8_t> g_cachedPps;

static int64_t g_firstAudioPts = 0;
static bool g_firstAudioPtsSet = false;
static RealtimeRtpSender g_realtimeRtpSender;
static RealtimeRtpSender g_realtimeRtpAudioSender;

static void handleRtpEncodedPacket(
    encoder_packet* packet
);

static uint32_t readRtpVideoEncoderConfiguredBitrate();

#if defined(NDEBUG)
static constexpr bool STREAM_DEBUG_BITRATE_DECISIONS = false;
static constexpr bool STREAM_DEBUG_KEYFRAME_REQUESTS = false;
static constexpr bool STREAM_FORWARD_OBS_DEBUG_LOGS = false;
#else
static constexpr bool STREAM_DEBUG_BITRATE_DECISIONS = true;
static constexpr bool STREAM_DEBUG_KEYFRAME_REQUESTS = true;
static constexpr bool STREAM_FORWARD_OBS_DEBUG_LOGS = true;
#endif

static uint32_t g_lastPliCount = 0;
static uint32_t g_lastFirCount = 0;
static uint32_t g_lastNackPacketCount = 0;
static auto g_lastKeyframeRequestAt = std::chrono::steady_clock::time_point{};

static std::mutex g_obsLogMutex;

static void nativeObsLogHandler(
    int logLevel,
    const char* format,
    va_list args,
    void*
)
{
    if (!format) {
        return;
    }

    if (
        logLevel == LOG_DEBUG &&
        !STREAM_FORWARD_OBS_DEBUG_LOGS
    ) {
        return;
    }

    char message[4096];

    va_list argsCopy;
    va_copy(argsCopy, args);

    const int written = std::vsnprintf(
        message,
        sizeof(message),
        format,
        argsCopy
    );

    va_end(argsCopy);

    if (written < 0) {
        return;
    }

    const char* levelName = "info";

    switch (logLevel) {
        case LOG_ERROR:
            levelName = "error";
            break;

        case LOG_WARNING:
            levelName = "warning";
            break;

        case LOG_DEBUG:
            levelName = "debug";
            break;

        case LOG_INFO:
        default:
            levelName = "info";
            break;
    }

    std::lock_guard<std::mutex> lock(
        g_obsLogMutex
    );

    std::cerr
        << "[OBS "
        << levelName
        << "] "
        << message;

    const size_t messageLength =
        std::strlen(message);

    if (
        messageLength == 0 ||
        message[messageLength - 1] != '\n'
    ) {
        std::cerr << '\n';
    }

    std::cerr.flush();
}

std::string ObsEngine::toUtf8Path(const fs::path& path)
{
    return path.u8string();
}

static void releaseRtpStreamingResources()
{
    if (g_rtpOutput) {
        obs_output_release(g_rtpOutput);
        g_rtpOutput = nullptr;
    }

    if (g_rtpVideoEncoder) {
        obs_encoder_release(g_rtpVideoEncoder);
        g_rtpVideoEncoder = nullptr;
    }

    if (g_rtpAudioEncoder) {
        obs_encoder_release(g_rtpAudioEncoder);
        g_rtpAudioEncoder = nullptr;
    }
}

static size_t findStartCode(const uint8_t* data, size_t size, size_t offset)
{
    for (size_t i = offset; i + 3 < size; i++) {
        if (data[i] == 0x00 && data[i + 1] == 0x00) {
            if (data[i + 2] == 0x01) {
                return i;
            }

            if (i + 4 < size && data[i + 2] == 0x00 && data[i + 3] == 0x01) {
                return i;
            }
        }
    }

    return size;
}

bool ObsEngine::waitForCaptureFrame(
    int timeoutMs,
    uint32_t& width,
    uint32_t& height,
    const std::atomic<bool>* cancelFlag
) const
{
    #if defined(_WIN32)
    width = 0;
    height = 0;

    if (timeoutMs < 0) {
        timeoutMs = 0;
    }

    const auto startedAt =
        std::chrono::steady_clock::now();

    const auto timeout =
        std::chrono::milliseconds(
            timeoutMs
        );

    while (true) {
        if (
            cancelFlag &&
            cancelFlag->load(std::memory_order_acquire)
        ) {
            std::cerr
                << "[Native Stream Engine] "
                << "capture frame wait aborted"
                << " reason=cancelled\n";

            return false;
        }

        if (isWgcTargetClosed()) {
            std::cerr
                << "[Native Stream Engine] "
                << "capture frame wait aborted"
                << " reason=target_closed"
                << "\n";

            return false;
        }

        if (isWgcFrameReady()) {
            width =
                getWgcFrameWidth();

            height =
                getWgcFrameHeight();

            if (
                width > 0 &&
                height > 0
            ) {
                std::cerr
                    << "[Native Stream Engine] "
                    << "capture frame ready "
                    << width
                    << "x"
                    << height
                    << "\n";

                return true;
            }
        }

        const auto now =
            std::chrono::steady_clock::now();

        if (
            now - startedAt >=
            timeout
        ) {
            std::cerr
                << "[Native Stream Engine] "
                << "capture frame wait timed out"
                << " timeoutMs="
                << timeoutMs
                << "\n";

            return false;
        }

        std::this_thread::sleep_for(
            std::chrono::milliseconds(10)
        );
    }
    #else

        width = 0;
        height = 0;

        if (timeoutMs < 0) {
            timeoutMs = 0;
        }

        const auto startedAt =
            std::chrono::steady_clock::now();

        const auto timeout =
            std::chrono::milliseconds(
                timeoutMs
            );

        while (true) {
            while (
                g_main_context_iteration(
                    nullptr,
                    false
                )
            ) {

            }
            
            if (
                cancelFlag &&
                cancelFlag->load(
                    std::memory_order_acquire
                )
            ) {
                std::cerr
                    << "[Native Stream Engine] "
                    << "PipeWire capture frame wait aborted"
                    << " reason=cancelled\n";

                return false;
            }

            if (g_captureSource) {
                const uint32_t sourceWidth =
                    obs_source_get_width(
                        g_captureSource
                    );

                const uint32_t sourceHeight =
                    obs_source_get_height(
                        g_captureSource
                    );

                if (
                    sourceWidth > 0 &&
                    sourceHeight > 0
                ) {
                    width = sourceWidth;
                    height = sourceHeight;

                    obs_data_t* privateSettings =
                        obs_source_get_private_settings(
                            g_captureSource
                        );

                    if (privateSettings) {
                        const char* portalAppId =
                            obs_data_get_string(
                                privateSettings,
                                "__nse_linux_portal_app_id"
                            );

                        LinuxPortalWindowIdentity portalWindowIdentity;

                        const char* restoreToken =
                            obs_data_get_string(
                                privateSettings,
                                "__nse_linux_restore_token"
                            );

                        std::cerr
                            << "[Native Stream Engine] "
                            << "portal restore token from source="
                            << (
                                restoreToken && *restoreToken
                                    ? restoreToken
                                    : "<none>"
                            )
                            << "\n";

                        if (restoreToken && *restoreToken) {
                            portalWindowIdentity =
                                resolveLinuxPortalWindowIdentityFromRestoreToken(
                                    restoreToken
                                );

                            std::cerr
                                << "[Native Stream Engine] "
                                << "resolved portal restore identity"
                                << " appId="
                                << (
                                    portalWindowIdentity.appId.empty()
                                        ? "<none>"
                                        : portalWindowIdentity.appId
                                )
                                << " title="
                                << (
                                    portalWindowIdentity.title.empty()
                                        ? "<none>"
                                        : portalWindowIdentity.title
                                )
                                << "\n";
                        }

                        LinuxPortalWindowMatch resolvedWindowMatch;

                        if (portalWindowIdentity.valid()) {
                            std::cerr
                                << "[Native Stream Engine] "
                                << "portal identity available for audio resolution"
                                << " appId="
                                << portalWindowIdentity.appId
                                << " title="
                                << portalWindowIdentity.title
                                << "\n";

                            resolvedWindowMatch =
                                resolveLinuxPortalWindowMatch(
                                    portalWindowIdentity
                                );

                            {
                                std::lock_guard<std::mutex> lock(
                                    linuxPortalWindowMatchMutex_
                                );

                                linuxPortalWindowMatch_ =
                                    resolvedWindowMatch;
                            }

                            if (resolvedWindowMatch.valid()) {
                                std::cerr
                                    << "[Native Stream Engine] "
                                    << "active portal window stored"
                                    << " uuid="
                                    << resolvedWindowMatch.uuid
                                    << " pid="
                                    << resolvedWindowMatch.pid
                                    << "\n";
                            }
                        }

                        std::cerr
                            << "[Native Stream Engine] "
                            << "portal app identity from source="
                            << (
                                portalAppId && *portalAppId
                                    ? portalAppId
                                    : "<none>"
                            )
                            << "\n";

                        const std::string portalExecutableTarget =
                            resolveLinuxExecutableTargetFromPid(
                                resolvedWindowMatch.pid
                            );

                        if (!portalExecutableTarget.empty()) {
                            std::cerr
                                << "[Native Stream Engine] "
                                << "portal executable target="
                                << portalExecutableTarget
                                << "\n";
                        }
                        
                        const std::vector<LinuxAudioApplication> audioApps =
                            listLinuxAudioApplications();

                        const uint32_t portalWindowPid =
                            resolvedWindowMatch.pid;

                        const LinuxAudioApplication* portalPidAudioApp = nullptr;

                        if (portalWindowPid != 0) {
                            for (const auto& audioApp : audioApps) {
                                if (audioApp.pid != portalWindowPid) {
                                    continue;
                                }

                                if (portalPidAudioApp) {
                                    std::cerr
                                        << "[Native Stream Engine] "
                                        << "multiple audio applications matched portal window pid="
                                        << portalWindowPid
                                        << "; refusing ambiguous PID audio selection"
                                        << "\n";

                                    portalPidAudioApp = nullptr;
                                    break;
                                }

                                portalPidAudioApp = &audioApp;
                            }
                        }

                        if (portalPidAudioApp) {
                            std::cerr
                                << "[Native Stream Engine] "
                                << "portal PID audio application match"
                                << " pid=" << portalPidAudioApp->pid
                                << " name=" << portalPidAudioApp->name
                                << " binary=" << portalPidAudioApp->binary
                                << " nodeName=" << portalPidAudioApp->nodeName
                                << "\n";
                        }

                        const std::string portalResolvedAudioTarget =
                            resolveLinuxPortalAudioTarget(
                                portalWindowIdentity,
                                audioApps
                            );

                        std::cerr
                            << "[Native Stream Engine] "
                            << "portal resolved audio target="
                            << (
                                portalResolvedAudioTarget.empty()
                                    ? "<none>"
                                    : portalResolvedAudioTarget
                            )
                            << "\n";

                        const LinuxAudioApplication* uniqueGameAudioApp =
                            findUniqueLinuxGameAudioApplication(
                                audioApps
                            );

                        const LinuxAudioApplication* uniqueWineAudioApp =
                            findUniqueLinuxWineAudioApplication(
                                audioApps
                            );
                        
                        if (uniqueWineAudioApp) {
                            std::cerr
                                << "[Native Stream Engine] "
                                << "unique Wine audio candidate"
                                << " name=" << uniqueWineAudioApp->name
                                << " binary=" << uniqueWineAudioApp->binary
                                << " pid=" << uniqueWineAudioApp->pid
                                << "\n";
                        } else {
                            std::cerr
                                << "[Native Stream Engine] "
                                << "unique Wine audio candidate not found"
                                << "\n";
                        }

                        if (uniqueGameAudioApp) {
                            std::cerr
                                << "[Native Stream Engine] "
                                << "unique game audio candidate"
                                << " name=" << uniqueGameAudioApp->name
                                << " binary=" << uniqueGameAudioApp->binary
                                << " pid=" << uniqueGameAudioApp->pid
                                << "\n";
                        } else {
                            std::cerr
                                << "[Native Stream Engine] "
                                << "unique game audio candidate not found"
                                << "\n";
                        }

                       if (
                            portalPidAudioApp &&
                            !portalPidAudioApp->name.empty()
                        ) {
                            g_linuxSelectedAudioTarget =
                                portalPidAudioApp->name;

                            std::cerr
                                << "[Native Stream Engine] "
                                << "selected audio target from portal window PID="
                                << g_linuxSelectedAudioTarget
                                << "\n";
                        } else if (!portalExecutableTarget.empty()) {
                            g_linuxSelectedAudioTarget =
                                portalExecutableTarget;

                            std::cerr
                                << "[Native Stream Engine] "
                                << "selected audio target from portal window executable fallback="
                                << g_linuxSelectedAudioTarget
                                << "\n";
                        } else if (!portalResolvedAudioTarget.empty()) {
                            g_linuxSelectedAudioTarget =
                                portalResolvedAudioTarget;

                            std::cerr
                                << "[Native Stream Engine] "
                                << "selected audio target from portal restore identity="
                                << g_linuxSelectedAudioTarget
                                << "\n";
                        } else if (portalAppId && *portalAppId) {
                            const LinuxApplicationIdentity identity =
                                resolveLinuxApplicationIdentity(
                                    portalAppId
                                );

                            std::cerr
                                << "[Native Stream Engine] "
                                << "resolved portal application"
                                << " portalId=" << identity.portalId
                                << " desktopId=" << identity.desktopId
                                << " name=" << identity.displayName
                                << " startupWMClass=" << identity.startupWMClass
                                << " exec=" << identity.execBasename
                                << " aliases=[";

                            for (
                                size_t i = 0;
                                i < identity.aliases.size();
                                ++i
                            ) {
                                if (i > 0) {
                                    std::cerr << ", ";
                                }

                                std::cerr
                                    << identity.aliases[i];
                            }

                            std::cerr << "]\n";

                            const LinuxAudioApplication* exactAudioMatch =
                                findExactLinuxAudioApplicationMatch(
                                    identity,
                                    audioApps
                                );

                            if (exactAudioMatch) {
                                std::cerr
                                    << "[Native Stream Engine] "
                                    << "exact audio application match"
                                    << " name=" << exactAudioMatch->name
                                    << " binary=" << exactAudioMatch->binary
                                    << " pid=" << exactAudioMatch->pid
                                    << " nodeName=" << exactAudioMatch->nodeName
                                    << "\n";

                                g_linuxSelectedAudioTarget =
                                    exactAudioMatch->binary;

                                std::cerr
                                    << "[Native Stream Engine] "
                                    << "selected audio target="
                                    << g_linuxSelectedAudioTarget
                                    << "\n";
                            } else {
                                std::cerr
                                    << "[Native Stream Engine] "
                                    << "exact audio application match not found"
                                    << "\n";

                                if (!identity.displayName.empty()) {
                                    g_linuxSelectedAudioTarget =
                                        identity.displayName;

                                    std::cerr
                                        << "[Native Stream Engine] "
                                        << "selected audio target from portal identity="
                                        << g_linuxSelectedAudioTarget
                                        << "\n";
                                }
                            }
                        } else if (
                            !portalWindowIdentity.valid() &&
                            uniqueGameAudioApp
                        ) {
                            g_linuxSelectedAudioTarget =
                                uniqueGameAudioApp->binary;

                            std::cerr
                                << "[Native Stream Engine] "
                                << "selected audio target from unique game fallback="
                                << g_linuxSelectedAudioTarget
                                << "\n";
                        } else if (
                            !portalWindowIdentity.valid() &&
                            uniqueWineAudioApp
                        ) {
                            g_linuxSelectedAudioTarget =
                                uniqueWineAudioApp->name;

                            std::cerr
                                << "[Native Stream Engine] "
                                << "selected audio target from unique Wine fallback="
                                << g_linuxSelectedAudioTarget
                                << "\n";
                        }

                        obs_data_release(
                            privateSettings
                        );
                    }

                    std::cerr
                        << "[Native Stream Engine] "
                        << "PipeWire capture source ready "
                        << width
                        << "x"
                        << height
                        << "\n";

                    return true;
                }
            }

            const auto now =
                std::chrono::steady_clock::now();

            if (
                now - startedAt >= timeout
            ) {
                std::cerr
                    << "[Native Stream Engine] "
                    << "PipeWire capture frame wait timed out"
                    << " timeoutMs="
                    << timeoutMs
                    << "\n";

                return false;
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(50)
            );
        }

    #endif
}

#if defined(__linux__)
LinuxPortalWindowMatch ObsEngine::activePortalWindowMatch() const
{
    std::lock_guard<std::mutex> lock(
        linuxPortalWindowMatchMutex_
    );

    return linuxPortalWindowMatch_;
}
#endif

bool ObsEngine::isOutputStateUnstable() const
{
    return g_obsOutputStateUnstable.load(
        std::memory_order_acquire
    );
}

bool ObsEngine::copyCaptureFrameBgra(
    std::vector<uint8_t>& pixels,
    uint32_t& width,
    uint32_t& height
) const
{
    #if defined(_WIN32)
    pixels.clear();
    width = 0;
    height = 0;

    if (!isWgcFrameReady()) {
        std::cerr
            << "[Native Stream Engine] "
            << "capture frame copy rejected"
            << " reason=frame_not_ready"
            << "\n";

        return false;
    }

    if (
        !copyWgcFrameBgra(
            pixels,
            width,
            height
        )
    ) {
        std::cerr
            << "[Native Stream Engine] "
            << "capture frame copy failed"
            << "\n";

        return false;
    }

    if (
        pixels.empty() ||
        width == 0 ||
        height == 0
    ) {
        pixels.clear();
        width = 0;
        height = 0;

        std::cerr
            << "[Native Stream Engine] "
            << "capture frame copy failed"
            << " reason=invalid_frame_data"
            << "\n";

        return false;
    }

    const size_t expectedSize =
        static_cast<size_t>(width) *
        static_cast<size_t>(height) *
        4;

    if (pixels.size() != expectedSize) {
        pixels.clear();
        width = 0;
        height = 0;

        std::cerr
            << "[Native Stream Engine] "
            << "capture frame copy failed"
            << " reason=unexpected_buffer_size"
            << "\n";

        return false;
    }

    std::cerr
        << "[Native Stream Engine] "
        << "capture frame copied "
        << width
        << "x"
        << height
        << " bytes="
        << pixels.size()
        << "\n";

    return true;
    #else

        return false;

    #endif
}

static size_t startCodeLength(const uint8_t* data, size_t size, size_t pos)
{
    if (pos + 3 <= size &&
        data[pos] == 0x00 &&
        data[pos + 1] == 0x00 &&
        data[pos + 2] == 0x01) {
        return 3;
    }

    if (pos + 4 <= size &&
        data[pos] == 0x00 &&
        data[pos + 1] == 0x00 &&
        data[pos + 2] == 0x00 &&
        data[pos + 3] == 0x01) {
        return 4;
    }

    return 0;
}

static bool sendVideoH264Nal(
    const uint8_t* data,
    size_t size,
    uint32_t timestamp,
    bool marker
)
{
    return g_realtimeRtpSender.sendH264Nal(
        data,
        size,
        timestamp,
        marker
    );
}

static void stopActiveRtpSenders()
{
    g_realtimeRtpSender.stop();
    g_realtimeRtpAudioSender.stop();
}

static void sendAnnexBNalsAsRtp(
    const uint8_t* data,
    size_t size,
    uint32_t timestamp
)
{
    if (!data || size == 0) {
        return;
    }

    struct NalView {
        const uint8_t* data = nullptr;
        size_t size = 0;
    };

    std::vector<NalView> nals;
    nals.reserve(16);

    size_t start = findStartCode(data, size, 0);

    while (start < size) {
        size_t prefixLen = startCodeLength(data, size, start);
        if (prefixLen == 0) break;

        size_t nalStart = start + prefixLen;
        size_t nextStart = findStartCode(data, size, nalStart);

        if (nextStart > nalStart) {
            const uint8_t* nalData = data + nalStart;
            size_t nalSize = nextStart - nalStart;

            while (nalSize > 0 && nalData[nalSize - 1] == 0x00) {
                nalSize--;
            }

            if (nalSize > 0) {
                nals.push_back({ nalData, nalSize });
            }
        }

        start = nextStart;
    }

    bool hasSps = false;
    bool hasPps = false;
    bool hasIdr = false;

    for (const auto& nal : nals) {
        uint8_t type = nal.data[0] & 0x1f;

        if (type == 7) {
            hasSps = true;
            g_cachedSps.assign(nal.data, nal.data + nal.size);
        } else if (type == 8) {
            hasPps = true;
            g_cachedPps.assign(nal.data, nal.data + nal.size);
        } else if (type == 5) {
            hasIdr = true;
        }
    }

    if (hasIdr && (!hasSps || !hasPps)) {

        if (!g_cachedSps.empty()) {
            sendVideoH264Nal(
                g_cachedSps.data(),
                g_cachedSps.size(),
                timestamp,
                false
            );
        }

        if (!g_cachedPps.empty()) {
            sendVideoH264Nal(
                g_cachedPps.data(),
                g_cachedPps.size(),
                timestamp,
                false
            );
        }
    }

    for (size_t i = 0; i < nals.size(); i++) {
        const bool marker = i == nals.size() - 1;

        sendVideoH264Nal(
            nals[i].data,
            nals[i].size,
            timestamp,
            marker
        );
    }
}


bool ObsEngine::createWgcScene(
    WgcTargetType targetType,
    uintptr_t hwnd,
    int monitorIndex,
    int delayMs,
		bool debugFrames
)
{
    #if defined(_WIN32)
    setWgcConfig(targetType, hwnd, monitorIndex, delayMs, debugFrames);

    obs_scene_t* scene =
        obs_scene_create_private(
            "Native WGC Scene"
        );
    if (!scene) {
        std::cerr << "Failed to create WGC scene\n";
        return false;
    }

    obs_data_t* settings = obs_data_create();

    obs_source_t* wgcSource =
        obs_source_create_private(
            "wgc_capture",
            "Native WGC Capture",
            settings
        );

    obs_data_release(settings);

    if (!wgcSource) {
        std::cerr << "Failed to create Native WGC source\n";
        obs_scene_release(scene);
        return false;
    }

    obs_sceneitem_t* item = obs_scene_add(scene, wgcSource);

    if (!item) {
        std::cerr << "Failed to add WGC source to scene\n";
        obs_source_release(wgcSource);
        obs_scene_release(scene);
        return false;
    }

    obs_video_info videoInfo{};

    if (!obs_get_video_info(&videoInfo)) {
        std::cerr
            << "Failed to read OBS video info for WGC scene\n";

        obs_source_release(wgcSource);
        obs_scene_release(scene);
        return false;
    }

    vec2 itemPosition{};
    vec2_set(
        &itemPosition,
        0.0f,
        0.0f
    );

    vec2 itemBounds{};
    vec2_set(
        &itemBounds,
        static_cast<float>(videoInfo.base_width),
        static_cast<float>(videoInfo.base_height)
    );

    obs_sceneitem_set_alignment(
        item,
        OBS_ALIGN_LEFT |
        OBS_ALIGN_TOP
    );

    obs_sceneitem_set_pos(
        item,
        &itemPosition
    );

    obs_sceneitem_set_bounds_type(
        item,
        OBS_BOUNDS_SCALE_INNER
    );

    obs_sceneitem_set_bounds_alignment(
        item,
        OBS_ALIGN_CENTER
    );

    obs_sceneitem_set_bounds(
        item,
        &itemBounds
    );

    std::cerr
        << "[Native Stream Engine] WGC scene item fitted to canvas "
        << videoInfo.base_width
        << "x"
        << videoInfo.base_height
        << "\n";

    captureCleared_ = false;

    g_scene = scene;
    g_captureSceneItem = item;
    g_captureSource = wgcSource;

    auto attachSourcePromise =
        std::make_shared<std::promise<void>>();

    std::future<void> attachSourceFuture =
        attachSourcePromise->get_future();

    obs_source_t* sceneSourceForAttach =
        obs_scene_get_source(scene);

    std::thread attachSourceWorker(
        [attachSourcePromise, sceneSourceForAttach]() {
            obs_set_output_source(
                0,
                sceneSourceForAttach
            );

            attachSourcePromise->set_value();
        }
    );

    constexpr auto kAttachSourceTimeout =
        std::chrono::milliseconds(750);

    const auto attachSourceStatus =
        attachSourceFuture.wait_for(
            kAttachSourceTimeout
        );

    if (attachSourceStatus == std::future_status::ready) {
        attachSourceWorker.join();
    } else {
        attachSourceWorker.detach();

        g_obsOutputStateUnstable.store(
            true,
            std::memory_order_release
        );

        std::cerr
            << "[Native Stream Engine] "
            << "createWgcScene: obs_set_output_source timed out"
            << " timeoutMs="
            << kAttachSourceTimeout.count()
            << " - treating this capture attempt as failed so the"
            << " caller can report an error and move on to the next"
            << " window/monitor\n";

        return false;
    }

    return true;
    #else

        return false;

    #endif
}

#if defined(__linux__)

bool ObsEngine::createPipeWireScene(
    CaptureType captureType,
    int monitorIndex,
    bool debugFrames
)
{
    (void)monitorIndex;
    (void)debugFrames;
    g_linuxSelectedAudioTarget.clear();

    const char* sourceId = nullptr;
    const char* sourceName = nullptr;

    switch (captureType) {
        case CaptureType::Monitor:
            sourceId =
                "pipewire-desktop-capture-source";
            sourceName =
                "Native PipeWire Monitor Capture";
            break;

        case CaptureType::Window:
        case CaptureType::Game:
        case CaptureType::Wgc:
            sourceId =
                "pipewire-window-capture-source";
            sourceName =
                "Native PipeWire Window Capture";
            break;

        default:
            std::cerr
                << "[Native Stream Engine] "
                << "unsupported Linux capture type\n";

            return false;
    }

    std::cerr
        << "[Native Stream Engine] "
        << "creating PipeWire capture"
        << " sourceId="
        << sourceId
        << "\n";

    obs_scene_t* scene =
        obs_scene_create_private(
            "Native PipeWire Scene"
        );

    if (!scene) {
        std::cerr
            << "[Native Stream Engine] "
            << "failed to create PipeWire scene\n";

        return false;
    }

    obs_data_t* settings =
        obs_data_create();

    obs_data_set_bool(
        settings,
        "ShowCursor",
        true
    );

    obs_source_t* pipeWireSource =
        obs_source_create_private(
            sourceId,
            sourceName,
            settings
        );

    obs_data_release(settings);

    if (!pipeWireSource) {
        std::cerr
            << "[Native Stream Engine] "
            << "failed to create PipeWire source"
            << " sourceId="
            << sourceId
            << "\n";

        obs_scene_release(scene);

        return false;
    }

    obs_sceneitem_t* item =
        obs_scene_add(
            scene,
            pipeWireSource
        );

    if (!item) {
        std::cerr
            << "[Native Stream Engine] "
            << "failed to add PipeWire source to scene\n";

        obs_source_release(pipeWireSource);
        obs_scene_release(scene);

        return false;
    }

    obs_video_info videoInfo{};

    if (!obs_get_video_info(&videoInfo)) {
        std::cerr
            << "[Native Stream Engine] "
            << "failed to read OBS video info"
            << " for PipeWire scene\n";

        obs_sceneitem_remove(item);
        obs_source_release(pipeWireSource);
        obs_scene_release(scene);

        return false;
    }

    vec2 itemPosition{};
    vec2_set(
        &itemPosition,
        0.0f,
        0.0f
    );

    vec2 itemBounds{};
    vec2_set(
        &itemBounds,
        static_cast<float>(
            videoInfo.base_width
        ),
        static_cast<float>(
            videoInfo.base_height
        )
    );

    obs_sceneitem_set_alignment(
        item,
        OBS_ALIGN_LEFT |
        OBS_ALIGN_TOP
    );

    obs_sceneitem_set_pos(
        item,
        &itemPosition
    );

    obs_sceneitem_set_bounds_type(
        item,
        OBS_BOUNDS_SCALE_INNER
    );

    obs_sceneitem_set_bounds_alignment(
        item,
        OBS_ALIGN_CENTER
    );

    obs_sceneitem_set_bounds(
        item,
        &itemBounds
    );

    captureCleared_ = false;

    g_scene = scene;
    g_captureSceneItem = item;
    g_captureSource = pipeWireSource;

    obs_source_t* sceneSource =
        obs_scene_get_source(
            scene
        );

    obs_set_output_source(
        0,
        sceneSource
    );

    std::cerr
        << "[Native Stream Engine] "
        << "PipeWire scene created"
        << " canvas="
        << videoInfo.base_width
        << "x"
        << videoInfo.base_height
        << " sourceId="
        << sourceId
        << "\n";

    return true;
}

#endif

bool ObsEngine::createCaptureScene(
    CaptureType captureType,
    uintptr_t hwnd,
    int delayMs,
    bool debugFrames,
    int monitorIndex
)
{
    #if defined(_WIN32)
    if (captureType == CaptureType::Monitor) {
        return createWgcScene(
            WgcTargetType::Monitor,
            0,
            monitorIndex,
            delayMs,
			debugFrames
        );
    }

    if (captureType == CaptureType::Window ||
        captureType == CaptureType::Game ||
        captureType == CaptureType::Wgc) {
        if (hwnd == 0) {
            std::cerr << "--capture window/game/wgc requires --hwnd <value>\n";
            return false;
        }

        return createWgcScene(
            WgcTargetType::Window,
            hwnd,
            0,
            delayMs,
						debugFrames
        );
    }

    return false;

    #elif defined(__linux__)

        return createPipeWireScene(
            captureType,
            monitorIndex,
            debugFrames
        );

    #else

        return false;

    #endif

}

bool ObsEngine::configureVideo(
    const ObsVideoConfig& videoConfig
)
{
    obs_video_info ovi = {};

    #if defined(_WIN32)
    ovi.graphics_module = "libobs-d3d11";
    #else
    ovi.graphics_module = "libobs-opengl";
    #endif
    ovi.fps_num = videoConfig.fps;
    ovi.fps_den = 1;

    ovi.base_width =
        static_cast<uint32_t>(
            videoConfig.outputWidth
        );

    ovi.base_height =
        static_cast<uint32_t>(
            videoConfig.outputHeight
        );

    ovi.output_width =
        static_cast<uint32_t>(
            videoConfig.outputWidth
        );

    ovi.output_height =
        static_cast<uint32_t>(
            videoConfig.outputHeight
        );

    ovi.output_format = VIDEO_FORMAT_NV12;
    ovi.colorspace = VIDEO_CS_709;
    ovi.range = VIDEO_RANGE_PARTIAL;
    ovi.adapter = 0;
    ovi.gpu_conversion = true;

    if (videoConfig.scaleFilter == "lanczos") {
        ovi.scale_type = OBS_SCALE_LANCZOS;
    } else if (
        videoConfig.scaleFilter == "bilinear"
    ) {
        ovi.scale_type = OBS_SCALE_BILINEAR;
    } else if (
        videoConfig.scaleFilter == "area"
    ) {
        ovi.scale_type = OBS_SCALE_AREA;
    } else {
        ovi.scale_type = OBS_SCALE_BICUBIC;
    }

    const int videoResult =
        obs_reset_video(
            &ovi
        );

    if (
        videoResult !=
        OBS_VIDEO_SUCCESS
    ) {
        std::cerr
            << "[Native Stream Engine] "
            << "obs_reset_video failed: "
            << videoResult
            << "\n";

        return false;
    }

    std::cerr
        << "[Native Stream Engine] "
        << "video configured "
        << videoConfig.outputWidth
        << "x"
        << videoConfig.outputHeight
        << " @ "
        << videoConfig.fps
        << " FPS"
        << " scale="
        << videoConfig.scaleFilter
        << "\n";

    return true;
}

bool ObsEngine::initialize(
    const fs::path& runtimeDir,
    const ObsVideoConfig& videoConfig
)
{
    shutdownCalled_ = false;
    captureCleared_ = false;

    #if defined(_WIN32)
    resetWgcTargetClosed();
    #endif

    bitrateUpdateScheduler_.reset();

    #if defined(_WIN32)
    const auto binDir = runtimeDir / "bin" / "64bit";
    const auto pluginBinDir = runtimeDir / "obs-plugins" / "64bit";
    #else
    const auto binDir = runtimeDir / "bin";
    const auto pluginBinDir = runtimeDir / "obs-plugins";
    #endif
    const auto pluginDataDir = runtimeDir / "data" / "obs-plugins";
    const auto libobsDataDir = runtimeDir / "data" / "libobs";

    std::cerr << "[Native Stream Engine] runtime: " << runtimeDir.string() << "\n";

    #if defined(_WIN32)

    if (!fs::exists(binDir / "obs.dll")) {
        std::cerr << "obs.dll not found: " << (binDir / "obs.dll").string() << "\n";
        return false;
    }

    #else

    if (!fs::exists(runtimeDir / "lib" / "libobs.so")) {
        std::cerr
            << "libobs.so not found: "
            << (runtimeDir / "lib" / "libobs.so").string()
            << "\n";

        return false;
    }

    #endif

        #if defined(_WIN32)

		HRESULT coResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		if (FAILED(coResult) && coResult != RPC_E_CHANGED_MODE) {
				std::cerr << "CoInitializeEx failed: " << std::hex << coResult << "\n";
		}

		HRESULT roResult = RoInitialize(RO_INIT_MULTITHREADED);
		if (FAILED(roResult) && roResult != RPC_E_CHANGED_MODE) {
				std::cerr << "RoInitialize failed: " << std::hex << roResult << "\n";
		}

        #endif

    base_set_log_handler(
        nativeObsLogHandler,
        nullptr
    );

    if (!obs_startup("en-US", nullptr, nullptr)) {
        std::cerr << "obs_startup failed\n";
        return false;
    }

    std::string libobsDataPath = toUtf8Path(libobsDataDir);
    std::replace(libobsDataPath.begin(), libobsDataPath.end(), '\\', '/');

    if (!libobsDataPath.empty() && libobsDataPath.back() != '/') {
        libobsDataPath += "/";
    }

    std::cerr << "libobs data path: " << libobsDataPath << "\n";

    obs_add_data_path(libobsDataPath.c_str());

    char* defaultEffectPath = obs_find_data_file("default.effect");
    if (!defaultEffectPath) {
        std::cerr << "OBS cannot find default.effect via data path\n";
        obs_shutdown();
        return false;
    }

    std::cerr << "Found default.effect: " << defaultEffectPath << "\n";
    bfree(defaultEffectPath);

    #if !defined(_WIN32)

    if (!configureVideo(videoConfig)) {
        std::cerr
            << "[Native Stream Engine] "
            << "initial video configuration failed\n";

        obs_shutdown();
        return false;
    }

    #endif

    #if defined(_WIN32)

    const std::string moduleBinPattern =
            toUtf8Path(pluginBinDir) + "/%module%.dll";

    #else

        const std::string moduleBinPattern =
            toUtf8Path(pluginBinDir) + "/%module%.so";

    #endif

    const std::string moduleDataPattern =
        toUtf8Path(pluginDataDir) + "/%module%";

    obs_add_module_path(
        moduleBinPattern.c_str(),
        moduleDataPattern.c_str()
    );

    #if defined(_WIN32)
    const char* safeModules[] = {
        "win-capture.dll",
        "win-wasapi.dll",
        "obs-ffmpeg.dll",
        "obs-x264.dll",
        "obs-qsv11.dll",
        "obs-nvenc.dll",
    };
    #else
    const char* safeModules[] = {
        "linux-pipewire.so",
        "linux-pulseaudio.so",
        "linux-pipewire-audio.so",
        "obs-ffmpeg.so",
        "obs-x264.so",
        "obs-qsv11.so",
        "obs-nvenc.so"
    };
    #endif

    for (const char* moduleName : safeModules) {
        obs_module_t* module = nullptr;

        fs::path modulePath = pluginBinDir / moduleName;
				fs::path moduleDataPath = pluginDataDir / moduleName;
				moduleDataPath.replace_extension();
        std::string modulePathStr = toUtf8Path(modulePath);
				std::string moduleDataPathStr = toUtf8Path(moduleDataPath);
				std::replace(modulePathStr.begin(), modulePathStr.end(), '\\', '/');
    		std::replace(moduleDataPathStr.begin(), moduleDataPathStr.end(), '\\', '/');

				int code = obs_open_module(
						&module,
						modulePathStr.c_str(),
						moduleDataPathStr.c_str()
				);

        if (code != MODULE_SUCCESS) {
            std::cerr << "Skipping module: " << moduleName << " code=" << code << "\n";
            continue;
        }

        if (!obs_init_module(module)) {
            std::cerr << "Failed to init module: " << moduleName << "\n";
            continue;
        }

        std::cerr << "Loaded module: " << moduleName << "\n";
    }

    obs_post_load_modules();

    std::cerr
        << "[Native Stream Engine] registered encoders:\n";

    for (size_t index = 0;; ++index) {
        const char* encoderId = nullptr;

        if (!obs_enum_encoder_types(
                index,
                &encoderId
            )) {
            break;
        }

        if (encoderId) {
            std::cerr
                << "  encoder: "
                << encoderId
                << "\n";
        }
    }

    #if defined(_WIN32)
    registerWgcSource();
    #endif

    setNativeRtpOutputPacketHandler(
        handleRtpEncodedPacket
    );

    registerNativeRtpOutput();

    #if defined(_WIN32)

    if (!configureVideo(videoConfig)) {
        std::cerr
            << "[Native Stream Engine] "
            << "initial video configuration failed\n";

        obs_shutdown();
        return false;
    }

    #endif

    obs_audio_info ai = {};
    ai.samples_per_sec = 48000;
    ai.speakers = SPEAKERS_STEREO;

    if (!obs_reset_audio(&ai)) {
        std::cerr << "obs_reset_audio failed\n";
        obs_shutdown();
        return false;
    }


    std::cerr << "Audio initialized: 48000Hz stereo\n";
    std::cerr << "OBS initialized\n";
    

    return true;
}

static bool obsEncoderExists(const std::string& targetId)
{
    const char* id = nullptr;

    for (size_t i = 0; obs_enum_encoder_types(i, &id); i++) {
        if (id && targetId == id) {
            return true;
        }
    }

    return false;
}

struct VideoEncoderCandidate {
    std::string id;
    std::string family;
};

static void appendEncoderCandidateIfAvailable(
    std::vector<VideoEncoderCandidate>& candidates,
    const char* encoderId,
    const char* family
)
{
    if (!encoderId || !family) {
        return;
    }

    if (!obsEncoderExists(encoderId)) {
        return;
    }

    const bool alreadyAdded = std::any_of(
        candidates.begin(),
        candidates.end(),
        [encoderId](
            const VideoEncoderCandidate& candidate
        ) {
            return candidate.id == encoderId;
        }
    );

    if (alreadyAdded) {
        return;
    }

    candidates.push_back({
        encoderId,
        family
    });
}

static std::vector<VideoEncoderCandidate>
buildVideoEncoderCandidates(
    const std::string& requestedEncoder
)
{
    std::vector<VideoEncoderCandidate> candidates;

    const auto appendNvenc = [&]() {
        appendEncoderCandidateIfAvailable(
            candidates,
            "obs_nvenc_h264_tex",
            "nvenc"
        );

        appendEncoderCandidateIfAvailable(
            candidates,
            "obs_nvenc_h264_cuda",
            "nvenc"
        );

        appendEncoderCandidateIfAvailable(
            candidates,
            "obs_nvenc_h264",
            "nvenc"
        );
    };

    const auto appendQsv = [&]() {
        appendEncoderCandidateIfAvailable(
            candidates,
            "obs_qsv11_v2",
            "qsv"
        );

        appendEncoderCandidateIfAvailable(
            candidates,
            "obs_qsv11",
            "qsv"
        );
    };

    const auto appendAmf = [&]() {
        appendEncoderCandidateIfAvailable(
            candidates,
            "h264_texture_amf",
            "amd"
        );

        appendEncoderCandidateIfAvailable(
            candidates,
            "h264_fallback_amf",
            "amd"
        );
    };

    const auto appendX264 = [&]() {
        appendEncoderCandidateIfAvailable(
            candidates,
            "obs_x264",
            "x264"
        );
    };

    if (requestedEncoder == "nvenc") {
        appendNvenc();
        appendQsv();
        appendAmf();

        return candidates;
    }

    if (
        requestedEncoder == "amd" ||
        requestedEncoder == "amf"
    ) {
        appendAmf();
        appendNvenc();
        appendQsv();

        return candidates;
    }

    if (requestedEncoder == "qsv") {
        appendQsv();
        appendNvenc();
        appendAmf();

        return candidates;
    }

    if (requestedEncoder == "x264") {
        appendX264();

        return candidates;
    }

    appendNvenc();
    appendQsv();
    appendAmf();

    return candidates;
}

static std::string classifyEncoderFamily(const std::string& encoderId)
{
    if (encoderId.find("nvenc") != std::string::npos) return "nvenc";
    if (
        encoderId.find("amf") != std::string::npos ||
        encoderId.find("amd") != std::string::npos
    ) return "amd";
    if (encoderId.find("qsv") != std::string::npos) return "qsv";
    if (encoderId.find("x264") != std::string::npos) return "x264";

    return "unknown";
}

static obs_data_t* createRtpVideoEncoderSettings(
    const std::string& family,
    int bitrateKbps
)
{
    constexpr int STREAM_KEYINT_SEC = 2;
    constexpr const char* STREAM_H264_PROFILE =
        "baseline";

    obs_data_t* settings = obs_data_create();

    obs_data_set_string(
        settings,
        "rate_control",
        "CBR"
    );

    obs_data_set_int(
        settings,
        "bitrate",
        bitrateKbps
    );

    obs_data_set_int(
        settings,
        "keyint_sec",
        STREAM_KEYINT_SEC
    );

    obs_data_set_string(
        settings,
        "profile",
        STREAM_H264_PROFILE
    );

    obs_data_set_int(
        settings,
        "bf",
        0
    );

    if (family == "nvenc") {
        obs_data_set_string(
            settings,
            "preset",
            "p2"
        );

        obs_data_set_string(
            settings,
            "tune",
            "ll"
        );

        obs_data_set_bool(
            settings,
            "lookahead",
            false
        );

        obs_data_set_bool(
            settings,
            "psycho_aq",
            false
        );

        obs_data_set_bool(
            settings,
            "adaptive_quantization",
            false
        );

        obs_data_set_int(
            settings,
            "multipass",
            0
        );

        obs_data_set_int(
            settings,
            "b_ref_mode",
            0
        );

        obs_data_set_bool(
            settings,
            "repeat_headers",
            true
        );

        obs_data_set_int(
            settings,
            "max_bitrate",
            bitrateKbps
        );

        obs_data_set_bool(
            settings,
            "split_encode",
            false
        );
    } else if (family == "qsv") {
        obs_data_set_int(
            settings,
            "target_usage",
            7
        );

        obs_data_set_bool(
            settings,
            "repeat_headers",
            true
        );
    } else if (family == "amd") {
        obs_data_set_string(
            settings,
            "preset",
            "speed"
        );

        obs_data_set_bool(
            settings,
            "repeat_headers",
            true
        );

        obs_data_set_int(
            settings,
            "max_bitrate",
            bitrateKbps
        );
    } else {
        obs_data_set_string(
            settings,
            "preset",
            "veryfast"
        );

        obs_data_set_string(
            settings,
            "tune",
            "zerolatency"
        );

        obs_data_set_bool(
            settings,
            "repeat_headers",
            true
        );

        obs_data_set_bool(
            settings,
            "use_bufsize",
            true
        );

        obs_data_set_int(
            settings,
            "buffer_size",
            bitrateKbps
        );

    }

    return settings;
}

bool ObsEngine::createDesktopAudioSource()
{
    std::cerr
        << "\nCreating desktop audio source...\n";

    obs_data_t* settings =
        obs_data_create();

    obs_data_set_string(
        settings,
        "device_id",
        "default"
    );

#if defined(_WIN32)

    constexpr const char* sourceId =
        "wasapi_output_capture";

    constexpr const char* sourceName =
        "Native Desktop Audio";

#elif defined(__linux__)

    constexpr const char* sourceId =
        "pulse_output_capture";

    constexpr const char* sourceName =
        "Native Desktop Audio";

#else

    obs_data_release(settings);

    std::cerr
        << "Desktop audio capture is not supported "
        << "on this platform\n";

    return false;

#endif

    g_audioSource = obs_source_create(
        sourceId,
        sourceName,
        settings,
        nullptr
    );

    obs_data_release(settings);

    if (!g_audioSource) {
        std::cerr
            << "Failed to create desktop audio source"
            << " sourceId="
            << sourceId
            << "\n";

        return false;
    }

    obs_source_set_audio_mixers(
        g_audioSource,
        1
    );

    obs_set_output_source(
        1,
        g_audioSource
    );

    std::cerr
        << "Desktop audio source created"
        << " sourceId="
        << sourceId
        << " device=default"
        << "\n";

    return true;
}

bool ObsEngine::createProcessAudioSource(
    uintptr_t hwnd,
    const std::string& targetName
)
{
#if defined(_WIN32)

    (void)targetName;

    std::cerr << "\nCreating process audio source...\n";

    WindowInfo info;
    if (!findWindowByHwnd(hwnd, info)) {
        std::cerr
            << "Failed to find window for process audio hwnd: "
            << hwnd << "\n";
        return false;
    }

    std::string obsWindow = buildObsWindowString(info);

    std::cerr << "Selected process audio window:\n";
    std::cerr << "  hwnd=" << info.hwnd << "\n";
    std::cerr << "  pid=" << info.pid << "\n";
    std::cerr << "  exe=" << info.exeName << "\n";
    std::cerr << "  class=" << info.className << "\n";
    std::cerr << "  title=" << info.title << "\n";
    std::cerr << "  obs window string=" << obsWindow << "\n";

    obs_data_t* settings = obs_data_create();

    obs_data_set_string(
        settings,
        "window",
        obsWindow.c_str()
    );

    obs_data_set_int(
        settings,
        "priority",
        1
    );

    g_audioSource = obs_source_create(
        "wasapi_process_output_capture",
        "Native Process Audio",
        settings,
        nullptr
    );

    obs_data_release(settings);

    if (!g_audioSource) {
        std::cerr
            << "Failed to create "
            << "wasapi_process_output_capture source\n";
        return false;
    }

#elif defined(__linux__)

    (void)hwnd;

    std::cerr
        << "\nCreating PipeWire application audio source...\n";

    const std::string effectiveTargetName =
        !targetName.empty()
            ? targetName
            : g_linuxSelectedAudioTarget;

    if (effectiveTargetName.empty()) {
        std::cerr
            << "Linux application audio requires targetName\n";
        return false;
    }

    obs_data_t* settings = obs_data_create();

    // 0 = single application capture
    obs_data_set_int(
        settings,
        "CaptureMode",
        0
    );

    // 0 = prefer/match application process binary
    obs_data_set_int(
        settings,
        "MatchPriorty",
        0
    );

    obs_data_set_bool(
        settings,
        "ExceptApp",
        false
    );

    obs_data_set_string(
        settings,
        "TargetName",
        effectiveTargetName.c_str()
    );

    g_audioSource = obs_source_create(
        "pipewire_audio_application_capture",
        "Native Application Audio",
        settings,
        nullptr
    );

    obs_data_release(settings);

    if (!g_audioSource) {
        std::cerr
            << "Failed to create "
            << "pipewire_audio_application_capture"
            << " target=" << effectiveTargetName
            << "\n";
        return false;
    }

    std::cerr
        << "PipeWire application audio source created"
        << " target=" << effectiveTargetName
        << "\n";

#else

    (void)hwnd;
    (void)targetName;

    std::cerr
        << "Process/application audio capture "
        << "is not supported on this platform\n";

    return false;

#endif

    obs_source_set_audio_mixers(
        g_audioSource,
        1
    );

    obs_set_output_source(
        1,
        g_audioSource
    );

    return true;
}

static void handleRtpEncodedPacket(
    encoder_packet* packet
)
{
    if (!packet) {
        return;
    }

    if (
        !packet->data ||
        packet->size == 0 ||
        packet->timebase_den == 0
    ) {
        return;
    }

    if (packet->type == OBS_ENCODER_AUDIO) {
        if (!g_firstAudioPtsSet) {
            g_firstAudioPts = packet->pts;
            g_firstAudioPtsSet = true;
        }

        const int64_t audioPtsDelta =
            packet->pts - g_firstAudioPts;

        const uint32_t audioRtpTimestamp =
            static_cast<uint32_t>(
                (
                    audioPtsDelta *
                    48000LL *
                    packet->timebase_num
                ) /
                packet->timebase_den
            );

        g_realtimeRtpAudioSender.sendEncodedPayload(
            packet->data,
            packet->size,
            audioRtpTimestamp,
            true
        );

        return;
    }

    if (packet->type != OBS_ENCODER_VIDEO) {
        return;
    }

    g_rtpPacketCount++;

    if (
        STREAM_DEBUG_KEYFRAME_REQUESTS &&
        packet->keyframe
    ) {
        std::cerr
            << "[Realtime RTP] encoded video keyframe"
            << " packetCount=" << g_rtpPacketCount
            << " size=" << packet->size
            << " pts=" << packet->pts
            << " dts=" << packet->dts
            << "\n";
    }

    if (!g_firstVideoPtsSet) {
        g_firstVideoPts = packet->pts;
        g_firstVideoPtsSet = true;
    }

    const int64_t ptsDelta =
        packet->pts - g_firstVideoPts;

    const uint32_t rtpTimestamp =
        static_cast<uint32_t>(
            (
                ptsDelta *
                90000LL *
                packet->timebase_num
            ) /
                packet->timebase_den
        );

    static auto g_firstVideoWallClock = std::chrono::steady_clock::now();
    static bool g_firstVideoWallClockSet = false;
    static uint64_t g_driftLogCounter = 0;

    if (!g_firstVideoWallClockSet) {
        g_firstVideoWallClock = std::chrono::steady_clock::now();
        g_firstVideoWallClockSet = true;
    }

    g_driftLogCounter++;

    if (g_driftLogCounter % 600 == 0) {
        const double ptsElapsedMs =
            static_cast<double>(ptsDelta) *
            1000.0 *
            packet->timebase_num /
            packet->timebase_den;

        const auto wallElapsed =
            std::chrono::steady_clock::now() - g_firstVideoWallClock;

        const double wallElapsedMs =
            std::chrono::duration<double, std::milli>(wallElapsed).count();

        std::cerr
            << "[Realtime RTP] drift-check"
            << " frameNum=" << g_driftLogCounter
            << " ptsElapsedMs=" << ptsElapsedMs
            << " wallElapsedMs=" << wallElapsedMs
            << " driftMs=" << (wallElapsedMs - ptsElapsedMs)
            << "\n";
    }

    sendAnnexBNalsAsRtp(
        packet->data,
        packet->size,
        rtpTimestamp
    );
}

bool ObsEngine::startRtpStreaming(
    const std::string& rtpIp,
    uint16_t rtpPort,
    uint8_t payloadType,
    uint32_t ssrc,
    int bitrate,
    const std::string& encoder,
    const std::string& audioRtpIp,
    uint16_t audioRtpPort,
    uint8_t audioPayloadType,
    uint32_t audioSsrc,
    uint32_t rtxSsrc,
    uint8_t rtxPayloadType
)
{
    if (g_rtpStreaming) {
        std::cerr << "[Realtime RTP] start ignored, RTP already started\n";
        return false;
    }

    const bool audioRtpEnabled =
        !audioRtpIp.empty() &&
        audioRtpPort > 0 &&
        audioSsrc > 0;

    const auto encoderCandidates =
        buildVideoEncoderCandidates(encoder);

    if (encoderCandidates.empty()) {
        std::cerr
            << "[Realtime RTP] no registered H264 encoder found"
            << " requestedEncoder=" << encoder
            << "\n";

        return false;
    }

    std::cerr
        << "[Realtime RTP] encoder candidates"
        << " requestedEncoder="
        << encoder
        << " count="
        << encoderCandidates.size()
        << "\n";

    for (
        const auto& candidate :
        encoderCandidates
    ) {
        std::cerr
            << "[Realtime RTP] encoder candidate"
            << " encoderId="
            << candidate.id
            << " family="
            << candidate.family
            << "\n";
    }


    std::cerr
        << "[Realtime RTP] start requested "
        << rtpIp << ":"
        << rtpPort
        << " pt="
        << static_cast<int>(payloadType)
        << " ssrc=" << ssrc
        << " rtxSsrc=" << rtxSsrc
        << " rtxPt=" << static_cast<int>(rtxPayloadType)
        << " bitrate=" << bitrate
        << " requestedEncoder=" << encoder
        << "\n";
    
    g_rtpPacketCount = 0;
    g_firstVideoPtsSet = false;
    g_cachedSps.clear();
    g_cachedPps.clear();

    g_firstAudioPtsSet = false;

    g_realtimeRtpSender.setLabel("video");

    g_realtimeRtpSender.setInitialBitrate(
        static_cast<uint32_t>(bitrate * 1000)
    );

    {
        std::lock_guard<std::mutex> lock(
            bitratePolicyMutex_
        );

        bitrateUpdateScheduler_.reset();
        policyCeilingBps_ = 0;
        localDesiredBitrateBps_ = 0;
    }

    if (
        !g_realtimeRtpSender.start(
            rtpIp,
            rtpPort,
            payloadType,
            ssrc,
            rtxSsrc,
            rtxPayloadType
        )
    ) {
        return false;
    }

    if (audioRtpEnabled) {
        g_realtimeRtpAudioSender.setLabel("audio");

        if (
            !g_realtimeRtpAudioSender.start(
                audioRtpIp,
                audioRtpPort,
                audioPayloadType,
                audioSsrc
            )
        ) {
            g_realtimeRtpSender.stop();

            return false;
        }

        std::cerr
            << "[Native RTP Audio] sender started "
            << audioRtpIp << ":"
            << audioRtpPort
            << " pt="
            << static_cast<int>(audioPayloadType)
            << " ssrc="
            << audioSsrc
            << "\n";
    }

    bool nativeOutputStarted = false;

    for (const auto& candidate : encoderCandidates) {
        std::cerr
            << "[Realtime RTP] trying video encoder"
            << " encoderId=" << candidate.id
            << " family=" << candidate.family
            << "\n";

        obs_data_t* videoSettings =
            createRtpVideoEncoderSettings(
                candidate.family,
                bitrate
            );

        const std::string encoderName =
            "RTP Encoder " +
            candidate.id;

        g_rtpVideoEncoder =
            obs_video_encoder_create(
                candidate.id.c_str(),
                encoderName.c_str(),
                videoSettings,
                nullptr
            );

        obs_data_release(videoSettings);

        if (!g_rtpVideoEncoder) {
            std::cerr
                << "[Realtime RTP] video encoder creation failed"
                << " encoderId=" << candidate.id
                << " family=" << candidate.family
                << "\n";

            continue;
        }

        obs_encoder_set_video(
            g_rtpVideoEncoder,
            obs_get_video()
        );

        const char* nativeOutputId =
            audioRtpEnabled
                ? "native_rtp_av_output"
                : "native_rtp_video_output";

        g_rtpOutput = obs_output_create(
            nativeOutputId,
            "Native RTP Output",
            nullptr,
            nullptr
        );

        std::cerr
            << "[Realtime RTP] creating native output"
            << " outputId="
            << nativeOutputId
            << " audio="
            << (audioRtpEnabled ? "yes" : "no")
            << "\n";

        if (!g_rtpOutput) {
            std::cerr
                << "[Realtime RTP] packet tap output creation failed"
                << " encoderId=" << candidate.id
                << " family=" << candidate.family
                << "\n";

            releaseRtpStreamingResources();

            continue;
        }

        if (audioRtpEnabled) {
            obs_data_t* audioSettings =
                obs_data_create();

            obs_data_set_int(
                audioSettings,
                "bitrate",
                160
            );

            g_rtpAudioEncoder =
                obs_audio_encoder_create(
                    "ffmpeg_opus",
                    "RTP Opus Encoder",
                    audioSettings,
                    0,
                    nullptr
                );

            obs_data_release(audioSettings);

            if (!g_rtpAudioEncoder) {
                std::cerr
                    << "[Realtime RTP] "
                    << "audio encoder creation failed"
                    << " encoderId=" << candidate.id
                    << " family=" << candidate.family
                    << "\n";

                releaseRtpStreamingResources();
                continue;
            }

            obs_encoder_set_audio(
                g_rtpAudioEncoder,
                obs_get_audio()
            );
        }

        obs_output_set_video_encoder(
            g_rtpOutput,
            g_rtpVideoEncoder
        );

        if (audioRtpEnabled) {
            obs_output_set_audio_encoder(
                g_rtpOutput,
                g_rtpAudioEncoder,
                0
            );
        }

        if (!obs_output_start(g_rtpOutput)) {
            const char* outputError =
                obs_output_get_last_error(
                    g_rtpOutput
                );

            std::cerr
                << "[Realtime RTP] video encoder runtime start failed"
                << " encoderId=" << candidate.id
                << " family=" << candidate.family
                << " outputError="
                << (
                    outputError &&
                    outputError[0] != '\0'
                        ? outputError
                        : "unknown"
                )
                << "\n";

            if (
                obs_output_active(
                    g_rtpOutput
                )
            ) {
                obs_output_stop(
                    g_rtpOutput
                );
            }

            releaseRtpStreamingResources();

            continue;
        }

        nativeOutputStarted = true;

        std::cerr
            << "[Realtime RTP] video encoder selected"
            << " requestedEncoder=" << encoder
            << " selectedEncoder=" << candidate.family
            << " encoderId=" << candidate.id
            << "\n";

        std::cerr
            << "[Realtime RTP] native output started"
            << " encoderId=" << candidate.id
            << " family=" << candidate.family
            << "\n";

        break;
    }

    if (!nativeOutputStarted) {
        std::cerr
            << "[Realtime RTP] all video encoder runtime attempts failed"
            << " requestedEncoder=" << encoder
            << " candidateCount=" << encoderCandidates.size()
            << "\n";

        stopActiveRtpSenders();

        return false;
    }

    const uint32_t startupConfiguredBitrateBps =
        readRtpVideoEncoderConfiguredBitrate();

    if (startupConfiguredBitrateBps > 0) {
        {
            std::lock_guard<std::mutex> lock(
                bitratePolicyMutex_
            );

            bitrateUpdateScheduler_.markApplied(
                startupConfiguredBitrateBps
            );

            policyCeilingBps_ =
                startupConfiguredBitrateBps;

            localDesiredBitrateBps_ =
                startupConfiguredBitrateBps;
        }

        g_realtimeRtpSender.setAppliedBitrate(
            startupConfiguredBitrateBps
        );

        std::cerr
            << "[Realtime RTP] startup bitrate authority"
            << " requested="
            << static_cast<uint32_t>(bitrate * 1000)
            << " configured="
            << startupConfiguredBitrateBps
            << "\n";
    } else {
        std::cerr
            << "[Realtime RTP] startup configured bitrate readback failed"
            << " requested="
            << static_cast<uint32_t>(bitrate * 1000)
            << "\n";
    }

    g_lastPliCount = 0;
    g_lastFirCount = 0;
    g_lastNackPacketCount = 0;
    g_lastKeyframeRequestAt =
        std::chrono::steady_clock::time_point{};

    g_rtpStreaming = true;
    
    return true;
}

void ObsEngine::stopRtpStreaming()
{
    if (!g_rtpStreaming) {
        return;
    }

    g_rtpStreaming = false;

    {
        std::lock_guard<std::mutex> lock(
            bitratePolicyMutex_
        );

        policyCeilingBps_ = 0;
        localDesiredBitrateBps_ = 0;

        bitrateUpdateScheduler_.reset();
    }

    std::cerr
        << "[Realtime RTP] stop requested\n";

    if (g_rtpOutput) {
        if (obs_output_active(g_rtpOutput)) {
            obs_output_stop(g_rtpOutput);
        }
    }

    stopActiveRtpSenders();

    releaseRtpStreamingResources();

    g_lastPliCount = 0;
    g_lastFirCount = 0;
    g_lastNackPacketCount = 0;

    g_lastKeyframeRequestAt = std::chrono::steady_clock::time_point{};
}

static bool requestRtpVideoKeyframe(const char* reason)
{
    if (!g_rtpStreaming || !g_rtpVideoEncoder) {
        if (STREAM_DEBUG_KEYFRAME_REQUESTS) {
            std::cerr
                << "[Realtime RTP] keyframe request ignored"
                << " reason="
                << (reason ? reason : "unknown")
                << " streaming="
                << (g_rtpStreaming ? "yes" : "no")
                << " encoder="
                << (g_rtpVideoEncoder
                        ? "available"
                        : "missing")
                << "\n";
        }

        return false;
    }

    const auto now =
        std::chrono::steady_clock::now();

    constexpr auto KEYFRAME_REQUEST_COOLDOWN =
        std::chrono::milliseconds(2500);

    const bool isNewSubscriberReason =
        reason != nullptr &&
        std::strcmp(reason, "new-subscriber") == 0;

    if (
        !isNewSubscriberReason &&
        g_lastKeyframeRequestAt.time_since_epoch().count() != 0 &&
        now - g_lastKeyframeRequestAt <
            KEYFRAME_REQUEST_COOLDOWN
    ) {
        if (STREAM_DEBUG_KEYFRAME_REQUESTS) {
            const auto elapsedMs =
                std::chrono::duration_cast<
                    std::chrono::milliseconds
                >(
                    now - g_lastKeyframeRequestAt
                ).count();

            std::cerr
                << "[Realtime RTP] keyframe request throttled"
                << " reason="
                << (reason ? reason : "unknown")
                << " elapsedMs=" << elapsedMs
                << " cooldownMs="
                << KEYFRAME_REQUEST_COOLDOWN.count()
                << "\n";
        }

        return false;
    }

    const char* encoderId =
        obs_encoder_get_id(g_rtpVideoEncoder);

    if (!encoderId) {
        std::cerr
            << "[Realtime RTP] keyframe request failed"
            << " reason="
            << (reason ? reason : "unknown")
            << " error=encoder_id_unavailable"
            << "\n";

        return false;
    }

    const bool isNvenc =
        std::strcmp(
            encoderId,
            "obs_nvenc_h264_tex"
        ) == 0 ||
        std::strcmp(
            encoderId,
            "obs_nvenc_h264"
        ) == 0;

    const bool isAmf =
        std::strcmp(encoderId, "h264_texture_amf") == 0 ||
        std::strcmp(encoderId, "h265_texture_amf") == 0;

    const bool isQsv =
        std::strcmp(encoderId, "obs_qsv11_v2") == 0 ||
        std::strcmp(encoderId, "obs_qsv11_hevc") == 0 ||
        std::strcmp(encoderId, "obs_qsv11_av1") == 0;

    if (!isNvenc && !isAmf && !isQsv) {
        std::cerr
            << "[Realtime RTP] keyframe request unsupported"
            << " reason="
            << (reason ? reason : "unknown")
            << " encoderId=" << encoderId
            << "\n";

        return false;
    }

    obs_data_t* settings =
        obs_encoder_get_settings(
            g_rtpVideoEncoder
        );

    if (!settings) {
        std::cerr
            << "[Realtime RTP] keyframe request failed"
            << " reason="
            << (reason ? reason : "unknown")
            << " encoderId=" << encoderId
            << " error=settings_unavailable"
            << "\n";

        return false;
    }

    const std::string method =
        isNvenc ? "nvenc_lightweight_reconfigure" :
        isAmf ? "amf_force_idr" :
        "qsv_reconfigure";

    if (isNvenc) {
        obs_data_set_bool(settings, "__nse_lightweight", true);
        obs_data_set_bool(settings, "__nse_force_idr_only", true);
    } else if (isQsv) {
        obs_data_set_bool(settings, "__nse_force_idr_only", true);
    }

    obs_encoder_update(
        g_rtpVideoEncoder,
        settings
    );

    obs_data_release(settings);

    g_lastKeyframeRequestAt = now;

    std::cerr
        << "[Realtime RTP] keyframe requested"
        << " reason="
        << (reason ? reason : "unknown")
        << " encoderId=" << encoderId
        << " method=" << method
        << " cooldownMs="
        << KEYFRAME_REQUEST_COOLDOWN.count()
        << "\n";

    return true;
}

static uint32_t readRtpVideoEncoderConfiguredBitrate()
{
    if (!g_rtpVideoEncoder) {
        return 0;
    }

    obs_data_t* settings =
        obs_encoder_get_settings(g_rtpVideoEncoder);

    if (!settings) {
        return 0;
    }

    const int64_t bitrateKbps =
        obs_data_get_int(
            settings,
            "bitrate"
        );

    obs_data_release(settings);

    if (bitrateKbps <= 0) {
        return 0;
    }

    return static_cast<uint32_t>(
        bitrateKbps * 1000
    );
}

static uint32_t updateRtpVideoEncoderBitrate(uint32_t bitrateBps)
{
    if (!g_rtpVideoEncoder || bitrateBps == 0) {
        return 0;
    }

    const int bitrateKbps =
        static_cast<int>(bitrateBps / 1000);

    const char* encoderIdRaw =
        obs_encoder_get_id(g_rtpVideoEncoder);

    const std::string encoderId =
        encoderIdRaw
            ? encoderIdRaw
            : "";

    const std::string encoderFamily =
        classifyEncoderFamily(encoderId);

    obs_data_t* settings =
        obs_encoder_get_settings(g_rtpVideoEncoder);

    if (!settings) {
        std::cerr
            << "[Realtime RTP] failed to read encoder settings"
            << " encoderId=" << encoderId
            << "\n";

        return 0;
    }

    obs_data_set_int(
        settings,
        "bitrate",
        bitrateKbps
    );

    obs_data_set_int(
        settings,
        "max_bitrate",
        bitrateKbps
    );

    if (encoderFamily == "x264") {
        obs_data_set_bool(
            settings,
            "use_bufsize",
            true
        );

        obs_data_set_int(
            settings,
            "buffer_size",
            bitrateKbps
        );
    }

    if (encoderFamily == "nvenc") {
        obs_data_set_bool(settings, "__nse_lightweight", true);
        obs_data_set_bool(settings, "__nse_force_idr_only", false);
    }

    obs_encoder_update(
        g_rtpVideoEncoder,
        settings
    );

    obs_data_release(settings);

    obs_data_t* configuredSettings =
        obs_encoder_get_settings(g_rtpVideoEncoder);

    if (!configuredSettings) {
        std::cerr
            << "[Realtime RTP] failed to read configured encoder bitrate"
            << " requestedBitrateKbps=" << bitrateKbps
            << " encoderId=" << encoderId
            << "\n";

        return 0;
    }

    const int64_t configuredBitrateKbps =
        obs_data_get_int(
            configuredSettings,
            "bitrate"
        );

    obs_data_release(configuredSettings);

    if (configuredBitrateKbps <= 0) {
        std::cerr
            << "[Realtime RTP] invalid configured encoder bitrate"
            << " requestedBitrateKbps=" << bitrateKbps
            << " configuredBitrateKbps=" << configuredBitrateKbps
            << " encoderId=" << encoderId
            << "\n";

        return 0;
    }

    const uint32_t configuredBitrateBps =
        static_cast<uint32_t>(
            configuredBitrateKbps * 1000
        );

    std::cerr
        << "[Realtime RTP] encoder bitrate configured"
        << " requestedBitrateKbps=" << bitrateKbps
        << " configuredBitrateKbps=" << configuredBitrateKbps
        << " encoderId=" << encoderId
        << " family=" << encoderFamily
        << "\n";

    return configuredBitrateBps;
}

bool ObsEngine::applyEffectiveTargetBitrateLocked(
    uint32_t effectiveTargetBitrateBps,
    const char* source
)
{
    if (
        !g_rtpStreaming ||
        !g_rtpVideoEncoder ||
        effectiveTargetBitrateBps == 0
    ) {
        return false;
    }

    const auto scheduled =
        bitrateUpdateScheduler_.update(
            effectiveTargetBitrateBps
        );

    if (scheduled.shouldApply) {
        const uint32_t configuredBitrateBps =
            updateRtpVideoEncoderBitrate(
                scheduled.bitrateBps
            );

        if (configuredBitrateBps > 0) {
            bitrateUpdateScheduler_.markApplied(
                configuredBitrateBps
            );

            g_realtimeRtpSender.setAppliedBitrate(
                configuredBitrateBps
            );
        }

        if (STREAM_DEBUG_BITRATE_DECISIONS) {
            std::cerr
                << "[Realtime RTP] effective target "
                << (configuredBitrateBps > 0 ? "applied" : "failed")
                << " source="
                << (source ? source : "unknown")
                << " effective=" << effectiveTargetBitrateBps
                << " scheduled=" << scheduled.bitrateBps
                << " configured=" << configuredBitrateBps
                << "\n";
        }
    } else if (STREAM_DEBUG_BITRATE_DECISIONS) {
        std::cerr
            << "[Realtime RTP] effective target held by scheduler"
            << " source="
            << (source ? source : "unknown")
            << " effective=" << effectiveTargetBitrateBps
            << "\n";
    }

    return true;
}

bool ObsEngine::setTargetBitrate(
    uint32_t targetBitrateBps
)
{
    if (targetBitrateBps == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(
        bitratePolicyMutex_
    );

    if (
        !g_rtpStreaming ||
        !g_rtpVideoEncoder
    ) {
        return false;
    }

    policyCeilingBps_ =
        targetBitrateBps;

    if (localDesiredBitrateBps_ == 0) {
        localDesiredBitrateBps_ =
            targetBitrateBps;
    }

    const uint32_t effectiveTargetBitrateBps =
        std::min(
            localDesiredBitrateBps_,
            policyCeilingBps_
        );

    if (STREAM_DEBUG_BITRATE_DECISIONS) {
        std::cerr
            << "[Realtime RTP] external policy updated"
            << " ceiling=" << policyCeilingBps_
            << " localDesired=" << localDesiredBitrateBps_
            << " effective=" << effectiveTargetBitrateBps
            << "\n";
    }

    return applyEffectiveTargetBitrateLocked(
        effectiveTargetBitrateBps,
        "external-policy"
    );
}

void ObsEngine::updateNetworkFeedback(
    const NetworkFeedback& feedback
)
{
    if (!g_rtpStreaming) {
        return;
    }

    if (STREAM_DEBUG_KEYFRAME_REQUESTS) {
    std::cerr
        << "[Realtime RTP] network feedback received"
        << " pliCount=" << feedback.pliCount
        << " firCount=" << feedback.firCount
        << " nackPacketCount="
        << feedback.nackPacketCount
        << " rttMs="
        << feedback.rttMs
        << "\n";
}

    const bool hasNewFir =
        feedback.firCount > g_lastFirCount;

    const bool hasNewNackPackets =
        feedback.nackPacketCount >
        g_lastNackPacketCount;

    g_lastPliCount =
        feedback.pliCount;

    g_lastFirCount =
        feedback.firCount;

    g_lastNackPacketCount =
        feedback.nackPacketCount;

    g_realtimeRtpSender.updateNetworkFeedback(
        feedback
    );

    const BitrateDecision bitrateDecision =
        g_realtimeRtpSender.bitrateDecision();

    if (
        bitrateDecision.shouldChangeEncoder &&
        bitrateDecision.targetBitrateBps > 0
    ) {
        std::lock_guard<std::mutex> lock(
            bitratePolicyMutex_
        );

        localDesiredBitrateBps_ =
            bitrateDecision.targetBitrateBps;

        const uint32_t effectiveTargetBitrateBps =
            policyCeilingBps_ > 0
                ? std::min(
                    localDesiredBitrateBps_,
                    policyCeilingBps_
                )
                : localDesiredBitrateBps_;

        if (STREAM_DEBUG_BITRATE_DECISIONS) {
            std::cerr
                << "[Realtime RTP] controller recommendation"
                << " desired=" << localDesiredBitrateBps_
                << " ceiling=" << policyCeilingBps_
                << " effective=" << effectiveTargetBitrateBps
                << " stable="
                << bitrateDecision.stableFeedbackCount
                << "\n";
        }

        applyEffectiveTargetBitrateLocked(
            effectiveTargetBitrateBps,
            "local-abr"
        );
    }

    if (feedback.keyframeRequested || hasNewFir) {
        const char* reason = nullptr;

        if (feedback.isNewSubscriberKeyframe) {
            reason = "new-subscriber";
        } else if (feedback.keyframeRequested && hasNewFir) {
            reason = "pli+fir";
        } else if (feedback.keyframeRequested) {
            reason = "pli-corroborated";
        } else {
            reason = "fir";
        }

        if (STREAM_DEBUG_KEYFRAME_REQUESTS) {
            std::cerr
                << "[Realtime RTP] receiver requested keyframe"
                << " reason=" << reason
                << " pliCount=" << feedback.pliCount
                << " firCount=" << feedback.firCount
                << "\n";
        }

        requestRtpVideoKeyframe(reason);
    }

    if (
        hasNewNackPackets &&
        STREAM_DEBUG_KEYFRAME_REQUESTS
    ) {
        std::cerr
            << "[Realtime RTP] NACK packets detected"
            << " nackPacketCount="
            << feedback.nackPacketCount
            << "\n";
    }
}

void ObsEngine::clearCapture()
{
    std::cerr << "[Native Stream DIAG] clearCapture() CALLED\n";

    #if defined(__linux__)
        {
            std::lock_guard<std::mutex> lock(
                linuxPortalWindowMatchMutex_
            );

            linuxPortalWindowMatch_ = {};
        }
    #endif

    if (captureCleared_) {
        std::cerr << "[Native Stream DIAG] clearCapture: already cleared, returning early\n";

        return;
    }

    captureCleared_ = true;

    obs_source_t* audioSourceToRelease = g_audioSource;
    obs_sceneitem_t* sceneItemToRemove = g_captureSceneItem;
    obs_source_t* captureSourceToRelease = g_captureSource;
    obs_scene_t* sceneToRelease = g_scene;

    auto cleanupPromise =
        std::make_shared<std::promise<void>>();

    std::future<void> cleanupFuture =
        cleanupPromise->get_future();

    std::thread cleanupWorker(
        [
            cleanupPromise,
            audioSourceToRelease,
            sceneItemToRemove,
            captureSourceToRelease,
            sceneToRelease
        ]() {
            std::cerr << "[Native Stream DIAG] cleanupWorker: thread started\n";

            obs_set_output_source(
                0,
                nullptr
            );

            std::cerr << "[Native Stream DIAG] cleanupWorker: obs_set_output_source(0) done\n";

            obs_set_output_source(
                1,
                nullptr
            );

            std::cerr << "[Native Stream DIAG] cleanupWorker: obs_set_output_source(1) done\n";

            if (audioSourceToRelease) {
                obs_source_release(
                    audioSourceToRelease
                );

                std::cerr << "[Native Stream DIAG] cleanupWorker: audio source released\n";
            }

            if (sceneItemToRemove) {
                obs_sceneitem_remove(
                    sceneItemToRemove
                );

                std::cerr << "[Native Stream DIAG] cleanupWorker: scene item removed\n";
            }

            if (captureSourceToRelease) {
                obs_source_release(
                    captureSourceToRelease
                );

                std::cerr << "[Native Stream DIAG] cleanupWorker: capture source released (triggers wgc_destroy synchronously)\n";
            }

            if (sceneToRelease) {
                obs_scene_release(
                    sceneToRelease
                );

                std::cerr << "[Native Stream DIAG] cleanupWorker: scene released\n";
            }

            std::cerr << "[Native Stream DIAG] cleanupWorker: all steps done, thread finishing\n";

            cleanupPromise->set_value();
        }
    );

    constexpr auto kCleanupTimeout =
        std::chrono::milliseconds(3000);

    const auto cleanupStatus =
        cleanupFuture.wait_for(
            kCleanupTimeout
        );

    if (cleanupStatus == std::future_status::ready) {
        cleanupWorker.join();

        g_audioSource = nullptr;
        g_captureSceneItem = nullptr;
        g_captureSource = nullptr;
        g_scene = nullptr;

        #if defined(_WIN32)
        resetWgcFrameState();
        #endif

        std::cerr
            << "[Native Stream Engine] "
            << "capture resources cleared\n";
    } else {
        cleanupWorker.detach();

        g_obsOutputStateUnstable.store(
            true,
            std::memory_order_release
        );

        std::cerr
            << "[Native Stream Engine] "
            << "clearCapture: cleanup timed out"
            << " timeoutMs="
            << kCleanupTimeout.count()
            << " - leaving ALL capture resources uncleaned (worker"
            << " still running in background); OBS state marked"
            << " unstable for a safe shutdown later\n";

        #if defined(_WIN32)
        resetWgcFrameState();
        #endif
    }
}

void ObsEngine::shutdown()
{
    if (shutdownCalled_) {
        return;
    }

    shutdownCalled_ = true;

    stopRtpStreaming();

    if (
        g_obsOutputStateUnstable.load(
            std::memory_order_acquire
        )
    ) {
        std::cerr
            << "[Native Stream Engine] "
            << "shutdown: OBS output state is already unstable"
            << " - skipping capture cleanup and obs_shutdown()"
            << " to avoid racing with a detached OBS worker;"
            << " terminating process directly\n";

        std::cerr.flush();

        std::_Exit(1);
    }

    clearCapture();

    if (
        g_obsOutputStateUnstable.load(
            std::memory_order_acquire
        )
    ) {
        std::cerr
            << "[Native Stream Engine] "
            << "shutdown: OBS output state became unstable during cleanup"
            << " - skipping obs_shutdown() and terminating process directly\n";

        std::cerr.flush();

        std::_Exit(1);
    }

    obs_shutdown();
}
