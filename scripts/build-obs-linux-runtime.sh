#!/usr/bin/env bash
set -euo pipefail

OBS_TAG="${OBS_TAG:-32.1.2}"
PIPEWIRE_AUDIO_TAG="${PIPEWIRE_AUDIO_TAG:-1.2.1}"
EXPECTED_OBS_COMMIT="fb4d98bf88fae5fc85cb11fc57f7c5e309282194"

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

PATCH_DIR="${REPO_ROOT}/patches/obs"
WORK_DIR="${REPO_ROOT}/.obs-build-tmp-linux"
OBS_SRC="${WORK_DIR}/obs-studio"
OBS_BUILD="${OBS_SRC}/build"
PIPEWIRE_AUDIO_SRC="${WORK_DIR}/obs-pipewire-audio-capture"
PIPEWIRE_AUDIO_BUILD="${PIPEWIRE_AUDIO_SRC}/build"
RUNTIME_DIR="${REPO_ROOT}/runtime/OBS-Studio-${OBS_TAG}-Linux-x86_64"

TARGETS=(
  libobs
  libobs-opengl
  obs-nvenc
  obs-nvenc-test
  obs-qsv11
  obs-ffmpeg
  obs-x264
  linux-pipewire
  linux-pulseaudio
)

log() {
  printf '\n==> %s\n' "$*"
}

die() {
  printf '\nERROR: %s\n' "$*" >&2
  exit 1
}

command -v git >/dev/null 2>&1 || die "git is required"
command -v cmake >/dev/null 2>&1 || die "cmake is required"
command -v ninja >/dev/null 2>&1 || die "ninja is required"

[[ -d "${PATCH_DIR}" ]] || die "Patch directory not found: ${PATCH_DIR}"

log "Cleaning temporary OBS build directory"
rm -rf "${WORK_DIR}"
mkdir -p "${WORK_DIR}"

log "Cloning OBS Studio ${OBS_TAG} with exact submodules"
git clone \
  --branch "${OBS_TAG}" \
  --depth 1 \
  --recurse-submodules \
  --shallow-submodules \
  https://github.com/obsproject/obs-studio.git \
  "${OBS_SRC}"

cd "${OBS_SRC}"

actual_obs_commit="$(
  git rev-parse HEAD
)"

if [[ "${actual_obs_commit}" != "${EXPECTED_OBS_COMMIT}" ]]; then
  die \
    "Unexpected OBS commit. Expected ${EXPECTED_OBS_COMMIT}, got ${actual_obs_commit}"
fi

log "Verified OBS commit ${actual_obs_commit}"

log "Applying native-stream-engine OBS patches"
shopt -s nullglob

patches=(
  "${PATCH_DIR}"/common-*.patch
  "${PATCH_DIR}"/linux-*.patch
)

