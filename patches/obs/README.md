# OBS patches for native-stream-engine

This directory contains the small OBS Studio source patches required by
native-stream-engine.

OBS Studio source is not vendored into this repository. The build scripts
clone the pinned OBS version into a temporary working directory and apply
the relevant patches before building the runtime.

The current OBS version is pinned to 32.1.2.

## Patch naming

Patch filenames are prefixed according to the platforms that use them:

- `common-*.patch`
  Applied on both Windows and Linux.

- `windows-*.patch`
  Applied only by the Windows OBS runtime build.

- `linux-*.patch`
  Applied only by the Linux OBS runtime build.

The build scripts use `git apply --check` before applying each patch and
fail immediately if a patch no longer matches the pinned OBS source.

## Current patches

### common-nvenc-lightweight-reconfig.patch

Modifies `plugins/obs-nvenc/nvenc.c`.

native-stream-engine uses two private OBS settings:

- `__nse_lightweight`
  Keeps `resetEncoder=0` for lightweight encoder reconfiguration.

- `__nse_force_idr_only`
  Requests an IDR frame without forcing an encoder reset.

This allows bitrate updates and PLI/keyframe handling to avoid unnecessary
full NVENC session resets.

### common-qsv-skip-noop-reconfig.patch

Modifies the OBS QSV encoder implementation.

The stock update path can call `MFXVideoENCODE_Reset()` during encoder
reconfiguration. The patch avoids the reset when the effective bitrate has
not changed.

A genuine QSV keyframe/PLI request can still use the heavier reset path.

### linux-pipewire-expose-restore-token.patch

Modifies the OBS Linux PipeWire screencast portal integration.

When the desktop portal returns a PipeWire restore token, the patch exposes
that token through the OBS source private settings using:

`__nse_linux_restore_token`

native-stream-engine uses this value to support Linux portal capture
restoration without maintaining a separate fork of the PipeWire capture
implementation.

## Encoder behavior

### NVIDIA NVENC

native-stream-engine uses the patched lightweight reconfiguration path for
bitrate updates and can request an IDR without resetting the encoder.

### Intel QSV

No-op reconfiguration is skipped when the bitrate has not changed.
Keyframe/PLI handling may still require the heavier QSV reset path.

### AMD AMF

AMD AMF support is provided through `obs-ffmpeg`.

OBS 32.1.2 does not use a separate `obs-amd-encoder` module in this runtime.
No native-stream-engine OBS patch is currently required for AMD AMF.

## Build scripts

Windows applies:

- `common-*.patch`
- `windows-*.patch`

Linux applies:

- `common-*.patch`
- `linux-*.patch`

Platform-specific patches are therefore never applied to the wrong OBS
runtime build.
