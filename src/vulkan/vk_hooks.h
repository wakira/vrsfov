// All Vulkan hooks implemented by the layer, plus the name -> function tables
// used by vrsfov_vkGetInstanceProcAddr / vrsfov_vkGetDeviceProcAddr.
#pragma once

#include <vulkan/vulkan.h>

#include "vulkan/vk_dispatch.h"

namespace vrsfov {

struct NamedHook {
  const char* name;
  PFN_vkVoidFunction fn;
};

// GetProcAddr entry points handed to the loader.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vrsfov_vkGetInstanceProcAddr(VkInstance instance,
                                                                    const char* name);
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vrsfov_vkGetDeviceProcAddr(VkDevice device,
                                                                  const char* name);

// Returns our hook for a name, or nullptr when the name is not hooked.
PFN_vkVoidFunction hookedInstanceFn(const char* name);  // instance + device hooks, for GIPA
PFN_vkVoidFunction hookedDeviceFn(const char* name);

// ---- instance level (vk_layer.cpp) ----
VKAPI_ATTR VkResult VKAPI_CALL vrsfov_vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                      const VkAllocationCallbacks* pAllocator,
                                                      VkInstance* pInstance);
VKAPI_ATTR void VKAPI_CALL vrsfov_vkDestroyInstance(VkInstance instance,
                                                   const VkAllocationCallbacks* pAllocator);
// Hot reload for standalone mode (fixed foveation without OpenXR), called from
// vkBeginCommandBuffer. Guards itself with a one second throttle.
void vrsfovMaybeReloadStandaloneConfig(DeviceData& data);
// Late activation for a device created before the XrInstance (same call site and
// throttle). No-op unless the device is waiting for an instance.
void vrsfovMaybeActivate(DeviceData& data);
// Standalone mode only: read the out-of-process publisher and publish its gaze
// and recommended size into the shared snapshot. Implemented in
// src/gaze/standalone_gaze.cpp; no-op unless this is an OpenVR game with
// VRSFOV_OPENVR_GAZE enabled.
void updateStandaloneGaze(DeviceData& data);
void vrsfovMaybeUpdateStandaloneGaze(DeviceData& data);
// Diagnostics: log the cumulative pipeline-injection counts at process exit.
void logPipelineStats();

// ---- device level (vk_device.cpp) ----
VKAPI_ATTR VkResult VKAPI_CALL vrsfov_vkCreateDevice(VkPhysicalDevice physicalDevice,
                                                    const VkDeviceCreateInfo* pCreateInfo,
                                                    const VkAllocationCallbacks* pAllocator,
                                                    VkDevice* pDevice);
VKAPI_ATTR void VKAPI_CALL vrsfov_vkDestroyDevice(VkDevice device,
                                                 const VkAllocationCallbacks* pAllocator);

// ---- image views (vk_framebuffer.cpp) ----
VKAPI_ATTR VkResult VKAPI_CALL vrsfov_vkCreateImageView(VkDevice device,
                                                       const VkImageViewCreateInfo* pCreateInfo,
                                                       const VkAllocationCallbacks* pAllocator,
                                                       VkImageView* pView);
VKAPI_ATTR void VKAPI_CALL vrsfov_vkDestroyImageView(VkDevice device, VkImageView imageView,
                                                    const VkAllocationCallbacks* pAllocator);
VKAPI_ATTR VkResult VKAPI_CALL vrsfov_vkCreateFramebuffer(
    VkDevice device, const VkFramebufferCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkFramebuffer* pFramebuffer);
VKAPI_ATTR void VKAPI_CALL vrsfov_vkDestroyFramebuffer(VkDevice device, VkFramebuffer framebuffer,
                                                      const VkAllocationCallbacks* pAllocator);

// ---- render passes (vk_renderpass.cpp) ----
VKAPI_ATTR VkResult VKAPI_CALL vrsfov_vkCreateRenderPass(VkDevice device,
                                                        const VkRenderPassCreateInfo* pCreateInfo,
                                                        const VkAllocationCallbacks* pAllocator,
                                                        VkRenderPass* pRenderPass);
VKAPI_ATTR VkResult VKAPI_CALL vrsfov_vkCreateRenderPass2(
    VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass);
VKAPI_ATTR VkResult VKAPI_CALL vrsfov_vkCreateRenderPass2KHR(
    VkDevice device, const VkRenderPassCreateInfo2* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkRenderPass* pRenderPass);
VKAPI_ATTR void VKAPI_CALL vrsfov_vkDestroyRenderPass(VkDevice device, VkRenderPass renderPass,
                                                     const VkAllocationCallbacks* pAllocator);

// ---- pipelines (vk_pipeline.cpp) ----
VKAPI_ATTR VkResult VKAPI_CALL vrsfov_vkCreateShaderModule(
    VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator, VkShaderModule* pShaderModule);
VKAPI_ATTR void VKAPI_CALL vrsfov_vkDestroyShaderModule(VkDevice device,
                                                       VkShaderModule shaderModule,
                                                       const VkAllocationCallbacks* pAllocator);
VKAPI_ATTR VkResult VKAPI_CALL vrsfov_vkCreateGraphicsPipelines(
    VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines);

// ---- command buffers (vk_commands.cpp) ----
VKAPI_ATTR VkResult VKAPI_CALL vrsfov_vkBeginCommandBuffer(VkCommandBuffer commandBuffer,
                                                          const VkCommandBufferBeginInfo* pBeginInfo);
VKAPI_ATTR void VKAPI_CALL vrsfov_vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool,
                                                      uint32_t commandBufferCount,
                                                      const VkCommandBuffer* pCommandBuffers);
VKAPI_ATTR void VKAPI_CALL vrsfov_vkCmdBeginRendering(VkCommandBuffer commandBuffer,
                                                     const VkRenderingInfo* pRenderingInfo);
VKAPI_ATTR void VKAPI_CALL vrsfov_vkCmdBeginRenderingKHR(VkCommandBuffer commandBuffer,
                                                        const VkRenderingInfo* pRenderingInfo);
VKAPI_ATTR void VKAPI_CALL vrsfov_vkCmdEndRendering(VkCommandBuffer commandBuffer);
VKAPI_ATTR void VKAPI_CALL vrsfov_vkCmdEndRenderingKHR(VkCommandBuffer commandBuffer);
VKAPI_ATTR void VKAPI_CALL vrsfov_vkCmdBeginRenderPass(VkCommandBuffer commandBuffer,
                                                      const VkRenderPassBeginInfo* pRenderPassBegin,
                                                      VkSubpassContents contents);
VKAPI_ATTR void VKAPI_CALL vrsfov_vkCmdBeginRenderPass2(
    VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pRenderPassBegin,
    const VkSubpassBeginInfo* pSubpassBeginInfo);
VKAPI_ATTR void VKAPI_CALL vrsfov_vkCmdBeginRenderPass2KHR(
    VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pRenderPassBegin,
    const VkSubpassBeginInfo* pSubpassBeginInfo);
VKAPI_ATTR void VKAPI_CALL vrsfov_vkCmdEndRenderPass(VkCommandBuffer commandBuffer);
VKAPI_ATTR void VKAPI_CALL vrsfov_vkCmdEndRenderPass2(VkCommandBuffer commandBuffer,
                                                     const VkSubpassEndInfo* pSubpassEndInfo);
VKAPI_ATTR void VKAPI_CALL vrsfov_vkCmdEndRenderPass2KHR(VkCommandBuffer commandBuffer,
                                                        const VkSubpassEndInfo* pSubpassEndInfo);

}  // namespace vrsfov
