# native-stream-engine architecture

This document describes the current **production libobs-based engine** as represented by the active source tree. It covers Windows and Linux, the media lifecycle, RTP transport, adaptive bitrate logic, and the boundary between platform-specific capture code and shared transport/service code.

---

## 1. Design goals

The production architecture is built around these rules:

1. **Process isolation**  
   The engine runs as a standalone executable. The host application does not link to libobs.

2. **Stable host protocol**  
   The host controls the engine with newline-delimited JSON over stdin/stdout.

3. **Shared transport, platform-specific capture**  
   Windows and Linux use different capture/audio integrations, but share OBS orchestration, encoder selection, RTP, pacing, feedback, and lifecycle code.

4. **Interactive latency over recording efficiency**  
   Encoder settings, packetization, pacing, and feedback handling are tuned for real-time communication.

5. **Deterministic dependency/runtime inputs**  
   OBS is pinned to a specific version and commit and is built through repository scripts with a controlled plugin graph and local patches.

6. **Explicit cleanup**  
   Capture sessions, encoders, RTP senders, sources, scenes, and OBS runtime state have explicit stop/cleanup paths.

---

## 2. High-level process architecture

```text
Host application
     │
     │ stdin/stdout JSON
     ▼
NativeService
     │
     ├──────── source/preview requests
     ├──────── capture lifecycle
     ├──────── runtime bitrate override
     ├──────── network feedback
     └──────── shutdown
     │
     ▼
ObsEngine
     │
     ├──────── platform video capture
     ├──────── platform audio capture
     ├──────── OBS scene/video/audio
     ├──────── encoder selection
     └──────── custom encoded OBS output
     │
     ▼
Native RTP output adapter
     │
     ▼
RealtimeRtpSender
     │
     ├──────── H.264 / Opus RTP
     ├──────── packet pacing
     ├──────── RTCP receive
     ├──────── NACK → RTX
     ├──────── TWCC extension
     └──────── telemetry
     │
     ▼
UDP network
```

---

## 3. Source layout and responsibilities

### `src/main.cpp`

Owns command-line dispatch.

Current commands:

- `--service`
- `--list`
- `--list-windows`
- `--list-sources [--json 1]`

Platform notes:

- `--list-windows` is Windows-only.
- Windows `--list-sources` enumerates monitors and visible windows.
- Linux `--list-sources` is not implemented yet and returns empty arrays in JSON mode.
- `--list` initializes OBS against the platform runtime and prints the available runtime/module/encoder inventory.

### `src/native_service.cpp`

Owns the long-running JSON command loop and session-level state.

Responsibilities include:

- parsing request IDs/types
- listing sources
- capture preview
- asynchronous capture startup
- capture cancellation
- first-frame validation
- audio-selection policy
- RTP startup
- target-close monitoring
- capture stop
- runtime bitrate updates
- network feedback
- orderly shutdown

Important session state is protected by atomics/mutexes instead of relying on a single synchronous capture-start call.

### `src/obs_engine.*`

Owns libobs lifecycle and the active media graph.

Responsibilities include:

- OBS startup/shutdown
- module loading
- video/audio reset
- capture scene creation
- platform capture source creation
- audio source creation
- encoder discovery/selection
- encoded output creation
- bitrate/keyframe updates
- RTP sender integration
- cleanup and unstable-state detection

### `src/rtp_output.*`

Registers two small custom OBS output types:

- `native_rtp_video_output`
- `native_rtp_av_output`

These outputs do not implement network transport themselves. They receive encoded OBS packets and forward them to the engine packet handler.

This is the boundary between the OBS encoder pipeline and the engine-owned RTP implementation.

### `src/realtime_rtp_sender.*`

Owns actual RTP/UDP transport.

Responsibilities include:

- RTP header generation
- sequence numbers
- SSRC/payload type
- H.264 NAL handling
- fragmentation
- TWCC header extension
- bounded send queue
- sender thread
- packet history
- RTX generation
- RTCP receive
- NACK parsing
- retransmission scheduling
- telemetry

It has Windows and Linux-specific wake/socket primitives behind the same sender behavior.

### `src/rtp_pacer.*`

Owns send timing and pacing state.

Responsibilities include:

- initial/applied bitrate tracking
- queue-depth-sensitive packet spacing
- adaptive pacing pressure from network feedback
- rate/burst control
- pacing telemetry
- integration with `BitrateController`

### `src/bitrate_controller.*`

Converts network conditions into a desired bitrate decision.

### `src/bitrate_update_scheduler.*`

Controls when bitrate decisions are actually applied to the video encoder, avoiding excessive encoder updates.

