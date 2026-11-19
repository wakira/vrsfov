# vrsfov

VRS-based foveated rendering for Linux VR using `VK_KHR_fragment_shading_rate`.

| Target | Artifacts | Use case | Gaze source |
|---|---|---|---|
| `pc` | `libvrsfov.so` (Vulkan + OpenXR layers), `vrsfov-gaze`, `vrsfov-overlay` | Native OpenXR, Proton OpenXR (`wineopenxr` + DXVK/vkd3d-proton), and OpenVR games | `XR_EXT_eye_gaze_interaction` (OpenXR) or `vrsfov-gaze` over `/dev/shm/vrsfov-gaze` (OpenVR) |
| `frame` | `libvrsfov.so` (Vulkan + OpenXR layers), `vrsfov-gaze` | Steam Frame: Proton OpenXR games (`wineopenxr` + DXVK) and OpenVR games | `XR_EXT_eye_gaze_interaction` (OpenXR) or `vrsfov-gaze` over `/dev/shm/vrsfov-gaze` (OpenVR) |

## How it works

- **Vulkan half**: Enables `VK_KHR_fragment_shading_rate` at device creation, attaches an `R8_UINT` shading-rate image to render passes and dynamic rendering, sets pipeline combiner ops (`KEEP`/`MAX`), and updates the rate map using **transfer commands only** (`vkCmdCopyBufferToImage` from precomputed pattern buffers at a gaze-dependent offset).
- **OpenXR half (both targets)**: Tracks the session's `VkDevice`, per-eye FOV, `xrEndFrame` projection views, and `XR_EXT_eye_gaze_interaction` pose, publishing an immutable snapshot for the Vulkan half. On Wine, `wineopenxr` bridges the game's OpenXR calls to the host loader, so the API layer loads in the game process exactly as it does natively.
- **OpenVR & standalone mode**: When an OpenVR client (`vrclient.so` / `vrclient.dll`) is loaded without OpenXR, the layer automatically enters standalone mode and reads per-eye gaze and recommended render size from the out-of-process `vrsfov-gaze` publisher (`/dev/shm/vrsfov-gaze`). On the `frame` target, `standalone = 1` is the default. No SteamVR client is ever created inside the game process.

Design details are in [`plan/00-overview.md`](plan/00-overview.md) and [`PLAN.md`](PLAN.md).

## Install

### Install script

```sh
./install.sh                  # Build & install PC target (layers + vrsfov-gaze systemd user service)
./install.sh --frame          # Build & install Steam Frame target
./install.sh --uninstall      # Stop service and remove installed layers/binaries
```

This installs:
- `~/.local/share/vrsfov/` (`libvrsfov.so`, `vrsfov-gaze`, `vrsfov-overlay`) with symlinks in `~/.local/bin/`
- `~/.local/share/vulkan/implicit_layer.d/VkLayer_vrsfov.json`
- `~/.local/share/openxr/1/api_layers/implicit.d/XrApiLayer_vrsfov.json` (both targets; `--frame` builds the OpenXR half too, disable with `-DVRSFOV_BUILD_OPENXR=OFF`)
- `~/.config/systemd/user/vrsfov-gaze.service` (stays idle at 0% CPU until an OpenVR game renders)

Enable per game in Steam launch options:

```sh
VRSFOV_ENABLE=1 %command%
```

*(For Proton OpenXR games, if `PRESSURE_VESSEL_IMPORT_OPENXR_1_LAYERS=1` is not in your user environment, use `VRSFOV_ENABLE=1 PRESSURE_VESSEL_IMPORT_OPENXR_1_LAYERS=1 %command%`.)*

### NixOS Home Manager

The flake exposes `packages.<system>.pc` (`default`) and `packages.<system>.frame`:

```nix
{ pkgs, inputs, ... }:
let
  vrsfovPkg = inputs.vrsfov.packages.${pkgs.stdenv.hostPlatform.system}.pc; # or .frame
in
{
  home.packages = [ vrsfovPkg ];

  xdg.dataFile."vulkan/implicit_layer.d/VkLayer_vrsfov.json".source =
    "${vrsfovPkg}/share/vulkan/implicit_layer.d/VkLayer_vrsfov.json";

  xdg.dataFile."openxr/1/api_layers/implicit.d/XrApiLayer_vrsfov.json".source =
    "${vrsfovPkg}/share/openxr/1/api_layers/implicit.d/XrApiLayer_vrsfov.json";

  home.sessionVariables.PRESSURE_VESSEL_IMPORT_OPENXR_1_LAYERS = "1";

  systemd.user.services.vrsfov-gaze = {
    Unit = {
      Description = "VRSFov SteamVR Eye-Tracked Gaze Publisher";
      After = [ "graphical-session.target" ];
    };
    Service = {
      Type = "simple";
      ExecStart = "${vrsfovPkg}/bin/vrsfov-gaze -q";
      Restart = "on-failure";
      RestartSec = 2;
    };
    Install.WantedBy = [ "default.target" ];
  };
}
```

## Build & Run from Source

