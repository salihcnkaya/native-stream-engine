# Building native-stream-engine

This guide describes the current production build workflow for **Windows and Linux**.

The project no longer uses the old manual `obs.def` / `make-obs-import-lib.ps1` workflow. OBS Studio is built from a pinned upstream source revision by platform-specific scripts in this repository.

---

## 1. Pinned OBS revision

Both runtime scripts build:

```text
OBS Studio 32.1.2
```

and verify the exact commit:

```text
fb4d98bf88fae5fc85cb11fc57f7c5e309282194
```

If the tag resolves to another commit, the scripts stop.

OBS source is cloned into temporary local build directories and is not vendored into Git.

---

## 2. Repository layout

Relevant tracked files:

```text
native-stream-engine/
├── compatibility/
│   └── obs-32.1.2/
│       ├── obs.lib
│       └── obsconfig.h
├── docs/
├── patches/
│   └── obs/
│       ├── common-nvenc-lightweight-reconfig.patch
│       ├── common-qsv-skip-noop-reconfig.patch
│       ├── linux-pipewire-expose-restore-token.patch
│       └── README.md
├── scripts/
│   ├── build-obs-linux-runtime.sh
│   └── build-obs-windows-runtime.ps1
├── src/
├── CMakeLists.txt
├── COPYRIGHT
├── LICENSE
└── README.md
```

Generated runtime directories:

```text
runtime/OBS-Studio-32.1.2-Windows-x64
runtime/OBS-Studio-32.1.2-Linux-x86_64
```

These runtimes are local/generated artifacts and should not be committed.

---

# Windows

## 3. Windows requirements

Required:

- Windows x64
- Visual Studio 2022 C++ build tools
- MSVC
- Windows SDK
- CMake
- Git
- PowerShell

The runtime builder uses the Visual Studio 2022 x64 generator.

---

## 4. Build the pinned Windows OBS runtime

From the repository root:

```powershell
.\scripts\build-obs-windows-runtime.ps1
```

If script execution policy blocks local scripts:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-obs-windows-runtime.ps1
```

### What the script does

The script:

1. removes/recreates its temporary OBS working tree,
2. shallow-clones OBS Studio 32.1.2 with required submodules,
3. verifies commit `fb4d98bf88fae5fc85cb11fc57f7c5e309282194`,
4. applies:
   - `common-*.patch`
   - `windows-*.patch`
5. restricts the OBS plugin/build graph to the modules needed by native-stream-engine,
6. configures a Visual Studio 2022 x64 OBS build,
7. builds the selected OBS/libobs/plugin/helper targets,
8. uses OBS runtime staging/dependency bundling,
9. publishes the runtime,
10. publishes the official libobs import library and generated `obsconfig.h`.

Default selected target set includes:

```text
libobs
libobs-d3d11
win-capture
win-wasapi
obs-ffmpeg
obs-ffmpeg-mux
obs-x264
obs-qsv11
obs-qsv-test
obs-nvenc
obs-nvenc-test
nse-runtime-stage
```

### Output

Runtime:

```text
runtime/OBS-Studio-32.1.2-Windows-x64/
```

Compatibility files:

```text
compatibility/obs-32.1.2/obs.lib
compatibility/obs-32.1.2/obsconfig.h
```

The compatibility files are generated from the same pinned OBS build used to create the runtime.

### Important

Do **not** recreate the old workflow:

```text
compatibility/obs-32.1.2/obs.def
scripts/make-obs-import-lib.ps1
scripts/build-patched-obs-plugins.ps1
```

Those files/workflows are obsolete.

No manual patched-DLL replacement is required.

---

## 5. Configure the Windows engine

After the runtime exists:

```powershell
cmake -S . -B build-windows-test -G "Visual Studio 17 2022" -A x64
```

CMake checks for:

```text
runtime/OBS-Studio-32.1.2-Windows-x64/bin/64bit/obs.dll
compatibility/obs-32.1.2/obs.lib
compatibility/obs-32.1.2/obsconfig.h
```

Configuration fails with an explicit error when a required item is missing.

---

## 6. Build Windows Release

```powershell
cmake --build build-windows-test --config Release -j
```

Expected executable:

```text
build-windows-test/Release/native-stream-engine.exe
```

The post-build step copies required Windows OBS runtime content next to the executable, including:

```text
bin/64bit runtime files
obs-plugins/
data/
```

---

## 7. Windows smoke test

Run from the repository root so the engine's runtime lookup can resolve the repository `runtime/` directory:

```powershell
.\build-windows-test\Release\native-stream-engine.exe --list
```

A successful run should initialize the pinned OBS runtime, enumerate loaded modules/encoders, shut down, and return normally.

Other useful commands:

```powershell
.\build-windows-test\Release\native-stream-engine.exe --list-windows

