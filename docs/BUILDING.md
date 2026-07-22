# Building native-stream-engine

This document explains how to build `native-stream-engine` from source on Windows.

## Supported environment

The current implementation supports:

- Windows 10 or Windows 11
- x64 architecture
- Microsoft Visual C++
- CMake 3.24 or newer
- C++17
- OBS Studio 32.1.2

Other operating systems, CPU architectures, OBS versions, and compiler toolchains are not currently supported.

## Requirements

Install the following components before building:

- Visual Studio with Microsoft C++ build tools
- Desktop development with C++ workload
- MSVC x64 compiler and linker
- Windows 10 or Windows 11 SDK
- CMake 3.24 or newer
- PowerShell 5.1 or newer
- Git, when cloning the repository from a remote source

Visual Studio 2022 or newer is recommended.

The project must be configured for the x64 architecture.

## Local repository layout

After downloading the required OBS files, the expected local layout is:

```text
native-stream-engine/
├── compatibility/
│   └── obs-32.1.2/
│       ├── obs.def
│       ├── obs.lib
│       └── obsconfig.h
├── deps/
│   └── obs-studio-32.1.2/
├── runtime/
│   └── OBS-Studio-32.1.2-Windows-x64/
├── scripts/
│   └── make-obs-import-lib.ps1
├── src/
├── CMakeLists.txt
├── BUILDING.md
├── COPYRIGHT
├── LICENSE
└── README.md
```

The following directories are intentionally excluded from Git:

```text
build/
deps/
runtime/
```

They contain generated outputs or locally installed dependencies.

## OBS Studio source files

Download the OBS Studio 32.1.2 source archive.

Extract the source tree into:

```text
deps/obs-studio-32.1.2/
```

After extraction, this file must exist:

```text
deps/obs-studio-32.1.2/libobs/obs.h
```

The archive must not create an additional nested source directory.

Correct:

```text
deps/obs-studio-32.1.2/libobs/obs.h
```

Incorrect:

```text
deps/obs-studio-32.1.2/obs-studio-32.1.2/libobs/obs.h
```

The source tree is used for the libobs headers required during compilation.

This project does not compile the complete OBS Studio source tree.

## OBS Studio Windows runtime

Download the OBS Studio 32.1.2 Windows x64 runtime archive.

Extract it into:

```text
runtime/OBS-Studio-32.1.2-Windows-x64/
```

After extraction, this file must exist:

```text
runtime/OBS-Studio-32.1.2-Windows-x64/bin/64bit/obs.dll
```

The expected runtime layout includes:

```text
runtime/OBS-Studio-32.1.2-Windows-x64/
├── bin/
│   └── 64bit/
├── data/
└── obs-plugins/
    └── 64bit/
```

The OBS runtime provides the DLLs, plugins, effects, locale files, and other data required while the engine is running.

## OBS compatibility layer

The repository includes a compatibility layer for OBS Studio 32.1.2:

```text
compatibility/obs-32.1.2/
├── obs.def
├── obs.lib
└── obsconfig.h
```

### obs.def

`obs.def` contains the exported libobs symbols currently required by the engine.

The export list is intentionally explicit and minimal. It should not automatically contain every export exposed by `obs.dll`.

When the engine starts using an additional libobs function, the corresponding exported symbol must be added manually to `obs.def`.

### obs.lib

`obs.lib` is an x64 import library generated from `obs.def`.

A verified copy is included in the repository so that normal builds do not require import-library regeneration.

### obsconfig.h

`obsconfig.h` supplies configuration definitions required by the OBS headers used by this project.

## Regenerating obs.lib

Regeneration is only required when:

- `obs.def` is changed
- the engine starts using another libobs export
- `obs.lib` is missing
- the compatibility layer is intentionally rebuilt

Open:

```text
Developer PowerShell for Visual Studio
```

Change to the repository root and run:

```powershell
.\scripts\make-obs-import-lib.ps1
```

The script reads:

```text
compatibility/obs-32.1.2/obs.def
```

and generates:

```text
compatibility/obs-32.1.2/obs.lib
```

The generated import library targets x64.

The script uses the Microsoft Library Manager:

```text
lib.exe
```

For that reason, it must be run from a Visual Studio developer terminal or another environment where `lib.exe` is available.

Do not regenerate `obs.def` automatically from every export in `obs.dll`. The compatibility surface should remain controlled and limited to the symbols actually used by the engine.

## Configure the project

Open PowerShell or Developer PowerShell in the repository root.

Configure a Visual Studio x64 build:

```powershell
cmake -S . -B build -A x64
```

The default dependency paths are:

```text
deps/obs-studio-32.1.2
runtime/OBS-Studio-32.1.2-Windows-x64
compatibility/obs-32.1.2
```

During configuration, CMake verifies that the required OBS source, runtime, import library, and compatibility header are present.

If one of these dependencies is missing, configuration stops with an error describing the expected path.

## Build the engine

Build the Release configuration:

```powershell
cmake --build build --config Release
```

The engine executable is generated at:

```text
build/Release/native-stream-engine.exe
```

As part of the post-build step, the contents of:

```text
runtime/OBS-Studio-32.1.2-Windows-x64/bin/64bit
```

are copied into:

```text
build/Release/
```

This places `obs.dll` and the other required runtime binaries next to the executable.

## Run the engine

Run the Release build from the repository root:

```powershell
.\build\Release\native-stream-engine.exe
```

The engine uses standard input and standard output for communication with its host process.

Commands and events use line-delimited JSON.

Each JSON message must be written on a single line and terminated with a newline character.

