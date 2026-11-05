// OpenXR hooks implemented by the layer.
#pragma once

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include "openxr/xr_dispatch.h"

namespace vrsfov {

struct NamedXrHook {
  const char* name;
  PFN_xrVoidFunction fn;
};

// Returns our hook for a name, or nullptr when the name is not hooked.
PFN_xrVoidFunction hookedXrFn(const char* name);

// ---- xr_layer.cpp ----
XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrGetInstanceProcAddr(XrInstance instance, const char* name,
                                                           PFN_xrVoidFunction* function);
XRAPI_ATTR XrResult XRAPI_CALL
vrsfov_xrCreateApiLayerInstance(const XrInstanceCreateInfo* createInfo,
                               const XrApiLayerCreateInfo* layerInfo, XrInstance* instance);
XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrDestroyInstance(XrInstance instance);
XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrGetSystem(XrInstance instance,
                                                 const XrSystemGetInfo* getInfo,
                                                 XrSystemId* systemId);

// ---- xr_session.cpp ----
XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrEnumerateViewConfigurationViews(
    XrInstance instance, XrSystemId systemId, XrViewConfigurationType viewConfigurationType,
    uint32_t viewCapacityInput, uint32_t* viewCountOutput, XrViewConfigurationView* views);
XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrCreateSession(XrInstance instance,
                                                     const XrSessionCreateInfo* createInfo,
                                                     XrSession* session);
XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrDestroySession(XrSession session);
XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrBeginSession(XrSession session,
                                                    const XrSessionBeginInfo* beginInfo);
XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrEndSession(XrSession session);
XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrCreateSwapchain(XrSession session,
                                                       const XrSwapchainCreateInfo* createInfo,
                                                       XrSwapchain* swapchain);
XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrEnumerateSwapchainImages(XrSwapchain swapchain,
                                                                uint32_t imageCapacityInput,
                                                                uint32_t* imageCountOutput,
                                                                XrSwapchainImageBaseHeader* images);
XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrDestroySwapchain(XrSwapchain swapchain);
XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrWaitFrame(XrSession session,
                                                 const XrFrameWaitInfo* frameWaitInfo,
                                                 XrFrameState* frameState);
XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrBeginFrame(XrSession session,
                                                  const XrFrameBeginInfo* frameBeginInfo);
XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrEndFrame(XrSession session,
                                                const XrFrameEndInfo* frameEndInfo);
XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrLocateViews(XrSession session,
                                                   const XrViewLocateInfo* viewLocateInfo,
                                                   XrViewState* viewState, uint32_t viewCapacityInput,
                                                   uint32_t* viewCountOutput, XrView* views);

// ---- xr_gaze.cpp ----
XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrSuggestInteractionProfileBindings(
    XrInstance instance, const XrInteractionProfileSuggestedBinding* suggestedBindings);
XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrAttachSessionActionSets(
    XrSession session, const XrSessionActionSetsAttachInfo* attachInfo);
XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrSyncActions(XrSession session,
                                                   const XrActionsSyncInfo* syncInfo);

// Publishes the gaze/eye state (called from the frame loop, xr_gaze.cpp).
void xrUpdateGaze(XrSessionData& session, XrTime displayTime);
// Suggests the eye-gaze profile for all gaze actions this layer owns, merging
// with the app's own suggestions for the same profile (xr_gaze.cpp).
bool xrSuggestGazeProfile(XrInstanceData& instance);
// Publishes device/swapchain/running state without touching the gaze timestamp.
void xrPublishSessionState(XrSessionData& session, XrInstanceData& instance);
// Hot reload of the config file, at most once per second (xr_session.cpp).
void xrMaybeReloadConfig(XrInstanceData& instance);

}  // namespace vrsfov
