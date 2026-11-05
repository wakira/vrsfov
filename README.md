# vrsfov

VRS-based foveated rendering for Linux VR, two build targets:

| Target | Artifact | What it does | Gaze source |
|---|---|---|---|
| `pc` | `libvrsfov.so` (Vulkan + OpenXR layers) | native Vulkan OpenXR apps and Proton games via wineopenxr + DXVK/vkd3d-proton | `XR_EXT_eye_gaze_interaction`, optional SteamVR OpenVR fallback |
| `frame` | `libvrsfov.so` (Vulkan layer) + `vrsfov-gaze` | Steam Frame / standalone Vulkan apps (OpenVR and native) | the `vrsfov-gaze` publisher over `/dev/shm/vrsfov-gaze` |

The shared Vulkan core enables `VK_KHR_fragment_shading_rate` at device creation,
adds a fragment-shading-rate (FSR) attachment to every render pass / dynamic
rendering, sets the pipeline combiner ops so the attachment is honored, and fills
the rate image with a foveated pattern using **transfer commands only**
(`vkCmdCopyBufferToImage` from a static, precomputed pattern buffer at a
gaze-dependent offset).

On the PC target, an OpenXR API layer learns the session's `VkDevice`, per-eye
FOV, the eye render regions of the last `xrEndFrame` and the gaze point, then
publishes an immutable snapshot for the Vulkan half. On the Frame target there is
no OpenXR half: the layer is standalone by default and reads the per-eye centre
that `vrsfov-gaze` publishes.

The design is documented in [`plan/`](plan/) — start at
[`plan/00-overview.md`](plan/00-overview.md). For details that the plan leaves
open, the Vulkan/OpenXR specifications win.

`~/.config/vrsfov/settings.ini` and `~/.local/state/vrsfov/vrsfov.log` are read.

## Status