### `src/network_feedback.h`

Defines the host feedback data passed through the service:

- loss ratio
- jitter
- optional RTT
- quality score
- receiver bitrate
- packet/byte counters
- NACK counters
- PLI/FIR counters
- keyframe request metadata

### Windows-specific files

- `window_utils.*`
- `wgc_capture.*`
- `native_wgc_source.*`
- `image_utils.*`

These own Windows source/window information, WGC setup, D3D11 frame transfer into the OBS source, and Windows image/preview utilities.

### Linux-specific files

- `linux_source_utils.*`

These own Linux application/audio identity resolution and portal/window/process association helpers used by the Linux PipeWire path.

---

## 4. OBS runtime architecture

OBS source is not vendored into the repository.

The runtime builders use:

```text
OBS Studio version: 32.1.2
Expected commit:
fb4d98bf88fae5fc85cb11fc57f7c5e309282194
```

The build scripts fail if the checked-out tag resolves to another commit.

### Common OBS patches

#### `common-nvenc-lightweight-reconfig.patch`

Adds native-stream-engine-specific private settings used to reduce unnecessary NVENC resets during bitrate updates and keyframe handling.

Private keys:

```text
__nse_lightweight
__nse_force_idr_only
```

#### `common-qsv-skip-noop-reconfig.patch`

Avoids unnecessary QSV encoder reset work when the effective bitrate did not change.

### Linux-only OBS patch

#### `linux-pipewire-expose-restore-token.patch`

Exposes the desktop portal restore token through a private OBS source setting:

```text
__nse_linux_restore_token
```

The engine uses portal identity/restore information to improve Linux application matching and application-audio resolution.

---

## 5. Windows capture architecture

### 5.1 Source discovery

Windows can enumerate:

- monitors
- visible top-level windows

Window metadata includes:

- HWND
- PID
- executable
- window class
- title

### 5.2 Video path

```text
HWND / monitor index
        │
        ▼
WGC target configuration
        │
        ▼
Windows.Graphics.Capture
        │
        ▼
D3D11 texture/frame
        │
        ▼
Native WGC OBS source
        │
        ▼
OBS scene
        │
        ▼
OBS video pipeline
```

The custom WGC source keeps OS-native capture details outside the host application while still presenting a normal OBS source to the engine scene.

### 5.3 First-frame gating

The service does not treat source creation alone as a successful capture.

After the scene is created it waits for a valid capture frame and dimensions before starting the final media/RTP path.

Current Windows first-frame wait is short compared with Linux because Windows WGC does not require an interactive portal flow.

### 5.4 Windows audio

Desktop audio uses:

```text
wasapi_output_capture
```

Process audio uses:

```text
wasapi_process_output_capture
```

The process-audio target is derived from the selected window identity.

Current `audio=auto` behavior:

- monitor capture → desktop audio
- non-monitor capture → process audio first
- Windows automatic process-audio failure → desktop audio fallback

---

## 6. Linux capture architecture

### 6.1 Video source

Linux uses OBS PipeWire desktop-capture sources.

The engine distinguishes monitor/window capture intent and creates the appropriate OBS PipeWire source. The desktop portal can be interactive, so Linux capture startup allows a substantially longer first-frame wait.

### 6.2 Portal identity

The Linux path observes private/source settings including portal identity and restore-token data.

The engine uses these values to resolve:

- portal application identity
- desktop entry identity
- executable identity
- portal window PID where possible

This information is also used by application audio matching.

### 6.3 Linux source enumeration limitation

The CLI/source-service path does not yet expose a real pre-capture Linux monitor/window list.

`--list-sources --json 1` currently returns:

```json
{"monitors":[],"windows":[]}
```

The actual source choice occurs through the portal capture flow.

### 6.4 Linux desktop audio

Desktop audio uses:

```text
pulse_output_capture
```

This works against the default output device through the system PulseAudio/PipeWire compatibility layer.

### 6.5 Linux application audio

The runtime includes `obs-pipewire-audio-capture`.

The engine uses:

```text
pipewire_audio_application_capture
```

with a resolved `TargetName`.

Application resolution can use:

- explicit `audioTarget` from the host
- portal/window PID match
- executable identity
- desktop/portal application identity
- unique game candidate fallback
- unique Wine candidate fallback

Ambiguous matches are rejected instead of silently selecting an arbitrary process.

For `audio=auto`, if application audio cannot be resolved for a non-monitor capture, Linux can continue the stream video-only.

---

## 7. Video configuration and quality

The service supports:

```text
720p30
720p60
1080p30
1080p60
```

Current initial bitrates:

