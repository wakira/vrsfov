#!/usr/bin/env bash
# Build and install the PC target (Vulkan + OpenXR implicit layers, vrsfov-gaze user service).
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
exec "$here/install.sh" --pc "$@"
