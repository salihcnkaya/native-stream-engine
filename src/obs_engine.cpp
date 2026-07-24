#include "obs_engine.h"

#include <obs.h>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <thread>
#include <chrono>
#include "window_utils.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#include <roapi.h>
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
    uint32_t& height
) const
{
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
}

bool ObsEngine::copyCaptureFrameBgra(
    std::vector<uint8_t>& pixels,
    uint32_t& width,
    uint32_t& height
) const
{
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

    obs_set_output_source(
        0,
        obs_scene_get_source(scene)
    );

    return true;
}

bool ObsEngine::createCaptureScene(
    CaptureType captureType,
    uintptr_t hwnd,
    int delayMs,
    bool debugFrames,
    int monitorIndex
)
{
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
}

bool ObsEngine::configureVideo(
    const ObsVideoConfig& videoConfig
)
{
    obs_video_info ovi = {};

    ovi.graphics_module = "libobs-d3d11";
    ovi.fps_num = videoConfig.fps;
    ovi.fps_den = 1;

    /*
     * Mevcut tasarÄ±mÄ±mÄ±zda OBS canvas ve output,
     * kullanÄ±cÄ±nÄ±n seÃ§tiÄŸi yayÄ±n Ã§Ã¶zÃ¼nÃ¼rlÃ¼ÄŸÃ¼nde tutuluyor.
     * WGC source gerÃ§ek kaynak boyutundan canvas iÃ§ine fit ediliyor.
     */
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

    resetWgcTargetClosed();

    bitrateUpdateScheduler_.reset();
    const auto binDir = runtimeDir / "bin" / "64bit";
    const auto pluginBinDir = runtimeDir / "obs-plugins" / "64bit";
    const auto pluginDataDir = runtimeDir / "data" / "obs-plugins";
    const auto libobsDataDir = runtimeDir / "data" / "libobs";

    std::cerr << "[Native Stream Engine] runtime: " << runtimeDir.string() << "\n";

    if (!fs::exists(binDir / "obs.dll")) {
        std::cerr << "obs.dll not found: " << (binDir / "obs.dll").string() << "\n";
        return false;
    }

		HRESULT coResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		if (FAILED(coResult) && coResult != RPC_E_CHANGED_MODE) {
				std::cerr << "CoInitializeEx failed: " << std::hex << coResult << "\n";
		}

		HRESULT roResult = RoInitialize(RO_INIT_MULTITHREADED);
		if (FAILED(roResult) && roResult != RPC_E_CHANGED_MODE) {
				std::cerr << "RoInitialize failed: " << std::hex << roResult << "\n";
		}

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

    obs_add_module_path(
        toUtf8Path(pluginBinDir).c_str(),
        (toUtf8Path(pluginDataDir) + "/%module%").c_str()
    );

    const char* safeModules[] = {
        "win-capture.dll",
        "win-wasapi.dll",
        "obs-ffmpeg.dll",
        "obs-x264.dll",
        "obs-qsv11.dll",
        "obs-amd-encoder.dll",
        "obs-nvenc.dll",
    };

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

    registerWgcSource();

    setNativeRtpOutputPacketHandler(
        handleRtpEncodedPacket
    );

    registerNativeRtpOutput();

    if (!configureVideo(videoConfig)) {
        obs_shutdown();
        return false;
    }


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

        obs_data_set_string(
            settings,
            "x264opts",
            "repeat-headers=1:scenecut=0:bframes=0"
        );
    }

    return settings;
}

bool ObsEngine::createDesktopAudioSource()
{
    std::cerr << "\nCreating desktop audio source...\n";

    obs_data_t* settings = obs_data_create();
    obs_data_set_string(settings, "device_id", "default");

    g_audioSource = obs_source_create(
        "wasapi_output_capture",
        "Native Desktop Audio",
        settings,
        nullptr
    );

    obs_data_release(settings);

    if (!g_audioSource) {
        std::cerr << "Failed to create wasapi_output_capture source\n";
        return false;
    }

    obs_source_set_audio_mixers(g_audioSource, 1);
    obs_set_output_source(1, g_audioSource);

    std::cerr << "Desktop audio source created and set as output audio source\n";
    return true;
}