```text
720p30   2500 kbps
720p60   4500 kbps
1080p30  5500 kbps
1080p60  8000 kbps
```

OBS video output is configured around the selected preset. Scaling is handled inside the OBS video pipeline.

---

## 8. Encoder architecture

Encoder selection is based on encoders actually registered by the loaded runtime.

### NVENC candidates

In order:

```text
obs_nvenc_h264_tex
obs_nvenc_h264_cuda
obs_nvenc_h264
```

when present.

### QSV candidates

```text
obs_qsv11_v2
obs_qsv11
```

when present.

### AMD candidates

```text
h264_texture_amf
h264_fallback_amf
```

when present.

### x264

```text
obs_x264
```

### Request policy

`auto`:

```text
NVENC → QSV → AMF
```

Explicit `nvenc`:

```text
NVENC → QSV → AMF
```

Explicit `qsv`:

```text
QSV → NVENC → AMF
```

Explicit `amd` / `amf`:

```text
AMF → NVENC → QSV
```

Explicit `x264`:

```text
x264 only
```

x264 is therefore available but is not the automatic last-resort candidate in the current production code.

### Common H.264 settings

- CBR
- 2 second keyframe interval
- baseline profile
- B-frames = 0
- repeated headers

NVENC additionally uses low-latency-oriented settings such as `p2`, `ll`, disabled lookahead, and disabled multipass.

---

## 9. Audio encoder architecture

OBS audio is initialized at:

```text
48 kHz
stereo
```

When audio RTP is enabled, the engine creates:

```text
ffmpeg_opus
```

with:

```text
160 kbps
```

The resulting encoded audio packets are forwarded through `native_rtp_av_output` to the separate audio RTP sender.

---

## 10. Encoded packet bridge

The engine deliberately does not use a normal OBS streaming/network output.

Instead:

```text
OBS encoder
    │
    ▼
native_rtp_video_output / native_rtp_av_output
    │
    ▼
encoded_packet callback
    │
    ▼
native packet handler
    │
    ├──────── video → H.264 RTP sender
    └──────── audio → Opus RTP sender
```

This keeps networking under native-stream-engine control.

---

## 11. RTP sender architecture

### 11.1 Packet size and queueing

The sender uses bounded in-memory queues and a fixed maximum RTP packet size. Large H.264 NAL units are fragmented before enqueue.

A dedicated sender loop drains queued media according to pacer decisions.

### 11.2 TWCC

Video RTP includes a transport-wide sequence number extension.

Current extension ID:

```text
5
```

TWCC sequence generation is owned by the RTP sender.

### 11.3 Packet history and RTX

The video sender stores recently transmitted RTP packets in a bounded history.

On RTCP NACK:

1. requested original RTP sequence numbers are parsed,
2. duplicate/too-frequent retransmission requests can be suppressed,
3. a bounded retransmission request is queued,
4. the original packet is rebuilt as RTX,
5. the original sequence number is placed in the RTX payload,
6. packet is transmitted using the configured RTX SSRC/PT.

Retransmission telemetry is kept separate from fresh-media counters.

### 11.4 Fresh-media fairness

RTX is not allowed to permanently starve fresh video. The sender services retransmission work while continuing to drain normal media.

### 11.5 Cross-platform wait model

Windows uses native event/socket wait objects.

Linux uses Unix descriptors/event wake mechanisms.

This difference is contained inside `RealtimeRtpSender`; the packetization/pacing behavior remains shared.

---

## 12. Pacing architecture

`RtpPacer` combines:

- target bitrate
- queue depth
- feedback state
- packet spacing
- rate/burst control

Video spacing adapts at queue-depth thresholds while audio uses its own lower spacing values.

The pacer records baseline and runtime telemetry including:

- feedback presence
- loss
- jitter
- RTT
- score
- bitrate
- packet counters
- current target bitrate
- adaptive pacing state

---

## 13. Network feedback and bitrate adaptation

### 13.1 Host feedback

`networkFeedback` is parsed into `NetworkFeedback`.

Fields include:

```text
packetLossPermille
jitterMs
hasRtt
rttMs
score
bitrate
packetCount
byteCount
nackCount
nackPacketCount
pliCount
firCount
keyframeReason
```

### 13.2 Flow

```text
networkFeedback
      │
      ▼
NativeService
      │
      ▼
ObsEngine::updateNetworkFeedback()
      │
      ├──────── keyframe handling
      │
      ▼
RealtimeRtpSender / RtpPacer
      │
      ▼
BitrateController
      │
      ▼
BitrateDecision
      │
      ▼
BitrateUpdateScheduler
      │
      ▼
ObsEngine::setTargetBitrate()
      │
      ▼
obs_encoder_update()
```

