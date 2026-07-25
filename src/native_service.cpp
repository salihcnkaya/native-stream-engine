#include "native_service.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "native_wgc_source.h"
#include "base64_utils.h"
#include "image_utils.h"
#include "obs_engine.h"
#include "wgc_capture.h"
#include "window_utils.h"

static std::unique_ptr<ObsEngine> g_engine;

static std::atomic<bool> g_captureActive = false;
static std::atomic<bool> g_captureStarting = false;

static std::atomic<bool> g_serviceRunning = false;
static std::atomic<bool> g_captureEndedNotified = false;

static std::mutex g_stdoutMutex;

static constexpr uint32_t PICKER_PREVIEW_FRAME_TIMEOUT_MS = 400;

static void shutdown_engine_state()
{
    /*
     * Watcher'Ä±n cleanup sÃ¼rerken yeni captureEnded eventi
     * Ã¼retmesini Ã¶nce engelle.
     */
    g_captureActive.store(
        false,
        std::memory_order_release
    );

    g_captureStarting.store(
        false,
        std::memory_order_release
    );

    g_captureEndedNotified.store(
        true,
        std::memory_order_release
    );

    /*
     * OBS ve RTP cleanup yalnÄ±zca service komut thread'inde
     * gerÃ§ekleÅŸir. Watcher thread buraya girmez.
     */
    if (g_engine) {
        g_engine->shutdown();
        g_engine.reset();
    }

    resetWgcTargetClosed();

    /*
     * Bir sonraki capture baÅŸlamadan Ã¶nce notification state'ini
     * temiz bÄ±rak. Capture aktif olmadÄ±ÄŸÄ± iÃ§in watcher Ã§alÄ±ÅŸmaz.
     */
    g_captureEndedNotified.store(
        false,
        std::memory_order_release
    );
}

static void cleanup_active_capture_state()
{
    /*
     * OBS context ve modÃ¼ller aÃ§Ä±k kalÄ±r.
     * YalnÄ±zca aktif RTP/output/source nesneleri temizlenir.
     */
    g_captureActive.store(
        false,
        std::memory_order_release
    );

    g_captureStarting.store(
        false,
        std::memory_order_release
    );

    g_captureEndedNotified.store(
        true,
        std::memory_order_release
    );

    if (g_engine) {
        g_engine->stopRtpStreaming();
        g_engine->clearCapture();
    }

    resetWgcTargetClosed();

    g_captureEndedNotified.store(
        false,
        std::memory_order_release
    );
}

static void cleanup_preview_capture_state()
{
    /*
     * Preview aktif bir gerÃ§ek yayÄ±n deÄŸildir.
     * YalnÄ±zca geÃ§ici WGC scene/source temizlenir;
     * OBS instance aÃ§Ä±k bÄ±rakÄ±lÄ±r.
     */
    g_captureActive.store(
        false,
        std::memory_order_release
    );

    g_captureStarting.store(
        false,
        std::memory_order_release
    );

    g_captureEndedNotified.store(
        true,
        std::memory_order_release
    );

    if (g_engine) {
        g_engine->clearCapture();
    }

    resetWgcTargetClosed();

    g_captureEndedNotified.store(
        false,
        std::memory_order_release
    );
}

static constexpr bool STREAM_DEBUG_NETWORK_FEEDBACK = false;

static std::filesystem::path get_runtime_dir()
{
    return
        std::filesystem::current_path() /
        "runtime" /
        "OBS-Studio-32.1.2-Windows-x64";
}


static std::string json_escape(
    const std::string& value
)
{
    std::string out;
    out.reserve(value.size());

    for (const char c : value) {
        switch (c) {
            case '\\':
                out += "\\\\";
                break;

            case '"':
                out += "\\\"";
                break;

            case '\n':
                out += "\\n";
                break;

            case '\r':
                out += "\\r";
                break;

            case '\t':
                out += "\\t";
                break;

            default:
                out += c;
                break;
        }
    }

    return out;
}

static void send_json(
    const std::string& json
)
{
    const std::lock_guard<std::mutex> lock(
        g_stdoutMutex
    );

    std::cout
        << json
        << '\n';

    std::cout.flush();
}