## Basic smoke test

After starting the engine, send a ping request through standard input:

```json
{ "id": "smoke-1", "command": "ping" }
```

The engine should return a response associated with the same request ID and confirm that it is alive.

After the ping test, send the shutdown command supported by the engine protocol.

The process should exit cleanly without remaining active in the background.

A successful basic verification consists of:

- CMake configuration completes
- Release build completes
- `native-stream-engine.exe` is generated
- `obs.dll` is copied beside the executable
- the engine starts successfully
- the ping request receives a pong response
- the shutdown request terminates the process cleanly

## Verify generated files

Confirm that the executable exists:

```powershell
Test-Path .\build\Release\native-stream-engine.exe
```

Expected result:

```text
True
```

Confirm that the OBS runtime DLL was copied:

```powershell
Test-Path .\build\Release\obs.dll
```

Expected result:

```text
True
```

Confirm that the OBS import library exists:

```powershell
Test-Path .\compatibility\obs-32.1.2\obs.lib
```

Expected result:

```text
True
```

## Clean build

To remove the existing build output:

```powershell
Remove-Item .\build -Recurse -Force -ErrorAction SilentlyContinue
```

Configure and build again:

```powershell
cmake -S . -B build -A x64

cmake --build build --config Release
```

A clean build should be used after:

- changing CMake configuration
- changing compiler options
- replacing the OBS compatibility library
- changing dependency paths
- upgrading Visual Studio or the Windows SDK
- encountering stale linker or generated-project errors

## Custom dependency paths

The default dependency locations may be overridden during CMake configuration.

Example:

```powershell
cmake -S . -B build -A x64 `
    -DOBS_SOURCE_DIR="C:\path\to\obs-source" `
    -DOBS_RUNTIME_DIR="C:\path\to\obs-runtime" `
    -DOBS_COMPATIBILITY_DIR="C:\path\to\obs-compatibility"
```

Absolute paths are recommended when overriding the defaults.

The custom OBS source directory must contain:

```text
libobs/obs.h
```

The custom runtime directory must contain:

```text
bin/64bit/obs.dll
```

The custom compatibility directory must contain:

```text
obs.def
obs.lib
obsconfig.h
```

## Troubleshooting

### OBS source was not found

Verify that the following file exists:

```text
deps/obs-studio-32.1.2/libobs/obs.h
```

Check whether the source archive was extracted into an extra nested directory.

### OBS runtime was not found

Verify that the following file exists:

```text
runtime/OBS-Studio-32.1.2-Windows-x64/bin/64bit/obs.dll
```

Make sure the Windows x64 runtime package was downloaded rather than only the OBS source archive.

### obs.lib was not found

The repository normally includes:

```text
compatibility/obs-32.1.2/obs.lib
```

If the file is missing, regenerate it from a Visual Studio developer terminal:

```powershell
.\scripts\make-obs-import-lib.ps1
```

### lib.exe was not found

Run the import-library script from:

```text
Developer PowerShell for Visual Studio
```

Also verify that the Visual Studio C++ build tools are installed.

### Unresolved external symbol

An unresolved libobs symbol may indicate that the required function is not listed in:

```text
compatibility/obs-32.1.2/obs.def
```

Verify that the symbol is exported by the exact `obs.dll` version used by the project.

Add only the required exported symbol to `obs.def`, regenerate `obs.lib`, delete the existing `build` directory, and perform a clean build.

### Duplicate symbol or invalid import-library errors

Do not generate the compatibility library from an unfiltered list of every export in `obs.dll`.

Use the controlled `obs.def` file included with the repository.

After correcting `obs.def`, regenerate `obs.lib` and perform a clean build.

### DLL was not found when starting the engine

Confirm that the post-build copy completed successfully:

```powershell
Test-Path .\build\Release\obs.dll
```

If the DLL is missing, verify the configured OBS runtime path and perform a clean Release build.

### OBS plugin could not be loaded

OBS plugins may depend on:

- additional DLLs
- plugin data directories
- locale files
- graphics resources
- helper executables

Confirm that the required OBS runtime files are available to the engine or embedding application.

The current CMake post-build step copies the contents of `bin/64bit`. Additional plugin and data directories may still be required by the final packaged application.

### The engine starts but capture initialization fails

Verify that:

- the OBS runtime version is exactly 32.1.2
- the source headers and runtime come from the same OBS version
- `obs.lib` was generated for x64
- the Windows SDK is installed
- the process has access to the required OBS plugin and data directories
- the selected capture target still exists
- the graphics adapter and driver support the requested capture and encoding path

### The process does not shut down

Send the protocol shutdown request and wait for the process to exit normally.

The host application should not terminate the process forcibly unless graceful shutdown has failed.

During development, verify that no `native-stream-engine.exe` process remains after the shutdown response.

## Version compatibility

The current compatibility files, source paths, runtime paths, and tested build configuration target OBS Studio 32.1.2.

Using a different OBS version without updating the compatibility layer may result in:

- missing exports
- linker failures
- binary incompatibility
- plugin loading failures
- changed libobs APIs
- runtime initialization failures
- crashes

Do not replace only the OBS runtime or only the OBS source tree with another version.

The source headers, runtime files, and compatibility layer must be treated as one versioned set.

## Distribution note

The local `runtime` directory is not committed to the repository.

A packaged application or release must provide all OBS runtime files required by the enabled capture and encoding features.

Building the executable successfully does not by itself guarantee that a separately packaged copy contains every required OBS plugin, data file, locale resource, or helper executable.

Runtime packaging should therefore be tested independently from the source build.