(( ${#patches[@]} > 0 )) || die \
  "No common-*.patch or linux-*.patch files found under ${PATCH_DIR}"

for patch in "${patches[@]}"; do
  printf '  -> %s\n' "$(basename "${patch}")"
  git apply --check --whitespace=nowarn "${patch}"
  git apply --whitespace=nowarn "${patch}"
done

shopt -u nullglob

log "Restricting OBS plugin graph to native-stream-engine requirements"

cat > "${OBS_SRC}/plugins/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.28...3.30)

option(ENABLE_PLUGINS "Enable building OBS plugins" ON)

if(NOT ENABLE_PLUGINS)
  set_property(GLOBAL APPEND PROPERTY OBS_FEATURES_DISABLED "Plugin Support")
  return()
endif()

set_property(GLOBAL APPEND PROPERTY OBS_FEATURES_ENABLED "Plugin Support")

add_obs_plugin(linux-pipewire PLATFORMS LINUX FREEBSD OPENBSD)

add_obs_plugin(linux-pulseaudio PLATFORMS LINUX FREEBSD OPENBSD)

add_obs_plugin(obs-ffmpeg)

add_obs_plugin(
  obs-nvenc
  PLATFORMS WINDOWS LINUX
  ARCHITECTURES x64 x86_64
)

add_obs_plugin(
  obs-qsv11
  PLATFORMS WINDOWS LINUX
  ARCHITECTURES x64 x86_64
)

add_obs_plugin(obs-x264)
EOF

log "Configuring minimal OBS build graph"
cmake \
  -S "${OBS_SRC}" \
  -B "${OBS_BUILD}" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_FRONTEND=OFF \
  -DENABLE_SCRIPTING=OFF \
  -DENABLE_NEW_MPEGTS_OUTPUT=OFF \
  -DENABLE_NVENC=ON \
  -DENABLE_QSV11=ON \
  -DENABLE_FFMPEG_NVENC=ON

log "Building only native-stream-engine OBS targets"
for target in "${TARGETS[@]}"; do
  printf '  -> %s\n' "${target}"
  cmake --build "${OBS_BUILD}" --target "${target}" --parallel
done

log "Cloning obs-pipewire-audio-capture ${PIPEWIRE_AUDIO_TAG}"

git clone \
  --branch "${PIPEWIRE_AUDIO_TAG}" \
  --depth 1 \
  https://github.com/dimtpap/obs-pipewire-audio-capture.git \
  "${PIPEWIRE_AUDIO_SRC}"

log "Configuring obs-pipewire-audio-capture"

cmake \
  -S "${PIPEWIRE_AUDIO_SRC}" \
  -B "${PIPEWIRE_AUDIO_BUILD}" \
  -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -Dlibobs_DIR="${OBS_BUILD}/libobs" \
  -DCMAKE_MODULE_PATH="${PIPEWIRE_AUDIO_SRC}/cmake;${OBS_SRC}/cmake/finders"

log "Building obs-pipewire-audio-capture"

cmake --build \
  "${PIPEWIRE_AUDIO_BUILD}" \
  --parallel

log "Recreating Linux OBS runtime"
rm -rf "${RUNTIME_DIR}"
mkdir -p \
  "${RUNTIME_DIR}/lib" \
  "${RUNTIME_DIR}/obs-plugins" \
  "${RUNTIME_DIR}/bin" \
  "${RUNTIME_DIR}/data"

log "Adding obs-pipewire-audio-capture to runtime"

cp -f \
  "${PIPEWIRE_AUDIO_BUILD}/linux-pipewire-audio.so" \
  "${RUNTIME_DIR}/obs-plugins/linux-pipewire-audio.so"

copy_shared_objects() {
  local destination="$1"
  local pattern="$2"

  local found=0
  while IFS= read -r -d '' file; do
    cp -a "${file}" "${destination}/"
    printf '  -> %s\n' "${file#${OBS_BUILD}/}"
    found=1
  done < <(
    find "${OBS_BUILD}" \
      \( -type f -o -type l \) \
      -name "${pattern}" \
      -print0
  )

  (( found == 1 ))
}

log "Collecting libobs"
copy_shared_objects "${RUNTIME_DIR}/lib" 'libobs.so*' \
  || die "libobs.so was not found"

log "Collecting OpenGL graphics module"
copy_shared_objects "${RUNTIME_DIR}/lib" 'libobs-opengl.so*' \
  || die "libobs-opengl.so was not found"

log "Collecting selected OBS plugins"
for plugin in obs-nvenc obs-qsv11 obs-ffmpeg obs-x264 linux-pipewire linux-pulseaudio; do
  found="$(
    find "${OBS_BUILD}" \
      -type f \
      -name "${plugin}.so" \
      -print -quit
  )"

  [[ -n "${found}" ]] || die "Plugin not found after build: ${plugin}.so"

  cp -a "${found}" "${RUNTIME_DIR}/obs-plugins/"
  printf '  -> %s\n' "${found#${OBS_BUILD}/}"
done

log "Collecting obs-nvenc-test helper"
nvenc_test="$(
  find "${OBS_BUILD}" \
    -type f \
    -name 'obs-nvenc-test' \
    -perm -u+x \
    -print -quit
)"

[[ -n "${nvenc_test}" ]] || die "obs-nvenc-test was not found"
cp -a "${nvenc_test}" "${RUNTIME_DIR}/bin/"
printf '  -> %s\n' "${nvenc_test#${OBS_BUILD}/}"

log "Collecting libobs data files"
if [[ -d "${OBS_SRC}/libobs/data" ]]; then
  mkdir -p "${RUNTIME_DIR}/data/libobs"
  cp -a "${OBS_SRC}/libobs/data/." "${RUNTIME_DIR}/data/libobs/"
fi

log "Collecting selected OBS plugin data"

PLUGIN_DATA_SOURCE="${OBS_BUILD}/rundir/Release/share/obs/obs-plugins"
PLUGIN_DATA_DEST="${RUNTIME_DIR}/data/obs-plugins"

mkdir -p "${PLUGIN_DATA_DEST}"

if [[ -d "${PIPEWIRE_AUDIO_SRC}/data" ]]; then
  mkdir -p "${PLUGIN_DATA_DEST}/linux-pipewire-audio"
  cp -a \
    "${PIPEWIRE_AUDIO_SRC}/data/." \
    "${PLUGIN_DATA_DEST}/linux-pipewire-audio/"

  printf '  -> data/obs-plugins/linux-pipewire-audio\n'
fi

for plugin in linux-pipewire linux-pulseaudio obs-ffmpeg obs-nvenc obs-qsv11 obs-x264; do
  if [[ -d "${PLUGIN_DATA_SOURCE}/${plugin}" ]]; then
    mkdir -p "${PLUGIN_DATA_DEST}/${plugin}"
    cp -a \
      "${PLUGIN_DATA_SOURCE}/${plugin}/." \
      "${PLUGIN_DATA_DEST}/${plugin}/"

    printf '  -> data/obs-plugins/%s\n' "${plugin}"
  fi
done

log "Collecting libobs public headers"

mkdir -p "${RUNTIME_DIR}/include"

while IFS= read -r -d '' header; do
  relative="${header#${OBS_SRC}/libobs/}"
  destination="${RUNTIME_DIR}/include/${relative}"

  mkdir -p "$(dirname "${destination}")"
  cp -a "${header}" "${destination}"
done < <(
  find "${OBS_SRC}/libobs" \
    -type f \
    \( -name '*.h' -o -name '*.hpp' \) \
    -print0
)

log "Collecting generated obsconfig.h"
obsconfig="$(
  find "${OBS_BUILD}" \
    -type f \
    -name 'obsconfig.h' \
    -print -quit
)"

