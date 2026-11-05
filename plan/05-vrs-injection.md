# 05 — VRS injection (render passes, framebuffers, pipelines, dynamic rendering, sync)

Only applies when `DeviceData::active`. Constants:
- `VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR` (= FSR_OPTIMAL below)
- `VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR`, `VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR`
- `VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR`
- `VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR` (0x00200000)

## 1. Render passes

### 1.1 vkCreateRenderPass (v1)
Convert to `VkRenderPassCreateInfo2` with `renderpass_convert.cpp` (pure, owns storage in a
`struct ConvertedRP { std::vector<VkAttachmentDescription2>; std::vector<VkSubpassDescription2>;
std::vector<std::vector<VkAttachmentReference2>> refs; std::vector<VkSubpassDependency2>;
std::vector<uint32_t> correlated; ... }`):
- Attachments: copy all fields; `sType = ..._2`.
- Subpasses: copy; `viewMask = multiview ? pViewMasks[i] : 0`. For every reference build
  `VkAttachmentReference2{attachment, layout, aspectMask}`; aspectMask = 0 except for input
  attachments: from `VkRenderPassInputAttachmentAspectCreateInfo` if it lists
  (subpass, inputAttachmentIndex), else derived from the attachment format
  (color → COLOR; depth formats → DEPTH; stencil formats → STENCIL; combined → both).
  `VK_ATTACHMENT_UNUSED` refs stay unused.
- Dependencies: copy; `viewOffset = multiview && i < dependencyCount ? pViewOffsets[i] : 0`.
- `pCorrelatedViewMasks` from multiview info.
- pNext of v1 info: allowed = {`VkRenderPassMultiviewCreateInfo`,
  `VkRenderPassInputAttachmentAspectCreateInfo`} (both consumed). Anything else
  (e.g. `VkRenderPassFragmentDensityMapCreateInfoEXT`) → do not modify; call next v1 as is.
Then go to §1.2 with the converted info and create via `CreateRenderPass2KHR`.

### 1.2 vkCreateRenderPass2(KHR) — injection
Skip (call next unchanged, record `modified=false`) when: top-level pNext contains
`VkRenderPassFragmentDensityMapCreateInfoEXT`, or any subpass pNext contains
`VkFragmentShadingRateAttachmentInfoKHR` or `VkMultisampledRenderToSingleSampledInfoEXT`.
Otherwise:
- `fsrIndex = attachmentCount`; append attachment
  `{format R8_UINT, samples 1, loadOp LOAD, storeOp DONT_CARE, stencil DONT_CARE x2,
    initialLayout FSR_OPTIMAL, finalLayout FSR_OPTIMAL}`.
- For each subpass, copy it and prepend
  `VkFragmentShadingRateAttachmentInfoKHR{ pFragmentShadingRateAttachment = &ref{fsrIndex,
  FSR_OPTIMAL, 0}, shadingRateAttachmentTexelSize = {t,t} }` to its pNext.
- Record `RenderPassInfo { modified, origAttachmentCount, fsrIndex, viewMaskUnion,
  hasColor (any subpass colorAttachmentCount > 0), multiview (viewMaskUnion != 0) }`.
- `vkDestroyRenderPass`: erase.

Why every render pass: pipelines created against render pass A must be compatible with
render pass B used at draw time; FSR attachment presence affects compatibility, so it must be
uniform. Consequence: skip rules above must stay rare.

## 2. Framebuffers

`vkCreateFramebuffer`: if renderPass not `modified` → passthrough. Else:
- Decide **candidate** using `classify::isEyeCandidateSize(width, height, snapshot)` (06 §3)
  and `hasColor`. If snapshot has no recommended size yet → candidate.
- Candidate: create a dedicated S image (`resources.createRateImage(ceil(w/t), ceil(h/t), L)`)
  where `L = (rp.multiview && caps.layeredAttachments) ? (msb(viewMaskUnion)+1) : 1`.
  Non-candidate: use the device's neutral image (resized lazily: if the current neutral image
  is smaller than `ceil(w/t) x ceil(h/t)`, create a bigger one; keep old ones alive until device
  destroy). Neutral image always has 1 layer (allowed by spec for any framebuffer).
- Non-imageless: new attachment array = app's + our view; call next.
- Imageless (`VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT`): find `VkFramebufferAttachmentsCreateInfo`
  in pNext, patch-call-restore its `attachmentImageInfoCount/pAttachmentImageInfos` with a copy
  + `VkFramebufferAttachmentImageInfo{usage = FSR attachment | TRANSFER_DST, width = sw,
  height = sh, layerCount = L, viewFormatCount = 1, pViewFormats = &R8_UINT}`.