bool ObsEngine::createProcessAudioSource(uintptr_t hwnd)
{
    std::cerr << "\nCreating process audio source...\n";

    WindowInfo info;
    if (!findWindowByHwnd(hwnd, info)) {
        std::cerr << "Failed to find window for process audio hwnd: "
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

    obs_data_set_string(settings, "window", obsWindow.c_str());

    // 1 = title exact
    // 0 = title, else same class
    // 2 = title, else same executable
    obs_data_set_int(settings, "priority", 1);

    g_audioSource = obs_source_create(
        "wasapi_process_output_capture",
        "Native Process Audio",
        settings,
        nullptr
    );

    obs_data_release(settings);

    if (!g_audioSource) {
        std::cerr << "Failed to create wasapi_process_output_capture source\n";
        return false;
    }

    obs_source_set_audio_mixers(g_audioSource, 1);
    obs_set_output_source(1, g_audioSource);

    std::cerr << "Process audio source created and set as output audio source\n";
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
    uint32_t audioSsrc
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

    bitrateUpdateScheduler_.reset();
    bitrateUpdateScheduler_.update(
        static_cast<uint32_t>(bitrate * 1000)
    );

    if (
        !g_realtimeRtpSender.start(
            rtpIp,
            rtpPort,
            payloadType,
            ssrc
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

        /*
        * Her encoder adayÄ± iÃ§in video encoder, output ve audio
        * encoder sÄ±fÄ±rdan oluÅŸturulur. Ã–nceki adayÄ±n output_start
        * denemesi baÅŸarÄ±sÄ±zsa hiÃ§bir OBS nesnesi yeniden kullanÄ±lmaz.
        */
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

        g_rtpOutput = obs_output_create(
            "native_rtp_output",
            "Native RTP Output",
            nullptr,
            nullptr
        );

        if (!g_rtpOutput) {
            std::cerr
                << "[Realtime RTP] packet tap output creation failed"
                << " encoderId=" << candidate.id
                << " family=" << candidate.family
                << "\n";

            releaseRtpStreamingResources();

            /*
            * Output oluÅŸturulamamasÄ± encoder'a Ã¶zgÃ¼ olmayabilir.
            * Yine de mevcut aday tamamen temizlenerek sÄ±radaki aday
            * denenir. Son adaydan sonra genel baÅŸarÄ±sÄ±zlÄ±k oluÅŸur.
            */
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

        /*
        * DÃ¶ngÃ¼ iÃ§indeki her baÅŸarÄ±sÄ±z attempt kendi OBS nesnelerini
        * temizledi. Burada yalnÄ±zca RTP sender'larÄ± kapatÄ±yoruz.
        */
        stopActiveRtpSenders();

        return false;
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

    if (!g_rtpStreaming) return;
    
    g_rtpStreaming = false;

    std::cerr << "[Realtime RTP] stop requested\n";

    stopActiveRtpSenders();

    if (g_rtpOutput) {

        if (obs_output_active(g_rtpOutput)) {
            obs_output_stop(g_rtpOutput);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));

    }    
    
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
        std::chrono::milliseconds(750);

    if (
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

    if (!isNvenc) {
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

    /*
    * OBS 32.1.2'de obs_encoder_update() void dÃ¶ner.
    * NVENC update yolu iÃ§eride encoder reconfigure yapar
    * ve forceIDR tetikler.
    */

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
        << " method=nvenc_reconfigure"
        << " cooldownMs="
        << KEYFRAME_REQUEST_COOLDOWN.count()
        << "\n";

    return true;
}

static void updateRtpVideoEncoderBitrate(uint32_t bitrateBps)
{
    if (!g_rtpVideoEncoder || bitrateBps == 0) {
        return;
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

        return;
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

    obs_encoder_update(
        g_rtpVideoEncoder,
        settings
    );

    obs_data_release(settings);

    std::cerr
        << "[Realtime RTP] encoder bitrate updated"
        << " bitrateKbps=" << bitrateKbps
        << " encoderId=" << encoderId
        << " family=" << encoderFamily
        << "\n";
}


bool ObsEngine::setTargetBitrate(uint32_t targetBitrateBps)
{
    if (
        !g_rtpStreaming ||
        !g_rtpVideoEncoder ||
        targetBitrateBps == 0
    ) {
        return false;
    }

    const auto scheduled =
        bitrateUpdateScheduler_.update(targetBitrateBps);

    if (scheduled.shouldApply) {
        updateRtpVideoEncoderBitrate(
            scheduled.bitrateBps
        );

        if (STREAM_DEBUG_BITRATE_DECISIONS) {
            std::cerr
                << "[Realtime RTP] policy target applied"
                << " requested=" << targetBitrateBps
                << " applied=" << scheduled.bitrateBps
                << "\n";
        }
    } else if (STREAM_DEBUG_BITRATE_DECISIONS) {
        std::cerr
            << "[Realtime RTP] policy target held by scheduler"
            << " requested=" << targetBitrateBps
            << "\n";
    }

    return true;
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

    const bool hasNewPli =
        feedback.pliCount > g_lastPliCount;

    const bool hasNewFir =
        feedback.firCount > g_lastFirCount;

    const bool hasNewNackPackets =
        feedback.nackPacketCount >
        g_lastNackPacketCount;

    /*
     * KarÅŸÄ±laÅŸtÄ±rmalar eski sayaÃ§lara karÅŸÄ± yapÄ±ldÄ±.
     * Åimdi son gÃ¶rÃ¼len deÄŸerleri gÃ¼ncelliyoruz.
     */
    g_lastPliCount =
        feedback.pliCount;

    g_lastFirCount =
        feedback.firCount;

    g_lastNackPacketCount =
        feedback.nackPacketCount;

    g_realtimeRtpSender.updateNetworkFeedback(
        feedback
    );

    if (hasNewPli || hasNewFir) {
        const char* reason = nullptr;

        if (hasNewPli && hasNewFir) {
            reason = "pli+fir";
        } else if (hasNewPli) {
            reason = "pli";
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
    if (captureCleared_) {
        return;
    }

    captureCleared_ = true;

    /*
     * Ã–nce scene'i OBS output slotlarÄ±ndan ayÄ±r.
     * BÃ¶ylece output tarafÄ±nÄ±n scene source Ã¼zerindeki
     * referansÄ± bÄ±rakÄ±lÄ±r.
     */
    obs_set_output_source(
        0,
        nullptr
    );

    obs_set_output_source(
        1,
        nullptr
    );

    if (g_audioSource) {
        obs_source_release(
            g_audioSource
        );

        g_audioSource = nullptr;
    }

    /*
     * Scene item'Ä± aÃ§Ä±kÃ§a kaldÄ±r.
     *
     * Bu iÅŸlem scene'in capture source Ã¼zerinde tuttuÄŸu
     * referansÄ± bÄ±rakÄ±r. obs_sceneitem_remove() sonrasÄ±nda
     * item pointer'Ä± artÄ±k kullanÄ±lmamalÄ±dÄ±r.
     */
    if (g_captureSceneItem) {
        obs_sceneitem_remove(
            g_captureSceneItem
        );

        g_captureSceneItem = nullptr;
    }

    /*
     * obs_source_create() ile aldÄ±ÄŸÄ±mÄ±z creator reference.
     * Scene item kaldÄ±rÄ±ldÄ±ktan sonra kendi referansÄ±mÄ±zÄ±
     * bÄ±rakÄ±yoruz.
     */
    if (g_captureSource) {
        obs_source_release(
            g_captureSource
        );

        g_captureSource = nullptr;
    }

    /*
     * ArtÄ±k output ve scene item referanslarÄ± yok.
     * Son olarak scene'in oluÅŸturucu referansÄ±nÄ± bÄ±rak.
     */
    if (g_scene) {
        obs_scene_release(
            g_scene
        );

        g_scene = nullptr;
    }

    resetWgcFrameState();
    
    std::cerr
        << "[Native Stream Engine] "
        << "capture resources cleared\n";
}

void ObsEngine::shutdown()
{
    if (shutdownCalled_) {
        return;
    }

    shutdownCalled_ = true;

    stopRtpStreaming();
    clearCapture();
    obs_shutdown();
}