[[ -n "${obsconfig}" ]] || die "Generated obsconfig.h was not found"
mkdir -p "${RUNTIME_DIR}/include"
cp -a "${obsconfig}" "${RUNTIME_DIR}/include/obsconfig.h"
printf '  -> %s\n' "${obsconfig#${OBS_BUILD}/}"

log "Fixing runtime RUNPATHs"

command -v patchelf >/dev/null 2>&1 \
  || die "patchelf is required to make the Linux runtime relocatable"

for so in "${RUNTIME_DIR}/obs-plugins/"*.so; do
  [[ -f "${so}" ]] || continue

  patchelf \
    --set-rpath '$ORIGIN/../lib' \
    "${so}"

  printf '  -> %s\n' "$(basename "${so}")"
done

for so in "${RUNTIME_DIR}/lib/libobs-opengl.so" \
          "${RUNTIME_DIR}/lib/libobs-opengl.so.30"; do
  [[ -e "${so}" ]] || continue

  patchelf \
    --set-rpath '$ORIGIN' \
    "${so}"
done

if [[ -f "${RUNTIME_DIR}/bin/obs-nvenc-test" ]]; then
  patchelf \
    --set-rpath '$ORIGIN/../lib' \
    "${RUNTIME_DIR}/bin/obs-nvenc-test"
fi

log "Runtime created"
printf '%s\n' "${RUNTIME_DIR}"

printf '\nSelected runtime contents:\n'
find "${RUNTIME_DIR}" -maxdepth 3 -type f -o -type l | sort