```sh
nix develop   # optional, on NixOS

# PC target
cmake -B build-pc -GNinja -DVRSFOV_USE_SYSTEM_HEADERS=ON -DVRSFOV_PLATFORM=pc
cmake --build build-pc
ctest --test-dir build-pc --output-on-failure

# Steam Frame target
cmake -B build-frame -GNinja -DVRSFOV_USE_SYSTEM_HEADERS=ON -DVRSFOV_PLATFORM=frame
cmake --build build-frame
ctest --test-dir build-frame --output-on-failure
```

| CMake Option | Default | Description |
|---|---|---|
| `VRSFOV_PLATFORM` | `pc` | `pc` (Vulkan + OpenXR) or `frame` (standalone default + OpenXR for OpenXR games) |
| `VRSFOV_BUILD_OPENXR` | `ON` | Build the OpenXR API layer half (always on for `pc`; `OFF` gives a Vulkan-only `frame` build) |
| `VRSFOV_USE_SYSTEM_HEADERS` | `OFF` | Use system Vulkan/OpenXR/OpenVR headers instead of FetchContent |
| `VRSFOV_BUILD_TESTS` | `ON` | Build unit tests and register `ctest` entries |
| `VRSFOV_BUILD_GPU_TESTS` | `ON` | Build headless GPU end-to-end tests |

To run directly from the build tree without installing to `~/.local`:

```sh
/absolute/path/to/vrsfov/scripts/run-pc.sh %command%
/absolute/path/to/vrsfov/scripts/run-frame.sh %command%
```

Both scripts set the loader manifest variables, export `VRSFOV_ENABLE=1`, and start `vrsfov-gaze -q` in the background if it is not already running. On Steam Frame, you can disable Valve's FDM layer for comparison with `VK_LOADER_LAYERS_DISABLE=VK_LAYER_VALVE_fdm_injection`.

## Configuration

Config file: `$XDG_CONFIG_HOME/vrsfov/settings.ini` (default `~/.config/vrsfov/settings.ini`).
Standalone mode hot-reloads this file once per second; OpenXR mode hot-reloads it on `xrWaitFrame`.

```ini
[global]
enabled = 1
eye_tracking = 1              ; 0 = fixed fovea (publisher eye size still used)
preset = lo                   ; lo | med | hi (Valve FDM geometry; explicit keys win)
inner_radius = 0.286          ; eye-tracked radii, normalized eye-image units
outer_radius = 0.95
fixed_inner_radius = 0.286    ; used when eye tracking is inactive/disabled
fixed_outer_radius = 0.95
inner_rate = 1x1              ; 1x1 1x2 2x1 2x2 2x4 4x2 4x4
middle_rate = 2x2
outer_rate = 2x2
horizontal_scale = 1.03       ; >1 widens the foveal ellipse
fixed_offset_x = 0.0          ; fixed-mode gaze offset, tangent units
fixed_offset_y = 0.0
smoothing = 0.0               ; gaze EMA factor (0.0 = off, 0.9 = heavy)
gaze_yflip = 0                ; mirror v if the eye image is composed vertically flipped
gaze_yflip_eye_images = 0     ; mirror v on intermediate layered eye targets (e.g. VTOL VR)
texel_size = 0                ; 0 = auto (16 clamped to device range)
size_tolerance = 0.02         ; eye-candidate aspect ratio tolerance
min_size_ratio = 0.5          ; candidate width >= ratio * recommended width
standalone = 0                ; 1 = foveate without OpenXR (default 1 on frame target; auto-enabled for OpenVR)
eye_size = auto               ; WxH (e.g. 2432x2304) or auto
debug_mode = 0                ; 0 = off, 1 = outer rate everywhere, 2 = inverted
draw_radii = off              ; stamp rate-map rings at radii: off | both | inner | outer
ring_width = 2                ; ring width in rate-image texels (1..8)
paint_gaze = 0                ; clear inner foveal box at pass end (left=green, right=red)
log_level = info              ; error | warn | info | debug | trace

[app:hello_xr]                ; matches XrApplicationInfo::applicationName (case-insensitive)
outer_rate = 2x2

[exe:SomeGame.exe]            ; matches basename of /proc/self/exe or any cmdline token
enabled = 0
```

Every key can be overridden via environment variable `VRSFOV_<UPPER_SNAKE_KEY>` (e.g. `VRSFOV_PRESET=med`, `VRSFOV_DRAW_RADII=both`). Precedence (low to high): defaults → `[global]` → `[exe:...]` → `[app:...]` → environment.

### Additional Environment Variables

