# native-stream-engine

![Platform](https://img.shields.io/badge/platform-Windows-blue)
![Language](https://img.shields.io/badge/C%2B%2B-17-blue)
![Build](https://img.shields.io/badge/build-CMake-green)
![License](https://img.shields.io/badge/license-GPL--2.0-red)
![libobs](https://img.shields.io/badge/libobs-32.1.2-orange)

High-performance native screen capture and RTP streaming engine built on top of libobs.

`native-stream-engine` is a Windows-native streaming backend designed for real-time communication applications.

Originally developed as the native streaming component of a Discord-style communication platform, it provides a lightweight service capable of capturing Windows applications, encoding video using hardware-accelerated H.264 encoders, streaming media over RTP, adapting bitrate to changing network conditions, and exposing a simple JSON-based IPC interface suitable for Electron or other desktop applications.

The project focuses on low-latency screen streaming while keeping the host application independent from OBS Studio internals. Communication between the host process and the engine is performed through line-delimited JSON messages over standard input and standard output, allowing the engine to run as a standalone background service.

> [!NOTE]
> This project currently targets Windows only and is designed to be embedded into desktop applications through a lightweight JSON IPC interface.

---

## Table of Contents

- [Features](#features)
- [Why native-stream-engine?](#why-native-stream-engine)
- [Architecture](#architecture)
- [Repository Structure](#repository-structure)
- [Supported Platform](#supported-platform)
- [Design Goals](#design-goals)
- [Capture Pipeline](#capture-pipeline)
- [Capture Sources](#capture-sources)
- [Encoder Selection](#encoder-selection)
- [Video Transport](#video-transport)
- [Audio Transport](#audio-transport)
- [Quality Presets](#quality-presets)
- [Adaptive Bitrate](#adaptive-bitrate)
- [Network Feedback](#network-feedback)
- [Running the Service](#running-the-service)
- [Command Line Interface](#command-line-interface)
- [IPC Overview](#ipc-overview)
- [IPC Commands](#ipc-commands)
- [Typical Session](#typical-session)
- [Embedding](#embedding)
- [Error Handling](#error-handling)
- [Threading Model](#threading-model)
- [Building](#building)
- [Dependencies](#dependencies)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [Documentation](#documentation)
- [License](#license)

---

## Features

Current capabilities include:

- Windows Graphics Capture (WGC)
- Window capture
- Monitor capture
- Hardware-accelerated H.264 encoding
- Automatic encoder selection
- NVIDIA NVENC support
- AMD AMF support
- Intel Quick Sync Video (QSV) support
- x264 software fallback
- RTP video streaming
- RTP audio streaming
- Adaptive bitrate control
- Live network feedback
- Runtime bitrate updates
- PNG thumbnail generation
- JSON IPC service
- Headless service mode
- Graceful startup and shutdown
- CMake build system
- C++17

---

## Why native-stream-engine?

Many modern communication platforms require a native media pipeline rather than relying exclusively on browser capture APIs.

This project was created to provide a reusable streaming backend that can be embedded into desktop applications while maintaining full control over:

- capture
- encoding
- transport
- bitrate adaptation
- process lifecycle
- host communication

Instead of exposing libobs directly to the application, the engine acts as an isolated process with a stable JSON protocol. This keeps the application architecture simpler while allowing the native component to evolve independently.

---

## Architecture

```text
               Host Application
          (Electron / Desktop App)
                      │
                      │ JSON IPC
                      ▼
          native-stream-engine
                      │
                      ▼
                  libobs
                      │
          ┌───────────┴───────────┐
          │                       │
          ▼                       ▼
   Capture Pipeline        Audio Pipeline
          │                       │
          └───────────┬───────────┘
                      ▼
              Encoder Selection
                      │
                      ▼
           RTP Video / RTP Audio
                      │
                      ▼
                  Network
```

The engine is designed as a separate executable rather than a shared library. This architecture isolates the native media stack from the host application and allows clean startup, shutdown, crash recovery, and independent versioning.

---

## Repository Structure

```text
native-stream-engine/
├── compatibility/
├── docs/
│   ├── ARCHITECTURE.md
│   └── BUILDING.md
├── scripts/
├── src/
├── CMakeLists.txt
├── BUILDING.md
├── COPYRIGHT
├── LICENSE
└── README.md
```

The following directories are intentionally excluded from version control:

```text
build/
deps/
runtime/
```

These directories contain generated build outputs or locally installed third-party dependencies.

---

## Supported Platform

The current implementation targets the following environment:

| Component     | Status    |
| ------------- | --------- |
| Windows       | Supported |
| x64           | Supported |
| MSVC          | Supported |
| CMake         | Supported |
| C++17         | Supported |
| libobs 32.1.2 | Supported |

The engine currently targets Windows only.

Support for additional operating systems may be introduced in future releases.

---

## Design Goals

The project follows several architectural principles:

- Keep the native engine independent from the host application.
- Communicate only through a documented JSON protocol.
- Minimize direct dependencies between the UI and the streaming backend.
- Prefer hardware acceleration whenever available.
- Fall back gracefully when hardware encoders are unavailable.
- Keep the capture pipeline deterministic and predictable.
- Support long-running background operation.
- Make build and runtime dependencies explicit.
- Avoid exposing unnecessary libobs implementation details.

These principles guide both the public API and the internal implementation.

---

## Capture Pipeline

The engine separates media acquisition, encoding, and transport into independent stages.

```text
Windows Graphics Capture
        │
        │
        ▼
 Frame Acquisition
        │
        ▼
 Resolution Scaling
        │
        ▼
 OBS Video Pipeline
        │
        ▼
 H.264 Encoder
        │
        ▼
 RTP Packetization
        │
        ▼
 UDP Transport
```

Each stage has a single responsibility, making the pipeline easier to maintain, debug, and extend.

The capture layer is responsible only for acquiring frames from the operating system. Scaling, encoding, and network transport are handled independently.

---

## Capture Sources

The engine currently supports two capture source types.

| Source  | Description                                                              |
| ------- | ------------------------------------------------------------------------ |
| Monitor | Captures an entire display using Windows Graphics Capture                |
| Window  | Captures an individual application window using Windows Graphics Capture |

The service can enumerate available capture sources before streaming begins, allowing the host application to present a source picker to the user.

---

## Encoder Selection

Hardware acceleration is preferred whenever it is available.

The engine automatically selects the best encoder supported by the current system unless the host explicitly requests one.

Current encoder families include:

| Encoder   | Hardware                  |
| --------- | ------------------------- |
| NVENC     | NVIDIA GPUs               |
| AMD AMF   | AMD GPUs                  |
| Intel QSV | Intel integrated graphics |
| x264      | Software fallback         |

When automatic selection is enabled, the engine attempts hardware encoders before falling back to software encoding.

```text
Requested Encoder
        │
        ▼
Hardware Available?
        │
   ┌────┴────┐
   │         │
 Yes         No
   │         │
   ▼         ▼
Hardware    x264
Encoder    Software
```

---

## Video Transport

Encoded H.264 frames are packetized into RTP before being transmitted.

```text
OBS Encoder
      │
      ▼
Encoded H.264
      │
      ▼
NAL Parser
      │
      ▼
RTP Packetizer
      │
      ▼
UDP Socket
```

The transport layer is intentionally separated from the encoder implementation, allowing the networking logic to evolve independently of the capture pipeline.

---

## Audio Transport

Audio follows an independent RTP pipeline.

```text
Audio Capture
      │
      ▼
Audio Encoder
      │
      ▼
RTP Audio Sender
      │
      ▼
UDP Socket
```

Separating video and audio transport simplifies synchronization and allows each stream to be managed independently.

---

## Quality Presets

The service currently exposes several predefined quality profiles.

| Preset  | Target                                                      |
| ------- | ----------------------------------------------------------- |
| 720p30  | 1280×720 @ 30 FPS                                           |
| 720p60  | 1280×720 @ 60 FPS                                           |
| 1080p30 | 1920×1080 @ 30 FPS                                          |
| 1080p60 | 1920×1080 @ 60 FPS                                          |
| source  | Native source resolution (subject to implementation limits) |

These presets provide predictable encoding behavior while keeping the IPC interface simple.

---

## Adaptive Bitrate

Network conditions rarely remain constant during a live stream.

Instead of using a fixed bitrate, the engine continuously evaluates runtime network feedback and adjusts the encoder target bitrate accordingly.

```text
Incoming Network Feedback
        │
        ▼
 Packet Loss
 RTT
 Jitter
 NACK
 PLI
 FIR
        │
        ▼
 Bitrate Controller
        │
        ▼
 Bitrate Scheduler
        │
        ▼
 Encoder Reconfiguration
```

The bitrate controller determines the desired target bitrate based on current network conditions.

The scheduler prevents unnecessary encoder reconfiguration by smoothing frequent bitrate fluctuations.

This approach improves stream stability while avoiding excessive encoder updates.

---

## Network Feedback

The engine accepts runtime feedback from the host application through the IPC interface.

The current feedback model includes:

- Packet loss ratio
- Round-trip time (RTT)
- Network jitter
- Estimated bitrate
- Packet count
- Byte count
- NACK statistics
- PLI count
- FIR count

These metrics are used by the adaptive bitrate controller to determine whether the encoder bitrate should be reduced, maintained, or gradually increased.

---

## Running the Service

The engine operates as a standalone background process.

Start the IPC service using:

```bash
native-stream-engine.exe --service
```

The service communicates with the host process through line-delimited JSON messages over standard input and standard output.

This design allows the engine to be embedded into Electron applications, desktop applications, or any other process capable of launching a child process.

---

## Command Line Interface

The executable exposes several utility commands.

| Command                   | Description                                 |
| ------------------------- | ------------------------------------------- |
| `--service`               | Starts the JSON IPC service                 |
| `--list`                  | Displays available OBS modules and encoders |
| `--list-windows`          | Enumerates visible desktop windows          |
| `--list-sources`          | Enumerates available capture sources        |
| `--list-sources --json 1` | Outputs capture sources as JSON             |

---

## IPC Overview

The service uses a simple request/response protocol.

Each request contains an identifier.

Every response includes the same identifier, allowing multiple asynchronous requests to be matched by the host application.

```text
Host Process
      │
      │ JSON
      ▼
native-stream-engine
      │
      │ JSON
      ▼
Host Process
```

Messages are encoded as UTF-8 JSON objects separated by newline characters.

---

## IPC Commands

### ping

Verifies that the service is alive.

Request

```json
{
	"id": 1,
	"type": "ping"
}
```

Response

```json
{
	"id": 1,
	"ok": true,
	"type": "pong"
}
```

---

### listSources

Returns all currently available capture sources.

Request

```json
{
	"id": 2,
	"type": "listSources"
}
```

Successful response

```json
{
  "id": 2,
  "ok": true,
  "type": "sources",
  "monitors": [...],
  "windows": [...]
}
```

The returned source identifiers can later be used when starting a capture session.

---

### startCapture

Starts a capture session.

Example request

```json
{
	"id": 3,
	"type": "startCapture",
	"capture": "window",
	"hwnd": 123456,
	"quality": "1080p60",
	"encoder": "auto",
	"rtpIp": "127.0.0.1",
	"rtpPort": 5004
}
```

Successful response

```json
{
	"id": 3,
	"ok": true,
	"type": "captureStarted"
}
```

If the requested capture cannot be started, the response contains `"ok": false` together with an error description.

---

### stopCapture

Stops the active capture session.

Request

```json
{
	"id": 4,
	"type": "stopCapture"
}
```

Response

```json
{
	"id": 4,
	"ok": true,
	"type": "captureStopped"
}
```

Stopping a capture releases the active media pipeline while keeping the service process alive.

---

### networkFeedback

Provides runtime network statistics used by the adaptive bitrate controller.

Example request

```json
{
	"id": 5,
	"type": "networkFeedback",
	"packetLossPermille": 12,
	"jitterMs": 6,
	"rttMs": 18,
	"score": 9,
	"bitrate": 4200000
}
```

Response

```json
{
	"id": 5,
	"ok": true,
	"type": "networkFeedbackAck"
}
```

The controller evaluates the supplied metrics and determines whether the encoder bitrate should be adjusted.

---

### setTargetBitrate

Overrides the encoder target bitrate.

Example request

```json
{
	"id": 6,
	"type": "setTargetBitrate",
	"targetBitrateBps": 4500000
}
```

Response

```json
{
	"id": 6,
	"ok": true,
	"type": "targetBitrateAck",
	"targetBitrateBps": 4500000
}
```

This command is primarily intended for host-controlled bitrate management.

---

### shutdown

Gracefully terminates the service.

Request

```json
{
	"id": 7,
	"type": "shutdown"
}
```

Response

```json
{
	"id": 7,
	"ok": true,
	"type": "shutdown_ack"
}
```

The engine releases active OBS resources before exiting.

---

## Typical Session

A typical interaction between the host application and the engine looks like this.

```text
Start Service
      │
      ▼
Ping
      │
      ▼
List Sources
      │
      ▼
User Selects Window
      │
      ▼
Start Capture
      │
      ▼
Receive RTP
      │
      ▼
Send Network Feedback
      │
      ▼
Adaptive Bitrate Updates
      │
      ▼
Stop Capture
      │
      ▼
Shutdown
```

---

## Embedding

The engine is intended to be launched as a child process.

A typical host application is responsible for:

- starting the executable
- writing JSON requests to stdin
- reading JSON responses from stdout
- handling asynchronous events
- forwarding network statistics
- stopping the process when no longer needed

The engine intentionally does not depend on Electron and may be integrated into any desktop application capable of spawning external processes.

---

## Error Handling

All IPC requests return a JSON response.

Every response contains:

- request identifier
- success flag
- response type

Additional error information is included when a request cannot be completed.

This keeps the protocol deterministic and simplifies host-side error handling.

---

## Threading Model

The service is designed around a long-running command loop.

Capture operations, encoder management, RTP transport, and resource cleanup are coordinated internally while presenting a synchronous request/response interface to the host application.

This allows the embedding application to remain unaware of the underlying native threading model.

---

## Building

Detailed build instructions are available in [BUILD Guide](docs/BUILDING.md).

The project uses CMake and targets modern MSVC toolchains with C++17.

---

## Dependencies

This project depends on the following major components.

| Dependency               | Purpose                               |
| ------------------------ | ------------------------------------- |
| libobs                   | Capture and media pipeline            |
| OBS Plugins              | Capture, encoder and platform modules |
| Windows Graphics Capture | Native screen capture                 |
| Windows Media Foundation | Platform media support                |
| WinSock                  | RTP transport                         |
| CMake                    | Build system                          |

The project intentionally keeps third-party dependencies to a minimum.

---

## Roadmap

The current implementation focuses on providing a stable Windows-native streaming backend.

Planned improvements include:

- [x] Windows Graphics Capture
- [x] Window capture
- [x] Monitor capture
- [x] Hardware H.264 encoding
- [x] Automatic encoder selection
- [x] RTP video transport
- [x] RTP audio transport
- [x] Adaptive bitrate controller
- [x] JSON IPC service
- [x] Headless service mode
- [ ] Multi-monitor synchronization improvements
- [ ] Additional codec support
- [ ] HEVC encoding
- [ ] AV1 encoding
- [ ] Linux support
- [ ] macOS support

The roadmap reflects current development priorities and may evolve over time.

---

## Contributing

Contributions are welcome.

Before opening a pull request, please:

- Keep changes focused and self-contained.
- Follow the existing coding style.
- Document new public functionality.
- Avoid introducing unnecessary third-party dependencies.
- Verify that the project builds successfully before submitting changes.

For significant architectural changes, opening an issue for discussion beforehand is recommended.

---

## Documentation

- 📖 [Architecture](docs/ARCHITECTURE.md)
- 🛠️ [Building Guide](docs/BUILDING.md)

---

## License

This repository is distributed under the terms of the GNU General Public License Version 2 (GPL-2.0).

See the [LICENSE](LICENSE) file for the complete license text.

---

## Third-Party Software

This project is built on top of **libobs**, which is developed and maintained by the OBS Studio project and licensed under the GNU GPL Version 2.

All respective copyrights remain with their original authors.

---

## Acknowledgements

This project would not be possible without the work of the OBS Studio contributors and the broader open-source community.

Special thanks to everyone involved in the development and maintenance of **libobs**.
