#!/usr/bin/env bash
# Runs vrsfov-overlay with the library paths SteamVR's client needs.
#
# On NixOS the store holds both word sizes of libglvnd and libuuid, and SteamVR's
# vrclient.so wants the 64-bit ones; a wrong pick or a missing one shows up as
# "VR_Init failed: vrclient Shared Lib Not Found (102)". The paths are resolved by
# ELF class here, so no store hashes need to be pasted and nothing breaks when the
# store is updated.
#
# Usage: scripts/overlay-run.sh [vrsfov-overlay arguments]
#   scripts/overlay-run.sh --once
#   scripts/overlay-run.sh --inner 0.18 --outer 0.45
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [ -x "$here/build-pc/vrsfov-overlay" ]; then
  bin="$here/build-pc/vrsfov-overlay"
else
  bin="$here/build/vrsfov-overlay"
fi
if [[ ! -x "$bin" ]]; then
  echo "build first: cmake --build build" >&2
  exit 1
fi

# ELF class lives at offset 4: 2 = 64-bit.
elf64() {
  local class
  class="$(od -An -j4 -N1 -tu1 "$1" 2>/dev/null | tr -d ' ')"
  [[ "$class" == "2" ]]
}

pick_dir() {  # $@ = candidate libraries, newest store path first
  local f
  for f in "$@"; do
    [[ -f "$f" ]] && elf64 "$f" && { dirname "$f"; return 0; }
  done
  return 1
}

# shellcheck disable=SC2086
gl="$(pick_dir $(ls -1d /nix/store/*libglvnd*/lib/libGL.so.1 2>/dev/null | tac) || true)"
# shellcheck disable=SC2086
uuid="$(pick_dir $(ls -1d /nix/store/*util-linux*/lib/libuuid.so.1 /nix/store/*libuuid*/lib/libuuid.so.1 2>/dev/null | tac) || true)"

paths=()
[[ -n "$gl" ]] && paths+=("$gl")
[[ -n "$uuid" ]] && paths+=("$uuid")
paths+=("/run/opengl-driver/lib")   # GLX/EGL vendor libraries for vrclient.so

if [[ -z "$gl" || -z "$uuid" ]]; then
  echo "warning: could not resolve 64-bit libglvnd/libuuid in the store;" >&2
  echo "VR_Init will likely fail with 'vrclient Shared Lib Not Found'" >&2
fi

joined="$(IFS=:; echo "${paths[*]}")"
export LD_LIBRARY_PATH="$joined${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$bin" "$@"
