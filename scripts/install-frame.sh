#!/usr/bin/env bash
# Build and install the Steam Frame target (Vulkan implicit layer, vrsfov-gaze user service).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "$here/install.sh" --frame "$@"
