# 04 — Vulkan layer core

Reference: Vulkan loader "LoaderLayerInterface.md" and `vulkan/vk_layer.h`.

## 1. Negotiation

```cpp
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* p) {
  if (p->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) return VK_ERROR_INITIALIZATION_FAILED;
  if (p->loaderLayerInterfaceVersion > 2) p->loaderLayerInterfaceVersion = 2;
  p->pfnGetInstanceProcAddr = vrsfov_vkGetInstanceProcAddr;
  p->pfnGetDeviceProcAddr = vrsfov_vkGetDeviceProcAddr;
  p->pfnGetPhysicalDeviceProcAddr = nullptr;
  return VK_SUCCESS;
}
```

## 2. Dispatch & data maps (vk_dispatch.h)

- Dispatch key: `inline void* key(const void* h) { return *(void**)h; }` — valid for
  VkInstance, VkPhysicalDevice (shares instance key), VkDevice, VkQueue, VkCommandBuffer
  (share device key).
- `InstanceData { VkInstance; uint32_t apiVersion; InstanceDispatch d; }` in
  `std::unordered_map<void*, std::unique_ptr<InstanceData>>`.
- `DeviceData { VkDevice; VkPhysicalDevice; InstanceData*; DeviceDispatch d; bool active;
  Caps caps; Resources res; maps for render passes, framebuffers, image views; std::shared_mutex lock; }`.
- Global maps guarded by one `std::mutex` (lookup cost is negligible vs. driver calls);
  per-device object maps guarded by `DeviceData::lock` (shared for reads in vkCmd* hooks).

InstanceDispatch (from next GIPA): `DestroyInstance, GetInstanceProcAddr,
EnumerateDeviceExtensionProperties, GetPhysicalDeviceProperties, GetPhysicalDeviceProperties2(KHR),
GetPhysicalDeviceFeatures2(KHR), GetPhysicalDeviceMemoryProperties,
GetPhysicalDeviceFormatProperties, GetPhysicalDeviceFragmentShadingRatesKHR`.
(Use the KHR alias of *2 functions if instance apiVersion < 1.1.)

DeviceDispatch (from next GDPA): `GetDeviceProcAddr, DestroyDevice, CreateImage, DestroyImage,
CreateImageView, DestroyImageView, CreateBuffer, DestroyBuffer, AllocateMemory, FreeMemory,
BindImageMemory, BindBufferMemory, MapMemory, GetImageMemoryRequirements,
GetBufferMemoryRequirements, CreateRenderPass, CreateRenderPass2KHR, DestroyRenderPass,
CreateFramebuffer, DestroyFramebuffer, CreateGraphicsPipelines, BeginCommandBuffer,
CmdBeginRenderPass, CmdBeginRenderPass2 / CmdBeginRenderPass2KHR, CmdBeginRendering /
CmdBeginRenderingKHR, CmdPipelineBarrier, CmdCopyBufferToImage, CmdClearColorImage`.

## 3. GetProcAddr

- `vrsfov_vkGetInstanceProcAddr(inst, name)`: return our hook for `vkGetInstanceProcAddr,
  vkCreateInstance, vkDestroyInstance, vkCreateDevice, vkGetDeviceProcAddr` and for all device
  hooks (§3 last bullet list). Otherwise forward to the next GIPA of `inst` (looked up via
  InstanceData). If `inst == NULL` and the name is not one of ours, return NULL.
- `vrsfov_vkGetDeviceProcAddr(dev, name)`: look up DeviceData. If `!active` → forward to next.
  If name is hooked: first `next = d.GetDeviceProcAddr(dev, name)`; if `next == NULL` return
  NULL (extension not enabled); else return our hook. Otherwise forward.
- Hooked device functions: `vkDestroyDevice, vkCreateRenderPass, vkCreateRenderPass2,
  vkCreateRenderPass2KHR, vkDestroyRenderPass, vkCreateFramebuffer, vkDestroyFramebuffer,
  vkCreateImageView, vkDestroyImageView, vkCreateGraphicsPipelines, vkBeginCommandBuffer,
  vkCmdBeginRenderPass, vkCmdBeginRenderPass2, vkCmdBeginRenderPass2KHR,
  vkCmdBeginRendering, vkCmdBeginRenderingKHR, vkCreateShaderModule, vkDestroyShaderModule,
  vkFreeCommandBuffers`.
  Each hook calls the *matching* next pointer (core vs KHR alias resolved per name).

## 4. vkCreateInstance

1. Find `VkLayerInstanceCreateInfo` in `pCreateInfo->pNext` with
   `sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && function == VK_LAYER_LINK_INFO`.
   Save `nextGipa = info->u.pLayerInfo->pfnNextGetInstanceProcAddr`; advance
   `info->u.pLayerInfo = info->u.pLayerInfo->pNext` (this mutation is the documented protocol).
