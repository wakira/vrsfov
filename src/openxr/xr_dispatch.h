// OpenXR dispatch tables and per-object data.
#pragma once

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/config.h"
#include "common/shared_state.h"

namespace vrsfov {

struct XrInstanceData;
struct XrSessionData;

// Next-layer entry points (resolved per instance through the next layer's
// xrGetInstanceProcAddr).
struct XrDispatch {
  PFN_xrGetInstanceProcAddr GetInstanceProcAddr = nullptr;
  PFN_xrDestroyInstance DestroyInstance = nullptr;
  PFN_xrGetInstanceProperties GetInstanceProperties = nullptr;
  PFN_xrGetSystem GetSystem = nullptr;
  PFN_xrEnumerateViewConfigurationViews EnumerateViewConfigurationViews = nullptr;
  PFN_xrGetSystemProperties GetSystemProperties = nullptr;
  PFN_xrStringToPath StringToPath = nullptr;
  PFN_xrCreateSession CreateSession = nullptr;
  PFN_xrDestroySession DestroySession = nullptr;
  PFN_xrBeginSession BeginSession = nullptr;
  PFN_xrEndSession EndSession = nullptr;
  PFN_xrCreateSwapchain CreateSwapchain = nullptr;
  PFN_xrEnumerateSwapchainImages EnumerateSwapchainImages = nullptr;
  PFN_xrDestroySwapchain DestroySwapchain = nullptr;
  PFN_xrCreateActionSet CreateActionSet = nullptr;
  PFN_xrDestroyActionSet DestroyActionSet = nullptr;
  PFN_xrCreateAction CreateAction = nullptr;
  PFN_xrDestroyAction DestroyAction = nullptr;
  PFN_xrSuggestInteractionProfileBindings SuggestInteractionProfileBindings = nullptr;
  PFN_xrAttachSessionActionSets AttachSessionActionSets = nullptr;
  PFN_xrSyncActions SyncActions = nullptr;
  PFN_xrGetActionStatePose GetActionStatePose = nullptr;
  PFN_xrCreateActionSpace CreateActionSpace = nullptr;
  PFN_xrDestroySpace DestroySpace = nullptr;
  PFN_xrLocateSpace LocateSpace = nullptr;
  PFN_xrCreateReferenceSpace CreateReferenceSpace = nullptr;
  PFN_xrLocateViews LocateViews = nullptr;
  PFN_xrWaitFrame WaitFrame = nullptr;
  PFN_xrBeginFrame BeginFrame = nullptr;
  PFN_xrEndFrame EndFrame = nullptr;
};

struct XrInstanceData {
  XrInstance instance = XR_NULL_HANDLE;  // handle returned by the next layer
  XrDispatch d;
  Config config;
  bool enabled = false;  // config.enabled as read at instance creation
  bool addedEyeGazeExtension = false;
  std::string appName;
  std::string configPath;
  int64_t configMtime = 0;
  int64_t lastConfigCheckNs = 0;
  XrSystemId systemId = XR_NULL_SYSTEM_ID;

  // Eye-gaze bindings. The app may suggest /interaction_profiles/ext/
  // eye_gaze_interaction itself (before or after this layer creates its action),
  // so suggestions are merged and re-suggested rather than replaced.
  std::vector<XrActionSuggestedBinding> appGazeBindings;
  std::vector<XrAction> ownGazeActions;
  XrAction appGazeAction = XR_NULL_HANDLE;  // app's own action for the gaze binding
};

struct XrSwapchainData {
  XrSwapchain swapchain = XR_NULL_HANDLE;
  XrSessionData* session = nullptr;
  uint32_t width = 0, height = 0, arraySize = 0;
  int64_t format = 0;
  bool registersImages = false;  // color or depth/stencil attachment usage
};

struct XrSessionData {
  XrSession session = XR_NULL_HANDLE;
  XrInstanceData* instance = nullptr;
  bool running = false;
  bool hasVulkanBinding = false;
  uint64_t vkDevice = 0;
  uint64_t vkPhysicalDevice = 0;
  uint32_t queueFamilyIndex = 0;
  XrViewConfigurationType viewConfig = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  XrSpace viewSpace = XR_NULL_HANDLE;

  // Eye gaze (XR_EXT_eye_gaze_interaction) plumbing.
  XrActionSet gazeSet = XR_NULL_HANDLE;
  XrAction gazeAction = XR_NULL_HANDLE;
  XrSpace gazeSpace = XR_NULL_HANDLE;
  // The app's own action for /user/eyes_ext/input/gaze_ext/pose, when it owns
  // that binding (OpenXR forbids a second binding for the same path).
  XrAction appGazeAction = XR_NULL_HANDLE;
  XrSpace appGazeSpace = XR_NULL_HANDLE;
  bool gazeSetAttached = false;
  bool gazeBindingsSuggested = false;
  bool gazeUnavailable = false;  // logged once
  bool syncedSinceLastGaze = false;
  // Last eye-tracked foveation centre from the OpenVR source (see openvr_gaze.cpp),
  // so a frame without a fresh sample does not snap the fovea back to the centre.
  float openvrU[2] = {0.5f, 0.5f};
  float openvrV[2] = {0.5f, 0.5f};
  bool openvrHave = false;

  // VkImage -> swapchain, for the Vulkan half's classification.
  SwapchainMap imagesByVkImage;

  // Last valid xrLocateViews result.
  struct CachedEye {
    XrPosef pose{};
    XrFovf fov{};
    bool valid = false;
  };
  CachedEye eyes[2];
  bool haveViewCache = false;

  // Gaze smoothing state.
  float smoothU[2] = {0.5f, 0.5f};
  float smoothV[2] = {0.5f, 0.5f};
  bool haveSmoothU[2] = {false, false};
  bool haveSmoothV[2] = {false, false};

  XrTime predictedDisplayTime = 0;
  XrTime lastGazeTime = 0;
  bool eyeTrackedLogged = false;
};

XrInstanceData* findXrInstance(XrInstance instance);
void addXrInstance(std::unique_ptr<XrInstanceData> data);
std::unique_ptr<XrInstanceData> removeXrInstance(XrInstance instance);

XrSessionData* findXrSession(XrSession session);
void addXrSession(std::unique_ptr<XrSessionData> data);
std::unique_ptr<XrSessionData> removeXrSession(XrSession session);

XrSwapchainData* findXrSwapchain(XrSwapchain swapchain);
void addXrSwapchain(std::unique_ptr<XrSwapchainData> data);
std::unique_ptr<XrSwapchainData> removeXrSwapchain(XrSwapchain swapchain);

// Snapshots of the sessions belonging to one instance (for the gaze bindings).
std::vector<XrSessionData*> xrSessionsForInstance(XrInstanceData* instance);

}  // namespace vrsfov
