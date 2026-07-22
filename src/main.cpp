#include "obs_engine.h"

#include <filesystem>
#include <iostream>
#include <string>

#include "native_service.h"
#include "wgc_capture.h"
#include "window_utils.h"

namespace fs = std::filesystem;

static std::string json_escape(
    const std::string& value
)
{
    std::string output;
    output.reserve(value.size());

    for (const char character : value) {
        switch (character) {
            case '\\':
                output += "\\\\";
                break;

            case '"':
                output += "\\\"";
                break;

            case '\n':
                output += "\\n";
                break;

            case '\r':
                output += "\\r";
                break;

            case '\t':
                output += "\\t";
                break;

            default:
                output += character;
                break;
        }
    }

    return output;
}

static void print_usage()
{
    std::cout
        << "\nUsage:\n"
        << "  native-stream-engine.exe --service\n"
        << "  native-stream-engine.exe --list\n"
        << "  native-stream-engine.exe --list-windows\n"
        << "  native-stream-engine.exe --list-sources [--json 1]\n";
}

static fs::path get_runtime_dir()
{
    return
        fs::current_path() /
        "runtime" /
        "OBS-Studio-32.1.2-Windows-x64";
}

static bool has_json_output_argument(
    int argc,
    char** argv
)
{
    for (int index = 2; index + 1 < argc; index++) {
        if (
            std::string(argv[index]) == "--json" &&
            std::string(argv[index + 1]) == "1"
        ) {
            return true;
        }
    }

    return false;
}

static int run_list_sources_command(
    bool jsonOutput
)
{
    const auto monitors = listMonitors();
    const auto windows = listVisibleWindows();

    if (jsonOutput) {
        std::cout << "{\"monitors\":[";

        for (size_t index = 0; index < monitors.size(); index++) {
            const auto& monitor = monitors[index];

            if (index > 0) {
                std::cout << ',';
            }

            std::cout
                << '{'
                << "\"type\":\"monitor\","
                << "\"monitorIndex\":"
                << monitor.index
                << ','
                << "\"name\":\""
                << json_escape(monitor.name)
                << "\","
                << "\"x\":"
                << monitor.x
                << ','
                << "\"y\":"
                << monitor.y
                << ','
                << "\"width\":"
                << monitor.width
                << ','
                << "\"height\":"
                << monitor.height
                << ','
                << "\"primary\":"
                << (
                    monitor.primary
                        ? "true"
                        : "false"
                )
                << '}';
        }

        std::cout << "],\"windows\":[";

        for (size_t index = 0; index < windows.size(); index++) {
            const auto& window = windows[index];

            if (index > 0) {
                std::cout << ',';
            }

            std::cout
                << '{'
                << "\"type\":\"window\","
                << "\"hwnd\":"
                << window.hwnd
                << ','
                << "\"pid\":"
                << window.pid
                << ','
                << "\"exe\":\""
                << json_escape(window.exeName)
                << "\","
                << "\"className\":\""
                << json_escape(window.className)
                << "\","
                << "\"title\":\""
                << json_escape(window.title)
                << "\""
                << '}';
        }

        std::cout << "]}\n";
        return 0;
    }

    std::cout
        << "\nAvailable capture sources:\n"
        << "\nMonitors:\n";

    for (const auto& monitor : monitors) {
        std::cout
            << "monitorIndex="
            << monitor.index
            << " name=\""
            << monitor.name
            << "\""
            << " pos="
            << monitor.x
            << ','
            << monitor.y
            << " size="
            << monitor.width
            << 'x'
            << monitor.height
            << " primary="
            << (
                monitor.primary
                    ? "true"
                    : "false"
            )
            << '\n';
    }

    std::cout << "\nWindows / Games:\n";
    printVisibleWindows();

    return 0;
}

static int run_obs_inventory_command()
{
    ObsEngine engine;

    ObsVideoConfig videoConfig;
    videoConfig.baseWidth = 1920;
    videoConfig.baseHeight = 1080;
    videoConfig.outputWidth = 1920;
    videoConfig.outputHeight = 1080;
    videoConfig.fps = 60;
    videoConfig.scaleFilter = "bicubic";

    if (!engine.initialize(
        get_runtime_dir(),
        videoConfig
    )) {
        std::cerr
            << "[Native Stream Engine] "
            << "failed to initialize OBS\n";

        engine.shutdown();
        return 1;
    }

    engine.shutdown();

    std::cerr
        << "[Native Stream Engine] "
        << "shutdown complete\n";

    return 0;
}

int main(
    int argc,
    char** argv
)
{
    if (argc < 2) {
        print_usage();
        return 0;
    }

    const std::string command =
        argv[1];

    const bool jsonOutput =
        command == "--list-sources" &&
        has_json_output_argument(
            argc,
            argv
        );

    if (
        command != "--service" &&
        !jsonOutput
    ) {
        std::cerr
            << "[Native Stream Engine] starting...\n";
    }

    if (command == "--service") {
        return runNativeService();
    }

    if (command == "--list-windows") {
        printVisibleWindows();
        return 0;
    }

    if (command == "--list-sources") {
        return run_list_sources_command(
            jsonOutput
        );
    }

    if (command == "--list") {
        return run_obs_inventory_command();
    }

    std::cerr
        << "[Native Stream Engine] unknown command: "
        << command
        << '\n';

    print_usage();
    return 1;
}