### 13.3 Lightweight encoder changes

The OBS patch layer exists because an encoder update can otherwise perform work heavier than native-stream-engine needs.

NVENC has a patched lightweight path.

QSV skips no-op reconfiguration where possible.

AMD AMF currently requires no native-stream-engine OBS source patch.

---

## 14. Capture service lifecycle

### 14.1 Idle

The service waits for JSON input and owns no active capture session.

### 14.2 `startCapture`

The request is validated first:

- quality
- capture type
- target HWND where required
- RTP settings
- audio settings

The service then starts capture work asynchronously and immediately emits:

```text
captureStarting
```

The worker:

1. validates/waits for the target,
2. creates or obtains the OBS engine,
3. configures video,
4. creates the capture scene,
5. waits for the first valid capture frame,
6. creates audio if requested,
7. creates encoders and RTP senders,
8. marks the session active,
9. emits `captureStartResult`.

### 14.3 Cancellation

If `stopCapture` arrives while startup is still in progress, `g_cancelRequested` is set.

Longer waits, including Linux first-frame wait, consult the cancellation flag.

### 14.4 Active target watcher

The service runs a watcher for the capture target.

When the active target closes, the service can clean the media state and emit:

```text
captureEnded
```

The notification is guarded to avoid duplicate target-ended events.

### 14.5 Stop

`stopCapture`:

- cancels startup if needed,
- stops RTP,
- releases session encoders/output,
- clears audio/capture scene state,
- returns the service to idle where possible.

### 14.6 Unstable OBS state

The engine tracks conditions where normal preview/capture cleanup cannot safely return OBS to a reusable state.

In those cases the service can reject another operation and require an engine/service restart rather than continuing with partially invalid state.

### 14.7 Shutdown

The service:

1. acknowledges shutdown,
2. requests cancellation,
3. waits for an in-flight start worker for a bounded time,
4. performs engine cleanup,
5. joins service watcher state,
6. exits.

A fail-safe process exit exists if an in-flight startup worker cannot be safely joined within the shutdown timeout.

---

## 15. IPC model

Messages are newline-delimited JSON objects.

Important request/event types:

```text
ping
pong
listSources
sources
capturePreview
capturePreviewResult
startCapture
captureStarting
captureStartResult
stopCapture
captureStopped
captureEnded
setTargetBitrate
targetBitrateAck
networkFeedback
networkFeedbackAck
networkFeedbackIgnored
simulateNetworkFeedback
simulateNetworkFeedbackAck
shutdown
shutdown_ack
```

The service preserves request IDs in responses where applicable.

---

## 16. Runtime ownership

### Long-lived/service resources

Depending on service state:

- OBS runtime
- module registrations
- shared engine object
- service/watcher state

### Capture-session resources

Created and released per capture:

- scene
- capture source
- audio source
- video encoder
- audio encoder
- native RTP output
- video sender
- audio sender
- capture target tracking

The implementation intentionally centralizes cleanup because OBS objects have ownership/reference-count interactions that are unsafe to tear down in arbitrary order.

---

## 17. Build/runtime boundary

The repository tracks:

- engine source
- OBS patches
- deterministic runtime builders
- Windows compatibility `obs.lib`
- Windows generated `obsconfig.h`

The repository does not vendor the full OBS source or generated platform runtimes.

Runtime locations:

```text
runtime/OBS-Studio-32.1.2-Windows-x64
runtime/OBS-Studio-32.1.2-Linux-x86_64
```

Windows CMake links against:

```text
compatibility/obs-32.1.2/obs.lib
```

and uses headers from the generated Windows runtime.

Linux CMake links directly against:

```text
runtime/OBS-Studio-32.1.2-Linux-x86_64/lib/libobs.so
```

and uses headers from that runtime.

---

## 18. Current known platform boundaries

### Windows

Implemented:

- source enumeration
- WGC video
- desktop audio
- process audio

### Linux

Implemented:

- portal/PipeWire capture
- desktop audio
- application audio
- portal restore-token/application matching

Not yet implemented:

- pre-capture monitor/window enumeration through `--list-sources`

### macOS

Not implemented in this production source tree.

---

## 19. Architectural invariant

The most important separation in the current engine is:

```text
capture/OBS/encoder
        │
        ▼
encoded packet callback
        │
        ▼
native-stream-engine transport
```

OBS owns capture, media graph, and encoding.

native-stream-engine owns RTP packetization, pacing, retransmission, feedback integration, and host control.

That boundary is what allows transport/network behavior to evolve independently from platform-specific capture code.
