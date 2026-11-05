#!/usr/bin/env bash
# Development helper: rebuild-free sync of the freshly built library into the
# user install, so the loader (which reads the manifest from the passwd home
# directory, not $HOME) picks up the current build.
#
#   scripts/dev-install.sh [--build-dir DIR] [--prefix DIR]
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "$script_dir/.." && pwd)"
if [ -n "${VRSFOV_BUILD_DIR:-}" ]; then
  build_dir="$VRSFOV_BUILD_DIR"
elif [ -d "$repo_dir/build-pc" ]; then
  build_dir="$repo_dir/build-pc"
else
  build_dir="$repo_dir/build"
fi
prefix="${VRSFOV_PREFIX:-$HOME/.local}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir) build_dir="$2"; shift 2 ;;
    --build-dir=*) build_dir="${1#*=}"; shift ;;
    --prefix) prefix="$2"; shift 2 ;;
    --prefix=*) prefix="${1#*=}"; shift ;;
    -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [[ ! -f "$build_dir/libvrsfov.so" ]]; then
  echo "error: $build_dir/libvrsfov.so not found; build first" >&2
  exit 1
fi

if [[ ! -f "$prefix/share/vulkan/implicit_layer.d/VkLayer_vrsfov.json" ]]; then
  echo "manifest missing; running scripts/install-pc.sh first"
  "$script_dir/install-pc.sh" --prefix "$prefix"
fi

install -D -m 0755 "$build_dir/libvrsfov.so" "$prefix/lib/vrsfov/libvrsfov.so"
echo "synced $build_dir/libvrsfov.so -> $prefix/lib/vrsfov/libvrsfov.so"
