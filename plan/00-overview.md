# VRSFov — VRS-based (dynamic) foveated rendering layer for Linux

Goal: an OpenXRToolkit-style foveated rendering layer for Linux VR apps that render with
**Vulkan**. The PC target covers native OpenXR apps and Proton games (wineopenxr +
DXVK/vkd3d-proton); the Steam Frame target covers standalone Vulkan/OpenVR titles. Use
`VK_KHR_fragment_shading_rate` (attachment-based VRS) with an image built from eye gaze
(`XR_EXT_eye_gaze_interaction`) or from a fixed center when eye tracking is unavailable.

Read the files in order:

| File | Contents |
|---|---|
| 00-overview.md | Architecture, key decisions, glossary |
| 01-build-and-layout.md | Repo layout, CMake, manifests, install |
| 02-shared-state-and-config.md | Process-wide shared state, config file, logging |
| 03-openxr-layer.md | OpenXR API layer: interception, swapchains, eye gaze, projection math |
| 04-vulkan-layer-core.md | Vulkan layer plumbing: dispatch, instance/device creation |
| 05-vrs-injection.md | Render pass / framebuffer / pipeline / dynamic rendering injection, sync |
| 06-pattern-and-classification.md | Shading-rate pattern, gaze→copy offsets, eye-target heuristics |
| 07-milestones-and-testing.md | Ordered work items with acceptance criteria, tests, pitfalls |

## 1. Architecture

```
            App (native Vulkan or Proton/DXVK)
             |                         |
     OpenXR calls                  Vulkan calls
             v                         v
  +---------------------+    +-------------------------+
  | OpenXR API layer    |    | Vulkan implicit layer   |
  | (XR_APILAYER_...)   |    | (VK_LAYER_VRSFOV)   |
  +----------+----------+    +-----------+-------------+
             |   writes        reads     |
             +----> SharedState <--------+      (same .so => same globals)
             v                         v
       OpenXR runtime             Vulkan driver
   (Monado/WiVRn/SteamVR/ALVR)
```

**One shared library** `libvrsfov.so`. On the PC target it is *both* an OpenXR API layer and a
Vulkan layer; both manifests point to the same absolute path, so `dlopen` returns the same
handle and both halves share globals (`SharedState`). On the Steam Frame target only the
Vulkan half is built, and the gaze comes from the out-of-process `vrsfov-gaze` publisher over
`/dev/shm/vrsfov-gaze`. Log the address of the SharedState singleton from each half at init so
this can be verified.

- **OpenXR half** knows: the session's VkDevice, recommended eye sizes, swapchain VkImages,
  which swapchain/rect/array-slice is which eye (from `xrEndFrame`), per-eye FOV, and
  per-eye gaze point in UV space (eye tracking or fixed). Publishes an immutable
  `Snapshot` every frame.
- **Vulkan half** enables `VK_KHR_fragment_shading_rate` at device creation, adds a
  fragment-shading-rate (FSR) attachment to render passes / dynamic rendering, sets pipeline
  combiner ops so the attachment is honored, and right before each render pass decides
  (via the snapshot) whether that pass is an eye render, then fills the FSR image with a
  foveated pattern using **transfer commands only** (`vkCmdCopyBufferToImage` from a static,
  precomputed, host-visible pattern buffer at a gaze-dependent offset).

## 2. Key design decisions (do not change without reason)

1. **Transfer-only pattern updates.** No compute shaders: binding a compute pipeline would
   clobber app-bound compute state. Pattern = static buffer of `(2*ew) x (2*eh)` texels with
   the fovea at its center; moving the gaze = changing the copy source offset. A static
   buffer means no CPU/GPU hazards; written once at creation (host-coherent memory, writes
   before `vkQueueSubmit` are implicitly visible).
2. **Modify all render passes** of an eligible device (append the FSR attachment to every
   subpass) so pipelines and render passes stay compatible automatically. Framebuffers that
   are not eye-candidates point at a shared *neutral* (all 1x1) image.
3. **Dynamic rendering:** attach the FSR image only when foveating; otherwise do nothing
   (zero overhead). All dynamic-rendering pipelines get
   `VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR`.
4. **Pipelines** get `VkPipelineFragmentShadingRateStateCreateInfoKHR{ {1,1}, {KEEP, MAX} }`
   (REPLACE instead of MAX if `fragmentShadingRateNonTrivialCombinerOps` is false). Without
   it the attachment is ignored (spec default is KEEP,KEEP).
5. **Opt-in via env var** `VRSFOV_ENABLE=1` for both manifests. The Vulkan half is also
   inert for a device unless an `XrInstance` of the OpenXR half exists at `vkCreateDevice`
   (OpenXR apps always create the XrInstance before the VkDevice).
6. **Do not modify app memory** except where unavoidable (marked spots use a
   "patch → call next → restore" pattern on `const_cast`ed structs).
7. Export only `xrNegotiateLoaderApiLayerInterface` and `vkNegotiateLoaderLayerInterfaceVersion`
   (linker version script). All other entry points are internal (`vrsfov_*`) and returned
   through the GetProcAddr functions. Link with `-Wl,-Bsymbolic`.
8. **Never link** `libvulkan` or `libopenxr_loader`. Every call goes through dispatch tables.

## 3. Glossary

- **t**: FSR texel size in pixels (square; typically 8 or 16).
- **S image**: R8_UINT FSR attachment image, `ceil(W/t) x ceil(H/t)` texels, 1 or N layers.
- **Pattern buffer P**: host-visible buffer of `2*ew x 2*eh` bytes, fovea center at `(ew, eh)`.
- **Rate value**: `(log2(fw) << 2) | log2(fh)`. 1x1=0, 1x2=1, 2x1=4, 2x2=5, 2x4=6, 4x2=9, 4x4=10.
- **Eye region**: `{layer, rect in pixels, gaze u, v}`; a **foveation plan** is a list of regions.
- **Eye candidate**: a render target whose size/aspect matches an eye render (see 06).
- **Neutral**: all texels = 0 (1x1, full rate).

## 4. Scope

In scope: PRIMARY_STEREO view configuration, Vulkan graphics binding (`XR_KHR_vulkan_enable`
and `enable2`), render passes v1/v2, dynamic rendering (core + KHR), multiview, side-by-side,
separate per-eye targets, fixed + eye-tracked foveation, INI config with per-app sections
and hot reload, logging, debug visualization modes.

Out of scope (v1): OpenGL apps, quad views (`XR_VARJO_quad_views`), `VK_EXT_shader_object`,
apps already using VRS or fragment density map (detected and left untouched), in-HMD menu,
render-pass suspend/resume across command buffers (best effort only, see 05).
