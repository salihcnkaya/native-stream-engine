# OBS libobs patches

This folder holds small source patches kept to fix a specific behavior in
certain OBS Studio encoder plugins (nvenc, qsv) that native-stream-engine
relies on. OBS Studio source is never vendored into this repo — the
patches are DIFFs applied to the relevant files at build time, against a
pinned OBS version, in a temporary folder, by `scripts/build-patched-obs-plugins.ps1`.

## Why these patches exist

On the WebRTC/RTP side, native-stream-engine calls `obs_encoder_update()`
whenever a PLI (Picture Loss Indication) arrives and whenever the adaptive
bitrate changes. What that actually does inside libobs depends entirely on
the encoder plugin:

- **obs-nvenc** (`plugins/obs-nvenc/nvenc.c`, `nvenc_update()`):
  `NV_ENC_RECONFIGURE_PARAMS.resetEncoder` and `.forceIDR` are always set
  to `1` TOGETHER. So every update fully resets the NVENC session
  (rate-control state, motion estimation history included). This causes a
  visible encode-pipeline hitch (freeze).

- **obs-qsv11** (`QSV_Encoder_Internal.cpp`, `ReconfigureEncoder()`):
  Same class of issue — `MFXVideoENCODE_Reset()` does a full reset on
  every update.

- **obs-ffmpeg/texture-amf.cpp** (AMD): Already does the right thing —
  no reset in CBR mode, the next frame is simply marked IDR via the
  `force_idr` flag. No patch needed for AMD.

## Patches

- `nvenc-lightweight-reconfig.patch` — STATUS: ready, integrated into
  obs_engine.cpp. Adds two new obs_data keys to `nvenc_update()`:
  - `__nse_lightweight` (bool): when true, `resetEncoder=0` is set.
  - `__nse_force_idr_only` (bool): when true, `forceIDR=1` is set
    (without a reset). native-stream-engine sends this as true for
    PLI/keyframe requests, and false for plain bitrate updates.

  IMPORTANT: I could not 100% confirm from NVIDIA documentation whether
  the `resetEncoder=0` + `forceIDR=1` combination is officially supported
  by the NVENC SDK — this should be verified on real hardware, checking
  encode callback logs (whether there's a frame gap, whether an IDR
  actually arrives). If it misbehaves, a fallback is to use
  `resetEncoder=0` only for bitrate-only updates (`force_idr_only=false`)
  and fall back to the old `resetEncoder=1` behavior for PLI-driven
  keyframe requests (with a much longer cooldown).

- `qsv-skip-noop-reconfig.patch` — STATUS: ready (partial), integrated
  into obs_engine.cpp. Stock `obs_qsv_update()` called
  `MFXVideoENCODE_Reset()` unconditionally on every call, with no
  condition check at all (worse than NVENC, which at least checked
  `can_change_bitrate`). This patch skips the reset when the bitrate
  hasn't actually changed.

  REMAINING GAP: On a genuine PLI keyframe request, QSV still falls
  through to `MFXVideoENCODE_Reset()` — QSV doesn't yet have an
  NVENC/AMF-level "just mark the next frame IDR without a reset"
  mechanism. Doing this properly requires passing Intel Media SDK/VPL's
  `mfxEncodeCtrl` struct (`FrameType = MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR
| MFX_FRAMETYPE_REF`) as a per-frame parameter to the
  `EncodeFrameAsync()` call inside `QSV_Encoder_Internal::Encode()` /
  `Encode_tex()`. I couldn't safely write that diff without seeing the
  full current contents of that file (I only saw fragments of it) — feel
  free to paste it in, or I can fetch it in full again in a future
  session to finish this.

## Changes on the native-stream-engine side

`src/obs_engine.cpp`:

- `requestRtpVideoKeyframe()` now works for NVENC + AMF + QSV (texture
  variants); it sets the correct keys for each family.
- `updateRtpVideoEncoderBitrate()` puts NVENC into lightweight mode
  (`__nse_lightweight=true`, `__nse_force_idr_only=false` — we
  deliberately don't force an IDR on a plain bitrate change). QSV
  already benefits automatically from the patch's own no-op-skip logic,
  no extra code was needed.
- This code still works unmodified against a stock, unpatched OBS
  runtime (unknown `__nse_*` keys silently resolve to `false`) — you
  just lose the patch's actual benefit in that case.