2. `apiVersion = pApplicationInfo ? pApplicationInfo->apiVersion : VK_API_VERSION_1_0`
   (0 means 1.0).
3. If apiVersion < 1.1 and `VK_KHR_get_physical_device_properties2` is not enabled and is
   available (`nextGipa(NULL,"vkEnumerateInstanceExtensionProperties")`), append it.
4. Call next `vkCreateInstance`; build InstanceData.

## 5. vkCreateDevice (vk_device.cpp)

1. Get link info (`VkLayerDeviceCreateInfo`, `VK_LAYER_LINK_INFO`): `nextGipa`, `nextGdpa`;
   advance chain.
2. Decide `active`:
   - `config.enabled` and `SharedState::liveXrInstances > 0`.
   - Device exposes `VK_KHR_fragment_shading_rate` and `VK_KHR_create_renderpass2` (or 1.2 device).
   - Query features via `GetPhysicalDeviceFeatures2` with
     `VkPhysicalDeviceFragmentShadingRateFeaturesKHR`: need `pipelineFragmentShadingRate` and
     `attachmentFragmentShadingRate`.
   - App's chain does NOT enable `VkPhysicalDeviceShadingRateImageFeaturesNV::shadingRateImage`
     or `VkPhysicalDeviceFragmentDensityMapFeaturesEXT::fragmentDensityMap` (mutually exclusive
     with FSR). If it does → inactive.
   - Format check: `R8_UINT` optimal tiling features include
     `VK_FORMAT_FEATURE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR`.
   If inactive: call next with unmodified create info; store DeviceData{active=false}.
3. Build modified create info (copy top-level struct):
   - Extensions: app list + missing of `VK_KHR_fragment_shading_rate`,
     `VK_KHR_create_renderpass2`; if instance apiVersion < 1.1 also `VK_KHR_multiview`,
     `VK_KHR_maintenance2` (deps of create_renderpass2).
   - Features: if the chain already has `VkPhysicalDeviceFragmentShadingRateFeaturesKHR`,
     patch-call-restore its `pipelineFragmentShadingRate`/`attachmentFragmentShadingRate` to
     TRUE (keep `primitiveFragmentShadingRate` as app set). Else prepend our own struct
     (`ours.pNext = ci.pNext; ci.pNext = &ours`).
4. Call next `vkCreateDevice`. If it fails, retry once with the original create info and mark
   inactive (log WARN).
5. Fill Caps:
   ```cpp
   struct Caps {
     VkExtent2D minTexel, maxTexel; uint32_t maxTexelAspect;   // from FSR properties
     bool layeredAttachments;            // layeredShadingRateAttachments
     bool nonTrivialCombiners;           // fragmentShadingRateNonTrivialCombinerOps
     bool withDepthStencilWrites, withShaderSampleMask, withSampleMask; // safety (05 §6)
     std::vector<VkExtent2D> supportedRates;   // vkGetPhysicalDeviceFragmentShadingRatesKHR
     uint32_t texel;                     // chosen texel size t
     bool dynamicRenderingAvailable;     // dynamic rendering enabled by app (1.3 feature or KHR ext)
     uint32_t memoryTypeDeviceLocal, memoryTypeHostVisibleCoherent;
   };
   ```
   Texel choice: `t = config.texelSize ? config.texelSize : 16`, clamp into
   `[max(minTexel.w,minTexel.h), min(maxTexel.w,maxTexel.h)]`, round down to a power of two.
   Use square texels only.
   Rate clamping: map each configured rate to the largest supported rate with
   `w <= rate.w && h <= rate.h` (always at least 1x1).
6. Log device name, active state, texel size, effective rates.

`vkDestroyDevice`: destroy all our resources (images, views, buffers, memory, neutral image)
before calling next; erase DeviceData.

## 6. Object tracking

- `vkCreateImageView`: call next; on success store `view → {image, viewType,
  baseArrayLayer, layerCount, format}`. `vkDestroyImageView`: erase, call next.
- Images: no tracking needed (swapchain images are known from the snapshot by VkImage handle).
- `vkBeginCommandBuffer`: call next; reset per-command-buffer state
  `CmdState { bool neutralInitialized = false; }` keyed by VkCommandBuffer (map in DeviceData).
  Remove entries in `vkFreeCommandBuffers`/`vkDestroyCommandPool`? Simpler: keep map keyed by
  handle and overwrite on each Begin; stale entries are harmless (bounded by live handles;
  also hook `vkFreeCommandBuffers` to erase if you want tidiness).
