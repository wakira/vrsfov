#!/usr/bin/env bash
# Build and install VRSFov user-wide so games do not need launch options.
#
#   ./install.sh                  # Build & install PC target (default)
#   ./install.sh --pc             # Build & install PC target (Vulkan + OpenXR layers + vrsfov-gaze service)
#   ./install.sh --frame          # Build & install Steam Frame target (Vulkan layer + vrsfov-gaze service)
#   ./install.sh --uninstall      # Remove installed layers, binaries, and stop the gaze service
#
# Installed locations:
#   ~/.local/share/vrsfov/libvrsfov.so
#   ~/.local/share/vrsfov/vrsfov-gaze
#   ~/.local/share/vrsfov/vrsfov-overlay  (PC target, when OpenVR lib is present)
#   ~/.local/bin/vrsfov-gaze              (symlink)
#   ~/.local/bin/vrsfov-overlay           (symlink, when installed)
#   ~/.local/share/vulkan/implicit_layer.d/VkLayer_vrsfov.json
#   ~/.local/share/openxr/1/api_layers/implicit.d/XrApiLayer_vrsfov.json  (PC target)
#   ~/.config/systemd/user/vrsfov-gaze.service
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
bin_dir="$HOME/.local/bin"

install_dir="$data_home/vrsfov"
vk_layer_dir="$data_home/vulkan/implicit_layer.d"
xr_layer_dir="$data_home/openxr/1/api_layers/implicit.d"
systemd_user_dir="$config_home/systemd/user"

vk_manifest="$vk_layer_dir/VkLayer_vrsfov.json"
xr_manifest="$xr_layer_dir/XrApiLayer_vrsfov.json"
service_file="$systemd_user_dir/vrsfov-gaze.service"

platform="pc"
action="install"

usage() {
  cat <<EOF
Usage: $0 [--pc | --frame | --uninstall | --help]

Options:
  --pc          Build and install the PC target (Vulkan + OpenXR implicit layers, vrsfov-gaze user service) [default]
  --frame       Build and install the Steam Frame target (Vulkan implicit layer, vrsfov-gaze user service)
  --uninstall   Stop the gaze service and remove installed layers and binaries
  -h, --help    Show this help message
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --pc|pc)
      platform="pc"
      action="install"
      shift
      ;;
    --frame|frame)
      platform="frame"
      action="install"
      shift
      ;;
    --uninstall|uninstall)
      action="uninstall"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

uninstall() {
  echo "==> Uninstalling VRSFov..."
  if command -v systemctl >/dev/null 2>&1; then
    systemctl --user disable --now vrsfov-gaze.service >/dev/null 2>&1 || true
  fi
  rm -f "$service_file"
  if command -v systemctl >/dev/null 2>&1; then
    systemctl --user daemon-reload >/dev/null 2>&1 || true
  fi
  pkill -x vrsfov-gaze >/dev/null 2>&1 || true
  rm -f /dev/shm/vrsfov-gaze /dev/shm/vrsfov-gaze.want 2>/dev/null || true

  rm -f "$vk_manifest" "$xr_manifest"
  rm -f "$bin_dir/vrsfov-gaze" "$bin_dir/vrsfov-overlay"
  rm -rf "$install_dir"

  echo "==> Uninstalled VRSFov."
}

if [[ "$action" == "uninstall" ]]; then
  uninstall
  exit 0
fi

build_dir="${VRSFOV_BUILD_DIR:-$here/build-$platform}"

