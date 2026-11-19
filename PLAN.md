# VRSFov implementation plan

Start with [`plan/00-overview.md`](plan/00-overview.md), then read the files in numerical order.
Implement the milestones in [`plan/07-milestones-and-testing.md`](plan/07-milestones-and-testing.md),
in order.

Platforms: `-DVRSFOV_PLATFORM=pc` builds the Vulkan layer plus the OpenXR API layer and
the out-of-process `vrsfov-gaze` publisher. `-DVRSFOV_PLATFORM=frame` builds the same
Vulkan/common core without OpenXR, with `standalone = 1` as the default and the same
publisher. The publisher is only used by OpenVR games, never by OpenXR applications.
The milestones below were written for the PC target; the Frame target reuses M0-M5,
M7, M10 and the shared tests.

Instructions for implementing agents:
- Use one milestone per task or session. Before you start a milestone, reread the pitfalls checklist in 07.
- Keep logic that doesn't call Vulkan or OpenXR (pattern, classify, gaze projection, render-pass conversion,
  SPIR-V scan, INI) in pure modules with unit tests.
- If the plan conflicts with the Vulkan or OpenXR spec, the spec wins. Record the change in the relevant plan file.

---

# Implementation status (M0–M9 done) and deviations

All milestones of `plan/07-milestones-and-testing.md` are implemented and committed (roughly one
commit per milestone). Everything that can be verified on this machine is; the parts that need a
headset or a real OpenXR runtime are verified against a **mock runtime** instead (see below).

| Milestone | Commit | Verified by |
|---|---|---|
| M0 scaffolding | `M0: scaffolding` | `cmake -B build -GNinja -DVRSFOV_USE_SYSTEM_HEADERS=ON && cmake --build build && ctest`; `nm -D --defined-only build/libvrsfov.so` shows only `vkNegotiateLoaderLayerInterfaceVersion` and `xrNegotiateLoaderApiLayerInterface` |
| M1 passthrough layer | `M1: Vulkan passthrough layer` | `VRSFOV_ENABLE=1 vkcube --c 200` and `vulkaninfo --summary` with `VK_LAYER_KHRONOS_validation`: rc=0, no VUIDs |
| M2 device activation | `M2: device activation` | log: `device ACTIVE: texel 8, rates inner 1x1 middle 2x2 outer 2x2 (supported 4), layered=0 nonTrivialCombiners=1` on RADV; llvmpipe: `inactive (VK_KHR_fragment_shading_rate not available)` |
| M3 pure logic | `M3: pure logic modules` | unit tests (config, pattern, classify, gaze projection, render-pass conversion, SPIR-V scan) |
| M4 dynamic rendering | `M4: dynamic rendering path` | `vk_vrs_test dynamic` and `dynamic-chain` pass with validation; coverage probe: center 0.000, corners 1.000; `nonEyeSize` all-zero |
| M5 render pass path | `M5: render pass path` | `vk_vrs_test rp1 rp2 multiview imageless` pass with validation; `gaze_sweep.sh` (4 gazes x 5 modes) fovea within one texel of the injected gaze |
| M6 OpenXR passthrough + state | `M6-M8: OpenXR half` | `xr_layer_probe` (loader discovers and negotiates the layer), `xr_vk_e2e` (real session/swapchain/slots through a mock runtime: the Vulkan half foveates the registered swapchain image) |
| M7 fixed foveation end-to-end | `M6-M8: OpenXR half` | `xr_vk_e2e`, and the `xr_vk_e2e_no_eye_gaze` variant: fixed foveation in the eye-image centre with a runtime without eye gaze |
| M8 eye tracking | `M6-M8: OpenXR half` | `xr_vk_e2e`: the layer suggests its own gaze binding, merges app suggestions, attaches/syncs the action set, queries the pose and follows the runtime's gaze (0.25, 0.297 measured for a 0.25, 0.30 gaze); `xr_vk_e2e_no_app_gaze_suggest` covers an app that never mentions eye gaze, `xr_vk_e2e_app_owns_gaze_binding` an app that owns the binding path; fallback path as above |
| M9 polish | `M6-M8` + this commit | config hot reload (`xrWaitFrame`, mtime + generation), `[exe:a,b]`/cmdline-token matching, 5-second stats report in `xrEndFrame`, README, run scripts |
| M10 standalone fixed foveation (not in the original plan) | `standalone mode` | `vk_vrs_test_standalone_env`, `vk_vrs_test_standalone_file`, `standalone_hot_reload`: a Vulkan-only process with no `XrInstance` gets the foveated pattern from `standalone = 1` + `eye_size = WxH`; a mismatched size stays neutral; rewriting the config during rendering is picked up |

