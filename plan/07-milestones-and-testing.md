# 07 — Milestones, tests, pitfalls

Implement strictly in order. Each milestone must build cleanly with `-Wall -Wextra` and pass
its acceptance checks before starting the next. Commit after each milestone.

## Milestones

**M0 — Scaffolding.** CMake, FetchContent headers, exports.map, manifests, install script,
log + config (+ INI parser) + shared_state, unit-test target (`ctest`).
✔ `cmake -B build && cmake --build build && ctest --test-dir build` passes (config/INI tests).
✔ `nm -D --defined-only build/libvrsfov.so` shows only the two negotiation symbols.

**M1 — Vulkan passthrough layer.** 04 §1–4, §5 without modifications (always inactive), §6.
✔ `VRSFOV_ENABLE=1 vkcube` (and `vulkaninfo --summary`) run; log shows layer loaded.
✔ With `VK_LAYER_KHRONOS_validation` also enabled: no validation errors attributable to us.

**M2 — Device activation.** Full 04 §5 (extensions, features, caps, texel/rate clamp).
Temporary debug env `VRSFOV_FORCE_ACTIVE=1` bypasses the `liveXrInstances` check for tests.
✔ On a VRS GPU (NVIDIA Turing+, AMD RDNA2+, Intel Arc/Xe) log shows active, texel, rates.
✔ On llvmpipe / old GPUs: inactive, app unaffected.

**M3 — Pure logic.** pattern.cpp, classify.cpp, gaze_projection.cpp, renderpass_convert.cpp,
spirv_scan.cpp with all unit tests listed in 03 §5.1, 05 §7, 06.
✔ All unit tests pass.

**M4 — Dynamic rendering path.** Resources (05 §5.2), recordRateUpdate, BeginRendering hook,
pipeline injection (05 §6). Debug env `VRSFOV_FAKE_EYE=WxH` makes the Vulkan half
synthesize a snapshot (session device = any, recommended size WxH, gaze (0.5,0.5), running).
✔ GPU test app (below) in dynamic-rendering mode passes with validation enabled.

**M5 — Render pass path.** 05 §1–3 incl. v1 conversion, imageless framebuffers, neutral image.
✔ GPU test app in renderpass v1, v2, multiview and imageless modes passes with validation.

**M6 — OpenXR passthrough + state.** 03 §1–3, §6, swapchain registry, recommended size.
✔ `hello_xr -g Vulkan2` and `-g Vulkan` on Monado (or any runtime) run with
`VRSFOV_ENABLE=1`; log shows layer order, session device, swapchains, slots.

**M7 — Fixed foveation end-to-end.** Wire snapshot to classification; fixed gaze.
✔ hello_xr: log shows foveated passes/frame > 0; `debug_mode = 1` makes the scene visibly
blocky; `debug_mode = 0` shows sharp center, coarse periphery (inspect in headset or the
runtime's mirror window). Validation-clean.

**M8 — Eye tracking.** 03 §4–5.
✔ On WiVRn + Quest Pro (or Monado with an eye-tracked device / simulated gaze): with
`debug_mode = 2` the blurry spot follows the gaze. Log reports eye tracking active.
✔ Runtime without the extension: graceful fallback to fixed (log INFO).

**M9 — Polish.** Hot reload, per-app/exe sections, stats logging, README (install, usage,
config reference, compatibility table, troubleshooting), uninstall script.

## GPU end-to-end test app (tests/vk_vrs_test)

A headless Vulkan program (no window) that loads the layer via env
(`VRSFOV_ENABLE=1 VRSFOV_FORCE_ACTIVE=1 VRSFOV_FAKE_EYE=1024x1024`):
1. Create instance (enable validation if available) and device. The fragment shader reads
   `gl_ShadingRateEXT` (SPIR-V capability `FragmentShadingRateKHR`), so the test app itself
   enables `VK_KHR_fragment_shading_rate` with no FSR features chained — this exercises the
   layer's "extension already enabled" path. Variant `--chain-features` chains the features
   struct with only `pipelineFragmentShadingRate = TRUE` to exercise the patch-call-restore path.
2. Render a fullscreen triangle into a 1024x1024 `R8_UINT` color target. Fragment shader
   (GLSL, `#extension GL_EXT_fragment_shading_rate : require`) writes `gl_ShadingRateEXT`
   to the output.
3. Read back. Expect: center texel = rate code of inner rate (0), corners = outer (10 or
   clamped), ring in between = middle (5). Exit code 0/1.
4. Modes via argv: `dynamic`, `rp1`, `rp2`, `multiview` (2 layers, viewMask 0b11),
   `imageless`, `nonEyeSize` (512x256 → expect all 0).
Shaders: compile with `glslangValidator`/`glslc` at build time if found; also commit the
generated `.spv` as C arrays (`tests/vk_vrs_test/shaders_spv.h`) so the test builds without them.

## Real-world validation matrix (document results in README)

| App | Path | Notes |
|---|---|---|
| hello_xr (OpenXR-SDK-Source) | render pass v1, separate eye images (array) | baseline |
| Godot 4 OpenXR | multiview render pass | |
| Blender VR / other native | | |
| Proton game via wineopenxr + DXVK | dynamic rendering, size heuristics (C/D) | main use case |
| vkd3d-proton (D3D12) title | dynamic rendering; app may already use VRS → untouched | |

Runtimes: Monado, WiVRn (eye tracking with Quest Pro), SteamVR Linux, ALVR.
Measure GPU frame time before/after (runtime perf overlay or MangoHud).

## Pitfalls checklist (re-read before each milestone)

1. Return NULL from GDPA for hooked functions whose next pointer is NULL.
2. Device-level functions must also be returned from GIPA.
3. Never call loader exports (`vkGetInstanceProcAddr` from libvulkan) — use the chain.
4. KHR vs core aliases: `vkCmdBeginRenderPass2KHR` hook must call next's `...2KHR` pointer.
5. Keep pNext structs we allocate alive until the next call returns (stack/vector storage).
6. Patch-call-restore on app structs must restore even on error paths.
7. Barriers: always `oldLayout = UNDEFINED` before overwriting S; final layout FSR_OPTIMAL.
8. The FSR attachment must not exceed limits: texel size power of two within
   `[minTexel, maxTexel]`, `maxFragmentShadingRateAttachmentTexelSizeAspectRatio` (square OK).
9. S extent must be ≥ `ceil(fbExtent / t)`; neutral image may be larger (allowed).
10. Thread safety: vkCmd* hooks run concurrently on many threads — read maps under shared lock,
    no global locks around next-layer calls.
11. Render pass compatibility: any skip rule in 05 §1.2 can break pipeline/render-pass
    compatibility; log WARN whenever a render pass is skipped.
12. Snapshot is from the previous frame for slots — acceptable; do not block waiting for it.
13. `XrGraphicsBindingVulkan2KHR` has the same `type` value as `XrGraphicsBindingVulkanKHR`.
14. OpenXR layer must forward `xrGetInstanceProcAddr` for unknown names and handle
    `XR_NULL_HANDLE` instance for the pre-instance functions.
15. Steam pressure-vessel: verify layers are visible inside the container (`--system` install).
16. Never log per-draw/per-pass at INFO; use LOG_ONCE / stats.
17. Out-of-scope features (shader objects, quad views) must degrade to "no foveation", never crash.