build_project() {
  local cmake_args=(
    -B "$build_dir"
    -S "$here"
    "-DVRSFOV_PLATFORM=$platform"
  )
  if command -v ninja >/dev/null 2>&1 && [[ ! -f "$build_dir/Makefile" ]]; then
    cmake_args+=(-GNinja)
  fi
  if [[ -n "${VRSFOV_USE_SYSTEM_HEADERS:-}" ]]; then
    cmake_args+=("-DVRSFOV_USE_SYSTEM_HEADERS=${VRSFOV_USE_SYSTEM_HEADERS}")
  fi

  echo "==> Building VRSFov ($platform) in $build_dir..."
  if command -v cmake >/dev/null 2>&1; then
    if [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
      cmake "${cmake_args[@]}"
    fi
    cmake --build "$build_dir"
  elif command -v nix >/dev/null 2>&1 && [[ -f "$here/flake.nix" ]]; then
    nix develop "$here" --command bash -c "
      if [[ ! -f '$build_dir/CMakeCache.txt' ]]; then
        cmake -B '$build_dir' -S '$here' -GNinja -DVRSFOV_PLATFORM='$platform' -DVRSFOV_USE_SYSTEM_HEADERS=ON
      fi
      cmake --build '$build_dir'
    "
  else
    echo "Error: cmake not found in PATH (and nix is unavailable)." >&2
    exit 1
  fi
}

build_project

if [[ ! -f "$build_dir/libvrsfov.so" ]]; then
  echo "Error: $build_dir/libvrsfov.so was not produced." >&2
  exit 1
fi

echo "==> Installing binaries to $install_dir..."
mkdir -p "$install_dir" "$bin_dir" "$vk_layer_dir"

# Copy via temporary file + mv so running processes with libvrsfov.so mapped are not SIGBUS'd.
install -m 755 "$build_dir/libvrsfov.so" "$install_dir/libvrsfov.so.new"
mv -f "$install_dir/libvrsfov.so.new" "$install_dir/libvrsfov.so"

if [[ -x "$build_dir/vrsfov-gaze" ]]; then
  install -m 755 "$build_dir/vrsfov-gaze" "$install_dir/vrsfov-gaze.new"
  mv -f "$install_dir/vrsfov-gaze.new" "$install_dir/vrsfov-gaze"
  ln -sfn "$install_dir/vrsfov-gaze" "$bin_dir/vrsfov-gaze"
fi

if [[ "$platform" == "pc" && -x "$build_dir/vrsfov-overlay" ]]; then
  install -m 755 "$build_dir/vrsfov-overlay" "$install_dir/vrsfov-overlay.new"
  mv -f "$install_dir/vrsfov-overlay.new" "$install_dir/vrsfov-overlay"
  ln -sfn "$install_dir/vrsfov-overlay" "$bin_dir/vrsfov-overlay"
else
  rm -f "$install_dir/vrsfov-overlay" "$bin_dir/vrsfov-overlay"
fi

echo "==> Installing implicit Vulkan layer manifest to $vk_manifest..."
cat > "$vk_manifest" <<EOF
{
  "file_format_version": "1.2.0",
  "layer": {
    "name": "VK_LAYER_VRSFOV_vrs",
    "type": "GLOBAL",
    "library_path": "$install_dir/libvrsfov.so",
    "api_version": "1.3.280",
    "implementation_version": "1",
    "description": "VRS injection for VRSFov",
    "enable_environment": { "VRSFOV_ENABLE": "1" },
    "disable_environment": { "VRSFOV_DISABLE": "1" }
  }
}
EOF

if [[ "$platform" == "pc" ]]; then
  echo "==> Installing implicit OpenXR API layer manifest to $xr_manifest..."
  mkdir -p "$xr_layer_dir"
  cat > "$xr_manifest" <<EOF
{
  "file_format_version": "1.0.0",
  "api_layer": {
    "name": "XR_APILAYER_VRSFOV_foveated_rendering",
    "library_path": "$install_dir/libvrsfov.so",
    "api_version": "1.0",
    "implementation_version": "1",
    "description": "VRS-based foveated rendering for Vulkan apps",
    "disable_environment": "VRSFOV_DISABLE",
    "enable_environment": "VRSFOV_ENABLE"
  }
}
EOF
else
  # Steam Frame target has no OpenXR layer half; ensure no stale PC OpenXR manifest remains.
  rm -f "$xr_manifest"
fi

if [[ -x "$install_dir/vrsfov-gaze" ]]; then
  echo "==> Installing systemd user service to $service_file..."
  mkdir -p "$systemd_user_dir"
  cat > "$service_file" <<EOF
[Unit]
Description=VRSFov SteamVR Eye-Tracked Gaze Publisher
Documentation=https://github.com/autrim64/Linux-Foveated-Rendering
After=graphical-session.target

[Service]
Type=simple
ExecStart=$install_dir/vrsfov-gaze -q
Restart=on-failure
RestartSec=2

[Install]
WantedBy=default.target
EOF

  if command -v systemctl >/dev/null 2>&1 && systemctl --user show-environment >/dev/null 2>&1 &&
     [[ "$systemd_user_dir" == "$HOME/.config/systemd/user" ]]; then
    systemctl --user daemon-reload
    systemctl --user enable vrsfov-gaze.service
    systemctl --user restart vrsfov-gaze.service
    echo "==> vrsfov-gaze.service enabled and running (idle until an OpenVR game renders)."
  else
    echo "==> Created $service_file (systemd user session not active; starting vrsfov-gaze in background)."
    if ! pgrep -x vrsfov-gaze >/dev/null 2>&1; then
      state="${XDG_STATE_HOME:-$HOME/.local/state}/vrsfov"
      mkdir -p "$state"
      nohup "$install_dir/vrsfov-gaze" -q >>"$state/gaze.log" 2>&1 &
    fi
  fi
fi

echo ""
echo "==> VRSFov ($platform) installed!"
echo "    Library:        $install_dir/libvrsfov.so"
echo "    Vulkan layer:   $vk_manifest"
if [[ "$platform" == "pc" ]]; then
  echo "    OpenXR layer:   $xr_manifest"
fi
if [[ -x "$install_dir/vrsfov-gaze" ]]; then
  echo "    Gaze publisher: $install_dir/vrsfov-gaze (systemd user service: vrsfov-gaze.service)"
fi
echo "    Config file:    $config_home/vrsfov/settings.ini"
echo "    Log file:       ${XDG_STATE_HOME:-$HOME/.local/state}/vrsfov/vrsfov.log"
echo "    To enable for a game: VRSFOV_ENABLE=1 %command%"
echo "    To uninstall:   $0 --uninstall"
