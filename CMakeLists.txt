cmake_minimum_required(VERSION 3.24)

project(native_stream_engine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

IF(WIN32) 

    if(NOT MSVC)
        message(FATAL_ERROR 
            "Windows build currently requires Microsoft Visual C++."
        )
    endif()

    set(
        OBS_RUNTIME_DIR
        "${CMAKE_SOURCE_DIR}/runtime/OBS-Studio-32.1.2-Windows-x64"
    )

    set(
        OBS_COMPATIBILITY_DIR
        "${CMAKE_SOURCE_DIR}/compatibility/obs-32.1.2"
    )

    set(OBS_RUNTIME_DLL "${OBS_RUNTIME_DIR}/bin/64bit/obs.dll")
    set(OBS_IMPORT_LIBRARY "${OBS_COMPATIBILITY_DIR}/obs.lib")
    set(OBS_CONFIG_HEADER "${OBS_COMPATIBILITY_DIR}/obsconfig.h")

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
            "Build the Windows OBS runtime first:\n"
            "  .\\scripts\\build-obs-windows-runtime.ps1"
        )
    endif()

    if(NOT EXISTS "${OBS_CONFIG_HEADER}")
        message(FATAL_ERROR
            "OBS compatibility header was not found.\n"
            "Expected file:\n"
            "  ${OBS_CONFIG_HEADER}"
        )
    endif()

elseif(UNIX AND NOT APPLE)

    set(
        OBS_RUNTIME_DIR
        "${CMAKE_SOURCE_DIR}/runtime/OBS-Studio-32.1.2-Linux-x86_64"
    )

    find_package(PkgConfig REQUIRED)
    find_package(
        Qt6
        REQUIRED
        COMPONENTS
            Core
            DBus
    )

    pkg_check_modules(
        GLIB
        REQUIRED
        IMPORTED_TARGET
        glib-2.0
        gio-2.0
    )

    pkg_check_modules(
        PIPEWIRE
        REQUIRED
        IMPORTED_TARGET
        libpipewire-0.3
    )

    set(
        OBS_CONFIG_HEADER
        "${OBS_RUNTIME_DIR}/include/obsconfig.h"
    )

    set(
        OBS_SHARED_LIBRARY
        "${OBS_RUNTIME_DIR}/lib/libobs.so"
    )

    if(NOT EXISTS "${OBS_CONFIG_HEADER}")
        message(FATAL_ERROR
            "OBS Linux configuration header was not found.\n"
            "Expected file:\n"
            "  ${OBS_CONFIG_HEADER}\n\n"
            "Run:\n"
            "  ./scripts/build-obs-linux-runtime.sh"
        )
    endif()

    if(NOT EXISTS "${OBS_SHARED_LIBRARY}")
        message(FATAL_ERROR
            "OBS Linux shared library was not found.\n"
            "Expected file:\n"
            "  ${OBS_SHARED_LIBRARY}\n\n"
            "Run:\n"
            "  ./scripts/build-obs-linux-runtime.sh"
        )
    endif()

    message(STATUS "Configuring native-stream-engine for Linux")
    message(STATUS "OBS runtime: ${OBS_RUNTIME_DIR}")
    message(STATUS "OBS library: ${OBS_SHARED_LIBRARY}")

else()

    message(FATAL_ERROR
        "Unsupported platform."
    )

endif()

set(NSE_COMMON_SOURCES
    src/main.cpp
    src/native_service.cpp
    src/obs_engine.cpp
    src/realtime_rtp_sender.cpp
    src/rtp_output.cpp
    src/rtp_pacer.cpp
    src/bitrate_controller.cpp
    src/bitrate_update_scheduler.cpp
    src/base64_utils.cpp
)

set(NSE_PLATFORM_SOURCES)

if(WIN32)
    list(APPEND NSE_PLATFORM_SOURCES
        src/window_utils.cpp
        src/wgc_capture.cpp
        src/native_wgc_source.cpp
        src/image_utils.cpp
    )
elseif(UNIX AND NOT APPLE)
    list(APPEND NSE_PLATFORM_SOURCES
        src/linux_source_utils.cpp
    )
endif()

add_executable(native-stream-engine
    ${NSE_COMMON_SOURCES}
    ${NSE_PLATFORM_SOURCES}
)

if(WIN32)

    target_include_directories(
        native-stream-engine
        PRIVATE
            "${OBS_RUNTIME_DIR}/include"
    )

elseif(UNIX AND NOT APPLE)

    target_include_directories(
        native-stream-engine
        PRIVATE
            "${OBS_RUNTIME_DIR}/include"
    )

endif()

if(MSVC)
    target_compile_options(
        native-stream-engine
        PRIVATE
            /W4
            /permissive-
            /external:anglebrackets
            /external:W0

            $<$<CONFIG:Release>:/GL>
    )

    target_link_options(
        native-stream-engine
        PRIVATE
            $<$<CONFIG:Release>:/LTCG>
            $<$<CONFIG:Release>:/OPT:REF>
            $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()

if(WIN32)

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

elseif(UNIX AND NOT APPLE)

    target_link_libraries(
        native-stream-engine
        PRIVATE
            "${OBS_SHARED_LIBRARY}"
            PkgConfig::GLIB
            PkgConfig::PIPEWIRE
            Qt6::Core
            Qt6::DBus
    )

    set_target_properties(
        native-stream-engine
        PROPERTIES
            BUILD_RPATH "${OBS_RUNTIME_DIR}/lib"
    )

endif()

target_compile_definitions(
    native-stream-engine
    PRIVATE
        _SILENCE_EXPERIMENTAL_COROUTINE_DEPRECATION_WARNINGS
        $<$<PLATFORM_ID:Windows>:NOMINMAX>
)

if(WIN32)

    add_custom_command(
        TARGET native-stream-engine
        POST_BUILD

        COMMAND
            ${CMAKE_COMMAND} -E copy_directory
            "${OBS_RUNTIME_DIR}/bin/64bit"
            "$<TARGET_FILE_DIR:native-stream-engine>"

        COMMAND
            ${CMAKE_COMMAND} -E copy_directory
            "${OBS_RUNTIME_DIR}/obs-plugins"
            "$<TARGET_FILE_DIR:native-stream-engine>/obs-plugins"

        COMMAND
            ${CMAKE_COMMAND} -E copy_directory
            "${OBS_RUNTIME_DIR}/data"
            "$<TARGET_FILE_DIR:native-stream-engine>/data"
    )

elseif(UNIX AND NOT APPLE)

    add_custom_command(
        TARGET native-stream-engine
        POST_BUILD
        COMMAND
            ${CMAKE_COMMAND} -E copy_if_different
            "${OBS_RUNTIME_DIR}/bin/obs-nvenc-test"
            "$<TARGET_FILE_DIR:native-stream-engine>/obs-nvenc-test"
    )

endif()
