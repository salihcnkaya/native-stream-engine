cmake_minimum_required(VERSION 3.24)

project(native_stream_engine LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR
        "native-stream-engine currently supports Windows only."
    )
endif()

if(NOT MSVC)
    message(FATAL_ERROR
        "native-stream-engine currently requires Microsoft Visual C++."
    )
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(
    OBS_SOURCE_DIR
    "${CMAKE_SOURCE_DIR}/deps/obs-studio-32.1.2"
)

set(
    OBS_RUNTIME_DIR
    "${CMAKE_SOURCE_DIR}/runtime/OBS-Studio-32.1.2-Windows-x64"
)

set(
    OBS_COMPATIBILITY_DIR
    "${CMAKE_SOURCE_DIR}/compatibility/obs-32.1.2"
)

set(OBS_HEADER "${OBS_SOURCE_DIR}/libobs/obs.h")
set(OBS_RUNTIME_DLL "${OBS_RUNTIME_DIR}/bin/64bit/obs.dll")
set(OBS_IMPORT_LIBRARY "${OBS_COMPATIBILITY_DIR}/obs.lib")
set(OBS_CONFIG_HEADER "${OBS_COMPATIBILITY_DIR}/obsconfig.h")

if(NOT EXISTS "${OBS_HEADER}")
    message(FATAL_ERROR
        "OBS Studio 32.1.2 source was not found.\n"
        "Expected file:\n"
        "  ${OBS_HEADER}"
    )
endif()

if(NOT EXISTS "${OBS_RUNTIME_DLL}")
    message(FATAL_ERROR
        "OBS Studio 32.1.2 Windows runtime was not found.\n"
        "Expected file:\n"
        "  ${OBS_RUNTIME_DLL}"
    )
endif()

if(NOT EXISTS "${OBS_IMPORT_LIBRARY}")
    message(FATAL_ERROR
        "OBS import library was not found.\n"
        "Expected file:\n"
        "  ${OBS_IMPORT_LIBRARY}\n\n"
        "Run:\n"
        "  .\\scripts\\make-obs-import-lib.ps1"
    )
endif()

if(NOT EXISTS "${OBS_CONFIG_HEADER}")
    message(FATAL_ERROR
        "OBS compatibility header was not found.\n"
        "Expected file:\n"
        "  ${OBS_CONFIG_HEADER}"
    )
endif()

add_executable(native-stream-engine
    src/main.cpp
    src/obs_engine.cpp
    src/window_utils.cpp
    src/wgc_capture.cpp
    src/native_wgc_source.cpp
    src/native_service.cpp
    src/realtime_rtp_sender.cpp
    src/rtp_pacer.cpp
    src/bitrate_controller.cpp
    src/bitrate_update_scheduler.cpp
    src/image_utils.cpp
    src/base64_utils.cpp
)

target_include_directories(
    native-stream-engine
    PRIVATE
        "${OBS_COMPATIBILITY_DIR}"
        "${OBS_SOURCE_DIR}/libobs"
)

if(MSVC)
    target_compile_options(
        native-stream-engine
        PRIVATE
            /W4
            /permissive-
            /external:anglebrackets
            /external:W0
    )
endif()

target_link_libraries(
    native-stream-engine
    PRIVATE
        "${OBS_IMPORT_LIBRARY}"
        RuntimeObject.lib
        windowsapp.lib
        d3d11.lib
        dxgi.lib
        Ws2_32
        windowscodecs
)

target_compile_definitions(
    native-stream-engine
    PRIVATE
        _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
)

add_custom_command(
    TARGET native-stream-engine
    POST_BUILD
    COMMAND
        ${CMAKE_COMMAND} -E copy_directory
        "${OBS_RUNTIME_DIR}/bin/64bit"
        "$<TARGET_FILE_DIR:native-stream-engine>"
)