.\build-windows-test\Release\native-stream-engine.exe --list-sources --json 1

.\build-windows-test\Release\native-stream-engine.exe --service
```

---

# Linux

## 8. Linux requirements

The engine currently targets Linux x86_64.

The Linux engine build directly requires development packages for:

- Qt6 Core
- Qt6 DBus
- GLib / GIO
- PipeWire

The OBS runtime build additionally needs the dependencies required by the selected OBS 32.1.2 modules.

Build tools used by the runtime script include:

- Bash
- Git
- CMake
- Ninja
- `patchelf`

The OBS and third-party module builds may require additional compiler/system development packages supplied by the Linux distribution.

---

## 9. Build the pinned Linux OBS runtime

Ensure the script is executable:

```bash
chmod +x scripts/build-obs-linux-runtime.sh
```

Run:

```bash
./scripts/build-obs-linux-runtime.sh
```

### Runtime builder pins

OBS:

```text
32.1.2
fb4d98bf88fae5fc85cb11fc57f7c5e309282194
```

PipeWire application-audio plugin:

```text
obs-pipewire-audio-capture 1.2.1
```

### What the script does

The script:

1. deletes/recreates `.obs-build-tmp-linux`,
2. clones OBS Studio 32.1.2 with submodules,
3. verifies the exact OBS commit,
4. applies:
   - `common-*.patch`
   - `linux-*.patch`
5. replaces the OBS plugin graph with the minimal native-stream-engine set,
6. configures OBS with Ninja/Release,
7. builds the selected OBS targets,
8. clones and builds `obs-pipewire-audio-capture`,
9. assembles the Linux runtime directory,
10. copies libobs and graphics libraries,
11. copies required plugins and plugin data,
12. copies libobs public headers and generated `obsconfig.h`,
13. fixes runtime RPATH/RUNPATH values with `patchelf`.

Selected OBS target set:

```text
libobs
libobs-opengl
obs-nvenc
obs-nvenc-test
obs-qsv11
obs-ffmpeg
obs-x264
linux-pipewire
linux-pulseaudio
```

Additional application-audio plugin:

```text
linux-pipewire-audio.so
```

### Output

```text
runtime/OBS-Studio-32.1.2-Linux-x86_64/
```

Expected high-level layout:

```text
runtime/OBS-Studio-32.1.2-Linux-x86_64/
├── bin/
│   └── obs-nvenc-test
├── data/
├── include/
│   └── obsconfig.h
├── lib/
│   ├── libobs.so...
│   └── libobs-opengl.so...
└── obs-plugins/
    ├── linux-pipewire.so
    ├── linux-pulseaudio.so
    ├── linux-pipewire-audio.so
    ├── obs-ffmpeg.so
    ├── obs-nvenc.so
    ├── obs-qsv11.so
    └── obs-x264.so
