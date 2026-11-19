#!/usr/bin/env bash
# Run a command (or Steam's %command%) with the Steam Frame layer from the build
# tree.
#
#   scripts/run-frame.sh ./game
#   scripts/run-frame.sh %command%       # Steam launch options
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${VRSFOV_BUILD_DIR:-$here/build-frame}"
if [[ ! -f "$build/manifests/vulkan/VkLayer_vrsfov.json" ]]; then
  build="$here/build"
fi
if [[ ! -f "$build/manifests/vulkan/VkLayer_vrsfov.json" ]]; then
  echo "build first: cmake -B build-frame -DVRSFOV_PLATFORM=frame" >&2
  exit 1
fi

export VK_LAYER_PATH="$build/manifests/vulkan${VK_LAYER_PATH:+:$VK_LAYER_PATH}"
export VK_INSTANCE_LAYERS="VK_LAYER_VRSFOV_vrs${VK_INSTANCE_LAYERS:+:$VK_INSTANCE_LAYERS}"
export VRSFOV_ENABLE=1
export VRSFOV_OPENVR_GAZE=1

# The layer writes the want file only for OpenVR games (vrclient.so mapped, no
# XrInstance); the publisher stays idle otherwise.
if [[ "${VRSFOV_START_GAZE:-1}" != "0" && -x "$build/vrsfov-gaze" ]] &&
   ! pgrep -x vrsfov-gaze >/dev/null 2>&1; then
  state="${XDG_STATE_HOME:-$HOME/.local/state}/vrsfov"
  mkdir -p "$state"
  nohup "$build/vrsfov-gaze" -q >>"$state/gaze.log" 2>&1 &
fi

if [[ $# -eq 0 ]]; then
  echo "usage: $0 <command...>" >&2
  exit 2
fi
exec "$@"