Current test suite: the PC target runs 18 entries (unit tests, 6 GPU modes, 5 end-to-end
mock-runtime tests, two standalone tests and the hot-reload test) plus the two layer probes; 59 unit
tests cover the pure modules. The Frame target runs 12 entries and 51 unit tests (no OpenXR e2e; includes the platform-default standalone test). The suite is self-contained: it enables the build-dir Vulkan layer with
`VK_INSTANCE_LAYERS`/`VK_LAYER_PATH` and points the OpenXR loader at `XR_API_LAYER_PATH`, so a clean
checkout does not need an installed layer first.

## How the OpenXR half is verified without a headset

`tests/xr_vk_e2e` plays the loader *and* a mock runtime in one process:

1. it creates a Vulkan instance, which makes the Vulkan loader load the layer (whichever copy the
   loader found — the test locates it in `/proc/self/maps` and `dlopen`s the *same file*, which is
   what makes both halves share `SharedState`),
2. it negotiates the API layer (`xrNegotiateLoaderApiLayerInterface`) and creates an instance
   through the layer's `createApiLayerInstance`, with a mock runtime as the "next" layer,
3. it drives a normal OpenXR frame loop (session, swapchain, action sets, `xrWaitFrame`,
   `xrLocateViews`, `xrSyncActions`, `xrEndFrame`) where the mock runtime reports the eye poses,
   the FOVs and the gaze pose,
4. it then renders a Vulkan frame into the registered swapchain image and reads the rate map back.