```

---

## 10. Configure the Linux engine

```bash
cmake -S . -B build-linux-nodeps-test -DCMAKE_BUILD_TYPE=Release
```

CMake requires:

```text
runtime/OBS-Studio-32.1.2-Linux-x86_64/include/obsconfig.h
runtime/OBS-Studio-32.1.2-Linux-x86_64/lib/libobs.so
```

and resolves:

```text
Qt6::Core
Qt6::DBus
glib-2.0
gio-2.0
libpipewire-0.3
```

The build uses the generated OBS runtime headers directly.

---

## 11. Build Linux Release

```bash
cmake --build build-linux-nodeps-test -j"$(nproc)"
```

Expected executable:

```text
build-linux-nodeps-test/native-stream-engine
```

The Linux post-build step also copies:

```text
obs-nvenc-test
```

next to the executable.

The build target is configured with an RPATH pointing at the generated OBS runtime `lib` directory for the local build.

---

## 12. Linux smoke test

From the repository root:

```bash
./build-linux-nodeps-test/native-stream-engine --list
```

A successful inventory run should:

- initialize libobs,
- load the selected Linux modules,
- enumerate encoders,
- shut down normally.

Useful service command:

```bash
./build-linux-nodeps-test/native-stream-engine --service
```

Linux source-enumeration note:

```bash
./build-linux-nodeps-test/native-stream-engine --list-sources --json 1
```

currently returns empty source arrays because Linux pre-capture enumeration is not implemented. PipeWire portal source selection occurs during capture creation.

---

# Shared build notes

## 13. OBS patches

Current patches:

```text
patches/obs/common-nvenc-lightweight-reconfig.patch
patches/obs/common-qsv-skip-noop-reconfig.patch
patches/obs/linux-pipewire-expose-restore-token.patch
```

Patch application is deterministic:

- each matching patch is checked with `git apply --check`,
- the script fails if a patch no longer applies,
- common patches are applied on both platforms,
- platform-specific patches are only applied to the matching platform.

See:

```text
patches/obs/README.md
```

for behavior details.

---

## 14. Cleaning engine builds

Windows:

```powershell
Remove-Item -Recurse -Force build-windows-test -ErrorAction SilentlyContinue
```

Linux:

```bash
rm -rf build-linux-nodeps-test
```

Reconfigure after significant CMake/runtime changes.

---

## 15. Cleaning/rebuilding OBS runtimes

The platform scripts manage their own temporary OBS source/build directories.

Windows default temporary directory:

```text
.obs-build-tmp
```

Linux default temporary directory:

```text
.obs-build-tmp-linux
```

The scripts rebuild the platform runtime from the pinned source rather than relying on an arbitrary system OBS installation.

---

## 16. Runtime lookup when running the engine

`main.cpp` resolves the platform runtime relative to the current working directory:

Windows:

```text
runtime/OBS-Studio-32.1.2-Windows-x64
```

Linux:

```text
runtime/OBS-Studio-32.1.2-Linux-x86_64
```

For repository smoke tests, run commands from the repository root.

Packaged desktop deployments may place the runtime according to the host application's packaging/preparation workflow; that workflow is outside this repository's CMake build itself.

---

## 17. Common Windows failures

### OBS runtime not found

Error references:

```text
runtime/OBS-Studio-32.1.2-Windows-x64/bin/64bit/obs.dll
```

Fix:

```powershell
.\scripts\build-obs-windows-runtime.ps1
```

### OBS import library not found

Expected:

```text
compatibility/obs-32.1.2/obs.lib
```

Rebuild the Windows OBS runtime. Do not recreate the removed `obs.def` workflow.

### OBS compatibility header not found

Expected:

```text
compatibility/obs-32.1.2/obsconfig.h
```

Rebuild the Windows OBS runtime.

---

## 18. Common Linux failures

### `libobs.so` or `obsconfig.h` missing

Run:

```bash
./scripts/build-obs-linux-runtime.sh
```

### Qt6 / DBus missing

CMake's `find_package(Qt6 COMPONENTS Core DBus)` failed. Install the distribution's Qt6 development packages providing Core and DBus.

### GLib / GIO missing

CMake resolves them through `pkg-config`.

Required pkg-config modules:

```text
glib-2.0
gio-2.0
```

### PipeWire development package missing

Required pkg-config module:

```text
libpipewire-0.3
```

### `patchelf` missing

The Linux runtime builder requires `patchelf` to make the assembled runtime relocatable.

---

## 19. Verification before committing/releasing build changes

At minimum verify the platform being changed.

### Windows

```powershell
.\scripts\build-obs-windows-runtime.ps1

Remove-Item -Recurse -Force build-windows-test -ErrorAction SilentlyContinue

cmake -S . -B build-windows-test -G "Visual Studio 17 2022" -A x64
cmake --build build-windows-test --config Release -j

.\build-windows-test\Release\native-stream-engine.exe --list
```

### Linux

```bash
./scripts/build-obs-linux-runtime.sh

rm -rf build-linux-nodeps-test

cmake -S . -B build-linux-nodeps-test -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux-nodeps-test -j"$(nproc)"

./build-linux-nodeps-test/native-stream-engine --list
```

For media/transport changes, `--list` alone is not sufficient. Run the relevant capture, audio, RTP, bitrate, and network regression tests before release.

---

## 20. Files that should not return

The following belong to the superseded build workflow and should not be reintroduced:

```text
compatibility/obs-32.1.2/obs.def
scripts/make-obs-import-lib.ps1
scripts/build-patched-obs-plugins.ps1
patches/obs/nvenc-lightweight-reconfig.patch
patches/obs/qsv-skip-noop-reconfig.patch
```

The patch replacements are:

```text
patches/obs/common-nvenc-lightweight-reconfig.patch
patches/obs/common-qsv-skip-noop-reconfig.patch
```

and runtime construction is handled by the two current platform scripts.
