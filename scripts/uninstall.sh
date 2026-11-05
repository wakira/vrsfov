#!/usr/bin/env bash
# Remove a user (default) or system install created by scripts/install-pc.sh.
#
#   scripts/uninstall.sh [--system] [--prefix DIR] [--keep-config]
set -euo pipefail

prefix="${VRSFOV_PREFIX:-$HOME/.local}"
keep_config=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --system) prefix="/usr"; shift ;;
    --prefix) prefix="$2"; shift 2 ;;
    --prefix=*) prefix="${1#*=}"; shift ;;
    --keep-config) keep_config=1; shift ;;
    -h|--help) sed -n '2,5p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

rm -f "$prefix/share/openxr/1/api_layers/implicit.d/XrApiLayer_vrsfov.json"
rm -f "$prefix/share/vulkan/implicit_layer.d/VkLayer_vrsfov.json"
rm -rf "$prefix/lib/vrsfov"
rmdir "$prefix/share/vrsfov" 2>/dev/null || true

# Frame publisher (present only for an install-frame.sh install).
unit_dir="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
rm -f "$unit_dir/vrsfov-gaze.service"
rm -rf "$unit_dir/vrsfov-gaze.service.d"
rm -f /dev/shm/vrsfov-gaze /dev/shm/vrsfov-gaze.want
if command -v systemctl >/dev/null 2>&1; then
  systemctl --user daemon-reload 2>/dev/null || true
fi

if [[ $keep_config -eq 0 ]]; then
  config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/vrsfov"
  rm -f "$config_dir/settings.ini"
  rmdir "$config_dir" 2>/dev/null || true
fi

echo "uninstalled vrsfov from $prefix"
