# native-stream-engine

![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux-blue)
![Language](https://img.shields.io/badge/C%2B%2B-17-blue)
![Build](https://img.shields.io/badge/build-CMake-green)
![License](https://img.shields.io/badge/license-GPL--2.0--or--later-red)
![libobs](https://img.shields.io/badge/libobs-32.1.2-orange)

High-performance native capture, encoding, and RTP streaming engine built on top of libobs.

`native-stream-engine` is a standalone native media service for low-latency desktop/application streaming. It is designed to be launched by a desktop host such as Electron and controlled through line-delimited JSON over standard input/output.

The current production implementation supports **Windows and Linux** with platform-specific capture/audio backends while sharing the same OBS, encoder, RTP, pacing, bitrate-control, and service layers.

> [!NOTE]
> This repository describes the current libobs-based production engine. macOS is not implemented in this codebase.

---

## Features

### Common

- Standalone JSON IPC service
- libobs 32.1.2 media pipeline
- H.264 hardware encoding
- NVIDIA NVENC
- Intel QSV
- AMD AMF through `obs-ffmpeg`
- Optional x264 when explicitly requested
- RTP video transport
- RTP Opus audio transport
- RTP/RTCP feedback handling
- RTX retransmission support
- TWCC RTP header extension
- Packet pacing and queue controls
- Runtime bitrate adaptation
- Runtime bitrate override
- PLI/FIR-driven keyframe handling
- Capture lifecycle monitoring
- Asynchronous/cancellable capture startup
- Deterministic OBS runtime build scripts
- CMake / C++17

### Windows

- Windows Graphics Capture (WGC)
- Window capture
- Monitor capture
- Visible-window enumeration
- WASAPI desktop audio
- WASAPI process audio
- D3D11 capture path

### Linux

- OBS PipeWire portal monitor/window capture
- PipeWire portal restore-token support
- PulseAudio desktop output capture
- PipeWire application audio capture
- Application-audio resolution using portal/application/process identity
- Linux-native RTP sender wake/socket path

---

## Architecture

```text
                       Host Application
                  (Electron / Desktop App)
                              │
                    line-delimited JSON
                              │
                              ▼
                    native-stream-engine
                              │
                  ┌───────────┴───────────┐
                  │                       │
                  ▼                       ▼
             NativeService             ObsEngine
                                          │
                       ┌──────────────────┼──────────────────┐
                       │                  │                  │
                       ▼                  ▼                  ▼
                 Video Capture       Audio Capture       libobs
                       │                  │                  │
                       └───────────┬──────┘                  │
                                   ▼                         │
                            H.264 / Opus Encoders ◄──────────┘
                                   │
                                   ▼
                           Native RTP Output
                                   │
                                   ▼
                         RealtimeRtpSender
                                   │
                      ┌────────────┼────────────┐
                      ▼            ▼            ▼
                    Pacer         RTX       RTCP/TWCC
                      │
                      ▼
                     UDP
```

The host application never links to libobs directly. The engine owns OBS startup, capture sources, encoders, encoded packet callbacks, transport, feedback, and cleanup.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the internal design.

---

## Platform support

| Area | Windows | Linux |
|---|---|---|
| Build | Supported | Supported |
| OBS runtime | Pinned source-built runtime | Pinned source-built runtime |
| Monitor capture | WGC | PipeWire portal |
| Window/application capture | WGC | PipeWire portal |
| Desktop audio | WASAPI output capture | PulseAudio output capture |
| Application/process audio | WASAPI process loopback | PipeWire application capture |
| Source enumeration CLI | Supported | Not implemented yet |
| NVENC | Supported when available | Supported when available |
| QSV | Supported when available | Supported when available |
| AMD AMF | Supported when OBS exposes the encoder | Platform/runtime dependent |
| x264 | Explicit request only | Explicit request only |
| macOS | Not implemented | Not implemented |

### Current Linux limitation

`--list-sources` does not yet enumerate Linux portal sources. On Linux it returns empty `monitors` and `windows` arrays in JSON mode. Capture selection is handled through the PipeWire/desktop portal flow during capture creation.

---

## Capture paths

### Windows video

```text
Selected HWND / monitor
        │
        ▼
Windows Graphics Capture
        │
        ▼
D3D11 texture
        │
        ▼
Native WGC OBS source
        │
        ▼
OBS scene / video pipeline
        │
        ▼
H.264 encoder
```

### Windows audio

For monitor/desktop capture:

```text
Default output device
        │
        ▼
WASAPI output capture
        │
        ▼
OBS audio
        │
        ▼
Opus
```

For window/game capture:

```text
Selected window
        │
        ▼
OBS WASAPI process output capture
        │
        ▼
OBS audio
        │
        ▼
Opus
```

When `audio=auto`, Windows uses desktop audio for monitor capture and first attempts process audio for a non-monitor capture. If automatic process audio creation fails, the service can fall back to desktop audio.

### Linux video

```text
Portal capture request
        │
        ▼
OBS linux-pipewire source
        │
        ▼
PipeWire stream
        │
        ▼
OBS scene / video pipeline
        │
        ▼
H.264 encoder
```

The Linux OBS patch exposes the portal restore token through a private source setting so the engine can use portal/application identity during capture and audio resolution.

### Linux audio

Desktop:

```text
Default PulseAudio/PipeWire-compatible output
        │
        ▼
pulse_output_capture
        │
        ▼
Opus
```

Application:

```text
Portal/application identity
        │
        ▼
PipeWire application resolution
        │
        ▼
pipewire_audio_application_capture
        │
        ▼
Opus
```

If Linux `audio=auto` cannot resolve application audio for a non-monitor capture, the current service can continue video-only rather than failing the entire stream.

---

## Encoder selection

The engine discovers encoder IDs registered by the runtime and builds an ordered candidate list.

### `encoder=auto`

Current automatic order:

1. NVIDIA NVENC
2. Intel QSV
3. AMD AMF

`auto` does **not** automatically append x264.

### Explicit requests

- `nvenc`: NVENC → QSV → AMF fallback order
- `qsv`: QSV → NVENC → AMF fallback order
- `amd` / `amf`: AMF → NVENC → QSV fallback order
- `x264`: x264 only

The selected H.264 configuration is optimized for interactive streaming:

- CBR
- 2-second keyframe interval
- baseline profile
- B-frames disabled
- low-latency NVENC tuning
- repeat headers enabled

The pinned OBS patches reduce unnecessary encoder resets during runtime bitrate and keyframe handling.

---

## Quality presets

The service accepts these quality names:

| Preset | Output | FPS | Initial video bitrate |
|---|---:|---:|---:|
| `720p30` | 1280×720 | 30 | 2500 kbps |
| `720p60` | 1280×720 | 60 | 4500 kbps |
| `1080p30` | 1920×1080 | 30 | 5500 kbps |
| `1080p60` | 1920×1080 | 60 | 8000 kbps |

Audio uses OBS `ffmpeg_opus` at 160 kbps when audio RTP is enabled.

---

## RTP transport

Encoded packets leave OBS through a small custom encoded-output adapter:

- `native_rtp_video_output` for video-only sessions
- `native_rtp_av_output` for video + audio sessions

The adapter forwards encoded packets to the engine-owned transport instead of using an OBS network output.

### Video transport

The RTP sender includes:

- H.264 NAL packetization
- fragmentation for large NAL units
- bounded media queue
- adaptive packet pacing
- queue-latency telemetry
- TWCC RTP header extension
- RTCP receive path
- NACK handling
- packet history
- RTX retransmissions
- duplicate RTX suppression
- bounded pending retransmission queue
- retransmission rate limiting
- fair servicing of fresh media and retransmissions

### Audio transport

Opus is sent over a separate RTP sender with its own destination, payload type, SSRC, pacing state, and counters.

---

## Network feedback and bitrate control

The host can send runtime network feedback to the service.

Current feedback fields include:

- packet loss
- jitter
- optional RTT
- score
- receiver-estimated bitrate
- packet count
- byte count
- NACK count
- NACK packet count
- PLI count
- FIR count
- keyframe reason

The feedback path is:

```text
Host feedback
     │
     ▼
NativeService
     │
     ▼
ObsEngine
     │
     ├──────────────► keyframe policy
     │
     ▼
RtpPacer / BitrateController
     │
     ▼
BitrateUpdateScheduler
     │
     ▼
OBS encoder update
```

The controller and scheduler avoid applying every small feedback fluctuation directly to the encoder.

---

## Capture lifecycle

Capture startup is asynchronous.

A normal successful start is:

```text
startCapture request
        │
        ▼
captureStarting
        │
        ▼
capture/source initialization
        │
        ▼
first frame confirmation
        │
        ▼
audio initialization
        │
        ▼
RTP initialization
        │
        ▼
captureStartResult (ok=true)
```

`stopCapture` can cancel an in-progress start. The service also watches the active capture target and can emit `captureEnded` when the target closes.

Shutdown performs explicit cleanup of RTP, encoders, sources, scenes, OBS state, and service threads.

---

## Command line

### Windows

```powershell
.\build-windows-test\Release\native-stream-engine.exe --service
.\build-windows-test\Release\native-stream-engine.exe --list
.\build-windows-test\Release\native-stream-engine.exe --list-windows
.\build-windows-test\Release\native-stream-engine.exe --list-sources --json 1
```

### Linux

```bash
./build-linux-nodeps-test/native-stream-engine --service
./build-linux-nodeps-test/native-stream-engine --list
./build-linux-nodeps-test/native-stream-engine --list-sources --json 1
```

`--list-windows` is Windows-only.

`--list` initializes the configured OBS runtime, prints runtime/module/encoder information, and shuts down.

---

## IPC commands and events

The service uses newline-delimited UTF-8 JSON.

Current request/response/event types include:

- `ping` / `pong`
- `listSources` / `sources`
- `capturePreview` / `capturePreviewResult`
- `startCapture`
- `captureStarting`
- `captureStartResult`
- `stopCapture` / `captureStopped`
- `captureEnded`
- `setTargetBitrate` / `targetBitrateAck`
- `networkFeedback` / `networkFeedbackAck`
- `networkFeedbackIgnored`
- `simulateNetworkFeedback` / `simulateNetworkFeedbackAck`
- `shutdown` / `shutdown_ack`

### Example: start capture

```json
{
  "id": 1,
  "type": "startCapture",
  "capture": "window",
  "hwnd": 123456,
  "quality": "1080p60",
  "audio": "auto",
  "encoder": "auto",
  "rtpIp": "127.0.0.1",
  "rtpPort": 5004,
  "payloadType": 102,
  "ssrc": 287454020,
  "rtxSsrc": 1432778632,
  "rtxPayloadType": 103,
  "audioRtpIp": "127.0.0.1",
  "audioRtpPort": 5006,
  "audioPayloadType": 111,
  "audioSsrc": 573785173
}
```

For Linux application audio the host may also provide `audioTarget`. When omitted, the engine can use the application identity resolved from the active portal capture where available.

---

## Repository structure

```text
native-stream-engine/
├── compatibility/
│   └── obs-32.1.2/
│       ├── obs.lib
│       └── obsconfig.h
├── docs/
│   ├── ARCHITECTURE.md
│   └── BUILDING.md
├── patches/
│   └── obs/
├── scripts/
│   ├── build-obs-linux-runtime.sh
│   └── build-obs-windows-runtime.ps1
├── src/
├── CMakeLists.txt
├── COPYRIGHT
├── LICENSE
└── README.md
```

Generated/local directories such as `runtime/`, build directories, temporary OBS source/build trees, and capture/debug output are not intended to be committed.

---

## OBS runtime model

OBS source is not vendored into this repository.

Both runtime builders clone **OBS Studio 32.1.2** and verify the exact commit:

```text
fb4d98bf88fae5fc85cb11fc57f7c5e309282194
```

They then apply native-stream-engine patches before building the required subset of OBS.

Current patches:

- `common-nvenc-lightweight-reconfig.patch`
- `common-qsv-skip-noop-reconfig.patch`
- `linux-pipewire-expose-restore-token.patch`

See [patches/obs/README.md](patches/obs/README.md).

The old manual `obs.def` / import-library generation workflow is no longer used. The Windows runtime builder publishes the official `obs.lib` and generated `obsconfig.h` from the same pinned OBS build.

---

## Building

See [docs/BUILDING.md](docs/BUILDING.md).

Short version:

### Windows

```powershell
.\scripts\build-obs-windows-runtime.ps1

cmake -S . -B build-windows-test -G "Visual Studio 17 2022" -A x64
cmake --build build-windows-test --config Release -j

.\build-windows-test\Release\native-stream-engine.exe --list
```

### Linux

```bash
./scripts/build-obs-linux-runtime.sh

cmake -S . -B build-linux-nodeps-test -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux-nodeps-test -j"$(nproc)"

./build-linux-nodeps-test/native-stream-engine --list
```

---

## Runtime directories

Generated OBS runtimes live under:

```text
runtime/OBS-Studio-32.1.2-Windows-x64
runtime/OBS-Studio-32.1.2-Linux-x86_64
```

They are generated build/runtime artifacts and are not intended to be versioned in Git.

---

## License

`native-stream-engine` is distributed under GPL-2.0-or-later. See [LICENSE](LICENSE) and [COPYRIGHT](COPYRIGHT).

The project links against and builds components from OBS Studio/libobs. Third-party components retain their respective licenses and copyrights.

---

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Building](docs/BUILDING.md)
- [OBS patches](patches/obs/README.md)