static int extract_id(
    const std::string& line
)
{
    static constexpr char key[] =
        "\"id\":";

    const size_t position =
        line.find(key);

    if (position == std::string::npos) {
        return 0;
    }

    const size_t valuePosition =
        position + sizeof(key) - 1;

    try {
        return std::stoi(
            line.substr(valuePosition)
        );
    } catch (...) {
        return 0;
    }
}

static std::string extract_string_value(
    const std::string& line,
    const std::string& key,
    const std::string& defaultValue = ""
)
{
    const std::string pattern =
        "\"" + key + "\":\"";

    const size_t position =
        line.find(pattern);

    if (position == std::string::npos) {
        return defaultValue;
    }

    const size_t valuePosition =
        position + pattern.size();

    const size_t endPosition =
        line.find('"', valuePosition);

    if (endPosition == std::string::npos) {
        return defaultValue;
    }

    return line.substr(
        valuePosition,
        endPosition - valuePosition
    );
}

static uint64_t extract_uint_value(
    const std::string& line,
    const std::string& key,
    uint64_t defaultValue = 0
)
{
    const std::string pattern =
        "\"" + key + "\":";

    const size_t position =
        line.find(pattern);

    if (position == std::string::npos) {
        return defaultValue;
    }

    const size_t valuePosition =
        position + pattern.size();

    try {
        return std::stoull(
            line.substr(valuePosition)
        );
    } catch (...) {
        return defaultValue;
    }
}

static bool wait_for_valid_capture_size(
    uintptr_t hwnd,
    int maxWaitMs,
    int intervalMs,
    int& outWidth,
    int& outHeight
)
{
    int elapsedMs = 0;

    while (elapsedMs <= maxWaitMs) {
        int width = 0;
        int height = 0;

        if (wgcGetCaptureSize(hwnd, intervalMs, width, height)) {
            if (width >= 320 && height >= 180) {
                outWidth = width;
                outHeight = height;
                return true;
            }

            std::cerr << "[Native Stream Service] capture target not ready yet: "
                      << width << "x" << height << "\n";
        }

        elapsedMs += intervalMs;
    }

    return false;
}

static void run_capture_target_watcher()
{
    using namespace std::chrono_literals;

    while (
        g_serviceRunning.load(
            std::memory_order_acquire
        )
    ) {
        const bool captureActive =
            g_captureActive.load(
                std::memory_order_acquire
            );

        const bool alreadyNotified =
            g_captureEndedNotified.load(
                std::memory_order_acquire
            );

        if (
            captureActive &&
            !alreadyNotified &&
            isWgcTargetClosed()
        ) {
            bool expected = false;

            if (
                g_captureEndedNotified
                    .compare_exchange_strong(
                        expected,
                        true,
                        std::memory_order_acq_rel
                    )
            ) {
                std::cerr
                    << "[Native Stream Service] "
                    << "capture target ended; notifying client\n";

                send_json(
                    "{"
                    "\"ok\":true,"
                    "\"type\":\"captureEnded\","
                    "\"reason\":\"target-closed\""
                    "}"
                );
            }
        }

        std::this_thread::sleep_for(
            100ms
        );
    }
}

