#!/usr/bin/env bash
# Install vrsfov as user (default) or system layers.
#
#   scripts/install-pc.sh [--system] [--prefix DIR] [--build-dir DIR]
#
# User install lays out:
#   <prefix>/lib/vrsfov/libvrsfov.so
#   <prefix>/share/openxr/1/api_layers/implicit.d/XrApiLayer_vrsfov.json
#   <prefix>/share/vulkan/implicit_layer.d/VkLayer_vrsfov.json
#   ~/.config/vrsfov/settings.ini            (only if absent)
#
# System install is needed for Steam's pressure-vessel container, which imports
# host Vulkan layers but does not see ~/.local OpenXR layers.
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "$script_dir/.." && pwd)"

prefix="${VRSFOV_PREFIX:-$HOME/.local}"
if [ -n "${VRSFOV_BUILD_DIR:-}" ]; then
  build_dir="$VRSFOV_BUILD_DIR"
elif [ -d "$repo_dir/build-pc" ]; then
  build_dir="$repo_dir/build-pc"
else
  build_dir="$repo_dir/build"
fi
system=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --system) prefix="/usr"; system=1; shift ;;
    --prefix) prefix="$2"; shift 2 ;;
    --prefix=*) prefix="${1#*=}"; shift ;;
    --build-dir) build_dir="$2"; shift 2 ;;
    --build-dir=*) build_dir="${1#*=}"; shift ;;
    -h|--help) sed -n '2,14p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

lib_src="$build_dir/libvrsfov.so"
if [[ ! -f "$lib_src" ]]; then
  echo "error: $lib_src not found; build first (cmake --build build)" >&2
  exit 1
fi

lib_dir="$prefix/lib/vrsfov"
xr_dir="$prefix/share/openxr/1/api_layers/implicit.d"
vk_dir="$prefix/share/vulkan/implicit_layer.d"

install -d "$lib_dir" "$xr_dir" "$vk_dir"
install -m 0755 "$lib_src" "$lib_dir/libvrsfov.so"

lib_path="$lib_dir/libvrsfov.so"

# Both manifests MUST reference the identical absolute path: the two halves share
# globals through the single dlopen'ed object.
sed "s|@VRSFOV_LIBRARY_PATH@|$lib_path|g" \
  "$repo_dir/manifests/XrApiLayer_vrsfov.json.in" > "$xr_dir/XrApiLayer_vrsfov.json"
sed "s|@VRSFOV_LIBRARY_PATH@|$lib_path|g" \
  "$repo_dir/manifests/VkLayer_vrsfov.json.in" > "$vk_dir/VkLayer_vrsfov.json"

config_dir="${XDG_CONFIG_HOME:-$HOME/.config}/vrsfov"
if [[ ! -f "$config_dir/settings.ini" ]]; then
  install -d "$config_dir"
  cat > "$config_dir/settings.ini" <<'INI'
; vrsfov settings — see README.md for the full reference.
[global]
enabled = 1
eye_tracking = 1
; Valve FDM 'lo' geometry: a .295 x .286 sharp centre and one coarse band.
preset = lo
inner_radius = 0.286
outer_radius = 0.95
inner_rate = 1x1
middle_rate = 2x2
outer_rate = 2x2
INI
  echo "wrote default config: $config_dir/settings.ini"
fi

echo "installed libvrsfov.so -> $lib_path"
echo "installed $xr_dir/XrApiLayer_vrsfov.json"
echo "installed $vk_dir/VkLayer_vrsfov.json"
echo
echo "enable per application with:  VRSFOV_ENABLE=1 <app>"
echo "or as a Steam launch option:  VRSFOV_ENABLE=1 %command%"
if [[ $system -eq 0 ]]; then
  echo
  echo "note: for Proton/pressure-vessel games use --system (Vulkan layers are"
  echo "      imported from the host, OpenXR layers under \$HOME only)."
fi
