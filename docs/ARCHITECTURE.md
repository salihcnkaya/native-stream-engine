# Architecture

## Contents

- [Overview](#overview)
- [Design Goals](#design-goals)
- [High-Level Architecture](#high-level-architecture)
- [Process Architecture](#process-architecture)
- [Component Architecture](#component-architecture)
- [Startup Sequence](#startup-sequence)
- [Shutdown Sequence](#shutdown-sequence)
- [Capture Pipeline](#capture-pipeline)
- [OBS Integration](#obs-integration)
- [Encoder Pipeline](#encoder-pipeline)
- [RTP Transport](#rtp-transport)
- [Adaptive Bitrate](#adaptive-bitrate)
- [IPC Architecture](#ipc-architecture)
- [Threading Model](#threading-model)
- [Resource Management](#resource-management)
- [Design Decisions](#design-decisions)
- [Error Handling](#error-handling)
- [Extending the Engine](#extending-the-engine)
- [Future Directions](#future-directions)

This document describes the internal architecture of **native-stream-engine**, the responsibilities of each subsystem, the interactions between components, and the design decisions behind the implementation.

Unlike the README, which provides an overview of the project and its public capabilities, this document focuses on how the engine works internally. It is intended for contributors, maintainers, and developers integrating the engine into their own applications.

The goal is that a developer should be able to understand the complete execution model of the engine before reading the implementation.

---

## Overview

`native-stream-engine` is implemented as an independent Windows executable that provides screen capture, video encoding, RTP streaming, and runtime control through a lightweight JSON IPC protocol.

The engine is intentionally separated from the host application. Instead of linking directly against libobs or exposing OBS-specific APIs, the host communicates with the engine through a stable request/response protocol over standard input and standard output.

This separation provides several important advantages:

- isolates the native media stack from the user interface
- simplifies crash recovery
- keeps the host application independent of libobs
- allows the engine to evolve without affecting the frontend
- enables language-independent integration
- keeps licensing boundaries explicit

From the perspective of the host application, the engine behaves as a long-running background service.

A typical session consists of:

1. launching the executable
2. establishing JSON IPC communication
3. enumerating capture sources
4. starting a capture session
5. forwarding runtime network statistics
6. stopping the capture
7. shutting down the service

Internally, these seemingly simple operations are coordinated by multiple subsystems responsible for capture, OBS integration, encoding, RTP transport, bitrate adaptation, and resource management.

---

## Design Goals

The architecture is built around several long-term design goals that influence every subsystem.

### Process Isolation

The media engine operates independently from the host application.

A failure inside the capture or encoding pipeline should never directly terminate the graphical user interface.

Likewise, restarting the engine should not require restarting the frontend.

---

### Stable Public Interface

The host application communicates exclusively through documented JSON messages.

No internal libobs structures, pointers, or implementation details are exposed outside of the engine.

This allows internal implementation changes without breaking external integrations.

---

### Modular Components

Each subsystem owns a clearly defined responsibility.

Examples include:

- capture acquisition
- OBS integration
- encoder management
- RTP packetization
- bitrate control
- IPC processing

Whenever possible, components communicate through well-defined interfaces rather than sharing implementation details.

---

### Predictable Resource Lifetime

The engine follows explicit ownership rules.

Resources are created during startup, reused whenever possible, and released deterministically during shutdown.

Long-running resources remain alive for the lifetime of the service, while temporary resources exist only for the duration of an active capture session.

---

### Hardware-First Strategy

Hardware acceleration is preferred whenever supported by the current system.

If a requested hardware encoder cannot be initialized, the engine falls back to software encoding instead of terminating the capture session.

This behavior prioritizes reliability over absolute performance.

---

### Low-Latency Streaming

The architecture is optimized for interactive communication rather than offline recording.

Latency is prioritized over compression efficiency.

Every subsystem is designed to minimize unnecessary buffering and reduce end-to-end delay.

---

### Extensibility

Future features should integrate into the existing architecture with minimal modification.

Examples include:

- additional capture backends
- new encoder implementations
- alternative transport protocols
- additional codecs
- platform-specific optimizations

The current design intentionally avoids assumptions that would prevent future expansion.

---

## High-Level Architecture

At a high level, the engine consists of six major layers.

```text
                    Host Application
               (Electron / Desktop App)
                          │
                          │
                     JSON IPC
                          │
                          ▼
                 Native Service Layer
                          │
          ┌───────────────┼───────────────┐
          │               │               │
          ▼               ▼               ▼
    Capture Layer    OBS Integration   Control Layer
          │               │               │
          └───────────────┼───────────────┘
                          ▼
                    Encoding Layer
                          │
                          ▼
                  RTP Transport Layer
                          │
                          ▼
                       Network
```

Each layer has a dedicated responsibility.

The host application never communicates directly with the lower layers.

Instead, every operation passes through the native service layer, which validates requests, coordinates resources, and dispatches work to the appropriate subsystem.

This keeps the public API small while allowing the implementation to remain flexible.

---

## Process Architecture

The engine is designed as a separate executable rather than a dynamically loaded library.

```text
+--------------------------------------------------------------+
|                    Host Application                          |
|--------------------------------------------------------------|
| UI                                                          |
| State Management                                            |
| Business Logic                                              |
| Electron                                                    |
+---------------------------┬----------------------------------+
                            │
                            │ Child Process
                            │
                            ▼
+--------------------------------------------------------------+
|                  native-stream-engine                        |
|--------------------------------------------------------------|
| JSON IPC                                                    |
| Native Service                                              |
| OBS Integration                                              |
| Capture Pipeline                                             |
| Encoder                                                      |
| RTP Sender                                                   |
| Bitrate Controller                                           |
+--------------------------------------------------------------+
                            │
                            ▼
                     Windows / libobs
```

The host launches the engine as a child process and keeps it alive for the duration of its session.

Communication is performed through line-delimited UTF-8 JSON messages over the standard input and standard output streams.

No shared memory, COM interfaces, sockets, or RPC mechanisms are required for normal operation.

This process model offers several advantages:

- complete separation between frontend and native code
- independent crash recovery
- simpler deployment
- language-independent integration
- reduced coupling
- explicit lifecycle management

From the perspective of the host application, the engine behaves like a deterministic service with a documented protocol rather than a native library exposing callable APIs.

This architectural decision influences every subsystem described in the following sections.

---

## Component Architecture

The engine is organized as a collection of focused components rather than a single monolithic implementation.

Each component owns a clearly defined responsibility and communicates with other components through explicit interfaces.

This separation reduces coupling, simplifies testing, and makes future extensions significantly easier.

The following diagram illustrates the relationship between the primary runtime components.

```text
                           NativeService
                                 │
     ┌───────────────┬───────────┴──────────────┬───────────────┐
     │               │                          │               │
     ▼               ▼                          ▼               ▼
 ObsEngine   Window Utilities        Capture Enumeration   IPC Helpers
     │
     │
     ├─────────────────────────────────────────────────────┐
     │                                                     │
     ▼                                                     ▼
NativeWgcSource                                    Image Utilities
     │                                                     │
     ▼                                                     ▼
WgcCapture                                      PNG / Base64 Encoding
     │
     ▼
OBS Scene
     │
     ▼
Video Encoder
     │
     ▼
RealtimeRtpSender
     │
     ▼
RtpPacer
     │
     ▼
BitrateController
     │
     ▼
BitrateUpdateScheduler
     │
     ▼
UDP Network
```

The following sections describe the responsibility of each component in more detail.

---

### NativeService

`NativeService` represents the public entry point of the engine.

Every request sent by the host application is received, validated, and dispatched here.

Its responsibilities include:

- processing JSON IPC messages
- validating command parameters
- managing service lifetime
- coordinating capture sessions
- creating and destroying the OBS engine
- forwarding runtime network feedback
- emitting asynchronous events

Importantly, `NativeService` does **not** perform capture, encoding, or RTP transmission itself.

Instead, it acts as the orchestration layer that coordinates the remaining subsystems.

This separation keeps the public interface independent from the implementation details of OBS.

---

### ObsEngine

`ObsEngine` is the core of the media pipeline.

It owns the lifetime of the underlying libobs context and is responsible for configuring the entire capture and encoding pipeline.

Its responsibilities include:

- initializing libobs
- loading OBS modules
- configuring video output
- creating scenes
- creating capture sources
- selecting encoders
- managing output objects
- starting RTP transmission
- stopping active capture sessions
- releasing OBS resources

Every operation that interacts directly with libobs is centralized inside this component.

No other subsystem communicates with libobs directly.

This design intentionally isolates OBS-specific logic from the rest of the engine.

---

### NativeWgcSource

`NativeWgcSource` implements the Windows Graphics Capture integration.

It serves as the bridge between the operating system's capture APIs and the OBS scene graph.

Its responsibilities include:

- creating WGC capture sessions
- acquiring frames from Windows
- tracking source resolution
- detecting target closure
- notifying the engine when capture ends

Unlike the encoder pipeline, this component has no knowledge of networking or RTP.

Its only responsibility is acquiring frames from the operating system.

---

### WgcCapture

`WgcCapture` contains the platform-specific capture implementation.

It communicates directly with Windows Graphics Capture APIs and exposes a simplified interface to the higher-level capture source.

This component is intentionally isolated from OBS-specific code.

If an additional capture backend is introduced in the future, it should follow the same abstraction rather than modifying higher layers.

---

### RealtimeRtpSender

`RealtimeRtpSender` owns the video transport pipeline.

Its responsibilities include:

- receiving encoded H.264 frames
- parsing NAL units
- packetizing RTP payloads
- transmitting UDP packets
- collecting runtime transport statistics
- processing RTCP feedback

This component has no knowledge of how frames were captured.

Likewise, it has no knowledge of the user interface or IPC layer.

Its responsibility begins only after an encoded frame has been produced.

---

### RtpPacer

Sending packets immediately after encoding often produces burst traffic.

`RtpPacer` smooths packet transmission by scheduling RTP packets over time instead of sending them all at once.

This improves:

- network stability
- congestion behavior
- packet spacing
- receiver performance

The pacer operates independently of the encoder implementation.

---

### BitrateController

`BitrateController` continuously evaluates current network conditions.

It considers information such as:

- packet loss
- round-trip time
- jitter
- network quality score

Based on these values it determines whether the encoder bitrate should:

- increase
- decrease
- remain unchanged

The controller produces bitrate decisions only.

It does not communicate directly with the encoder.

---

### BitrateUpdateScheduler

Rapid bitrate changes may reduce stream stability.

The scheduler acts as a filtering layer between the controller and the encoder.

Its responsibilities include:

- rate limiting encoder updates
- suppressing insignificant bitrate changes
- preventing unnecessary encoder reconfiguration
- smoothing network oscillations

Only decisions approved by the scheduler are eventually applied to the encoder.

---

### Image Utilities

The image utilities provide helper functionality that is independent from OBS.

Current responsibilities include:

- BGRA frame resizing
- PNG encoding
- thumbnail generation

These utilities are primarily used when generating preview images before a capture session begins.

They intentionally remain independent from the streaming pipeline.

---

### Base64 Utilities

Preview images are transmitted through the JSON IPC interface.

The Base64 utilities convert binary PNG data into a transport-safe textual representation suitable for JSON messages.

Keeping this functionality isolated avoids introducing encoding logic into unrelated components.

---

### Window Utilities

The window utility module provides helper functions related to desktop window enumeration.

Typical responsibilities include:

- enumerating visible windows
- retrieving window titles
- obtaining process identifiers
- exposing available capture targets

This functionality is reused both by command-line utilities and by the IPC service.

---

## Component Relationships

Although each subsystem has a distinct responsibility, they operate together as a single runtime pipeline.

The overall dependency direction intentionally remains one-way.

```text
NativeService
      │
      ▼
ObsEngine
      │
      ▼
Capture Source
      │
      ▼
Encoder
      │
      ▼
RealtimeRtpSender
      │
      ▼
Bitrate Components
      │
      ▼
Network
```

Lower-level components never call back into higher-level application logic.

This one-directional dependency structure significantly reduces architectural complexity and helps prevent cyclic dependencies as the project grows.

---

## Startup Sequence

The engine follows a deterministic startup sequence designed to ensure that every subsystem is initialized in a well-defined order.

Rather than creating every resource during process startup, the engine initializes only the components required for the current operation.

This minimizes startup cost, avoids unnecessary resource allocation, and allows the service to remain idle while waiting for commands.

The complete startup flow is illustrated below.

```text
Host Application
        │
        ▼
Launch native-stream-engine.exe
        │
        ▼
main()
        │
        ▼
Parse Command Line
        │
        ▼
Run Native Service
        │
        ▼
Wait For JSON Commands
        │
        ▼
Receive startCapture
        │
        ▼
Create ObsEngine
        │
        ▼
Initialize libobs
        │
        ▼
Load OBS Modules
        │
        ▼
Configure Video
        │
        ▼
Create Scene
        │
        ▼
Create Capture Source
        │
        ▼
Initialize Encoder
        │
        ▼
Start RTP Sender
        │
        ▼
Streaming
```

Each stage is described below.

---

### Process Startup

Execution begins inside `main()`.

The command line determines which operating mode should be started.

Typical modes include:

- service mode
- source enumeration
- OBS inventory
- window enumeration

Only service mode creates the long-running IPC loop.

The remaining commands execute a specific task before terminating.

This separation keeps utility operations lightweight while avoiding unnecessary initialization.

---

### Native Service Initialization

When the service mode is selected, control is transferred to the native service.

The service performs several initialization steps before accepting requests.

These include:

- preparing global state
- creating synchronization primitives
- starting the capture watcher thread
- preparing the JSON communication loop

At this stage no OBS resources have been allocated.

The service intentionally remains idle until the host requests a capture session.

---

### Idle State

Most of the engine lifetime is spent waiting.

During this period:

- no encoder exists
- no capture source exists
- no scene exists
- no RTP socket is transmitting

The service simply waits for incoming JSON commands.

This design significantly reduces CPU usage while the engine is idle.

---

### Capture Request

The first significant transition occurs when the host sends a `startCapture` request.

The request contains information such as:

- capture type
- monitor or window identifier
- quality preset
- encoder preference
- RTP destination

The service validates these parameters before any expensive resources are allocated.

Invalid requests are rejected immediately.

---

### OBS Initialization

If an OBS instance does not already exist, `ObsEngine` initializes libobs.

Initialization includes:

- locating the runtime directory
- loading OBS modules
- configuring the graphics subsystem
- creating the video context
- preparing encoder infrastructure

The OBS instance remains alive after initialization and may be reused by subsequent capture sessions.

This avoids repeatedly creating and destroying the entire OBS runtime.

---

### Video Configuration

Once libobs has been initialized, the engine configures the video pipeline.

Typical configuration includes:

- base resolution
- output resolution
- frame rate
- scaling filter

These settings determine the characteristics of the downstream encoding pipeline.

Changing them later may require parts of the pipeline to be recreated.

---

### Scene Creation

OBS organizes media through scenes.

The engine creates a dedicated scene for each capture session.

The scene itself contains no capture logic.

Instead, it serves as the container for one or more capture sources.

This separation mirrors the architecture of OBS itself.

---

### Capture Source Creation

After the scene has been created, the requested capture source is attached.

Depending on the request this may represent:

- a monitor
- an application window

The capture source begins producing frames once the underlying Windows Graphics Capture session becomes active.

At this point no RTP packets have yet been transmitted.

---

### Encoder Initialization

Once frames become available, the engine selects and initializes the requested encoder.

If automatic selection is enabled, hardware encoders are attempted first.

The current priority is:

```text
NVENC
   │
   ▼
AMD AMF
   │
   ▼
Intel QSV
   │
   ▼
x264
```

If a hardware encoder cannot be initialized, the engine automatically falls back to software encoding.

The capture session continues whenever possible.

---

### RTP Initialization

After the encoder has been created, the RTP subsystem is initialized.

This includes:

- opening the UDP socket
- configuring RTP state
- preparing packetization
- initializing pacing
- setting the initial bitrate

At this point the transport layer is fully prepared.

---

### First Encoded Frame

The first captured frame passes through the complete media pipeline.

```text
Capture Source
        │
        ▼
OBS Scene
        │
        ▼
Video Encoder
        │
        ▼
H.264 Bitstream
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

After the first frame has been transmitted, the engine enters its normal streaming state.

Subsequent frames follow the same path until the capture session ends.

---

### Runtime Operation

While streaming is active, multiple subsystems continue operating simultaneously.

These include:

- frame acquisition
- encoder execution
- RTP transmission
- pacing
- bitrate adaptation
- capture monitoring

The service remains responsive to incoming IPC requests throughout the lifetime of the capture session.

Operations such as bitrate updates or capture termination are processed without restarting the pipeline.

---

### State Transitions

The engine transitions through a small number of well-defined runtime states.

```text
Stopped
    │
    ▼
Service Running
    │
    ▼
Waiting For Commands
    │
    ▼
Capture Starting
    │
    ▼
Streaming
    │
    ▼
Capture Stopping
    │
    ▼
Waiting For Commands
```

Each state owns a predictable set of resources.

This deterministic lifecycle simplifies cleanup, improves reliability, and reduces the likelihood of resource leaks.

---

## Shutdown Sequence

Stopping a capture session is designed to be as deterministic as the startup process.

Rather than relying on automatic destruction or process termination, every subsystem is shut down in an explicit and predictable order.

This approach minimizes resource leaks, prevents dangling OBS objects, and ensures that subsequent capture sessions start from a clean state.

The shutdown sequence is illustrated below.

```text
Host Application
        │
        ▼
Stop Capture Request
        │
        ▼
NativeService
        │
        ▼
Stop RTP Transmission
        │
        ▼
Destroy Output
        │
        ▼
Destroy Encoder
        │
        ▼
Destroy Capture Source
        │
        ▼
Destroy Scene
        │
        ▼
Return To Idle State
```

The service itself continues running after the capture session has ended.

Only the resources associated with the active session are released.

---

### Capture Stop Request

Shutdown begins when the host sends a stop request through the IPC interface.

The request immediately prevents new capture operations from being scheduled.

Any resources still processing the current frame are allowed to complete before destruction begins.

This avoids partially processed frames and inconsistent encoder state.

---

### RTP Shutdown

The first subsystem to stop is the RTP transport.

Stopping network transmission before destroying the encoder guarantees that no additional encoded frames are submitted after the transport has been closed.

Typical operations include:

- stopping packet transmission
- flushing pending packets
- closing UDP sockets
- releasing transport state

After this point no additional RTP packets are generated.

---

### Encoder Destruction

Once transport has stopped, the encoder is released.

Destroying the encoder before removing capture sources guarantees that no additional frame requests are generated during cleanup.

Hardware encoder resources are also released during this stage.

Depending on the active encoder this may include GPU-specific resources managed internally by OBS.

---

### Capture Source Removal

After encoding has stopped, the capture source is removed from the active OBS scene.

Removing the source automatically terminates frame acquisition from Windows Graphics Capture.

No additional frames enter the media pipeline after this point.

---

### Scene Cleanup

Once every source has been removed, the temporary scene itself is destroyed.

Scenes are intentionally treated as session-specific resources.

Each capture session creates its own scene rather than attempting to reuse previous scene graphs.

This guarantees predictable behavior between independent capture sessions.

---

### OBS Runtime

Destroying a capture session does **not** necessarily destroy the OBS runtime.

The engine keeps the initialized libobs instance alive whenever possible.

Reusing the existing OBS context avoids:

- repeated graphics initialization
- module reloads
- unnecessary GPU resource allocation
- increased startup latency

Only the session-specific objects are recreated.

The underlying OBS runtime remains available for future capture requests.

---

### Returning To Idle

After cleanup has completed the engine returns to its idle state.

```text
Streaming
      │
      ▼
Stopping
      │
      ▼
Cleanup
      │
      ▼
Waiting For Commands
```

At this point:

- no capture source exists
- no encoder exists
- no RTP sender is active
- no scene is active

The service continues waiting for new IPC requests.

---

### Service Shutdown

The entire process terminates only when the host application explicitly requests the service to stop or when the process itself exits.

Before termination the engine performs a complete cleanup.

The final shutdown sequence includes:

1. stopping any active capture session
2. stopping RTP transmission
3. releasing OBS objects
4. shutting down libobs
5. terminating worker threads
6. releasing remaining resources
7. exiting the process

By centralizing shutdown inside the service layer, every exit path follows the same cleanup procedure.

This eliminates duplicated cleanup logic and greatly reduces the likelihood of resource leaks.

---

### Shutdown Guarantees

The engine is designed to guarantee the following conditions after shutdown:

- no active capture session remains
- no encoder instance remains
- no RTP socket remains open
- no OBS scene remains allocated
- no capture source remains attached
- all worker threads have terminated
- the process can immediately start another capture session

These guarantees simplify both development and integration, since the host application never needs to manually reset internal engine state.

---

## Capture Pipeline

The capture pipeline is responsible for transforming pixels displayed on the user's screen into a real-time H.264 RTP stream.

Although this process appears simple from the perspective of the host application, internally it consists of multiple independent stages that operate together as a continuous media pipeline.

The pipeline has been intentionally designed as a sequence of loosely coupled stages.

Each stage performs exactly one responsibility before forwarding its output to the next stage.

This architecture improves maintainability, simplifies debugging, and makes future extensions significantly easier.

The complete capture pipeline is illustrated below.

```text
Windows Desktop
        │
        ▼
Windows Graphics Capture
        │
        ▼
NativeWgcSource
        │
        ▼
OBS Source
        │
        ▼
OBS Scene
        │
        ▼
OBS Video Pipeline
        │
        ▼
Video Encoder
        │
        ▼
Encoded H.264 Frames
        │
        ▼
RealtimeRtpSender
        │
        ▼
RTP Packetizer
        │
        ▼
RtpPacer
        │
        ▼
UDP Network
```

Each stage has a dedicated responsibility.

No stage performs work that belongs to another subsystem.

This separation keeps the pipeline predictable and allows individual stages to evolve independently.

---

### Stage 1 — Frame Acquisition

The pipeline begins with Windows Graphics Capture (WGC).

WGC is responsible for capturing desktop frames directly from the Windows compositor.

Depending on the request, the capture target may represent:

- a monitor
- an application window

The capture backend continuously receives new frames from the operating system whenever the target produces visual updates.

At this stage the engine has not yet performed any rendering, encoding, or networking.

The operating system simply delivers raw image frames.

---

### Stage 2 — Native Capture Source

The acquired frames are forwarded into `NativeWgcSource`.

This component acts as the bridge between the Windows capture APIs and libobs.

Its responsibilities include:

- creating the capture session
- tracking capture resolution
- forwarding new frames into OBS
- detecting target closure
- notifying the engine when capture stops unexpectedly

No encoding occurs inside this component.

Its only responsibility is moving frames from Windows into the OBS pipeline.

---

### Stage 3 — OBS Source

Once a frame enters OBS, it becomes a standard OBS source.

From this point forward the engine benefits from the existing rendering infrastructure provided by libobs.

Using the OBS source abstraction provides several advantages.

For example:

- automatic GPU rendering
- hardware accelerated scaling
- future support for filters
- future scene composition
- compatibility with additional OBS components

The capture backend itself remains independent from these features.

---

### Stage 4 — Scene Composition

The source is attached to an OBS scene.

Although current capture sessions typically contain a single source, using a scene abstraction provides flexibility for future extensions.

Examples include:

- overlays
- cursor rendering
- multiple sources
- transitions
- compositing

Separating sources from scenes mirrors the internal architecture of OBS itself.

---

### Stage 5 — Video Rendering

OBS renders the complete scene into a GPU texture.

This rendering stage is responsible for:

- scaling
- color conversion
- texture management
- GPU synchronization

The resulting rendered frame represents the final image that will eventually be encoded.

No network operations have yet occurred.

---

### Stage 6 — Video Encoding

The rendered frame is submitted to the selected video encoder.

Depending on system capabilities, this may be:

- NVIDIA NVENC
- AMD AMF
- Intel Quick Sync Video
- x264

The encoder converts GPU-rendered frames into an H.264 elementary stream suitable for real-time transport.

The output of this stage consists of encoded NAL units rather than raw image data.

This dramatically reduces bandwidth requirements while preserving visual quality.

---

### Stage 7 — RTP Preparation

Encoded NAL units are forwarded to the RTP subsystem.

The transport layer performs several operations before transmission.

These include:

- parsing H.264 NAL units
- constructing RTP packets
- assigning sequence numbers
- generating timestamps
- managing synchronization state

The packetizer intentionally has no knowledge of screen capture or OBS.

It only processes encoded video.

---

### Stage 8 — Packet Pacing

Sending every packet immediately after encoding may produce large traffic bursts.

The pacing subsystem distributes packet transmission over time.

This improves:

- packet spacing
- congestion behavior
- receiver stability
- overall stream smoothness

Packet pacing becomes increasingly important on unstable or bandwidth-constrained networks.

---

### Stage 9 — Network Transmission

Finally, paced RTP packets are transmitted over UDP.

At this stage the engine has completed all media processing.

Responsibility for delivery now belongs to the network infrastructure and the receiving application.

---

### Continuous Pipeline

Unlike traditional media applications, the pipeline does not terminate after processing a single frame.

Instead, every captured frame follows exactly the same path.

```text
Frame 1
        │
        ▼
Capture
        │
        ▼
Render
        │
        ▼
Encode
        │
        ▼
Packetize
        │
        ▼
Transmit

Frame 2
        │
        ▼
Capture
        │
        ▼
Render
        │
        ▼
Encode
        │
        ▼
Packetize
        │
        ▼
Transmit

...
```

Multiple stages may operate concurrently.

While one frame is being encoded, another frame may already be captured by Windows, and previously encoded packets may still be transmitted by the pacing subsystem.

This pipelined execution maximizes throughput while minimizing end-to-end latency.

---

### Pipeline Design Principles

Several architectural principles influenced the design of the capture pipeline.

- Each stage owns exactly one responsibility.
- Data always flows in a single direction.
- Lower layers never depend on higher layers.
- Components communicate only through explicit interfaces.
- Every stage can evolve independently without affecting unrelated subsystems.

These principles significantly reduce architectural complexity while making the media pipeline easier to understand, debug, and extend.

---

## OBS Integration

libobs is the core multimedia framework used by the engine.

Rather than implementing a custom rendering and encoding pipeline from scratch, the engine delegates graphics rendering, frame composition, hardware encoder management, and video processing to libobs while maintaining complete control over the surrounding application architecture.

OBS is therefore treated as an internal implementation detail rather than a public dependency.

The host application never communicates with libobs directly.

All interaction is performed through the engine's internal abstraction layer.

The integration consists of several independent responsibilities.

```text
                  Native Service
                         │
                         ▼
                    ObsEngine
                         │
      ┌──────────────────┼──────────────────┐
      │                  │                  │
      ▼                  ▼                  ▼
OBS Initialization   Scene System     Encoder System
      │                  │                  │
      └──────────────────┼──────────────────┘
                         ▼
                  Graphics Context
                         │
                         ▼
                  Video Output Pipeline
```

Each subsystem is described below.

---

### OBS Initialization

The OBS runtime is initialized only when it is first required.

Starting libobs is a relatively expensive operation involving graphics initialization, module discovery, encoder registration, and creation of internal rendering resources.

Performing this work during process startup would unnecessarily increase startup latency for operations that never begin a capture session.

Instead, the engine initializes OBS lazily.

This design provides several advantages:

- lower process startup time
- reduced memory usage while idle
- faster utility commands
- simpler service lifetime management

Once initialized, the OBS runtime remains available for subsequent capture sessions.

The runtime is intentionally reused instead of recreated for every stream.

---

### Module Loading

OBS functionality is provided through dynamically loaded modules.

During initialization the engine loads the required runtime modules before capture can begin.

Examples include:

- Windows capture
- Windows audio capture
- FFmpeg integration
- hardware encoder plugins

Module loading is treated as part of the initialization phase rather than normal runtime operation.

If a required module cannot be loaded, initialization fails before any capture session begins.

This avoids partially initialized media pipelines.

---

### Graphics Context

OBS internally owns the graphics context used for rendering.

The engine intentionally delegates GPU resource management to libobs instead of attempting to manage graphics resources itself.

Responsibilities handled by the graphics subsystem include:

- GPU texture allocation
- render target management
- shader execution
- color conversion
- graphics synchronization

Keeping these responsibilities inside OBS significantly reduces implementation complexity while benefiting from years of production-tested rendering code.

---

### Scene Management

OBS organizes visual content through scenes.

Each capture session creates a dedicated scene that acts as the root of the rendering pipeline.

Although the current engine typically captures only a single source, using scenes provides several long-term architectural advantages.

Future capabilities such as:

- overlays
- annotations
- cursor rendering
- transitions
- multiple simultaneous sources

can all be introduced without redesigning the rendering architecture.

The scene therefore represents the composition stage rather than the capture stage.

---

### Capture Sources

Capture sources are responsible only for acquiring visual content.

The engine currently integrates Windows Graphics Capture through a custom source implementation.

Each source has exactly one responsibility:

- receive frames from Windows
- expose those frames to OBS

Sources intentionally remain independent from:

- networking
- RTP
- bitrate control
- encoder selection
- IPC

This separation allows future capture backends to be introduced without modifying unrelated systems.

---

### Video Configuration

Before streaming begins, OBS is configured with the video parameters requested by the host application.

Typical configuration includes:

- base resolution
- output resolution
- frame rate
- color format
- scaling method

These settings define the characteristics of the rendering pipeline before the first frame is processed.

Once streaming has begun, changing certain parameters may require parts of the pipeline to be recreated.

For this reason, video configuration is treated as part of session initialization rather than normal runtime operation.

---

### Rendering Pipeline

After capture begins, every frame passes through the OBS rendering pipeline.

A simplified representation is shown below.

```text
Capture Source
        │
        ▼
OBS Source
        │
        ▼
Scene Composition
        │
        ▼
GPU Rendering
        │
        ▼
Final Video Frame
        │
        ▼
Video Encoder
```

OBS performs rendering entirely on the graphics device whenever possible.

This minimizes unnecessary CPU copies while taking advantage of hardware acceleration provided by the operating system and graphics driver.

---

### Resource Lifetime

The engine distinguishes between two categories of OBS resources.

#### Long-Lived Resources

These remain alive for the lifetime of the OBS runtime.

Examples include:

- graphics subsystem
- loaded modules
- video subsystem
- encoder registry

These objects are expensive to create and therefore reused whenever possible.

---

#### Session Resources

These exist only while a capture session is active.

Examples include:

- scenes
- capture sources
- encoder instances
- active outputs

When streaming stops, these resources are released while the underlying OBS runtime remains initialized.

Separating long-lived resources from session resources significantly reduces startup time for consecutive capture sessions while keeping cleanup predictable.

---

### Why OBS Is Wrapped

The engine deliberately avoids exposing libobs directly to external applications.

Instead, all OBS interaction is centralized inside `ObsEngine`.

This architectural decision provides several important benefits.

- The host application remains independent from OBS internals.
- Internal implementation details can evolve without changing the public API.
- Platform-specific code remains isolated.
- OBS upgrades become easier to manage.
- Future rendering backends can be introduced behind the same abstraction.

From the perspective of the host application, OBS effectively does not exist.

The application communicates only with the engine, while the engine remains solely responsible for managing the complete OBS lifecycle.

---

## Encoder Pipeline

Once OBS has produced the final rendered frame, responsibility shifts from the rendering subsystem to the encoder pipeline.

The encoder pipeline transforms rendered video frames into a compressed H.264 bitstream suitable for low-latency real-time transmission.

Unlike offline video encoding, the engine prioritizes responsiveness and stability over maximum compression efficiency.

Encoder decisions therefore focus on minimizing latency while maintaining acceptable visual quality under changing network conditions.

The overall encoder pipeline is shown below.

```text
OBS Rendered Frame
         │
         ▼
Encoder Selection
         │
         ▼
Encoder Initialization
         │
         ▼
Frame Submission
         │
         ▼
H.264 Encoding
         │
         ▼
Encoded NAL Units
         │
         ▼
RealtimeRtpSender
```

Each stage has a dedicated responsibility.

---

### Encoder Selection

Before a capture session begins, the engine selects the most appropriate encoder available on the current system.

The objective is not to select the fastest encoder in every situation, but to provide the best balance between latency, visual quality, CPU usage, and hardware availability.

Whenever possible, dedicated hardware encoders are preferred.

A typical selection order is:

```text
NVIDIA NVENC
        │
        ▼
AMD AMF
        │
        ▼
Intel Quick Sync Video
        │
        ▼
x264 Software Encoder
```

The exact encoder used depends entirely on runtime capabilities.

The host application is not required to understand platform-specific encoder implementations.

---

### Hardware-First Strategy

Modern GPUs include dedicated video encoding hardware that operates independently of the graphics pipeline.

Using these encoders provides several advantages.

- significantly lower CPU usage
- lower encoding latency
- improved performance during gaming
- higher system responsiveness

For this reason, the engine always attempts to initialize a supported hardware encoder before considering software encoding.

This strategy allows streaming to remain efficient even on systems under heavy graphical load.

---

### Software Fallback

Hardware acceleration cannot be guaranteed.

The selected encoder may be unavailable because of:

- unsupported hardware
- missing runtime components
- driver limitations
- initialization failures
- unavailable encoding sessions

Rather than terminating the capture session, the engine automatically falls back to software encoding.

This behavior favors reliability over absolute performance.

Users can continue streaming even on systems without dedicated hardware encoding support.

---

### Encoder Initialization

Once an encoder has been selected, it is initialized with the parameters provided by the capture session.

Typical configuration includes:

- output resolution
- frame rate
- bitrate
- profile
- keyframe interval
- rate control mode

Initialization occurs only once for each capture session.

After successful initialization, the encoder becomes part of the active media pipeline.

---

### Frame Submission

Every rendered OBS frame is submitted directly to the active encoder.

The encoder processes frames sequentially.

For each input frame, the encoder produces one or more encoded H.264 NAL units.

The encoder itself is completely unaware of networking, RTP, or packet transmission.

Its only responsibility is video compression.

---

### H.264 Bitstream

The output of the encoder consists of H.264 Network Abstraction Layer (NAL) units.

These units form the elementary video stream consumed by the RTP subsystem.

Typical NAL types include:

- SPS
- PPS
- IDR frames
- P-frames
- SEI messages

The encoder pipeline does not packetize these structures.

They remain intact until processed by the transport layer.

---

### Keyframes

Keyframes play an important role in stream recovery and decoder synchronization.

Unlike predictive frames, keyframes contain a complete representation of the image and can therefore be decoded independently.

The engine supports both:

- periodic keyframe generation
- externally requested keyframes

Keyframe requests may originate from network feedback or receiver synchronization requirements.

The encoder is responsible for producing the requested frame.

The transport layer simply forwards the resulting bitstream.

---

### Runtime Bitrate Updates

Streaming conditions are rarely constant.

Available bandwidth may increase or decrease throughout the lifetime of a capture session.

Instead of recreating the encoder whenever conditions change, the engine updates encoder parameters dynamically whenever supported.

Typical runtime adjustments include:

- bitrate reduction
- bitrate increase

These updates allow the stream to adapt to changing network conditions while avoiding unnecessary interruptions.

Not every bitrate decision results in an encoder update.

Intermediate filtering is performed by the bitrate scheduling subsystem described later in this document.

---

### Encoder Lifetime

Encoder instances are considered session-specific resources.

Each active capture session owns exactly one encoder instance.

```text
Capture Starts
        │
        ▼
Create Encoder
        │
        ▼
Encode Frames
        │
        ▼
Runtime Bitrate Updates
        │
        ▼
Stop Capture
        │
        ▼
Destroy Encoder
```

Once streaming ends, the encoder is destroyed together with the remaining session resources.

The underlying OBS runtime continues running and may create a new encoder for future sessions.

---

### Design Principles

The encoder subsystem follows several architectural principles.

- Encoder selection is performed only once per session.
- Hardware acceleration is preferred whenever available.
- Software fallback is automatic.
- Encoding remains independent from networking.
- Runtime bitrate updates avoid unnecessary encoder recreation.
- The transport layer never depends on encoder implementation details.

These principles allow the encoding subsystem to remain isolated from the remainder of the streaming pipeline while providing flexibility for future codec and hardware support.

---

### Future Extensibility

The encoder abstraction intentionally avoids assumptions specific to H.264.

Future versions of the engine may introduce additional codecs through the same architecture.

Potential future additions include:

- H.265 / HEVC
- AV1
- platform-specific hardware encoders
- improved rate control algorithms

Since higher layers interact only with the abstract encoder interface, introducing new encoding technologies requires minimal changes outside the encoder subsystem.

---

## RTP Transport

Once the encoder has produced an H.264 bitstream, responsibility moves to the transport subsystem.

The transport layer is responsible for converting encoded video into a continuous RTP stream suitable for real-time communication.

Unlike the rendering and encoding stages, the transport layer has no knowledge of graphics, OBS, or capture devices.

Its responsibility begins only after encoded H.264 data becomes available.

The complete transport pipeline is shown below.

```text
Encoded H.264 Frames
         │
         ▼
NAL Unit Processing
         │
         ▼
RTP Packetization
         │
         ▼
Timestamp Generation
         │
         ▼
Sequence Number Assignment
         │
         ▼
Packet Pacing
         │
         ▼
UDP Transmission
```

Every stage performs one well-defined responsibility before forwarding packets to the next stage.

This separation keeps networking concerns isolated from the remainder of the media pipeline.

---

### Transport Responsibilities

The RTP subsystem is responsible for:

- consuming encoded H.264 frames
- parsing H.264 NAL units
- constructing RTP packets
- assigning sequence numbers
- generating RTP timestamps
- pacing packet transmission
- sending UDP packets
- processing network feedback

Importantly, the transport layer never interacts directly with OBS.

Likewise, the encoder has no knowledge of RTP.

This strict separation keeps both subsystems independent.

---

### Encoded Frame Processing

The encoder produces one or more H.264 NAL units for every rendered frame.

Before transmission begins, these NAL units are analyzed by the transport layer.

Typical operations include:

- identifying NAL boundaries
- determining packetization strategy
- preparing payload metadata

No modification is performed to the encoded image itself.

The transport layer only prepares the data for network transmission.

---

### RTP Packetization

Network packets are significantly smaller than encoded video frames.

Large encoded frames are therefore divided into multiple RTP packets.

The packetizer is responsible for:

- constructing RTP headers
- splitting oversized payloads
- preserving decoding order
- marking frame boundaries

The receiver can reconstruct the original encoded frame using only the transmitted RTP packets.

The packetizer intentionally remains independent from the underlying encoder implementation.

---

### Sequence Numbers

Every transmitted RTP packet receives a monotonically increasing sequence number.

Sequence numbers allow the receiver to:

- detect packet loss
- restore packet ordering
- identify duplicate packets

The sender maintains sequence state for the lifetime of the active transport session.

When a new streaming session begins, a new RTP sequence is created.

---

### RTP Timestamps

Each encoded video frame is associated with an RTP timestamp.

Timestamps allow the receiver to determine:

- presentation timing
- playback synchronization
- frame ordering

Unlike wall-clock time, RTP timestamps represent progression within the media stream.

This abstraction allows synchronization without depending on the local system clock.

---

### Packet Pacing

Encoded frames often produce bursts of network traffic.

Sending every RTP packet immediately may temporarily exceed available bandwidth and increase packet loss.

The pacing subsystem distributes packet transmission over time.

```text
Without Pacing

████████████████████

With Pacing

██  ██  ██  ██  ██  ██
```

Smoother transmission provides several advantages.

- reduced burst traffic
- improved congestion behavior
- lower packet loss
- more stable receiver buffering

The pacing subsystem operates independently of the encoder and packetizer.

---

### UDP Transmission

After pacing, packets are transmitted using UDP.

UDP is intentionally chosen because real-time communication favors low latency over guaranteed delivery.

Lost packets are generally preferable to delayed packets in interactive streaming scenarios.

The transport layer therefore prioritizes continuous delivery instead of retransmission.

---

### Runtime Network Feedback

The transport subsystem continuously receives network feedback from the receiving application.

Typical feedback includes:

- packet loss
- round-trip time
- jitter
- estimated available bandwidth

This information is forwarded to the adaptive bitrate subsystem.

The transport layer itself does not decide how bitrate should change.

Its responsibility is limited to collecting and forwarding transport statistics.

---

### Separation of Concerns

The transport subsystem intentionally owns only networking responsibilities.

It never performs:

- screen capture
- rendering
- encoding
- bitrate decision making
- user interaction

Similarly, higher layers never manipulate RTP packets directly.

This clear separation allows networking improvements without affecting the remainder of the engine.

---

### Transport Lifetime

The RTP transport exists only while a capture session is active.

```text
Capture Starts
        │
        ▼
Create RTP Sender
        │
        ▼
Open UDP Socket
        │
        ▼
Packetize Frames
        │
        ▼
Transmit RTP
        │
        ▼
Receive Network Feedback
        │
        ▼
Stop Transport
        │
        ▼
Close Socket
```

Once streaming stops, all transport resources are released.

The next capture session creates a completely new transport instance.

---

### Design Principles

Several architectural principles guided the design of the RTP subsystem.

- Transport begins only after encoding has completed.
- Networking remains independent from rendering.
- Packetization remains independent from encoder implementation.
- Network feedback is collected but not interpreted by the transport layer.
- Transport resources are session-scoped.
- Every packet follows the same deterministic transmission pipeline.

These principles keep the networking layer modular while allowing future protocol improvements with minimal impact on higher-level components.

---

## Adaptive Bitrate

Network conditions continuously change during real-time communication.

Available bandwidth, latency, packet loss, and network stability may fluctuate significantly throughout a streaming session.

Rather than transmitting at a fixed bitrate, the engine continuously adapts encoder settings according to current network conditions.

The adaptive bitrate system is designed to maximize stream quality while preserving playback stability.

Instead of reacting aggressively to every network fluctuation, the engine evaluates feedback over time and applies controlled bitrate adjustments only when necessary.

The adaptive bitrate architecture is illustrated below.

```text
Network
    │
    ▼
Transport Statistics
    │
    ▼
Network Feedback
    │
    ▼
BitrateController
    │
    ▼
BitrateUpdateScheduler
    │
    ▼
Encoder Update
    │
    ▼
Updated Stream
```

Each component owns a single responsibility.

---

### Design Goals

The adaptive bitrate subsystem is designed around several primary objectives.

- maintain stream continuity
- avoid unnecessary bitrate oscillation
- recover quickly after temporary congestion
- minimize encoder reconfiguration
- provide predictable behavior under unstable networks

Rather than attempting to maximize image quality at all times, the system prioritizes uninterrupted real-time communication.

---

### Network Feedback

The transport layer continuously gathers runtime network statistics.

Examples include:

- packet loss
- round-trip time
- jitter
- estimated available bandwidth

These values are combined into a normalized feedback structure.

The adaptive bitrate subsystem depends only on this abstract representation.

It does not communicate directly with RTP packets or transport internals.

This abstraction keeps congestion control independent from the networking implementation.

---

### BitrateController

`BitrateController` is responsible for evaluating current network conditions.

Its purpose is not to communicate with the encoder directly.

Instead, it determines what bitrate should ideally be used under the current conditions.

The controller continuously analyzes incoming feedback and produces one of three possible outcomes:

- increase bitrate
- decrease bitrate
- keep the current bitrate

The controller itself performs no encoder updates.

This separation makes bitrate evaluation deterministic and easy to evolve independently from encoder management.

---

### Gradual Adaptation

Network quality rarely changes instantaneously.

Likewise, bitrate should not jump aggressively after every feedback sample.

Instead, the controller performs gradual adaptation.

Under healthy network conditions, bitrate increases progressively.

During congestion, bitrate reductions occur more conservatively than a simple one-to-one reaction.

This behavior provides smoother visual quality while reducing unnecessary stream instability.

---

### BitrateUpdateScheduler

Encoder reconfiguration is relatively expensive.

Updating bitrate too frequently may reduce stream stability and increase unnecessary encoder work.

For this reason, bitrate decisions are passed through the `BitrateUpdateScheduler`.

The scheduler is responsible for deciding **when** a bitrate update should actually be applied.

Typical responsibilities include:

- limiting update frequency
- filtering insignificant changes
- avoiding redundant encoder updates
- smoothing short-lived network fluctuations

Only approved bitrate changes reach the encoder.

---

### Preventing Oscillation

One of the primary goals of the scheduler is preventing oscillation.

Without additional filtering, rapidly alternating network measurements may repeatedly increase and decrease encoder bitrate.

Such behavior would produce unstable image quality and unnecessary encoder reconfiguration.

Instead, the scheduler favors stable operating points.

Temporary network variations are often ignored until sufficient evidence exists that conditions have genuinely changed.

This approach produces significantly smoother long-running sessions.

---

### Congestion Recovery

Temporary congestion does not necessarily indicate a permanently degraded network.

After network conditions recover, bitrate should increase again—but not immediately.

The engine therefore performs recovery gradually.

```text
Bitrate

High ────────────────┐
                     │
                     │
                     ▼
               Congestion
                     │
                     ▼
           Controlled Reduction
                     │
                     ▼
          Stable Operation
                     │
                     ▼
        Progressive Recovery
                     │
                     ▼
               Normal Quality
```

This strategy avoids repeatedly entering congestion immediately after recovery.

---

### Encoder Updates

When the scheduler approves a bitrate adjustment, the new bitrate is forwarded to the active encoder.

Whenever supported by the selected encoder implementation, bitrate changes are applied dynamically.

This allows the stream to adapt without interrupting the capture session.

The adaptive bitrate subsystem intentionally avoids destroying and recreating encoder instances for ordinary bitrate adjustments.

Maintaining encoder continuity significantly improves streaming stability.

---

### Separation of Responsibilities

Each component in the adaptive bitrate subsystem owns a distinct responsibility.

```text
Transport
      │
      ▼
Collect Statistics
      │
      ▼
BitrateController
      │
      ▼
Evaluate Network
      │
      ▼
BitrateUpdateScheduler
      │
      ▼
Schedule Updates
      │
      ▼
Encoder
```

This layered design prevents networking, congestion control, scheduling, and encoding from becoming tightly coupled.

Each subsystem can evolve independently without affecting unrelated components.

---

### Runtime Behavior

Throughout an active streaming session, the adaptive bitrate system operates continuously.

For every new feedback sample, the same sequence occurs.

```text
Receive Feedback
        │
        ▼
Evaluate Network
        │
        ▼
Determine Target Bitrate
        │
        ▼
Filter Decision
        │
        ▼
Update Encoder
        │
        ▼
Continue Streaming
```

This loop repeats for the entire lifetime of the capture session.

Because the process is continuous, the stream naturally adapts to changing network conditions without requiring manual intervention.

---

### Design Principles

The adaptive bitrate subsystem follows several architectural principles.

- Network monitoring is separated from bitrate decision making.
- Bitrate evaluation is separated from encoder updates.
- Temporary fluctuations should not trigger immediate reactions.
- Stable behavior is preferred over aggressive optimization.
- Encoder reconfiguration should occur only when meaningful.
- Recovery should be progressive rather than instantaneous.

Together, these principles produce a streaming experience that remains responsive while avoiding unnecessary quality oscillation under real-world network conditions.

---

## IPC Architecture

The host application communicates with the engine exclusively through a lightweight JSON-based IPC protocol.

Rather than exposing a native C API or linking directly against the engine, communication occurs through the standard input and standard output streams of the engine process.

From the perspective of the host application, the engine behaves as an independent service that accepts requests and produces responses.

The overall communication model is shown below.

```text
+---------------------------+
|     Host Application      |
| (Electron / Desktop App)  |
+-------------+-------------+
              │
              │ JSON Request
              ▼
      Standard Input (stdin)
              │
              ▼
+---------------------------+
|   native-stream-engine    |
|---------------------------|
|      Command Parser       |
|      NativeService        |
+-------------+-------------+
              │
              │ Execute
              ▼
      Internal Components
              │
              │ JSON Response / Event
              ▼
     Standard Output (stdout)
              │
              ▼
+---------------------------+
|     Host Application      |
+---------------------------+
```

This architecture intentionally keeps the public communication layer small, predictable, and language-independent.

---

### Design Goals

The IPC layer was designed around several core principles.

- platform-independent communication
- implementation independence
- deterministic request handling
- human-readable messages
- minimal external dependencies

The host application should never need knowledge of internal OBS objects, encoder instances, or transport implementation details.

Every interaction occurs through documented commands.

---

### Request / Response Model

Most communication follows a traditional request/response pattern.

The host submits a command.

The engine validates the request, performs the requested operation, and returns either a successful result or an error.

A simplified flow is shown below.

```text
Host
   │
   │ Request
   ▼
Engine
   │
   │ Execute
   ▼
Engine
   │
   │ Response
   ▼
Host
```

This synchronous interaction is appropriate for operations such as:

- starting capture
- stopping capture
- enumerating sources
- querying capabilities
- requesting thumbnails

Each request is processed independently.

---

### Asynchronous Events

Not every event originates from the host application.

Certain events occur independently while the engine is running.

Examples include:

- capture target closed
- capture session ended
- unexpected runtime error
- stream state changes
- diagnostic notifications

In these situations, the engine emits asynchronous events without waiting for an explicit request.

```text
Windows
    │
    ▼
Engine Detects Event
    │
    ▼
Generate Notification
    │
    ▼
JSON Event
    │
    ▼
Host Application
```

This event-driven model allows the host application to react immediately to runtime changes without continuously polling the engine.

---

### Message Format

All IPC messages use UTF-8 encoded JSON.

Every message represents exactly one logical command or event.

Although individual message formats differ, they all follow the same conceptual structure.

```text
Incoming Message

Request
    │
    ▼
Validation
    │
    ▼
Execution
    │
    ▼
Response
```

Keeping a consistent message structure simplifies debugging, logging, and future protocol evolution.

---

### Command Validation

Every incoming command is validated before execution.

Validation typically includes:

- required fields
- parameter types
- supported values
- current engine state

Invalid requests are rejected before reaching lower-level components.

This protects internal subsystems from inconsistent or incomplete input.

---

### State Awareness

Certain commands are valid only while the engine is in a specific state.

For example, starting a capture session while another session is already active may require different handling than starting from an idle state.

The IPC layer therefore considers the current service state before dispatching requests.

This prevents invalid state transitions and simplifies internal resource management.

---

### Error Reporting

Errors are propagated through structured IPC responses.

Rather than exposing implementation-specific failures, the engine reports errors using stable protocol messages.

Typical categories include:

- invalid request
- unsupported operation
- initialization failure
- capture failure
- encoder failure
- transport failure

Separating protocol errors from implementation details keeps the public interface stable even as the internal architecture evolves.

---

### Why Standard Streams?

The engine intentionally communicates through standard input and standard output instead of using sockets, RPC frameworks, or shared memory.

This approach provides several practical advantages.

- no additional networking layer
- simple child-process integration
- language-independent communication
- easy debugging from a terminal
- deterministic process ownership
- straightforward deployment

Because the host application already owns the engine process, standard streams provide an efficient and reliable communication channel without introducing unnecessary complexity.

---

### Separation from Internal Components

The IPC layer is intentionally isolated from the media pipeline.

It does not perform:

- rendering
- capture
- encoding
- packetization
- bitrate control

Instead, it translates external requests into internal operations performed by the appropriate subsystem.

Likewise, lower-level components never generate protocol messages directly.

All communication with the outside world is centralized inside the service layer.

This architecture keeps protocol management independent from media processing.

---

### Protocol Evolution

The communication protocol is designed to evolve over time without requiring fundamental architectural changes.

New commands, optional parameters, and additional events can be introduced while preserving compatibility with existing integrations.

Because every feature is accessed through the same service boundary, expanding the protocol does not require exposing additional native APIs or modifying the overall process model.

The IPC layer therefore serves as a stable contract between the host application and the native streaming engine.

---

## Threading Model

The engine is designed around a multi-threaded execution model.

Different categories of work have fundamentally different performance characteristics and timing requirements.

Running all operations on a single thread would increase latency, reduce responsiveness, and unnecessarily couple unrelated subsystems.

Instead, the engine separates independent responsibilities across multiple execution contexts.

A simplified view of the threading architecture is shown below.

```text
                    Main Service Thread
                            │
      ┌─────────────────────┼─────────────────────┐
      │                     │                     │
      ▼                     ▼                     ▼
 IPC Processing      Capture Pipeline      Runtime Control
                                              │
                                              ▼
                                      Network Feedback
                                              │
                                              ▼
                                     Bitrate Scheduling

                OBS Internal Threads
                        │
                        ▼
            Rendering / Encoding / GPU Work

                Transport Thread(s)
                        │
                        ▼
                 RTP Packet Transmission
```

The exact implementation may evolve over time, but the architectural responsibilities remain the same.

---

### Design Goals

The threading model is designed around several objectives.

- minimize end-to-end latency
- maximize responsiveness
- isolate blocking operations
- prevent unnecessary synchronization
- simplify resource ownership
- allow independent subsystem evolution

The goal is not to maximize thread count, but to execute work in the most appropriate context.

---

### Service Thread

The service thread owns the public interface of the engine.

Its responsibilities include:

- reading IPC messages
- validating requests
- dispatching commands
- coordinating subsystem lifetime
- generating responses
- emitting asynchronous events

The service thread intentionally avoids performing long-running media operations.

Instead, it coordinates the work performed by other components.

This ensures that incoming commands remain responsive even while streaming is active.

---

### Capture Execution

Frame acquisition operates independently of IPC processing.

Windows Graphics Capture and the OBS rendering pipeline continuously produce frames while the host application remains free to send additional commands.

Separating capture from command processing prevents media activity from blocking external communication.

---

### OBS Internal Execution

libobs internally manages several execution contexts.

These include work related to:

- rendering
- graphics synchronization
- encoder execution
- GPU resource management

The engine intentionally delegates these responsibilities to libobs rather than introducing duplicate scheduling logic.

From the perspective of the engine, OBS behaves as an internal subsystem responsible for media processing.

---

### Transport Execution

The transport subsystem operates independently from rendering and encoding.

Its responsibilities include:

- packet preparation
- packet pacing
- UDP transmission
- transport statistics

Because networking behavior differs significantly from rendering workloads, transport activities remain isolated from graphics processing.

This separation improves responsiveness while reducing interference between GPU-bound and network-bound operations.

---

### Runtime Feedback

Network feedback arrives continuously throughout an active streaming session.

Rather than interrupting the media pipeline, feedback is processed independently and forwarded to the adaptive bitrate subsystem.

This allows congestion control decisions to occur concurrently with ongoing rendering and transmission.

The encoder continues processing frames while network conditions are evaluated in parallel.

---

### Synchronization

Although multiple execution contexts operate simultaneously, ownership rules remain explicit.

Each subsystem owns its internal state.

Shared resources are accessed only through well-defined synchronization points.

Whenever communication between components is required, ownership is transferred explicitly rather than allowing unrestricted concurrent access.

This reduces the likelihood of:

- race conditions
- inconsistent state
- deadlocks
- unpredictable execution order

---

### Blocking Operations

Potentially expensive operations are intentionally isolated from latency-sensitive execution paths.

Examples include:

- runtime initialization
- graphics setup
- encoder creation
- capture teardown

These operations occur relatively infrequently compared to normal frame processing.

Keeping them separate prevents temporary initialization work from affecting steady-state streaming performance.

---

### Thread Lifetime

Different threads exist for different durations.

Some remain active for the lifetime of the service.

Others exist only while a capture session is active.

```text
Service Starts
       │
       ▼
Service Thread
       │
       ├─────────────────────────────┐
       │                             │
       ▼                             ▼
Capture Session Begins        Waiting For Commands
       │
       ▼
Capture / OBS / Transport Work
       │
       ▼
Capture Session Ends
       │
       ▼
Session Threads Exit
       │
       ▼
Service Thread Continues
```

This distinction keeps idle resource usage low while allowing capture sessions to allocate only the execution contexts they require.

---

### Design Principles

The threading architecture follows several guiding principles.

- Long-running media operations should never block IPC processing.
- Rendering, encoding, and transport remain logically independent.
- Each subsystem owns its own execution context whenever practical.
- Synchronization should be explicit and minimal.
- Thread ownership must remain deterministic throughout the lifetime of the process.

These principles help maintain low latency, predictable behavior, and long-term maintainability while allowing the engine to scale as new capabilities are introduced.

---

## Resource Management

Efficient resource management is a fundamental design principle of the engine.

Real-time media applications allocate and release a wide variety of operating system, graphics, networking, and multimedia resources throughout their lifetime.

Improper ownership or cleanup can lead to memory leaks, GPU resource exhaustion, dangling references, or unstable behavior during subsequent capture sessions.

For this reason, every resource within the engine has a clearly defined owner and a deterministic lifetime.

The engine intentionally distinguishes between long-lived resources and session-scoped resources.

---

### Resource Ownership

Every resource belongs to exactly one component.

Ownership is never shared implicitly between unrelated subsystems.

```text
NativeService
        │
        ▼
    ObsEngine
        │
        ├─────────────┐
        ▼             ▼
 Capture        RTP Sender
        │             │
        ▼             ▼
  Session      Network Resources
```

Each component is responsible for:

- creating its own resources
- maintaining resource validity
- releasing owned resources
- preventing access after destruction

Higher-level components coordinate lifetime but do not directly manipulate resources owned by lower layers.

---

### Long-Lived Resources

Some resources remain alive for nearly the entire lifetime of the process.

These resources are relatively expensive to create and are therefore reused whenever possible.

Examples include:

- initialized libobs runtime
- loaded OBS modules
- graphics subsystem
- encoder registry
- service infrastructure

Creating these resources only once reduces startup latency and avoids repeated initialization overhead.

They remain available even after individual capture sessions have ended.

---

### Session Resources

Session resources exist only while an active capture session is running.

Typical examples include:

- active scene
- capture source
- encoder instance
- RTP sender
- transport state
- pacing state

These objects are created when streaming begins and destroyed when streaming ends.

```text
Capture Starts
       │
       ▼
Create Session Resources
       │
       ▼
Streaming
       │
       ▼
Release Session Resources
```

Keeping session resources isolated prevents previous capture sessions from affecting future ones.

---

### Lazy Allocation

Resources are allocated only when they are required.

For example:

- creating an encoder only when streaming begins
- creating an RTP sender only when transmission starts
- creating capture sources only for active sessions

This approach provides several advantages.

- reduced idle memory usage
- faster process startup
- lower GPU utilization
- simpler lifetime management

The engine intentionally avoids allocating expensive multimedia resources while idle.

---

### Deterministic Cleanup

Every allocated resource has a corresponding cleanup path.

Cleanup follows the reverse order of initialization.

```text
Initialization

Graphics
    │
    ▼
Scene
    │
    ▼
Capture Source
    │
    ▼
Encoder
    │
    ▼
Transport

Shutdown

Transport
    │
    ▼
Encoder
    │
    ▼
Capture Source
    │
    ▼
Scene
    │
    ▼
Graphics
```

Destroying resources in reverse order prevents invalid dependencies during shutdown.

---

### Failure Recovery

Initialization may fail at any stage.

For example:

- graphics initialization
- encoder creation
- capture source creation
- network initialization

The engine treats partially initialized state as temporary.

If initialization fails, any successfully created resources are released before the error is reported.

This guarantees that failed startup attempts do not leave the engine in an inconsistent state.

---

### Resource Isolation

Subsystems intentionally avoid accessing resources owned by other components.

For example:

- the transport layer never owns OBS objects
- the encoder never owns networking resources
- capture sources never own transport state

This separation greatly simplifies maintenance because each subsystem can evolve independently.

It also reduces accidental lifetime dependencies between unrelated components.

---

### Lifetime Hierarchy

Resource lifetime follows a strict hierarchy.

```text
Process Lifetime
        │
        ▼
Service Lifetime
        │
        ▼
OBS Runtime
        │
        ▼
Capture Session
        │
        ▼
Encoder
        │
        ▼
Transport
```

A child resource can never outlive its parent.

For example:

- an encoder cannot exist without an active capture session
- a capture session cannot exist without an initialized OBS runtime
- the OBS runtime cannot exist after the service terminates

This hierarchy keeps ownership relationships straightforward and prevents orphaned resources.

---

### Resource Reuse

Whenever possible, the engine reuses expensive infrastructure rather than recreating it.

Examples include:

- initialized OBS runtime
- loaded modules
- graphics initialization

In contrast, lightweight session resources are recreated for every stream.

This strategy balances performance with predictable cleanup.

The result is lower startup latency while preserving complete isolation between capture sessions.

---

### Design Principles

The resource management system follows several architectural principles.

- Every resource has exactly one owner.
- Resource lifetime is deterministic.
- Initialization and cleanup always occur in matching pairs.
- Expensive infrastructure is reused whenever practical.
- Session resources remain isolated from one another.
- Cleanup always occurs in reverse initialization order.
- Parent resources always outlive child resources.

These principles help ensure that long-running streaming sessions remain stable while allowing consecutive capture sessions to execute without requiring process restarts.

---

## Design Decisions

This section documents the major architectural decisions made during the development of the engine and the rationale behind each choice.

Understanding **why** the system is designed this way is just as important as understanding **how** it works.

Many implementation details may evolve over time, but the underlying architectural principles are intended to remain stable.

---

### Independent Process Instead of a Shared Library

One of the earliest design decisions was to implement the engine as a standalone executable rather than exposing a native shared library.

Instead of linking directly against the engine, the host application launches it as a child process and communicates through IPC.

This approach provides several important advantages.

- complete process isolation
- simplified crash recovery
- explicit process ownership
- language-independent integration
- easier deployment
- reduced coupling between the frontend and native code

If the engine encounters an unexpected failure, the host application can recover by restarting the process without affecting the rest of the application.

This separation significantly improves long-term maintainability.

---

### JSON-Based IPC

Communication between the host application and the engine uses UTF-8 encoded JSON messages.

Alternative approaches such as binary protocols or custom serialization formats were intentionally avoided.

JSON was selected because it is:

- human-readable
- easy to debug
- language-independent
- simple to extend
- widely supported

The slight serialization overhead is negligible compared to video encoding and network transmission.

Prioritizing simplicity and maintainability provides greater long-term value than micro-optimizing protocol performance.

---

### Standard Streams Instead of Sockets

The engine communicates through standard input and standard output.

Because the host application already owns the engine process, introducing an additional networking layer would unnecessarily increase architectural complexity.

Using standard streams provides:

- deterministic communication
- no port management
- no firewall interaction
- simpler deployment
- straightforward debugging

The result is a lightweight communication model with minimal external dependencies.

---

### OBS as an Internal Dependency

libobs is intentionally treated as an internal implementation detail.

The public API of the engine never exposes OBS types, objects, or concepts.

Instead, the engine provides its own abstraction layer.

This offers several benefits.

- host applications remain independent from OBS
- OBS upgrades are easier to manage
- implementation details remain encapsulated
- future rendering backends remain possible

This decision significantly reduces long-term coupling.

---

### Session-Oriented Architecture

Streaming is modeled as a temporary session rather than a permanent runtime state.

Each capture session owns its own:

- scene
- capture source
- encoder
- transport state

When streaming ends, these resources are destroyed while the underlying service continues running.

This prevents state from leaking between independent capture sessions.

---

### Lazy Initialization

Expensive multimedia infrastructure is created only when required.

Rather than initializing every subsystem during process startup, the engine allocates resources only after receiving the appropriate command.

This approach improves:

- startup performance
- idle resource usage
- service responsiveness
- overall scalability

The engine therefore remains lightweight while idle.

---

### Separation of Responsibilities

Each subsystem owns a clearly defined responsibility.

For example:

- capture acquires frames
- OBS renders scenes
- the encoder compresses video
- RTP transmits packets
- adaptive bitrate evaluates network conditions
- IPC coordinates external communication

Responsibilities intentionally do not overlap.

This keeps the architecture modular and significantly simplifies maintenance.

---

### One-Way Dependency Flow

The engine follows a one-directional dependency model.

Higher-level components coordinate lower-level systems, but lower-level components never depend on application logic.

```text
Host Application
        │
        ▼
NativeService
        │
        ▼
ObsEngine
        │
        ▼
Capture
        │
        ▼
Encoder
        │
        ▼
Transport
```

This structure minimizes cyclic dependencies and makes the architecture easier to reason about as the project grows.

---

### Hardware-First Philosophy

Whenever possible, the engine prefers dedicated hardware encoders over software encoding.

Dedicated hardware significantly reduces CPU utilization and improves overall responsiveness during gaming and screen sharing.

If hardware acceleration is unavailable, the engine automatically falls back to software encoding rather than terminating the capture session.

This strategy prioritizes compatibility and reliability.

---

### Long-Lived Service, Short-Lived Sessions

The service itself is intended to remain alive for an extended period.

Individual capture sessions are comparatively short-lived.

Separating service lifetime from session lifetime allows expensive infrastructure to be reused while ensuring that capture-specific resources are recreated for every new stream.

This balance improves both performance and reliability.

---

### Architecture Before Optimization

Throughout development, architectural clarity has consistently taken priority over premature optimization.

The project intentionally favors:

- explicit ownership
- predictable lifetimes
- modular components
- maintainable abstractions
- stable public interfaces

Only after these architectural goals are satisfied are implementation-level optimizations considered.

This philosophy has guided the overall structure of the engine from its earliest revisions and continues to influence future development.

---

## Error Handling

Robust error handling is essential for long-running real-time applications.

Unlike short-lived command-line utilities, the engine is expected to remain operational even after individual failures occur.

For this reason, errors are treated as isolated events whenever possible rather than process-ending conditions.

The primary objective of the error handling strategy is to preserve service availability while ensuring that failures remain predictable and observable.

---

### Design Goals

The error handling architecture is built around several core principles.

- isolate failures whenever possible
- preserve service stability
- fail fast during initialization
- recover gracefully during runtime
- report meaningful errors to the host application
- avoid partially initialized states

These principles help ensure that individual failures do not compromise the overall integrity of the engine.

---

### Failure Categories

Not every failure is handled in the same way.

The engine distinguishes between several categories of runtime failures.

#### Configuration Errors

These occur before streaming begins.

Examples include:

- invalid command parameters
- unsupported encoder configuration
- missing runtime files
- invalid capture target

Configuration errors prevent the requested operation from starting but do not affect the running service.

---

#### Initialization Failures

Initialization failures occur while constructing a new capture session.

Examples include:

- graphics initialization failure
- encoder creation failure
- OBS initialization failure
- transport initialization failure

If initialization cannot complete successfully, any partially created resources are released before reporting the error.

No incomplete capture session is allowed to remain active.

---

#### Runtime Failures

Runtime failures occur after streaming has already started.

Examples include:

- capture target closed
- encoder failure
- transport interruption
- unexpected media pipeline errors

Whenever possible, runtime failures terminate only the active capture session rather than the entire engine.

The service itself remains available for future requests.

---

### Error Propagation

Errors always propagate upward through well-defined boundaries.

```text
Low-Level Component
        │
        ▼
ObsEngine
        │
        ▼
NativeService
        │
        ▼
IPC Response / Event
        │
        ▼
Host Application
```

Lower-level components report failures to their immediate owner.

Only the service layer communicates failures to the outside world.

This prevents implementation details from leaking into the public API.

---

### Fail Fast During Initialization

Initialization follows a fail-fast philosophy.

If a required subsystem cannot be initialized correctly, startup stops immediately.

Attempting to continue with an incomplete media pipeline would increase architectural complexity and create difficult-to-debug runtime behavior.

Failing early produces a predictable system state and simplifies recovery.

---

### Graceful Runtime Recovery

Once streaming has begun, the engine favors graceful recovery whenever possible.

Examples include:

- stopping only the active capture session
- releasing temporary resources
- returning to the idle state
- remaining available for subsequent requests

This behavior minimizes disruption for the host application and avoids unnecessary process restarts.

---

### Resource Cleanup

Every failure path follows the same ownership rules as normal shutdown.

Resources are released in reverse initialization order.

This guarantees that failed operations leave the engine in the same clean state as a successful shutdown.

Cleanup is considered part of the error handling strategy rather than an independent concern.

---

### Error Reporting

The host application receives structured error information through the IPC protocol.

Rather than exposing implementation-specific details, the engine reports stable, high-level error categories.

Typical information includes:

- operation that failed
- failure category
- descriptive message
- current service state

This provides sufficient context for user-facing applications while preserving internal implementation flexibility.

---

### Logging

Internal logging and external error reporting serve different purposes.

Logging is intended primarily for:

- debugging
- diagnostics
- development
- maintenance

IPC error responses, on the other hand, are intended for application logic.

Keeping these concerns separate allows diagnostic output to evolve without affecting protocol compatibility.

---

### Design Principles

The error handling strategy follows several architectural principles.

- Failures should remain localized whenever possible.
- Initialization should fail immediately if required conditions cannot be satisfied.
- Runtime failures should preserve service availability whenever practical.
- Every failure path must perform deterministic cleanup.
- Internal implementation details should remain hidden behind stable protocol messages.

These principles help ensure that the engine remains predictable, maintainable, and resilient during both development and production use.

---

## Extending the Engine

The engine has been designed with long-term extensibility in mind.

Rather than optimizing exclusively for the current feature set, the architecture emphasizes clear subsystem boundaries and stable internal interfaces.

As a result, most future functionality can be introduced by extending existing components instead of redesigning the overall architecture.

The objective is that new capabilities should integrate naturally into the existing pipeline while minimizing changes to unrelated subsystems.

---

### Guiding Principles

When extending the engine, new functionality should follow the same architectural principles as the existing implementation.

New features should:

- have a single, well-defined responsibility
- integrate through existing subsystem boundaries
- avoid introducing cyclic dependencies
- preserve deterministic resource ownership
- maintain the existing process model
- remain independent from unrelated components

Architectural consistency is generally more valuable than minimizing the number of source files or classes.

---

### Adding New Capture Backends

The capture pipeline intentionally separates frame acquisition from rendering, encoding, and transport.

This allows additional capture technologies to be introduced without modifying the remainder of the streaming pipeline.

Future capture implementations may include platform-specific APIs or alternative capture mechanisms while continuing to produce frames through the same internal interfaces.

The rendering, encoding, transport, and IPC layers should remain unaffected by these changes.

---

### Supporting Additional Codecs

The transport architecture intentionally avoids assumptions beyond receiving encoded video data.

As a result, future encoder implementations can be introduced with minimal impact on higher-level components.

Potential additions include newer video codecs, improved hardware encoder integrations, or platform-specific encoding technologies.

Ideally, selecting a different codec should affect only the encoder layer while preserving the remainder of the media pipeline.

---

### Alternative Transport Protocols

Although the current implementation focuses on RTP-based streaming, the transport layer is intentionally isolated from the rendering and encoding pipeline.

Future transport implementations could coexist alongside RTP while reusing the same capture and encoding infrastructure.

By separating media production from media delivery, protocol evolution remains largely independent from the rest of the engine.

---

### Platform Expansion

The current implementation targets Windows.

However, most architectural decisions intentionally avoid assumptions that are specific to a single operating system.

Platform-dependent functionality is concentrated primarily within:

- capture backends
- operating system utilities
- graphics integration

Higher-level systems such as IPC, resource management, adaptive bitrate, and transport remain largely platform-agnostic.

This separation simplifies future platform support while preserving a common architecture across operating systems.

---

### Additional Runtime Features

The current architecture also provides natural extension points for higher-level streaming functionality.

Examples include:

- stream overlays
- annotation layers
- cursor enhancements
- recording support
- diagnostics
- runtime metrics
- advanced monitoring
- additional media sources

Whenever possible, these capabilities should be implemented by extending existing subsystems rather than introducing parallel implementations.

---

### Preserving Architectural Boundaries

As the project grows, maintaining clear subsystem boundaries becomes increasingly important.

New features should avoid bypassing existing abstractions simply because doing so appears more convenient.

For example:

- transport components should not manipulate capture objects
- encoder components should not communicate directly with IPC
- capture components should remain independent from networking

Respecting these boundaries helps maintain a modular architecture as complexity increases.

---

### Backward Compatibility

Whenever practical, new capabilities should extend the existing interfaces rather than replacing them.

Stable public interfaces reduce integration effort for host applications and simplify long-term maintenance.

Internal implementations may evolve significantly over time, but external integrations should require as few breaking changes as possible.

---

### Design Philosophy

The architecture intentionally favors evolution through composition rather than modification.

Instead of rewriting existing systems when new requirements emerge, new functionality should be introduced by extending well-defined components and interfaces.

This philosophy has guided the project from its earliest revisions and remains one of its primary architectural goals.

---

## Future Directions

The current architecture establishes a solid foundation for a production-grade real-time streaming engine.

While the existing implementation already provides a complete end-to-end streaming pipeline, the architecture has intentionally been designed to accommodate future capabilities without requiring fundamental redesign.

Rather than predicting specific features, this section outlines the architectural direction of the project.

---

### A Stable Core

The long-term objective is to preserve a stable architectural core while allowing individual subsystems to evolve independently.

The overall process model, subsystem boundaries, and ownership rules should remain consistent even as new capabilities are introduced.

Maintaining architectural stability reduces long-term maintenance costs and improves confidence when extending the engine.

---

### Modular Growth

Future development should primarily occur through the addition of new components rather than modification of existing ones.

Examples include:

- additional capture backends
- new encoder implementations
- additional transport protocols
- platform-specific optimizations
- improved diagnostics
- enhanced monitoring capabilities

Whenever practical, new functionality should integrate through existing subsystem interfaces instead of bypassing established architectural boundaries.

---

### Cross-Platform Expansion

Although the current implementation targets Windows, the architecture intentionally separates platform-independent systems from operating system specific functionality.

This separation provides a clear path toward supporting additional platforms in the future.

Platform-specific code should remain concentrated within:

- capture implementations
- graphics integration
- operating system utilities

Higher-level systems such as IPC, transport, adaptive bitrate, resource management, and service coordination should remain largely platform-independent.

---

### Protocol Evolution

The IPC protocol is expected to grow over time as new functionality becomes available.

New commands, optional parameters, and additional asynchronous events should extend the existing protocol without unnecessarily breaking compatibility with existing integrations.

A stable communication contract allows host applications to evolve independently from the native implementation.

---

### Media Pipeline Evolution

The media pipeline has been intentionally divided into independent stages.

As a result, improvements within one stage should require minimal changes elsewhere.

Examples include:

- supporting additional codecs
- introducing improved rate control algorithms
- replacing transport implementations
- extending rendering capabilities

Each subsystem should continue to communicate through well-defined interfaces rather than implementation-specific behavior.

---

### Maintaining Architectural Simplicity

As the project grows, preserving architectural clarity becomes increasingly important.

Complexity should be introduced only when it provides measurable value.

Whenever multiple solutions are technically viable, preference should generally be given to the approach that improves maintainability, readability, and long-term evolution.

Keeping subsystem responsibilities narrowly focused remains one of the project's primary architectural principles.

---

### Long-Term Maintainability

The architecture prioritizes maintainability over short-term optimization.

Clear ownership, deterministic resource lifetime, modular components, and explicit subsystem boundaries provide a foundation that can support future growth without significantly increasing complexity.

This philosophy encourages incremental improvement rather than large-scale redesign.

---

### Closing Remarks

The engine is intended to be more than a collection of media components.

It is designed as a modular, embeddable, and maintainable real-time streaming platform.

Every major architectural decision—from process isolation and IPC design to resource ownership and adaptive bitrate control—has been made with long-term sustainability in mind.

As new features and platforms are introduced, the implementation may evolve considerably.

The architectural principles described throughout this document, however, are expected to remain the foundation upon which future development continues.