int runNativeService()
{
    std::cerr << "[Native Stream Service] started\n";

		g_serviceRunning.store(
        true,
        std::memory_order_release
    );

    std::thread captureWatcherThread(
        run_capture_target_watcher
    );

    std::string line;

    while (std::getline(std::cin, line)) {
				int id = extract_id(line);
        if (line.find("\"type\":\"ping\"") != std::string::npos) {
            send_json(
								"{\"id\":" + std::to_string(id) + ",\"ok\":true,\"type\":\"pong\"}"
						);
            continue;
        }

				if (line.find("\"type\":\"listSources\"") != std::string::npos) {
						auto monitors = listMonitors();
						auto windows = listVisibleWindows();

						std::string response =
							"{\"id\":" + std::to_string(id) +
							",\"ok\":true,\"type\":\"sources\",\"monitors\":[";
						for (size_t i = 0; i < monitors.size(); i++) {
								const auto& m = monitors[i];

								if (i > 0) response += ",";

								response += "{";
								response += "\"type\":\"monitor\",";
								response += "\"monitorIndex\":" + std::to_string(m.index) + ",";
								response += "\"name\":\"" + json_escape(m.name) + "\",";
								response += "\"x\":" + std::to_string(m.x) + ",";
								response += "\"y\":" + std::to_string(m.y) + ",";
								response += "\"width\":" + std::to_string(m.width) + ",";
								response += "\"height\":" + std::to_string(m.height) + ",";
								response += "\"primary\":" + std::string(m.primary ? "true" : "false");
								response += "}";
						}

						response += "],\"windows\":[";

						for (size_t i = 0; i < windows.size(); i++) {
								const auto& w = windows[i];

								if (i > 0) response += ",";

								response += "{";
								response += "\"type\":\"window\",";
								response += "\"hwnd\":" + std::to_string(w.hwnd) + ",";
								response += "\"pid\":" + std::to_string(w.pid) + ",";
								response += "\"exe\":\"" + json_escape(w.exeName) + "\",";
								response += "\"exePath\":\"" + json_escape(w.exePath) + "\",";
								response += "\"className\":\"" + json_escape(w.className) + "\",";
								response += "\"title\":\"" + json_escape(w.title) + "\"";
								response += "}";
						}

						response += "]}";

						send_json(response);
						continue;
				}

								if (
						line.find(
								"\"type\":\"capturePreview\""
						) != std::string::npos
				) {
						/*
						* Preview ve gerÃ§ek yayÄ±n aynÄ± global OBS engine'i
						* kullanÄ±yor. YayÄ±n aktifken preview baÅŸlatmÄ±yoruz.
						*/
						if (
								g_captureActive.load(
										std::memory_order_acquire
								) ||
								g_captureStarting.load(
										std::memory_order_acquire
								)
						) {
								send_json(
										"{\"id\":" +
										std::to_string(id) +
										",\"ok\":false,"
										"\"error\":\"capture engine busy\"}"
								);

								continue;
						}

						if (g_engine) {
								cleanup_preview_capture_state();
						}

						g_captureStarting.store(
								true,
								std::memory_order_release
						);

						g_captureEndedNotified.store(
								false,
								std::memory_order_release
						);

						resetWgcTargetClosed();

						const std::string capture =
								extract_string_value(
										line,
										"capture",
										"monitor"
								);

						const uintptr_t hwnd =
								static_cast<uintptr_t>(
										extract_uint_value(
												line,
												"hwnd",
												0
										)
								);

						const int monitorIndex =
								static_cast<int>(
										extract_uint_value(
												line,
												"monitorIndex",
												0
										)
								);

						const uint32_t maxWidth =
								static_cast<uint32_t>(
										extract_uint_value(
												line,
												"maxWidth",
												320
										)
								);

						const uint32_t maxHeight =
								static_cast<uint32_t>(
										extract_uint_value(
												line,
												"maxHeight",
												180
										)
								);

						CaptureType captureType =
								CaptureType::Monitor;

						if (capture == "window") {
								captureType =
										CaptureType::Window;
						} else if (capture == "game") {
								captureType =
										CaptureType::Game;
						} else if (capture == "wgc") {
								captureType =
										CaptureType::Wgc;
						} else if (capture != "monitor") {
								g_captureStarting.store(
										false,
										std::memory_order_release
								);

								send_json(
										"{\"id\":" +
										std::to_string(id) +
										",\"ok\":false,"
										"\"error\":\"invalid preview capture type\"}"
								);

								continue;
						}

						if (
								captureType != CaptureType::Monitor &&
								hwnd == 0
						) {
								g_captureStarting.store(
										false,
										std::memory_order_release
								);

								send_json(
										"{\"id\":" +
										std::to_string(id) +
										",\"ok\":false,"
										"\"error\":\"window preview requires hwnd\"}"
								);

								continue;
						}

						if (
								maxWidth == 0 ||
								maxHeight == 0 ||
								maxWidth > 1920 ||
								maxHeight > 1080
						) {
								g_captureStarting.store(
										false,
										std::memory_order_release
								);

								send_json(
										"{\"id\":" +
										std::to_string(id) +
										",\"ok\":false,"
										"\"error\":\"invalid preview dimensions\"}"
								);

								continue;
						}

						/*
						* OBS video output'u preview iÃ§in kÃ¼Ã§Ã¼k tutuyoruz.
						* WGC source kendi gerÃ§ek kaynak Ã§Ã¶zÃ¼nÃ¼rlÃ¼ÄŸÃ¼nÃ¼ yine
						* ayrÄ± texture iÃ§inde Ã¼retir.
						*/
						ObsVideoConfig previewVideoConfig;

						previewVideoConfig.outputWidth =
								maxWidth;

						previewVideoConfig.outputHeight =
								maxHeight;

						previewVideoConfig.fps = 30;

						previewVideoConfig.scaleFilter =
								"bilinear";

							if (!g_engine) {
									g_engine =
											std::make_unique<ObsEngine>();

									if (
											!g_engine->initialize(
													get_runtime_dir(),
													previewVideoConfig
											)
									) {
											shutdown_engine_state();

											send_json(
													"{\"id\":" +
													std::to_string(id) +
													",\"ok\":false,"
													"\"error\":\"preview OBS initialize failed\"}"
											);

											continue;
									}
							} else {
									if (
											!g_engine->configureVideo(
													previewVideoConfig
											)
									) {
											shutdown_engine_state();

											send_json(
													"{\"id\":" +
													std::to_string(id) +
													",\"ok\":false,"
													"\"error\":\"preview video configure failed\"}"
											);

											continue;
									}
							}

						if (
								!g_engine->createCaptureScene(
										captureType,
										hwnd,
										0,
										false,
										monitorIndex
								)
						) {
								cleanup_preview_capture_state();

								send_json(
										"{\"id\":" +
										std::to_string(id) +
										",\"ok\":false,"
										"\"error\":\"preview capture scene failed\"}"
								);

								continue;
						}

						uint32_t sourceWidth = 0;
						uint32_t sourceHeight = 0;

						if (
								!g_engine->waitForCaptureFrame(
										PICKER_PREVIEW_FRAME_TIMEOUT_MS,
										sourceWidth,
										sourceHeight
								)
						) {
								cleanup_preview_capture_state();

								send_json(
										"{\"id\":" +
										std::to_string(id) +
										",\"ok\":false,"
										"\"error\":\"preview frame timeout\"}"
								);

								continue;
						}

						std::vector<uint8_t> sourcePixels;

						if (
								!g_engine->copyCaptureFrameBgra(
										sourcePixels,
										sourceWidth,
										sourceHeight
								)
						) {
								cleanup_preview_capture_state();

								send_json(
										"{\"id\":" +
										std::to_string(id) +
										",\"ok\":false,"
										"\"error\":\"preview frame copy failed\"}"
								);

								continue;
						}

						std::vector<uint8_t> thumbnailPixels;

						uint32_t thumbnailWidth = 0;
						uint32_t thumbnailHeight = 0;

						if (
								!resizeBgraFrame(
										sourcePixels,
										sourceWidth,
										sourceHeight,
										maxWidth,
										maxHeight,
										thumbnailPixels,
										thumbnailWidth,
										thumbnailHeight
								)
						) {

								cleanup_preview_capture_state();

								send_json(
										"{\"id\":" +
										std::to_string(id) +
										",\"ok\":false,"
										"\"error\":\"preview resize failed\"}"
								);

								continue;
						}

						/*
						* BÃ¼yÃ¼k ham BGRA frame'e artÄ±k ihtiyacÄ±mÄ±z yok.
						* JSON hazÄ±rlanÄ±rken bellekte tutmayalÄ±m.
						*/
						sourcePixels.clear();
						sourcePixels.shrink_to_fit();

						std::vector<uint8_t> pngBytes;

						if (
								!encodeBgraFrameToPng(
										thumbnailPixels,
										thumbnailWidth,
										thumbnailHeight,
										pngBytes
								)
						) {
								cleanup_preview_capture_state();

								send_json(
										"{\"id\":" +
										std::to_string(id) +
										",\"ok\":false,"
										"\"error\":\"preview PNG encode failed\"}"
								);

								continue;
						}

						thumbnailPixels.clear();
						thumbnailPixels.shrink_to_fit();

						const std::string pngBase64 =
								encodeBase64(
										pngBytes
								);

						if (pngBase64.empty()) {
								cleanup_preview_capture_state();

								send_json(
										"{\"id\":" +
										std::to_string(id) +
										",\"ok\":false,"
										"\"error\":\"preview base64 encode failed\"}"
								);

								continue;
						}

						std::cerr
								<< "[Native Stream Service] "
								<< "preview captured"
								<< " capture="
								<< capture
								<< " monitorIndex="
								<< monitorIndex
								<< " hwnd="
								<< hwnd
								<< " source="
								<< sourceWidth
								<< "x"
								<< sourceHeight
								<< " thumbnail="
								<< thumbnailWidth
								<< "x"
								<< thumbnailHeight
								<< " pngBytes="
								<< pngBytes.size()
								<< " base64Chars="
								<< pngBase64.size()
								<< "\n";

						/*
						* Ã–nce capture/OBS kaynaklarÄ±nÄ± tamamen kapat.
						* ArdÄ±ndan baÅŸarÄ± cevabÄ±nÄ± gÃ¶nder.
						*/
						cleanup_preview_capture_state();

						std::string response =
								"{\"id\":" +
								std::to_string(id) +
								",\"ok\":true,"
								"\"type\":\"capturePreviewResult\",";

						response +=
								"\"capture\":\"" +
								json_escape(capture) +
								"\",";

						response +=
								"\"monitorIndex\":" +
								std::to_string(monitorIndex) +
								",";

						response +=
								"\"hwnd\":" +
								std::to_string(hwnd) +
								",";

						response +=
								"\"sourceWidth\":" +
								std::to_string(sourceWidth) +
								",";

						response +=
								"\"sourceHeight\":" +
								std::to_string(sourceHeight) +
								",";

						response +=
								"\"width\":" +
								std::to_string(thumbnailWidth) +
								",";

						response +=
								"\"height\":" +
								std::to_string(thumbnailHeight) +
								",";

						response +=
								"\"mimeType\":\"image/png\",";

						response +=
								"\"dataUrl\":\"data:image/png;base64," +
								pngBase64 +
								"\"}";

						send_json(response);

						continue;
				}

				if (line.find("\"type\":\"startCapture\"") != std::string::npos) {
						if (g_captureActive.load(std::memory_order_acquire) ||
								g_captureStarting.load(std::memory_order_acquire)) {
								send_json(
										"{\"id\":" + std::to_string(id) +
										",\"ok\":false,\"error\":\"capture already active\"}"
								);
								continue;
						}
						if (g_engine) {
							cleanup_active_capture_state();
						}
						g_captureStarting.store(
								true,
								std::memory_order_release
						);

						g_captureEndedNotified.store(
								false,
								std::memory_order_release
						);

						resetWgcTargetClosed();

						std::string capture = extract_string_value(line, "capture", "monitor");
						const int monitorIndex = static_cast<int>(extract_uint_value(line, "monitorIndex", 0));
						std::string audio = extract_string_value(line, "audio", "auto");
						std::string quality = extract_string_value(line, "quality", "720p60");
						std::string rtpIp = extract_string_value(line, "rtpIp", "");
						std::string encoder = extract_string_value(line, "encoder", "auto");

						uint16_t rtpPort = static_cast<uint16_t>(
								extract_uint_value(line, "rtpPort", 0)
						);

						uint8_t payloadType = static_cast<uint8_t>(
								extract_uint_value(line, "payloadType", 102)
						);

						uint32_t ssrc = static_cast<uint32_t>(
								extract_uint_value(line, "ssrc", 0)
						);

						std::string audioRtpIp = extract_string_value(line, "audioRtpIp", "");

						uint16_t audioRtpPort = static_cast<uint16_t>(
								extract_uint_value(line, "audioRtpPort", 0)
						);

						uint8_t audioPayloadType = static_cast<uint8_t>(
								extract_uint_value(line, "audioPayloadType", 111)
						);

						uint32_t audioSsrc = static_cast<uint32_t>(
								extract_uint_value(line, "audioSsrc", 0)
						);

						bool audioRtpEnabled =
								!audioRtpIp.empty() &&
								audioRtpPort > 0 &&
								audioSsrc > 0;

						if (audioRtpEnabled) {
								std::cerr << "[Native Stream Service] Audio RTP enabled: "
													<< audioRtpIp << ":"
													<< audioRtpPort
													<< " pt=" << static_cast<int>(audioPayloadType)
													<< " ssrc=" << audioSsrc
													<< "\n";
						}

						bool rtpEnabled =
								!rtpIp.empty() &&
								rtpPort > 0 &&
								ssrc > 0;

						if (rtpEnabled) {
								std::cerr << "[Native Stream Service] RTP enabled: "
													<< rtpIp << ":"
													<< rtpPort
													<< " pt=" << static_cast<int>(payloadType)
													<< " ssrc=" << ssrc
													<< "\n";
						}		

						uintptr_t hwnd = static_cast<uintptr_t>(
								extract_uint_value(line, "hwnd", 0)
						);

						CaptureType captureType = CaptureType::Monitor;

						if (capture == "window") {
								captureType = CaptureType::Window;
						} else if (capture == "game") {
								captureType = CaptureType::Game;
						} else if (capture == "wgc") {
								captureType = CaptureType::Wgc;
						}						

						if (captureType != CaptureType::Monitor && hwnd == 0) {
								g_captureStarting.store(
										false,
										std::memory_order_release
								);

								send_json(
										"{\"id\":" + std::to_string(id) +
										",\"ok\":false,\"error\":\"window/game capture requires hwnd\"}"
								);

								continue;
						}


						ObsVideoConfig videoConfig;
						videoConfig.baseWidth = 1920;
						videoConfig.baseHeight = 1080;
						videoConfig.outputWidth = 1280;
						videoConfig.outputHeight = 720;
						videoConfig.fps = 60;
						videoConfig.scaleFilter = "lanczos";


						if (captureType != CaptureType::Monitor && hwnd != 0) {
								int detectedWidth = 0;
								int detectedHeight = 0;

								if (!wait_for_valid_capture_size(
												hwnd,
												15000,
												750,
												detectedWidth,
												detectedHeight
										)) {
										g_captureStarting.store(
												false,
												std::memory_order_release
										);
										
										send_json(
												"{\"id\":" + std::to_string(id) +
												",\"ok\":false,\"error\":\"capture target did not become active in time\"}"
										);

										continue;
								}

								videoConfig.baseWidth = detectedWidth;
								videoConfig.baseHeight = detectedHeight;

								std::cerr << "[Native Stream Service] detected capture size: "
													<< detectedWidth << "x" << detectedHeight << "\n";
						}						

						if (quality == "720p30") {
								videoConfig.outputWidth = 1280;
								videoConfig.outputHeight = 720;
								videoConfig.fps = 30;
						} else if (quality == "720p60") {
								videoConfig.outputWidth = 1280;
								videoConfig.outputHeight = 720;
								videoConfig.fps = 60;
						} else if (quality == "1080p30") {
								videoConfig.outputWidth = 1920;
								videoConfig.outputHeight = 1080;
								videoConfig.fps = 30;
						} else if (quality == "1080p60") {
								videoConfig.outputWidth = 1920;
								videoConfig.outputHeight = 1080;
								videoConfig.fps = 60;
						} else if (quality == "source") {
								videoConfig.outputWidth = videoConfig.baseWidth;
								videoConfig.outputHeight = videoConfig.baseHeight;
								videoConfig.fps = 60;
						} else {
								std::cerr << "[Native Stream Service] unknown quality, falling back to 720p60: "
													<< quality
													<< "\n";
						}

						if (!g_engine) {
								g_engine =
										std::make_unique<ObsEngine>();

								if (
										!g_engine->initialize(
												get_runtime_dir(),
												videoConfig
										)
								) {
										shutdown_engine_state();

										send_json(
												"{\"id\":" +
												std::to_string(id) +
												",\"ok\":false,"
												"\"error\":\"OBS initialize failed\"}"
										);

										continue;
								}
						} else {
								if (
										!g_engine->configureVideo(
												videoConfig
										)
								) {
										shutdown_engine_state();

										send_json(
												"{\"id\":" +
												std::to_string(id) +
												",\"ok\":false,"
												"\"error\":\"OBS video configure failed\"}"
										);

										continue;
								}
						}

						if (!g_engine->createCaptureScene(
										captureType,
										hwnd,
										1000,
										false,
										monitorIndex
								)) {
					
								cleanup_active_capture_state();
								send_json(
										"{\"id\":" + std::to_string(id) +
										",\"ok\":false,\"error\":\"create capture scene failed\"}"
								);
								continue;
						}

						uint32_t captureFrameWidth = 0;
						uint32_t captureFrameHeight = 0;

						if (
								!g_engine->waitForCaptureFrame(
										3000,
										captureFrameWidth,
										captureFrameHeight
								)
						) {
								cleanup_active_capture_state();
								send_json(
										"{\"id\":" +
										std::to_string(id) +
										",\"ok\":false,"
										"\"error\":\"capture frame unavailable\"}"
								);

								continue;
						}

						std::cerr
								<< "[Native Stream Service] "
								<< "first capture frame confirmed "
								<< captureFrameWidth
								<< "x"
								<< captureFrameHeight
								<< "\n";

						const bool audioStreamingEnabled =
										audioRtpEnabled &&
										audio != "none";

						bool audioOk = true;

						if (!audioStreamingEnabled) {
										std::cerr
														<< "[Native Stream Service] "
														<< "audio capture disabled\n";
						} else if (audio == "desktop") {
										audioOk =
														g_engine->createDesktopAudioSource();
						} else if (audio == "process") {
										audioOk =
														g_engine->createProcessAudioSource(hwnd);
						} else {
										if (captureType == CaptureType::Monitor) {
														audioOk =
																		g_engine->createDesktopAudioSource();
										} else {
														audioOk =
																		g_engine->createProcessAudioSource(hwnd);

														if (!audioOk) {
																		audioOk =
																						g_engine->createDesktopAudioSource();
														}
										}
						}

						if (!audioOk) {
								cleanup_active_capture_state();
								send_json(
										"{\"id\":" + std::to_string(id) +
										",\"ok\":false,\"error\":\"create audio source failed\"}"
								);
								continue;
						}

						if (rtpEnabled) {
								int bitrate = 6000;

								if (quality == "720p30") {
										bitrate = 2500;
								} else if (quality == "720p60") {
										bitrate = 4500;
								} else if (quality == "1080p30") {
										bitrate = 5500;
								} else if (quality == "1080p60" || quality == "source") {
										bitrate = 8000;
								} else {
										bitrate = 4500;
								}

								if (!g_engine->startRtpStreaming(
												rtpIp,
												rtpPort,
												payloadType,
												ssrc,
												bitrate,
												encoder,
												audioStreamingEnabled ? audioRtpIp : std::string{},
												audioStreamingEnabled ? audioRtpPort : 0,
												audioPayloadType,
												audioStreamingEnabled ? audioSsrc : 0
								)) {
										cleanup_active_capture_state();
										send_json(
												"{\"id\":" + std::to_string(id) +
												",\"ok\":false,\"error\":\"start RTP streaming failed\"}"
										);

										continue;
								}
						}								

						g_captureActive.store(
								true,
								std::memory_order_release
						);

						g_captureStarting.store(
								false,
								std::memory_order_release
						);

						g_captureEndedNotified.store(
								false,
								std::memory_order_release
						);

						send_json(
								"{\"id\":" + std::to_string(id) +
								",\"ok\":true,\"type\":\"captureStarted\"}"
						);

						continue;
				}
	
				if (line.find("\"type\":\"stopCapture\"") != std::string::npos) {
						cleanup_active_capture_state();

						send_json(
								"{\"id\":" + std::to_string(id) +
								",\"ok\":true,\"type\":\"captureStopped\"}"
						);

						continue;
				}

				if (line.find("\"type\":\"setTargetBitrate\"") != std::string::npos) {
						const uint32_t targetBitrateBps = static_cast<uint32_t>(
								extract_uint_value(line, "targetBitrateBps", 0)
						);

						if (
									!g_engine ||
									!g_captureActive.load(
											std::memory_order_acquire
									)
							) {
								send_json(
										"{\"id\":" + std::to_string(id) +
										",\"ok\":false,\"error\":\"capture is not active\"}"
								);

								continue;
						}

						if (targetBitrateBps == 0) {
								send_json(
										"{\"id\":" + std::to_string(id) +
										",\"ok\":false,\"error\":\"invalid target bitrate\"}"
								);

								continue;
						}

						const bool applied =
								g_engine->setTargetBitrate(targetBitrateBps);

						send_json(
								"{\"id\":" + std::to_string(id) +
								",\"ok\":" + std::string(applied ? "true" : "false") +
								",\"type\":\"targetBitrateAck\"" +
								",\"targetBitrateBps\":" +
								std::to_string(targetBitrateBps) +
								"}"
						);

						continue;
				}

				if (line.find("\"type\":\"networkFeedback\"") != std::string::npos) {
						NetworkFeedback feedback;

						feedback.packetLossRatio = static_cast<double>(
								extract_uint_value(line, "packetLossPermille", 0)
						) / 1000.0;

						feedback.jitterMs = static_cast<uint32_t>(
								extract_uint_value(line, "jitterMs", 0)
						);

						feedback.rttMs = static_cast<uint32_t>(
								extract_uint_value(line, "rttMs", 0)
						);

						feedback.score = static_cast<uint32_t>(
								extract_uint_value(line, "score", 10)
						);

						feedback.bitrateBps = static_cast<uint32_t>(
								extract_uint_value(line, "bitrate", 0)
						);

						feedback.packetCount = static_cast<uint64_t>(
								extract_uint_value(line, "packetCount", 0)
						);

						feedback.byteCount = static_cast<uint64_t>(
								extract_uint_value(line, "byteCount", 0)
						);

						feedback.nackCount = static_cast<uint32_t>(
								extract_uint_value(line, "nackCount", 0)
						);

						feedback.nackPacketCount = static_cast<uint32_t>(
								extract_uint_value(line, "nackPacketCount", 0)
						);

						feedback.pliCount = static_cast<uint32_t>(
								extract_uint_value(line, "pliCount", 0)
						);

						feedback.firCount = static_cast<uint32_t>(
								extract_uint_value(line, "firCount", 0)
						);

						feedback.hasFeedback = true;

						if (
									!g_engine ||
									!g_captureActive.load(
											std::memory_order_acquire
									)
							) {
								send_json(
										"{\"id\":" + std::to_string(id) +
										",\"ok\":true,\"type\":\"networkFeedbackIgnored\"}"
								);

								continue;
						}

						g_engine->updateNetworkFeedback(feedback);

						if (STREAM_DEBUG_NETWORK_FEEDBACK) {
								std::cerr << "[Native Stream Service] network feedback"
													<< " loss=" << feedback.packetLossRatio
													<< " jitterMs=" << feedback.jitterMs
													<< " rttMs=" << feedback.rttMs
													<< " score=" << feedback.score
													<< " bitrate=" << feedback.bitrateBps
													<< " packetCount=" << feedback.packetCount
													<< " byteCount=" << feedback.byteCount
													<< " nackCount=" << feedback.nackCount
													<< " nackPacketCount=" << feedback.nackPacketCount
													<< " pliCount=" << feedback.pliCount
													<< " firCount=" << feedback.firCount
													<< "\n";
						}

						send_json(
								"{\"id\":" + std::to_string(id) +
								",\"ok\":true,\"type\":\"networkFeedbackAck\"}"
						);

						continue;
				}

				if (line.find("\"type\":\"simulateNetworkFeedback\"") != std::string::npos) {
						NetworkFeedback feedback;

						feedback.packetLossRatio = static_cast<double>(
								extract_uint_value(line, "packetLossPermille", 0)
						) / 1000.0;

						feedback.jitterMs = static_cast<uint32_t>(
								extract_uint_value(line, "jitterMs", 0)
						);

						feedback.rttMs = static_cast<uint32_t>(
								extract_uint_value(line, "rttMs", 0)
						);

						feedback.score = static_cast<uint32_t>(
								extract_uint_value(line, "score", 10)
						);

						feedback.hasFeedback = true;

						if (
									g_engine &&
									g_captureActive.load(
											std::memory_order_acquire
									)
							) {
								g_engine->updateNetworkFeedback(feedback);
						}

						send_json(
								"{\"id\":" + std::to_string(id) +
								",\"ok\":true,\"type\":\"simulateNetworkFeedbackAck\"}"
						);

						continue;
				}

				if (
						line.find(
								"\"type\":\"shutdown\""
						) != std::string::npos
				) {
						send_json(
								"{\"id\":" +
								std::to_string(id) +
								",\"ok\":true,"
								"\"type\":\"shutdown_ack\"}"
						);

						shutdown_engine_state();

						break;
				}


        send_json(
            "{\"ok\":false,\"error\":\"unknown command\",\"raw\":\"" +
            json_escape(line) +
            "\"}"
        );
    }

		g_serviceRunning.store(
				false,
				std::memory_order_release
		);

		if (captureWatcherThread.joinable()) {
				captureWatcherThread.join();
		}

		shutdown_engine_state();

		return 0;
}