- Record `FramebufferInfo { renderPassInfo copy, width, height, layers, imageless,
  std::vector<VkImageView> appViews, RateImage* image, bool neutral }`.
- `vkDestroyFramebuffer`: call next, then destroy the dedicated S image **deferred**: push to
  `pendingDestroy` with the current `frameIndex`; actually free when `frameIndex >= +4`
  (checked in vkCreate*/vkCmdBegin* hooks) or at device destroy. (The app may destroy a
  framebuffer only after GPU completion, so immediate destroy is also legal — deferred is
  just defensive. Immediate is acceptable for v1.)

## 3. vkCmdBeginRenderPass / vkCmdBeginRenderPass2(KHR)

```
fb = lookup(pRenderPassBegin->framebuffer); if !fb || !fb->rp.modified -> passthrough
snap = SharedState::snapshot()
if fb->neutral:
    ensureNeutralInitialized(cmd)   // once per command buffer, §5.3
else:
    views = fb->imageless ? from VkRenderPassAttachmentBeginInfo : fb->appViews
    plan = classify::planForTarget(targetDesc{fb->width, fb->height, layers, multiview views,
                                              color images (via view map), hasColor}, snap, device)
    recordRateUpdate(cmd, fb->image, plan)    // §5.1/5.2: clear to neutral if plan empty
if imageless: patch-call-restore VkRenderPassAttachmentBeginInfo with views + our view
call next
```
Never record anything when the render pass begins *inside* a secondary command buffer
recorded with RENDER_PASS_CONTINUE (not possible for Begin anyway).

## 4. Dynamic rendering: vkCmdBeginRendering(KHR)

```
info = pRenderingInfo
if pNext has VkRenderingFragmentShadingRateAttachmentInfoKHR or
   VkRenderingFragmentDensityMapAttachmentInfoEXT -> passthrough
if info->flags & VK_RENDERING_RESUMING_BIT:
    reuse lastAttachedView for this command buffer (CmdState) if any, without recording
    transfers (no commands allowed between suspend/resume); else passthrough. -> call next
W = renderArea.offset.x + extent.width, H likewise
plan = classify::planForTarget(... colorAttachments' imageViews, viewMask, layerCount ...)
if plan empty -> passthrough (no attachment => full rate)
img = resources.getDynamicRateImage(ceil(W/t), ceil(H/t), L)   // cache keyed by (w,h,L); L as §2
recordRateUpdate(cmd, img, plan)
prepend VkRenderingFragmentShadingRateAttachmentInfoKHR{ imageView = img.view,
   imageLayout = FSR_OPTIMAL, shadingRateAttachmentTexelSize = {t,t} } to a copy of info
if flags & SUSPENDING: CmdState.lastAttachedView = img.view
call next with the copy
```
Layer count rule (spec VUIDs): with `viewMask != 0` the FSR view's layerCount must be 1 or
> msb(viewMask); without multiview it must be 1 or >= `layerCount`. Use 1 unless multiview
and `layeredAttachments`.

Sharing a cached image across several passes in one or many command buffers on the same queue is
correct because each update is fenced by barriers (§5.1).

## 5. Recording the update (vk_commands.cpp / vk_resources.cpp)

### 5.1 recordRateUpdate(cmd, RateImage& img, const FoveationPlan& plan)
```
barrier1: image img, all layers, oldLayout UNDEFINED (discard), newLayout TRANSFER_DST_OPTIMAL,
          srcStage FRAGMENT_SHADING_RATE_ATTACHMENT_BIT, srcAccess 0,
          dstStage TRANSFER, dstAccess TRANSFER_WRITE
if plan.regions empty or they don't cover every layer fully:
    vkCmdClearColorImage(img, TRANSFER_DST, uint32 {0,0,0,0}, all layers)
    if plan.regions not empty: barrier TRANSFER_WRITE -> TRANSFER_WRITE (same layout)
for each region r: append VkBufferImageCopy (06 §2) — grouped by pattern buffer;
    vkCmdCopyBufferToImage(patternBuffer, img, TRANSFER_DST_OPTIMAL, regions)
barrier2: TRANSFER_DST -> FSR_OPTIMAL, srcStage TRANSFER/TRANSFER_WRITE,
          dstStage FRAGMENT_SHADING_RATE_ATTACHMENT_BIT / FSR_ATTACHMENT_READ
```
Use `vkCmdPipelineBarrier` (works on 1.0 + extensions; do not require sync2).
All of this is outside any render pass — valid because it is recorded right before Begin.