| Variable | Description |
|---|---|
| `VRSFOV_ENABLE=1` | Enable the implicit Vulkan and OpenXR layers |
| `VRSFOV_DISABLE=1` | Force both layers off |
| `VRSFOV_DISABLE_VK=1` / `VRSFOV_DISABLE_XR=1` | Keep layers loaded but disable the Vulkan or OpenXR half (for bisecting) |
| `VRSFOV_LOG_STDERR=1` | Mirror all log levels to `stderr` (useful with `PROTON_LOG=1`) |
| `VRSFOV_OPENVR_GAZE=0` | Disable reading the `vrsfov-gaze` shared-memory publisher in standalone/OpenVR mode (enabled by default) |
| `VRSFOV_OPENVR_CLIENT=2` | `vrsfov-gaze` OpenVR client type: `2`/other (default), `1`/background, `3`/overlay, `5`/utility |
| `VRSFOV_STEAMVR_DIR=/path` | Path to `SteamVR/bin/linux64` (or `linuxarm64`) if installed in a non-standard location |
| `VRSFOV_START_GAZE=0` | Do not auto-start `vrsfov-gaze` in `run-pc.sh` / `run-frame.sh` |
| `VRSFOV_FORCE_ACTIVE=1` | Activate the Vulkan half without an `XrInstance` (testing) |
| `VRSFOV_FAKE_EYE=WxH` / `VRSFOV_FAKE_GAZE=u,v` | Synthesize eye size and gaze snapshot (testing) |

## Visual Verification & Debugging

Logs are appended to `$XDG_STATE_HOME/vrsfov/vrsfov.log` (default `~/.local/state/vrsfov/vrsfov.log`, rotated at 16 MB) with a `==== vrsfov [vk|xr] pid N <exe> ====` header per process.

- **In-pass gaze box (`paint_gaze = 1` / `VRSFOV_PAINT_GAZE=1`)**: Clears the inner foveal box at the end of each foveated eye pass (green for left eye, red for right eye) using `vkCmdClearAttachments`.
- **Rate-map rings (`draw_radii = both` / `VRSFOV_DRAW_RADII=both`)**: Stamps a thin anisotropic rate ring (`2x1`/`1x2`) at the inner and outer radii so you can verify driver VRS attachment support in-headset or on the desktop mirror.
- **SteamVR overlay (`vrsfov-overlay`)**: Separate process that draws the foveal radii or a solid gaze box (`--box`) as a SteamVR compositor overlay quad without touching the game's rendering (`scripts/overlay-run.sh` sets the required library paths on NixOS; run `vrsfov-overlay --help` for options).
- **Live game monitor**: `scripts/steam-game-check.sh <appid>` streams `vrsfov.log` alongside relevant Proton/loader lines while launching a Steam game.
- **Headless testing**: `ctest --test-dir build-pc` runs unit tests, loader protocol probes (`vk_layer_probe`, `xr_layer_probe`), headless GPU tests across render pass modes (`dynamic`, `rp1`, `rp2`, `multiview`, `imageless`, `layers2`), and mock-runtime OpenXR→Vulkan end-to-end tests (`xr_vk_e2e*`).

## Troubleshooting

| Symptom | Fix |
|---|---|
| Nothing in `vrsfov.log` for the game | Add `VRSFOV_ENABLE=1 %command%` to the game's Steam launch options. For Proton OpenXR games, also ensure `PRESSURE_VESSEL_IMPORT_OPENXR_1_LAYERS=1` is set. |
| `device … capable but waiting for an OpenXR instance` | Normal when a game creates its `VkDevice` before `XrInstance` or before loading `vrclient` (e.g. VTOL VR). The layer activates automatically on the next command buffer once OpenXR or OpenVR initializes. |
| `layer inactive (VK_KHR_fragment_shading_rate not available)` | GPU/driver does not support attachment VRS (e.g. `llvmpipe`), or the app already uses `VK_NV_shading_rate_image` / `VK_EXT_fragment_density_map`. |
| Layer active (`attaching rate image` in log) but hard to notice | On RADV (RDNA2/3/4), max rate is `2x2` (half resolution in periphery). Test with `VRSFOV_DRAW_RADII=both`, `VRSFOV_PAINT_GAZE=1`, or `VRSFOV_DEBUG_MODE=2`. |
| Fovea is inverted vertically | Set `gaze_yflip = 1` (or `gaze_yflip_eye_images = 1` if the game renders intermediate layered eye images upside down, like VTOL VR). |
| HMD reports bogus gaze center without eye tracking hardware | Set `eye_tracking = 0` in `settings.ini` to keep fixed foveation while still using `vrsfov-gaze`'s recommended per-eye render size. |
| Wrong target foveated in standalone mode | Check `render target WxH` lines with `VRSFOV_LOG_LEVEL=debug` and set `eye_size = WxH` for the game's `[exe:...]` section. |

## Limitations

- **RADV**: `maxFragmentShadingRateAttachmentTexelSize` is `8x8` and supported rates are `1x1`, `1x2`, `2x1`, `2x2` (`4x4` clamps to `2x2`). `layeredShadingRateAttachments` is unsupported on RADV, so multiview passes share a single-layer rate image.
- **Turnip (Steam Frame, Adreno 750)**: verified, including the GPU test suite. Turnip does not make `vkCmdCopyBufferToImage` writes visible to the fragment-shading-rate attachment when the dependency is expressed with `VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR` alone (only the tail of the rate image was visible), so the layer hands the rate image over with a conservative `TRANSFER -> ALL_GRAPHICS` barrier.
- **Scope**: Only Vulkan apps (including D3D11/D3D12 via DXVK/vkd3d-proton) are supported. OpenGL apps and apps already using VRS/FDM or `VK_EXT_shader_object` are left untouched.