Everything runs on the machine it was developed on: RADV (RX 9070 XT), Mesa 26.x, Vulkan
1.4. No headset was available, so the OpenXR half is verified against a **mock OpenXR runtime**
(`tests/xr_vk_e2e`, see [Testing](#testing)); Monado 25.1 with its Simulated HMD loads and negotiates
the layer but its compute compositor cannot start in this environment, so a full `xrgears`/`hello_xr`
session is still untested. See `PLAN.md` for the complete status, evidence and the list of deviations
from the plan.

## Build

```sh
nix develop          # or: nix-shell    (NixOS)

# PC: Vulkan layer + OpenXR layer
cmake -B build-pc -GNinja -DVRSFOV_USE_SYSTEM_HEADERS=ON -DVRSFOV_PLATFORM=pc
cmake --build build-pc
ctest --test-dir build-pc --output-on-failure

# Steam Frame: Vulkan layer + gaze publisher, no OpenXR half
cmake -B build-frame -GNinja -DVRSFOV_USE_SYSTEM_HEADERS=ON -DVRSFOV_PLATFORM=frame
cmake --build build-frame
ctest --test-dir build-frame --output-on-failure
```

Without Nix, CMake fetches pinned `Vulkan-Headers` and `OpenXR-SDK` headers with
`FetchContent` (no loader is built); the Frame target skips OpenXR entirely.
`-DVRSFOV_USE_SYSTEM_HEADERS=ON` uses system headers instead. Build options:

| Option | Default | Meaning |
|---|---|---|
| `VRSFOV_PLATFORM` | `pc` | `pc` (Vulkan + OpenXR) or `frame` (Vulkan only, standalone by default) |
| `VRSFOV_USE_SYSTEM_HEADERS` | `OFF` | use system Vulkan/OpenXR headers instead of FetchContent |
| `VRSFOV_BUILD_TESTS` | `ON` | build the unit tests and register `ctest` entries |
| `VRSFOV_BUILD_GPU_TESTS` | `ON` | build the headless GPU end-to-end test app |

## Run without installing

There are no install scripts and no CMake install rules. The build tree is the
install; the standard loader environment variables point at its manifests:

```sh
# PC
scripts/run-pc.sh ./my_vulkan_vr_app
# Steam launch options:
#   /absolute/path/to/vrsfov/scripts/run-pc.sh %command%

# Steam Frame
scripts/run-frame.sh ./game
#   /absolute/path/to/vrsfov/scripts/run-frame.sh %command%
```

`run-pc.sh` sets:

```sh
VK_LAYER_PATH=$PWD/build-pc/manifests/vulkan
VK_INSTANCE_LAYERS=VK_LAYER_VRSFOV_vrs
XR_API_LAYER_PATH=$PWD/build-pc/manifests/openxr
XR_ENABLE_API_LAYERS=XR_APILAYER_VRSFOV_foveated_rendering
PRESSURE_VESSEL_IMPORT_OPENXR_1_LAYERS=1
VRSFOV_ENABLE=1
```

`run-frame.sh` sets only the Vulkan variables, `VRSFOV_ENABLE=1` and
`VRSFOV_OPENVR_GAZE=1`, and starts `vrsfov-gaze` once if it is not already
running (`VRSFOV_START_GAZE=0` disables that). Nothing is written to `/usr` or to
the loader's implicit-layer directories. `VRSFOV_DISABLE=1` forces the layer off.
The only files touched outside the build tree are the config
(`~/.config/vrsfov/settings.ini`) and the log
(`~/.local/state/vrsfov/vrsfov.log`).

## Steam Frame

The Frame target is the same Vulkan core without the OpenXR half:

```sh
cmake -B build-frame -GNinja -DVRSFOV_USE_SYSTEM_HEADERS=ON -DVRSFOV_PLATFORM=frame
cmake --build build-frame
```

Launch a game with:

```sh
scripts/run-frame.sh %command%
```

or set the same variables by hand:

```sh
VK_LAYER_PATH=$PWD/build-frame/manifests/vulkan \
VK_INSTANCE_LAYERS=VK_LAYER_VRSFOV_vrs \
VRSFOV_ENABLE=1 VRSFOV_OPENVR_GAZE=1 %command%
```

Start `build-frame/vrsfov-gaze -q` once (or let `run-frame.sh` do it) if you do
not already have a user service for it.

`standalone = 1` is the compile-time default on this target, so no config edit is
needed for a first run. `vrsfov-gaze` is the only process that talks to SteamVR;
the layer reads `/dev/shm/vrsfov-gaze`. The publisher stays disconnected until a
game writes `/dev/shm/vrsfov-gaze.want` (once per second while rendering), which
keeps it from becoming the OpenVR session manager at boot. Turn Valve's own
foveation layer off for a clean comparison:

```sh
VK_LOADER_LAYERS_DISABLE=VK_LAYER_VALVE_fdm_injection
```

On the Frame, `eye_tracking` in the config only controls the unused OpenXR path;
the shm publisher is enabled by `VRSFOV_OPENVR_GAZE`. `VRSFOV_OPENVR_CLIENT`
selects the publisher's SteamVR client type (2/other by default).

## Testing

`ctest --test-dir build-pc` / `ctest --test-dir build-frame`:

| Test | What it covers |
|---|---|
| `unit` | 59 unit tests on the PC target (config/INI, pattern, classify, gaze projection, gaze shm, render-pass conversion, SPIR-V scan, OpenXR helpers); 51 on the frame target |
| `vk_vrs_test_*` | GPU end-to-end with a synthesized snapshot (`dynamic`, `rp1`, `rp2`, `multiview`, `imageless`, `nonEyeSize`) |
| `xr_layer_probe` | the OpenXR loader discovers and negotiates the API layer |
| `vk_layer_probe` | loader-protocol requirements of the Vulkan layer: it must provide `vkCreateInstance` for *any* instance argument (loaders and winevulkan ask with a handle of their own) and must not crash on handles it does not own |
| `xr_vk_e2e` | a mock OpenXR runtime drives the OpenXR half and the Vulkan half must foveate at the gaze it published |
| `xr_vk_e2e_no_eye_gaze` | the same with a runtime that has no eye gaze: fixed-foveation fallback |
| `xr_vk_e2e_no_app_gaze_suggest` | the app never suggests the eye-gaze profile: the layer binds and uses its own action |
| `xr_vk_e2e_app_owns_gaze_binding` | the app owns the gaze binding path: the layer drives the app's action instead |
| `xr_vk_e2e_device_first` | Vulkan device created before the XrInstance: late activation still follows gaze |
| `d3d11_smoke` (script) | the layer works with DXVK: `bash tests/d3d11_smoke/run-with-wine.sh` builds a small D3D11 program with mingw-w64 and runs it under wine + DXVK, plain and with the layer foveating |
| `vk_vrs_test_standalone_env` / `_file` | standalone fixed foveation without any OpenXR instance, configured by environment / by an `[exe:…]` section |
| `vk_vrs_test_frame_default` | the frame build foveates without an explicit `standalone = 1` (platform default) |
| `standalone_hot_reload` | rewriting `settings.ini` while a standalone app renders is picked up |

The GPU tests are self-contained: they enable the build-dir layer with `VK_LAYER_PATH` and
`VK_INSTANCE_LAYERS` (OpenXR tests also set `XR_API_LAYER_PATH`), so no installed layer is required.
They need a Vulkan device with attachment VRS.

### Watching a game attempt live

`scripts/steam-game-check.sh <appid>` prints the launch options to paste into the
game's Steam properties and then streams, in one terminal, our layer's log lines
and the interesting parts of Proton's log (loader/layer messages, vrclient, OpenXR,
DXVK instance creation, crashes) while you start the game — much easier to judge
than reading the files afterwards.

### The quickest SteamVR/OpenXR check

SteamVR ships a small OpenXR sample, which is the fastest way to see the OpenXR
half working against the real runtime and HMD:

```sh
# with SteamVR running
VRSFOV_ENABLE=1 VRSFOV_LOG_LEVEL=debug \
  ~/.local/share/Steam/steamapps/common/SteamVR/bin/linux64/helloxr
grep -E "runtime: SteamVR|created with VkDevice|recommended per-eye|eye tracking|ACTIVE|foveated" \
  ~/.local/state/vrsfov/vrsfov.log
```

And the quickest Proton check needs no VR at all: run any D3D11/D3D12 game through
Proton with standalone fixed foveation, which proves the container imports the
layer and that it foveates:

```
VRSFOV_ENABLE=1 VRSFOV_STANDALONE=1 VRSFOV_DEBUG_MODE=2 %command%
```

(`debug_mode = 2` makes the foveated region stand out as a blocky patch, and on a
flat game you can see it on the monitor.)

### Without a headset

The Vulkan half can be driven with a synthesized snapshot:

```sh
export VRSFOV_ENABLE=1 VRSFOV_FORCE_ACTIVE=1 VRSFOV_FAKE_EYE=1024x1024
build-pc/tests/vk_vrs_test dynamic     # dynamic rendering
build-pc/tests/vk_vrs_test rp1         # render pass v1
build-pc/tests/vk_vrs_test rp2         # render pass v2
build-pc/tests/vk_vrs_test multiview   # 2-layer multiview render pass
build-pc/tests/vk_vrs_test imageless   # imageless framebuffer
build-pc/tests/vk_vrs_test nonEyeSize  # 512x256 target -> must stay full rate
build-pc/tests/vk_vrs_test dynamic --coverage   # driver-independent probe
bash tests/vk_vrs_test/gaze_sweep.sh         # the fovea must follow an injected gaze
```

`VRSFOV_FAKE_GAZE=0.25,0.30` places the fovea anywhere in the eye image; the test app verifies
the fovea's centroid matches it. The `--coverage` flag uses a `gl_FragCoord`-based probe instead of
`gl_ShadingRateEXT`, so it also works on drivers that do not report the applied rate through the
built-in.

### Seeing the fovea without judging sharpness (`draw_radii`)

The effect of VRS is a change of *sharpness*, which is hard to see in a moving
scene, and on hardware whose supported rates are only `1x1` and `2x2` the outer
radius draws no visible boundary at all (`middle_rate` and `outer_rate` clamp to
the same rate). `draw_radii` stamps a thin band at the radii instead:

```
VRSFOV_DRAW_RADII=both VRSFOV_RING_WIDTH=4 %command%
```

or in `settings.ini`:

```
draw_radii = both            ; off | both | inner | outer
ring_width = 4               ; band width in rate-image texels (1..8)
```

The band is drawn with an anisotropic rate (`2x1`/`1x2`) when the device supports
one, because that rate differs from both `1x1` and `2x2` and therefore shows up as
a real circle rather than as the edge of a zone. So you get two concentric circles
around the fovea (one per radius), in the headset and in the desktop mirror. If the
hardware has no such rate, the circle degrades to the edge of the inner zone, which
the log says:

```
INFO [vk] draw_radii=1: circle at r=0.40 rate 2x1, circle at r=0.75 rate 2x1, 4 texel(s) wide
```

This is also the strongest available test that the rate map reaches the image: if
the circles appear, the driver is honouring the attachment, so any "I cannot see
it" after that is about how subtle `2x2` looks, not about the layer. Turn it off
(`draw_radii = off`) for real use.

### SteamVR-native gaze source (experimental, `VRSFOV_OPENVR_GAZE=1`)

The layer now suggests its own eye-gaze binding, so an app that never mentions
`/interaction_profiles/ext/eye_gaze_interaction` still gets a bound action. Some
SteamVR versions have been observed reporting that action as inactive even with the
binding present (measured on SteamVR 2.18.3 with the Frame's vrlink driver), while
SteamVR's *own* eye tracking is available to the renderer
(`IVRSystem::GetEyeTrackedFoveationCenter`, `Prop_SupportsXrEyeGazeInteraction_Bool`).
The optional source below covers that case.

With `VRSFOV_OPENVR_GAZE=1` the layer loads `libopenvr_api.so` from the same
install as the `vrclient.so` already mapped into the process, initialises an
"Other" SteamVR client (`VRSFOV_OPENVR_CLIENT` selects the type; 2 is the
default, 1/background is refused gaze by SteamVR), logs what it finds and uses the
per-eye foveation centre when the runtime provides it:

```
INFO [xr] VRSFOV_OPENVR_GAZE=1: runtime 2.18.3, HMD 'Deckard MP', IVRSystem_026,
     Prop_SupportsXrEyeGazeInteraction=true, GetEyeTrackedFoveationCenter=ok
INFO [xr] eye tracking active (OpenVR source)
```

For **OpenVR-only games** (no `XrInstance`, so the OpenXR half never runs) the same
source is available in standalone mode: with `standalone = 1` and
`VRSFOV_OPENVR_GAZE=1`, the Vulkan half polls the per-eye centre at ~120 Hz,
applies `smoothing`/`gaze_yflip`, and publishes it as the snapshot's gaze, so those
titles get gaze-following foveation instead of a fixed spot (`fixed_offset_x/y` no
longer apply). Look for:

```
INFO [vk] standalone gaze (VRSFOV_OPENVR_GAZE): client type 2; … GetEyeTrackedFoveationCenter=ok
INFO [vk] standalone gaze: eye tracking active (OpenVR source), centre left (0.6xx, 0.5xx) right (0.6xx, 0.5xx)
```

The snapshot carries the gaze per eye, so a per-eye or side-by-side target gets each
eye's own centre and only a single-target pass uses their average.

On PC, `run-pc.sh` also starts `build-pc/vrsfov-gaze` as an out-of-process
fallback. Some games keep the OpenXR gaze action active in menus but report it
inactive in gameplay; the layer then tries an in-process OpenVR client and, if
that has no data, reads the publisher over `/dev/shm/vrsfov-gaze`. The publisher
is a separate SteamVR client, so it is not tied to the game's OpenXR session.
`VRSFOV_START_GAZE=0` stops the launcher from starting it; `VRSFOV_OPENVR_GAZE=0`
disables both OpenVR fallbacks. If SteamVR is not in a standard location, set
`VRSFOV_STEAMVR_DIR=/path/to/SteamVR/bin/linux64`.

It is opt-in and off by default because it adds a second SteamVR client to the
game's process; SteamVR may also refuse gaze to a secondary client, in which case
the log says `GetEyeTrackedFoveationCenter=no data` and fixed foveation continues. The header is taken from the system (the dev shell sets
`VRSFOV_OPENVR_INCLUDE_DIR`) or downloaded at configure time; the library itself is
never linked, only `dlopen`ed.

### Seeing the radii in VR: `vrsfov-overlay`

The rate-map rings (`draw_radii`) express the radii as a change of *sharpness*, which
on hardware whose rates are 1x1/2x2 is subtle. `vrsfov-overlay` instead draws them as
a **solid-coloured SteamVR overlay quad**:

```sh
build-pc/vrsfov-overlay                     # radii read from settings.ini
build-pc/vrsfov-overlay --inner 0.18 --outer 0.45 --color-inner 00ff00 --gaze on
```

It is a **separate process**, started when you want the marker, and it draws through
OpenVR's `IVROverlay` - the compositor puts it over whatever scene is running. That
matters twice over: it works over **OpenVR and OpenXR applications alike** (they are
both just scenes to the compositor, so there is nothing per-API to support), and it
**cannot disturb the app** - no injected commands, no pipelines, nothing written into
the app's attachments. That is the pattern fpsVR, OVR Advanced Settings and XSOverlay
use, and it is deliberately not the pattern that hung the GPU here (an earlier
attempt drew with `vkCmdClearAttachments` inside a pass that has the shading-rate
attachment, which is not something to do).

How the geometry works: the quad spans the chosen eye's field of view
(`IVRSystem::GetProjectionRaw`), so a normalised radius `r` maps to `r *
horizontal_scale / 2` of the quad's width and `r / 2` of its height - the same
normalisation the rate pattern uses - and since both the eye buffer and the quad are
linear in the tangent of the view angle, the ring lands on the real fovea boundary.
It is approximate in one respect: a single quad serves both eyes, whose FOVs differ
slightly, so the match is best for the eye you pick with `--eye`.

| Flag | Meaning |
|---|---|
| `--inner R` / `--outer R` | radii; default from the config's `inner_radius` / `outer_radius` |
| `--width W` | ring thickness, normalised radius units (default 0.01) |
| `--hscale S` | horizontal scale; default from the config |
| `--distance M` | quad distance in metres (default 1.0) |
| `--alpha A` | overlay opacity (default 0.85) |
| `--color-inner RRGGBB` / `--color-outer RRGGBB` | ring colours |
| `--eye left\|right\|average` | whose FOV sizes the quad (default average) |
| `--gaze on\|off` | follow `GetEyeTrackedFoveationCenter` (default on) |
| `--flip-v`, `--bgra` | if the ring looks mirrored vertically, or its colour is wrong |
| `--once` | create the overlays, print status, exit (no loop) |
| `--config PATH` | settings.ini to read defaults from |

Two caveats. SteamVR must be reachable: outside the Steam runtime this needs the
runtime's library paths (`LD_LIBRARY_PATH` with `SteamVR/bin/linux64` and the Steam
runtime's 64-bit lib directory), which is the same environment SteamVR gives its own
helper processes. And it is **untested against a live headset** - the geometry, the
texture generator and the flags are all exercised offline (`tests/unit`), but the
overlay calls themselves have not been run yet.

### The OpenXR half without a runtime

`build-pc/tests/xr_vk_e2e` plays the OpenXR loader *and* a mock runtime in one process:

1. it creates a Vulkan instance, so the Vulkan loader loads the layer; the test finds that exact
   library in `/proc/self/maps` and `dlopen`s it, which is what makes the two halves share one
   `SharedState` (this also verifies the "one `.so`, two manifests" assumption of the design),
2. it negotiates the API layer and creates an instance through the layer's `createApiLayerInstance`
   with the mock as the "next" layer,
3. it runs a normal frame loop — session with a Vulkan binding, swapchain with registered
   `VkImage`s, action set suggestion/attach/sync, `xrWaitFrame`/`xrLocateViews`/`xrBeginFrame`/
   `xrEndFrame` — where the mock reports the eye poses, FOVs and the gaze pose,
4. it renders a Vulkan frame into the registered swapchain image and checks the read-back rate map:
   the full-rate fovea must sit on the gaze the runtime reported, and must move when the gaze moves.

With `VRSFOV_E2E_NO_EYE_GAZE=1` the mock runtime has no eye gaze: the test then requires the layer not
to ask for the extension, to leave the app's action sets alone, and to keep the fovea fixed in the
centre of the eye image.

## Configuration

`$XDG_CONFIG_HOME/vrsfov/settings.ini` (default `~/.config/vrsfov/settings.ini`):

```ini
[global]
enabled = 1
eye_tracking = 1
preset = lo                  ; lo | med | hi (Valve FDM geometry; explicit keys win)
inner_radius = 0.286         ; eye-tracked radii, normalized eye-image units
outer_radius = 0.95
fixed_inner_radius = 0.286   ; used when there is no eye tracking
fixed_outer_radius = 0.95
inner_rate = 1x1             ; 1x1 1x2 2x1 2x2 2x4 4x2 4x4
middle_rate = 2x2
outer_rate = 2x2
horizontal_scale = 1.03      ; >1 widens the foveal ellipse
fixed_offset_x = 0.0         ; fixed-mode gaze offset, tangent units
fixed_offset_y = 0.0
smoothing = 0.0              ; gaze EMA, 0.9 = heavy
gaze_yflip = 0               ; mirror v if the eye image is composed flipped
texel_size = 0               ; 0 = auto (16 clamped to the device range)
size_tolerance = 0.02        ; eye-candidate aspect tolerance
min_size_ratio = 0.5         ; candidate width >= ratio * recommended width
debug_mode = 0               ; 0 off, 1 = whole eye coarse, 2 = inverted
draw_radii = off             ; debug circles: off | both | inner | outer
ring_width = 2               ; circle width in rate-image texels (1..8)
log_level = info             ; error | warn | info | debug | trace

[app:hello_xr]               ; XrApplicationInfo::applicationName, case-insensitive
outer_rate = 2x2

[exe:SomeGame.exe]           ; basename of /proc/self/exe or of any cmdline token
enabled = 0
```

Every key is also an environment variable: `VRSFOV_<UPPER_SNAKE_KEY>`, e.g.
`VRSFOV_OUTER_RATE=2x2`. Precedence (low to high): defaults, `[global]`,
`[exe:...]`, `[app:...]`, environment.

Removed keys: `swapchain_only`, `stale_behavior` and `skip_depth_only`. Stale gaze now
always falls back to the fixed centre, depth-only passes are always skipped, and the
size heuristics always run after the XR swapchain lookup. If an old `settings.ini`
still contains them, delete the lines to silence the unknown-key warning.

Test/debug environment variables (not part of the config file):

| Variable | Meaning |
|---|---|
| `VRSFOV_ENABLE` | load the layers (`1`) |
| `VRSFOV_DISABLE` | force both layers off |
| `VRSFOV_FORCE_ACTIVE` | activate the Vulkan half without an `XrInstance` (testing) |
| `VRSFOV_FAKE_EYE=WxH` | synthesize the snapshot instead of running the OpenXR half |
| `VRSFOV_FAKE_GAZE=u,v` | gaze point used with `VRSFOV_FAKE_EYE` |

Logs: `$XDG_STATE_HOME/vrsfov/vrsfov.log`. The file is **appended** to and
each process that loads the layer writes a header, so a game, SteamVR's compositor
and the runtime can all be inspected together (it is rotated to `.1`, `.2`, `.3` at 16 MB):

```
==== vrsfov [vk] pid 9279 VTOLVR.exe ====
```

Warnings and worse also go to stderr; `VRSFOV_LOG_STDERR=1` sends **all**
levels to stderr too, which is what you want for a Steam game because its output
lands in the Steam console log. Set `log_level = debug` (or
`VRSFOV_LOG_LEVEL=debug`) to see the per-target classification, the injected
pipelines and every rate image update.

## What the layer does, in short

- **Eligible devices only**: the device must expose
  `VK_KHR_fragment_shading_rate` + attachment/pipeline VRS features, `R8_UINT`
  must be usable as a rate attachment, and the app must not use NV shading rate
  image or EXT fragment density map. Everything else stays untouched.
- **A device created before the `XrInstance` is not lost**: Vulkan graphics is
  often initialised before OpenXR (VTOL VR does), and device features are
  immutable, so such a device gets our extensions and features enabled right away
  and activates as soon as an `XrInstance` appears (the `vkBeginCommandBuffer`
  hook notices). Render passes, framebuffers and pipelines created before that
  point stay unmodified. Plain Vulkan apps (no OpenXR, no standalone) are still
  never injected into.
- **Every render pass of an eligible device** gets the FSR attachment appended
  (pipelines and render passes stay compatible automatically). Framebuffers that
  are not eye candidates point at a shared all-zero *neutral* image.
- **Eye candidates** are found from the OpenXR swapchain images and `xrEndFrame`
  rectangles, then by multiview stereo, eye-sized targets and side-by-side
  layouts, with aspect/size tolerances.
- **Dynamic rendering** only gets the rate attachment when there is something to
  foveate; otherwise nothing is recorded at all.
- **Pipelines** get `VkPipelineFragmentShadingRateStateCreateInfoKHR{1x1,
  KEEP/MAX}` and, for dynamic rendering, the
  `RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT` flag. Pipelines that cannot be
  combined with VRS (fragment depth/stencil writes, sample masks, sample shading)
  are created with `KEEP/KEEP` so our attachment is ignored for them.
- **No compute shaders, no app memory modified** other than documented
  patch-call-restore spots (device features, framebuffer attachments, pipeline
  flags).

## Troubleshooting

### A game crashes or refuses to start

**First check the loader's verdict** in the game's output (with `VK_LOADER_DEBUG=error,warn,layer`):

```
[Vulkan Loader] ERROR:  loader_create_instance_chain: Failed to find 'vkCreateInstance'
```

If that appears, the layer returned `NULL` from `vkGetInstanceProcAddr(…, "vkCreateInstance")`
and the loader unloaded it, failing *every* Vulkan instance creation in the process
(vrclient, wineopenxr and DXVK then all fail, and the game dies). This was a real bug
(loaders and winevulkan ask for `vkCreateInstance` with an instance handle the layer has
never seen); it is fixed and guarded by the `vk_layer_probe` test — make sure the
build you are launching is current (`cmake --build build-pc`).

Three switches let you bisect without editing code; use them one at a time (and
add `DISABLE_LSFG=1` if you have the Lossless Scaling frame-generation layer
installed, which is implicit and therefore always on):

| Launch options | What it means |
|---|---|
| `VRSFOV_DISABLE=1 %command%` | our layers are not loaded at all (baseline: does the game start?) |
| `VRSFOV_ENABLE=1 VRSFOV_DISABLE_VK=1 VRSFOV_DISABLE_XR=1 %command%` | both layers are loaded and negotiated but do nothing ("mere presence") |
| `VRSFOV_ENABLE=1 VRSFOV_STANDALONE=1 VRSFOV_EYE_SIZE=WxH VRSFOV_DISABLE_XR=1 %command%` | the Vulkan half foveates, the OpenXR half is inert |
| `VRSFOV_ENABLE=1 %command%` (and no `standalone`) | the Vulkan half is loaded but inactive; an OpenXR app still gets the OpenXR half |

`VRSFOV_LOG_STDERR=1` plus `PROTON_LOG=1` puts our lines and Proton's log into
the Steam console, and the log file keeps every process's session separately (look
for the `==== vrsfov [vk] pid N <game> ====` header).

VRChat writes crash dumps to
`steamapps/compatdata/438100/pfx/drive_c/users/steamuser/AppData/Local/Temp/VRChat/VRChat/Crashes/`
and Easy Anti-Cheat logs to `.../AppData/Roaming/EasyAntiCheat/<product>/.../anticheatlauncher.log`.

Check the log first: `$XDG_STATE_HOME/vrsfov/vrsfov.log` (set
`VRSFOV_LOG_LEVEL=debug` for the per-target classification, the injected
pipelines and every rate image update).

| Symptom | Likely cause |
|---|---|
| nothing in the log for a game, no header line with the game's name | the layer was not enabled for that process: the game needs `VRSFOV_ENABLE=1` **in its own launch options** (verify with `grep "adding PID" ~/.steam/steam/logs/gameprocess_log.txt`, the command line is logged verbatim) |
| nothing in the log, no `[vk]`/`[xr]` lines | the layer is not enabled or not found: launch through `scripts/run-pc.sh`, or set `VK_LAYER_PATH`/`VK_INSTANCE_LAYERS` and `XR_API_LAYER_PATH`/`XR_ENABLE_API_LAYERS` to the build's `manifests/` directories |
| `layer inactive (no OpenXR instance)` | the `XrInstance` is created *after* the `VkDevice` but no instance exists at all yet (plain Vulkan app, or the app crashed before OpenXR init); `VRSFOV_FORCE_ACTIVE=1` forces the layer on for testing |
| `device … capable but waiting for an OpenXR instance` | the `VkDevice` was created before the `XrInstance` (VTOL VR does this). Nothing is wrong: the device activates on the next command buffer once the instance appears, and only objects created before that stay unmodified. If it never activates, the app never created an instance — use `standalone = 1` + `eye_size` for it |
| `layer inactive (VK_KHR_fragment_shading_rate not available)` | the device/driver has no attachment VRS (e.g. llvmpipe), or the app already uses NV shading rate image / EXT fragment density map |
| `layer inactive (R8_UINT is not a fragment shading rate attachment format)` | the driver does not support the required attachment format |
| layer active but nothing is foveated | the render target is not recognised as an eye render (see the `classify` lines): try `[global] size_tolerance`/`min_size_ratio`; for XR apps check that `xrEndFrame` with a two-view projection layer is reached |
| fovea in the wrong place | the gaze comes from the runtime: check `eye tracking active`/`unavailable` in the log, and `fixed_offset_x`/`fixed_offset_y` for the fixed mode |
| layer foveates (`foveated` + `attaching` in the log) but the picture looks the same | expected on hardware whose rates are only `1x1`/`2x2`: the whole effect is "half resolution" in a region. Use `draw_radii = both` to see the radii as circles, and `debug_mode = 1`/`0` to A/B the maximum difference the device can produce. If even the `draw_radii` circles are invisible, check the `pipeline injection:` summary — the pipelines are probably not honouring the attachment |
| `draw_radii` circles are visible but only in one eye | the target was classified as a single eye; the `classify` line says which (render area vs framebuffer, viewMask/layers) |
| everything is blocky (`debug_mode = 1` left on) | set `debug_mode = 0` |
| Steam/Proton game: no layer (nothing in the log with the game's name) | launch the game through `scripts/run-pc.sh %command%` (or set the `VK_LAYER_PATH`/`VK_INSTANCE_LAYERS`/`XR_API_LAYER_PATH`/`XR_ENABLE_API_LAYERS` variables yourself); check that the build's `manifests/` paths are readable from inside the container |
| Steam/Proton game starts but the fovea is not where the eye render is | the target-sizes are in the log at debug level (`render target WxH …`): set `eye_size` to what the game really renders, or omit `eye_size` to foveate every colour target |
| the game exits within a second of launching | nothing to foveate yet: the layer only acts once the game creates its Vulkan device. Fix the game's startup first (anti-cheat, Proton version) |
| validation complains about a pipeline/render pass combination | please report it with the log; the layer skips render passes it cannot modify and logs a WARN |

## Known limitations / notes

- **RADV (RDNA4)**: `maxFragmentShadingRateAttachmentTexelSize` is 8x8 and the
  only supported rates are 2x2, 2x1, 1x2, 1x1. The `lo` default therefore uses
  1x1 inside and 2x2 outside; `4x4` in a hand-written config clamps to 2x2. The
  outer radius draws no visible boundary (both sides are 2x2) unless
  `draw_radii` is on, which marks it with a `2x1` band.
  `layeredShadingRateAttachments` is unsupported, so multiview renders use a
  single-layer rate image (one rectangle for both eyes).
- **No headset validation yet.** The OpenXR half is verified against a mock
  runtime (see the testing section) and Monado 25.1 loads/negotiates the layer,
  but a complete session against a real runtime/headset (`hello_xr`, `xrgears` on
  Monado, WiVRn, SteamVR) has not been run here. Eye tracking has only been
  exercised with a simulated gaze pose.
- `XR_VARJO_quad_views` and `VK_EXT_shader_object` apps are out of scope: apps
  using their own VRS or a fragment density map are detected and left untouched.
- Render pass suspend/resume across command buffers is handled for dynamic
  rendering (best effort, as in the plan) and not for render passes.
- The gaze action set is attached together with the app's action sets; apps that
  never call `xrAttachSessionActionSets` get fixed foveation (logged once).

## Fixed foveation for any Vulkan app (standalone mode)

`standalone = 1` makes the Vulkan layer foveate on its own, with no OpenXR session anywhere —
for OpenVR games (SteamVR's OpenVR path) and plain Vulkan apps. It publishes a synthetic
snapshot (session device = this device, running, fixed gaze) and then uses the size-based
eye-target heuristics.

```ini
[exe:SomeGame.exe]        ; or [global], or VRSFOV_* in the environment
standalone = 1
eye_size = 2432x2304      ; the game's per-eye render size, WxH
fixed_inner_radius = 0.35 ; the sharp spot, in normalized eye-image units
fixed_outer_radius = 0.80
outer_rate = 2x2          ; 4x4 clamps to 2x2 on RADV
fixed_offset_x = 0.0      ; centre the spot; 0.5 would move it to u = 0.75
```

Or without touching the file:

```sh
VRSFOV_ENABLE=1 VRSFOV_STANDALONE=1 VRSFOV_EYE_SIZE=2432x2304 ./game
```

How it behaves:

- A target is foveated when its aspect ratio and size look like the eye render
  (`eye_size`, `size_tolerance = 0.02`, `min_size_ratio = 0.5`, up to 4x the width). One
  eye-sized target gets a single centre rectangle; a double-wide target is split left/right
  (checked before the single-eye case, so the sharp spot is not placed between the eyes);
  a multiview target gets both layers. Everything else stays at full rate, and depth-only
  passes are always skipped.
- **No eye tracking** (OpenVR has no standard gaze API): the spot is wherever
  `fixed_offset_x`/`fixed_offset_y` put it, unwrapped with a 90° FOV convention. With
  `VRSFOV_OPENVR_GAZE=1` it follows SteamVR's eye-tracked centre instead.
- `[app:…]` sections cannot match (a Vulkan-only app has no `applicationName`); `[exe:…]`
  and `[global]` do.
- The config is read at device creation and **hot reloaded** once per second while command
  buffers are being recorded (`standalone config reloaded (generation N)`); the pattern
  buffers are rebuilt when the generation changes.
- With no `eye_size`, every colour target is foveated at the centre (logged as a warning):
  useful to see the effect, noisy for real use.

Verified on this machine (`vk_vrs_test_standalone_*` and `standalone_hot_reload` in `ctest`,
plus the maps in the testing section): a Vulkan-only process with no `XrInstance` gets the
foveated pattern, a mismatched `eye_size` stays neutral, and a config edit during rendering
takes effect.

## SteamVR and Proton

SteamVR **is** an OpenXR runtime, so this is not an either/or: OpenXR is the API the app
speaks, SteamVR is the runtime behind it. Anything that speaks OpenXR and renders with
Vulkan gets foveated — native apps and Proton games alike. (OpenVR-only games are the ones
this does not cover, see below.)

Verified on this machine:

- `steamxr_linux64.json` (the SteamVR OpenXR runtime manifest) points at
  `.../SteamVR/bin/linux64/vrclient.so`, which implements `XR_EXT_eye_gaze_interaction`,
  `/user/eyes_ext` and the `/user/eyes_ext/input/gaze_ext/pose` binding this layer suggests.
  Eye-tracked foveation therefore works with SteamVR whenever the HMD/driver actually
  reports gaze; otherwise the log says `eye tracking unavailable` and fixed foveation is used.
- Proton/pressure-vessel games get the layer from the same environment variables as
  native apps; `scripts/run-pc.sh` sets `VK_LAYER_PATH`, `VK_INSTANCE_LAYERS`,
  `XR_API_LAYER_PATH`, `XR_ENABLE_API_LAYERS` and
  `PRESSURE_VESSEL_IMPORT_OPENXR_1_LAYERS=1` before `%command%`. Nothing needs to be
  copied into the container or installed under `/usr`. Check the loader's verdict with
  `VK_LOADER_DEBUG=layer` / `XR_LOADER_DEBUG=all` in the launch options.
- SteamVR's own processes (`vrserver`, `vrmonitor`, the compositor) load the Vulkan layer
  too but stay **inactive**: they never create an `XrInstance`, so their rendering is
  untouched.
- Our layer coexists with Steam's own layers (`steam_overlay`, `steam_fossilize`): it never
  advertises a physical-device proc-addr and does not intercept physical-device calls.

Usage with SteamVR, in Steam launch options:

```
VRSFOV_ENABLE=1 VRSFOV_LOG_LEVEL=debug %command%
```

and look for `runtime: SteamVR`, `session … created with VkDevice …`, `eye tracking …` in
`$XDG_STATE_HOME/vrsfov/vrsfov.log`. `XR_LOADER_DEBUG=all` shows the loader loading
the layer.

**OpenVR games** never create an `XrInstance`, so the normal path leaves the Vulkan half
inactive and there is no per-eye render information from OpenXR. Fixed foveation is still
available through [standalone mode](#fixed-foveation-for-any-vulkan-app-standalone-mode)
(`standalone = 1` + `eye_size = WxH`): the target is picked by size instead of by the
swapchain registry, and the gaze is fixed. Eye *tracking* for OpenVR would need a vendor
gaze bridge (there is no OpenVR equivalent of `XR_EXT_eye_gaze_interaction`) — see
`PLAN.md`. Non-Vulkan apps (OpenGL, D3D9/11 without DXVK) are out of scope, and a flat
non-VR Vulkan game is inert unless standalone mode is enabled. SteamVR's compositor warp is
never modified: only the application's own eye renders are.

## Compatibility

| Runtime (OpenXR) / driver | State |
|---|---|
| RADV (RDNA4, attachment VRS, texel 8, max 2x2) | verified here, including the full OpenXR→Vulkan path with a mock runtime |
| llvmpipe / lavapipe | verified inactive (no `VK_KHR_fragment_shading_rate`), apps unaffected |
| Monado 25.1 (Simulated HMD) | layer loads and negotiates; compute compositor could not start in the test environment |
| NVIDIA (Turing+), Intel (Xe), AMD RDNA2/3 | expected to work (texel 8–16, up to 4x4); not tested here |
| SteamVR | runtime side verified here (OpenXR runtime manifest, `XR_EXT_eye_gaze_interaction` + binding path, pressure-vessel imports both layers); a headset run is still untested |
| WiVRn / ALVR | not tested here; ALVR/WiVRn are OpenXR runtimes, so the same path applies |

Applications: native Vulkan OpenXR apps (hello_xr, xrgears, Godot 4, Blender VR)
and Proton games via wineopenxr + DXVK/vkd3d-proton. OpenGL apps and apps that
already use VRS are left untouched.