### 5.2 Resources
- `RateImage { VkImage, VkDeviceMemory, VkImageView, uint32 w, h, layers }`:
  `VkImageCreateInfo{2D, R8_UINT, {w,h,1}, mip 1, arrayLayers L, samples 1, OPTIMAL,
  usage FSR_ATTACHMENT | TRANSFER_DST, EXCLUSIVE, initialLayout UNDEFINED}`, device-local memory
  (one allocation per image; fine — few images). View type `2D` if L == 1 else `2D_ARRAY`.
- `PatternBuffer { VkBuffer, VkDeviceMemory, void* mapped, uint32 ew, eh; uint64 configGen; Rates }`:
  usage TRANSFER_SRC, HOST_VISIBLE|HOST_COHERENT, persistently mapped, filled once (06 §1).
  Cache keyed by `(ew, eh, configGeneration, debugMode)`.
- All resource maps guarded by a `std::mutex` in DeviceData (vkCmd* hooks can be multi-threaded).

### 5.3 ensureNeutralInitialized(cmd)
Per command buffer, the first time a neutral-framebuffer render pass begins: barrier
(UNDEFINED→TRANSFER_DST), clear to 0, barrier (→FSR_OPTIMAL) on the *current* neutral image(s);
set `CmdState.neutralInitialized`. Re-clearing in each command buffer is cheap and avoids
needing a queue submit of our own.

## 6. Pipelines (vk_pipeline.cpp)

`vkCreateGraphicsPipelines`: copy the create-info array. For each element i:
1. Eligible if `renderPass == VK_NULL_HANDLE` (dynamic rendering) or renderPass is `modified`.
2. Skip if pDynamicState contains `VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR` or pNext
   contains `VkPipelineFragmentShadingRateStateCreateInfoKHR` (app manages VRS).
3. Safety: scan the fragment stage SPIR-V (`spirv_scan`, §7) and the multisample state. Use
   combiner `KEEP` (i.e. disable our VRS for this pipeline) when:
   - shader writes FragDepth/FragStencilRefEXT and `!caps.withDepthStencilWrites`,
   - shader writes SampleMask and `!caps.withShaderSampleMask`,
   - `pMultisampleState->pSampleMask` non-null with a non-all-ones mask and `!caps.withSampleMask`,
   - shader declares SampleRateShading capability / `sampleShadingEnable` (VRS + per-sample is pointless).
   If the stage uses `VkShaderModule` created earlier, keep a `module → ScanResult` map by
   hooking `vkCreateShaderModule`/`vkDestroyShaderModule`; also handle inline
   `VkShaderModuleCreateInfo` in the stage pNext. If unknown (e.g. pipeline library without
   fragment stage) → assume safe.
4. Prepend `VkPipelineFragmentShadingRateStateCreateInfoKHR{ fragmentSize {1,1}, combinerOps
   { KEEP, caps.nonTrivialCombiners ? MAX : REPLACE } }` (or `{KEEP, KEEP}` when unsafe).
5. If `renderPass == VK_NULL_HANDLE`: add
   `VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR` to `flags`; if pNext
   has `VkPipelineCreateFlags2CreateInfoKHR`, patch-call-restore its `flags` instead (same bit
   value `VK_PIPELINE_CREATE_2_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR`). Apply to
   pipeline libraries too so that linked pipelines are consistent.
6. Call next with the copied array. Store the per-element structs in a `std::vector` sized
   up-front (pointers must stay stable).

## 7. spirv_scan (pure, unit tested)

Input: `const uint32_t* code, size_t words`. Verify magic `0x07230203`; iterate instructions
from word 5 (`wordCount = w >> 16`, `opcode = w & 0xFFFF`, stop on wordCount 0):
- `OpCapability (17)`: capability `SampleRateShading (35)` → `sampleRate = true`.
- `OpDecorate (71)` with decoration `BuiltIn (11)`: value `FragDepth (22)` → depth,
  `SampleMask (20)` → sampleMask (only matters if it is an Output variable; treat any as true),
  `FragStencilRefEXT (5014)` → stencil.
Return `{writesDepthStencil, writesSampleMask, sampleRate}`.
