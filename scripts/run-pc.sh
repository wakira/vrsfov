#!/usr/bin/env bash
# Run a command (or Steam's %command%) with the PC layer from the build tree.
#
#   scripts/run-pc.sh ./my_app
#   scripts/run-pc.sh %command%          # Steam launch options
#
# No install step and no writes outside the build/config/log directories. The
# loader manifests in build-pc/manifests are pointed at with their standard
# environment variables, so Steam and pressure-vessel pass them through.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build="${VRSFOV_BUILD_DIR:-$here/build-pc}"
if [[ ! -f "$build/manifests/vulkan/VkLayer_vrsfov.json" ]]; then
  build="$here/build"
fi
if [[ ! -f "$build/manifests/vulkan/VkLayer_vrsfov.json" ]]; then
  echo "build first: cmake -B build-pc -DVRSFOV_PLATFORM=pc" >&2
  exit 1
fi

export VK_LAYER_PATH="$build/manifests/vulkan${VK_LAYER_PATH:+:$VK_LAYER_PATH}"
export VK_INSTANCE_LAYERS="VK_LAYER_VRSFOV_vrs${VK_INSTANCE_LAYERS:+:$VK_INSTANCE_LAYERS}"
export XR_API_LAYER_PATH="$build/manifests/openxr${XR_API_LAYER_PATH:+:$XR_API_LAYER_PATH}"
export XR_ENABLE_API_LAYERS="XR_APILAYER_VRSFOV_foveated_rendering"
export PRESSURE_VESSEL_IMPORT_OPENXR_1_LAYERS=1
export VRSFOV_ENABLE=1

# The publisher is only contacted when the native OpenXR action goes inactive.
# It stays idle otherwise (on-demand want file).
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