The program asserts the merge behaviour (extension appended, gaze binding merged into the app's
suggestion for the shared profile, action set attached and synced together with the app's), that our
gaze pose query reaches the runtime, and that the rate map's full-rate region sits on the gaze the
runtime reported. With `VRSFOV_E2E_NO_EYE_GAZE=1` the same test checks the fixed-foveation fallback.

Monado 25.1 is installed as well: with its Simulated HMD the layer is loaded by the OpenXR loader,
negotiates, and reaches instance creation (its `XR_EXT_eye_gaze_interaction` retry path is exercised
because the simulated device has no eyes), but the compute compositor cannot start in this
environment (no DRM connector for its direct-mode window), so no complete `xrgears` session could be
run here. That is the remaining gap versus the plan's "hello_xr on a runtime" criterion.

## Deviations from the plan (and why)

1. **Loader layer discovery (M1/M4 test infrastructure).** The tests are self-contained: they point
   the Vulkan loader at the build's `manifests/vulkan` with `VK_LAYER_PATH` + `VK_INSTANCE_LAYERS`,
   and the OpenXR loader at `manifests/openxr` with `XR_API_LAYER_PATH`. Nothing has to be installed.
   The end-to-end test additionally dlopens the exact library the loader loaded (found in
   `/proc/self/maps`), which also verifies the "one .so, shared globals" assumption of the design.
2. **`VRSFOV_FAKE_GAZE=u,v`** added next to the planned `VRSFOV_FAKE_EYE=WxH`, and
   `VRSFOV_E2E_NO_EYE_GAZE` for the mock runtime: they make the gaze→pattern path (05 §5, 06 §2) and
   the fixed-foveation fallback verifiable without a headset.
3. **GPU test app needs both dynamic rendering spellings.** With a 1.2 instance the core
   `vkCmdBeginRendering` is not available (only `vkCmdBeginRenderingKHR`); calling it tears down with a
   NULL dispatch entry. The test app therefore requests 1.3 by default and resolves
   `vkCmdBeginRendering{,KHR}` through `vkGetDeviceProcAddr` at runtime (`--api-1.2` selects the KHR
   path). The layer itself hooks both names and calls the matching next pointer (07 pitfall 4).
4. **Second, driver-independent probe.** Besides the planned `gl_ShadingRateEXT` code probe the test app
   has `--coverage`, which derives the effective rate from `gl_FragCoord` coherence over 2x2 blocks.
5. **Removed knobs that did not earn their place.** `swapchain_only`, `stale_behavior` and
   `skip_depth_only` are gone: the swapchain registry already wins when it is available and the size
   heuristics are harmless otherwise, stale gaze now always falls back to the fixed centre, and
   depth-only passes never have a colour image to sharpen. `preset` (`lo`/`med`/`hi`) and `gaze_yflip`
   came over from the Steam Frame variant; the defaults are the Valve `lo` geometry.
6. **Rate clamping detail (04 §5 step 5).** "Largest supported rate with `w <= rate.w && h <= rate.h`"
   is implemented as maximum area, ties broken by the first entry, always falling back to 1x1.
7. **Render pass creation fallback (05 §1.2).** After injection the pass is created through
   `vkCreateRenderPass2{,KHR}`; if that fails the original v1 call is retried and the pass is recorded
   as unmodified (same as the skip rules). Skips and conversions both log at WARN, as pitfall 11 asks.
8. **Resources are freed in `vkDestroyDevice`** (`Resources::destroyAll`), not only in
   `vkDestroyFramebuffer`: rate images and pattern buffers must outlive any in-flight command buffer.
9. **OpenXR dispatch data renamed** (`XrInstanceData`/`XrSessionData`/`XrSwapchainData` and
   `findXr*`/`addXr*`/`removeXr*`): the Vulkan half already uses those names, and both live in one
   library.
10. **Eye gaze corner case (03 §4):** if the app itself suggests the same binding path
    (`/user/eyes_ext/input/gaze_ext/pose`), our binding is not appended (the OpenXR spec forbids
    duplicate binding paths). Eye tracking then stays unavailable and fixed foveation is used; this is
    logged once at INFO.
11. **Gaze suggestion merge** happens on the *first* suggestion for that profile only; our own
    suggestion is made when the action set is created (03 §3) and the app's later suggestion for the
    same profile is merged with ours.
12. **Stats logging (02 §3)** is driven by `xrEndFrame` but is also the only place frames are counted;
    it reports every 5 seconds as specified and once immediately.
13. **Manifest `api_version` stays `1.3.280`** as the plan specifies, so the loader warns when an app
    requests API 1.4 ("Layer uses API version 1.3 which is older than the application specified API
    version of 1.4"). Raise the manifest value if the pinned FetchContent headers are ever bumped.
14. **INI values may carry an inline comment**: the parser strips the rest of the line from the first
    unquoted `;` or `#` (`inner_radius = 0.18 ; sharp spot`). The plan only says "comments", and the
    first real config file written by hand showed that without this every annotated key is rejected as
    invalid and silently keeps its default.
15. **The fake-eye path honours `fixed_offset_x`/`fixed_offset_y`** (mapped with a 90 degree FOV
    convention) and the effective config is logged at instance/device creation, so the effect can be
    moved and inspected without a runtime or headset.
16. **Standalone mode (M10, beyond the plan)**: `standalone = 1` + `eye_size = WxH` activates the
    Vulkan half without any `XrInstance` (for OpenVR games and plain Vulkan apps), publishes a
    synthetic snapshot flagged `Snapshot::synthetic` (so the 200 ms staleness rule does not apply
    to a constant synthetic gaze) and relies on the size-based heuristics of 06 §3 C/D. The Vulkan
    half loads the config itself in that case (`loadConfigForProcess`; `[app:...]` sections cannot
    match) and hot reloads it from `vkBeginCommandBuffer`, throttled to one check per second.
    Eye tracking is impossible without OpenXR (fixed gaze + `fixed_offset_x/y` instead).
17. **Loader protocol fix (07 pitfall 1, generalised)**: `vkGetInstanceProcAddr` must return a
    usable `vkCreateInstance` for *any* `instance` argument, not only for `VK_NULL_HANDLE`. Loaders
    (and winevulkan) ask for it with a handle of their own before any instance exists; answering
    `NULL` makes the loader print `loader_create_instance_chain: Failed to find 'vkCreateInstance'`,
    unload the layer and fail instance creation for the whole process — which is what made Proton
    games (VRChat under SteamVR) crash with the layer merely present. Unknown instance/device
    handles are now forwarded through the last resolved next-layer entry point instead of being
    reported as "missing". `tests/vk_layer_probe` guards this.
18. **`VkFramebufferAttachmentImageInfo` for imageless framebuffers** uses the rate-image texel extent
    and layer count (`sw`, `sh`, `L`) exactly as 05 §2 prescribes; validation is clean with a 2-layer
    multiview framebuffer and a 1-layer rate image.
19. **Synthesized snapshots accept every device** (`Snapshot::anyDevice`): in standalone/fake-eye mode
    the snapshot used to be installed per device creation with that device's handle, so in a process
    with several devices (a game's DXVK device and SteamVR's `vrclient` device) only the last one
    could be foveated and every pass on the game's own device was rejected as "other VkDevice" — the
    layer was active and logging, but nothing was foveated. Synthesized snapshots now cover all of
    them.
20. **Passes that are not foveated say why** (`skip-...` notes at debug level: device not active,
    unknown command buffer/device, framebuffer not ours, render pass not modified, resume without a
    rate image) and `vkCreateGraphicsPipelines` reports a per-second summary of injected/skipped
    pipelines with the skip reason, so "nothing happened" can be told apart from "nothing reached the
    layer".
21. **`draw_radii` / `ring_width` (debug overlay, beyond the plan)**: stamps a thin band at the inner
    and/or outer radius in the rate map, so the fovea can be located by eye even where the rates on
    both sides of a radius are equal (`middle_rate`/`outer_rate` both clamp to `2x2` on RADV). The
    band prefers an anisotropic rate (`2x1`/`1x2`, checked against the device's supported set) because
    it differs from both `1x1` and `2x2`; otherwise it degrades to the edge of a zone and the log says
    so. It is a sharpness contour, not colour, which is all a rate map can express.
22. **Late activation (beyond the plan)**: the plan activates the Vulkan half only when an
    `XrInstance` exists at `vkCreateDevice` time, and the no-instance case was documented as "use
    `VRSFOV_FORCE_ACTIVE` or standalone". Real games initialise Vulkan graphics *before*
    OpenXR (VTOL VR: device 2 s before the instance), and that shape foveated nothing at all.
    Now the capability probes run first, our extensions and features are enabled whenever the device
    is capable and the layer is enabled, and the device activates as soon as an instance appears -
    detected from the throttled `vkBeginCommandBuffer` hook that already serves standalone hot
    reload. Two consequences, both logged: render passes, framebuffers and pipelines created before
    activation stay unmodified (dynamic rendering, which DXVK/Unity use for their eye passes, is
    unaffected because it is decided per pass), and `vkGetDeviceProcAddr` must hand out our hooks to
    a device that is only *pending* - apps resolve device entry points once, so anything given out
    while inactive would keep bypassing the layer forever. Plain Vulkan apps still get no injection
    (they simply never activate). `xr_vk_e2e_device_first` (env `VRSFOV_E2E_VK_DEVICE_FIRST=1`) covers
    the order, including that the fovea follows the gaze after activation.
23. **SteamVR-native gaze source (`VRSFOV_OPENVR_GAZE`, beyond the plan)**: the plan's only
    dynamic path is `XR_EXT_eye_gaze_interaction`. On this machine's runtime (SteamVR 2.18.3 + the
    Frame's vrlink driver) that path is inert: the extension is granted, the action set is created and
    attached, `supportsEyeGazeInteraction` is true, and the action never becomes active - for a Proton
    game and for a native hello_xr alike. SteamVR's own eye tracking is exposed to the renderer
    instead (`IVRSystem::GetEyeTrackedFoveationCenter`, `Prop_SupportsXrEyeGazeInteraction_Bool`, both
    public in the OpenVR SDK). The out-of-process `vrsfov-gaze` publisher owns the OpenVR client
    and publishes the per-eye foveation centre over `/dev/shm/vrsfov-gaze` (deviation 29). Read the
    per-eye foveation centre, which is a screen-space point and needs no projection: it replaces the
    per-eye projection and is marked in the stats line as the publisher source. It is opt-in
    (`VRSFOV_OPENVR_GAZE=1`) and only used for OpenVR games. The publisher logs everything it finds
    (`GetEyeTrackedFoveationCenter=ok|no data`) so the outcome is never ambiguous, and no OpenVR
    IPC occurs on the render thread. Only the OpenVR header is a build dependency (system include
    dir or a pinned download); the library is `dlopen`ed, so the layer still works when it is
    absent. `openvrNdcToUv` (NDC, +y up -> eye-image UV, v down) is unit tested.
24. **The SteamVR-native gaze source also covers standalone mode**: an OpenVR-only game never creates
    an `XrInstance`, so only standalone mode activates the Vulkan half - and previously that meant a
    fixed spot in the middle of each eye. The layer now reads the out-of-process publisher's
    seqlocked shared-memory record on the throttled `vkBeginCommandBuffer` call site, applies
    `smoothing`/`gaze_yflip`, and publishes the result into `Snapshot::eyes[]`. Per-eye rather than
    averaged, because the snapshot and `resolveGaze()` already work per eye: per-eye/side-by-side
    targets get each eye's own centre and only a single-target pass falls back to the average that
    `01 §...`/`classify` computes. The publisher samples at 60 Hz and the layer only does bounded
    local reads; no OpenVR call is made in the game process.
25. **OpenXR gaze binding was never suggested unless the app did it.** The action set was created and
    attached, but `xrSuggestInteractionProfileBindings` for
    `/interaction_profiles/ext/eye_gaze_interaction` was only called when the app itself suggested that
    profile. An action without a binding always reports `isActive=false`, which looked exactly like
    "the runtime's gaze action is inert". `createGazeObjects()` now suggests its own binding, remembers
    and merges app suggestions made before or after the action exists, and only gives up when the app
    already owns the binding path (OpenXR forbids a duplicate path).
26. **Self-contained tests and build tree layout.** The Vulkan and OpenXR manifests are generated into
    `build/manifests/vulkan` and `build/manifests/openxr`; GPU tests set
    `VK_LAYER_PATH`/`VK_INSTANCE_LAYERS`, `xr_layer_probe` sets `XR_API_LAYER_PATH`, and the hot-reload
    script takes the binary path from `VRSFOV_TEST_BIN`. No test depends on an installed layer, and
    the Vulkan loader no longer warns about the OpenXR manifest in its path.
27. **OpenVR recommended eye size for auto `eye_size`.** Standalone mode had no equivalent of the
    OpenXR `xrLocateViews` recommended image size, so leaving `eye_size` unset meant "every colour
    target is an eye target". The out-of-process publisher calls
    `IVRSystem::GetRecommendedRenderTargetSize()` after connecting and publishes the result into
    `GazeShm`; the layer copies it into `Snapshot::recommendedW/H` when `eye_size` is `0`/`auto`.
    An explicit `eye_size` always wins and the OpenXR path still uses `xrLocateViews`. This lets
    OpenVR-only titles (X-Plane 11) keep the desktop mirror and HUD passes neutral without knowing
    the per-eye resolution in advance. The query shares the existing `VRSFOV_OPENVR_GAZE=1` opt-in
    and the OpenVR-game gate, so it never creates a SteamVR client for a plain standalone app.
28. **Reuse-only in-process OpenVR worker (superseded).** The first in-process worker called
    `VR_InitInternal` when no `IVRSystem` was available yet. On X-Plane 11 that succeeded, the
    following `VR_GetGenericInterface` still failed with "Not Initialized (109)", and the
    `OpenVrClient` destructor then called `VR_ShutdownInternal`; the cycle repeated every few
    seconds. That competed with the game's own OpenVR client and froze the app. A reuse-only worker
    (load the mapped `libopenvr_api.so`, call only `VR_GetGenericInterface`, never init/shutdown)
    removed the freeze, but still calling `IVRSystem` from a second thread produced glitchy
    overlays. The in-process path was therefore removed in favour of the publisher.
29. **Out-of-process publisher restored; OpenVR games only.** `vrsfov-gaze` is again the only
    process that talks to SteamVR. It creates a non-scene client (`VRSFOV_OPENVR_CLIENT`, default
    2/other), polls `IVRSystem::GetEyeTrackedFoveationCenter` at 60 Hz and
    `GetRecommendedRenderTargetSize()` once per connection, and publishes both over
    `/dev/shm/vrsfov-gaze` (`GazeShm` version 2). The layer never loads `libopenvr_api.so`; it only
    maps the shared-memory record. It writes `/dev/shm/vrsfov-gaze.want` only when **all** of these
    hold: standalone mode is active, no OpenXR loader is mapped, no `XrInstance` exists, and
    an OpenVR client (`vrclient.so` or Wine's `vrclient.dll`) is mapped. The
    OpenXR path no longer falls back to OpenVR, so plain Vulkan and OpenXR applications never touch
    the publisher. `run-pc.sh`/`run-frame.sh` start the publisher once (unless
    `VRSFOV_START_GAZE=0`); it stays disconnected while the want file is stale.
    `eye_tracking = 0` keeps the publisher's recommended size but skips the gaze,
    which covers HMDs/runtimes that report a centre without real eye tracking.
