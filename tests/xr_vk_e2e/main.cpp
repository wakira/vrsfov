// End-to-end test for the OpenXR -> Vulkan handoff without a headset or runtime.
//
// The program plays two roles in one process:
//   * a *mock OpenXR runtime* that the layer chains to (instance/session/actions/
//     swapchain/gaze), and the OpenXR loader's side: it negotiates the API layer
//     through xrNegotiateLoaderApiLayerInterface and creates an instance through
//     the layer's createApiLayerInstance,
//   * a Vulkan application whose device is created with the layer enabled, which
//     then renders into the registered swapchain image.
//
// Because both halves of the layer are the same shared object, the mock OpenXR
// instance makes the Vulkan half active through the real `liveXrInstances` gate
// (no VRSFOV_FORCE_ACTIVE), and the rendered rate map must follow the gaze
// the mock runtime reports.
//
// Usage: xr_vk_e2e <path-to-libvrsfov.so>
#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dlfcn.h>

#include "shaders_spv.h"

namespace {

[[noreturn]] void fail(const char* what) {
  fprintf(stderr, "FAIL: %s\n", what);
  exit(1);
}

// ---------------------------------------------------------------------------
// Mock OpenXR runtime
// ---------------------------------------------------------------------------

struct Recorded {
  bool sawInstanceCreate = false;
  bool eyeGazeExtensionRequested = false;
  int getSystemCalls = 0;
  int enumerateViewConfigCalls = 0;
  int createSessionCalls = 0;
  int createActionSetCalls = 0;
  int createActionCalls = 0;
  int createActionSpaceCalls = 0;
  int suggestCalls = 0;
  uint32_t lastSuggestCount = 0;
  int attachCalls = 0;
  uint32_t lastAttachCount = 0;
  int syncCalls = 0;
  uint32_t lastSyncCount = 0;
  int locateViewsCalls = 0;
  int getActionStatePoseCalls = 0;
  int locateSpaceCalls = 0;
  XrAction lastPoseAction = XR_NULL_HANDLE;
  int endFrameCalls = 0;
  int waitFrameCalls = 0;
  int beginFrameCalls = 0;
};

struct MockRuntime {
  Recorded rec;

  // Handles (opaque, only used as identities).
  XrInstance instance = reinterpret_cast<XrInstance>(0x1000);
  XrSession session = reinterpret_cast<XrSession>(0x2000);
  XrActionSet appActionSet = reinterpret_cast<XrActionSet>(0x3000);
  XrActionSet gazeActionSet = XR_NULL_HANDLE;
  XrAction gazeAction = XR_NULL_HANDLE;
  XrSpace gazeSpace = XR_NULL_HANDLE;
  XrSpace appSpace = XR_NULL_HANDLE;
  XrSpace viewSpace = XR_NULL_HANDLE;
  XrSwapchain appSwapchain = reinterpret_cast<XrSwapchain>(0x4000);
  XrPath gazeProfilePath = static_cast<XrPath>(0x77);
  XrPath gazeBindingPath = static_cast<XrPath>(0x78);

  VkImage swapchainImages[2]{};

  uint32_t recommendedWidth = 1024;
  uint32_t recommendedHeight = 1024;

  // Gaze the runtime reports, in the eye-image UV space of the (symmetric)
  // eye FOV used below.
  float gazeU = 0.5f;
  float gazeV = 0.5f;

  XrTime displayTime = 1000000000;  // 1 s in XrTime nanoseconds
  XrFovf eyeFov{};
  bool supportsEyeGaze = true;
};

MockRuntime g_mock;

XrFovf symmetricFov(float halfAngle) {
  XrFovf fov{};
  fov.angleLeft = -halfAngle;
  fov.angleRight = halfAngle;
  fov.angleUp = halfAngle;
  fov.angleDown = -halfAngle;
  return fov;
}

// Quaternion rotating (0,0,-1) onto `d`.
XrQuaternionf quaternionFromForward(const XrVector3f& d) {
  const XrVector3f f{0.f, 0.f, -1.f};
  const float dot = f.x * d.x + f.y * d.y + f.z * d.z;
  XrQuaternionf q{};
  if (dot < -0.9999f) {  // 180 degrees: any perpendicular axis
    q.x = 0.f;
    q.y = 1.f;
    q.z = 0.f;
    q.w = 0.f;
    return q;
  }
  const XrVector3f axis{f.y * d.z - f.z * d.y, f.z * d.x - f.x * d.z, f.x * d.y - f.y * d.x};
  q.x = axis.x;
  q.y = axis.y;
  q.z = axis.z;
  q.w = 1.f + dot;
  const float len = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
  if (len > 0.f) {
    q.x /= len;
    q.y /= len;
    q.z /= len;
    q.w /= len;
  }
  return q;
}

extern "C" {

XrResult XRAPI_CALL mockEnumerateInstanceExtensionProperties(const char* /*layerName*/,
                                                             uint32_t capacity, uint32_t* count,
                                                             XrExtensionProperties* props) {
  static const char* withGaze[] = {XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME,
                                   XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
  static const char* withoutGaze[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
  const char* const* names = g_mock.supportsEyeGaze ? withGaze : withoutGaze;
  const uint32_t total = g_mock.supportsEyeGaze ? 2u : 1u;
  if (count) *count = total;
  if (!props) return XR_SUCCESS;
  if (capacity < total) return XR_ERROR_SIZE_INSUFFICIENT;
  for (uint32_t i = 0; i < total; ++i) {
    props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    strncpy(props[i].extensionName, names[i], XR_MAX_EXTENSION_NAME_SIZE - 1);
  }
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockEnumerateApiLayerProperties(uint32_t* count, XrApiLayerProperties*) {
  if (count) *count = 0;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockDestroyInstance(XrInstance) { return XR_SUCCESS; }

XrResult XRAPI_CALL mockGetInstanceProperties(XrInstance, XrInstanceProperties* props) {
  if (props) {
    props->type = XR_TYPE_INSTANCE_PROPERTIES;
    props->runtimeVersion = XR_MAKE_VERSION(1, 0, 0);
    strncpy(props->runtimeName, "mock runtime", XR_MAX_RUNTIME_NAME_SIZE - 1);
  }
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockGetSystem(XrInstance, const XrSystemGetInfo*, XrSystemId* systemId) {
  g_mock.rec.getSystemCalls++;
  if (systemId) *systemId = static_cast<XrSystemId>(1);
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockGetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* props) {
  if (props) {
    props->type = XR_TYPE_SYSTEM_PROPERTIES;
    props->systemId = 1;
    props->vendorId = 1;
    strncpy(props->systemName, "mock", XR_MAX_SYSTEM_NAME_SIZE - 1);
    for (auto* node = reinterpret_cast<XrBaseOutStructure*>(props->next); node;
         node = node->next) {
      if (node->type == XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT) {
        reinterpret_cast<XrSystemEyeGazeInteractionPropertiesEXT*>(node)->supportsEyeGazeInteraction =
            g_mock.supportsEyeGaze ? XR_TRUE : XR_FALSE;
      }
    }
  }
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockEnumerateViewConfigurationViews(XrInstance, XrSystemId,
                                                        XrViewConfigurationType viewConfig,
                                                        uint32_t capacity, uint32_t* count,
                                                        XrViewConfigurationView* views) {
  g_mock.rec.enumerateViewConfigCalls++;
  const uint32_t total = 2;
  if (count) *count = total;
  if (!views) return XR_SUCCESS;
  if (capacity < total) return XR_ERROR_SIZE_INSUFFICIENT;
  for (uint32_t i = 0; i < total; ++i) {
    views[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    views[i].recommendedImageRectWidth = g_mock.recommendedWidth;
    views[i].recommendedImageRectHeight = g_mock.recommendedHeight;
    views[i].maxImageRectWidth = g_mock.recommendedWidth;
    views[i].maxImageRectHeight = g_mock.recommendedHeight;
    views[i].recommendedSwapchainSampleCount = 1;
    views[i].maxSwapchainSampleCount = 1;
  }
  (void)viewConfig;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockStringToPath(XrInstance, const char* path, XrPath* out) {
  if (!out) return XR_ERROR_VALIDATION_FAILURE;
  if (strcmp(path, "/interaction_profiles/ext/eye_gaze_interaction") == 0) {
    *out = g_mock.gazeProfilePath;
  } else if (strcmp(path, "/user/eyes_ext/input/gaze_ext/pose") == 0) {
    *out = g_mock.gazeBindingPath;
  } else {
    *out = static_cast<XrPath>(std::hash<std::string>{}(path));
  }
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockCreateSession(XrInstance, const XrSessionCreateInfo*, XrSession* session) {
  g_mock.rec.createSessionCalls++;
  if (session) *session = g_mock.session;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockDestroySession(XrSession) { return XR_SUCCESS; }
XrResult XRAPI_CALL mockBeginSession(XrSession, const XrSessionBeginInfo*) { return XR_SUCCESS; }
XrResult XRAPI_CALL mockEndSession(XrSession) { return XR_SUCCESS; }

XrResult XRAPI_CALL mockCreateSwapchain(XrSession, const XrSwapchainCreateInfo*, XrSwapchain* out) {
  if (out) *out = g_mock.appSwapchain;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockEnumerateSwapchainImages(XrSwapchain, uint32_t capacity, uint32_t* count,
                                                 XrSwapchainImageBaseHeader* images) {
  const uint32_t total = 2;
  if (count) *count = total;
  if (!images) return XR_SUCCESS;
  if (capacity < total) return XR_ERROR_SIZE_INSUFFICIENT;
  for (uint32_t i = 0; i < total; ++i) {
    auto* vk = reinterpret_cast<XrSwapchainImageVulkanKHR*>(images + i);
    vk->type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
    vk->next = nullptr;
    vk->image = g_mock.swapchainImages[i];
  }
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockDestroySwapchain(XrSwapchain) { return XR_SUCCESS; }

XrResult XRAPI_CALL mockCreateActionSet(XrInstance, const XrActionSetCreateInfo*,
                                        XrActionSet* actionSet) {
  g_mock.rec.createActionSetCalls++;
  g_mock.gazeActionSet = reinterpret_cast<XrActionSet>(0x3001);
  if (actionSet) *actionSet = g_mock.gazeActionSet;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockCreateAction(XrActionSet, const XrActionCreateInfo*, XrAction* action) {
  g_mock.rec.createActionCalls++;
  g_mock.gazeAction = reinterpret_cast<XrAction>(0x3002);
  if (action) *action = g_mock.gazeAction;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockCreateActionSpace(XrSession, const XrActionSpaceCreateInfo*, XrSpace* space) {
  g_mock.rec.createActionSpaceCalls++;
  g_mock.gazeSpace = reinterpret_cast<XrSpace>(0x3003);
  if (space) *space = g_mock.gazeSpace;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo*,
                                             XrSpace* space) {
  g_mock.viewSpace = reinterpret_cast<XrSpace>(0x3004);
  if (space) *space = g_mock.viewSpace;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockDestroySpace(XrSpace) { return XR_SUCCESS; }
XrResult XRAPI_CALL mockDestroyAction(XrAction) { return XR_SUCCESS; }
XrResult XRAPI_CALL mockDestroyActionSet(XrActionSet) { return XR_SUCCESS; }

XrResult XRAPI_CALL mockSuggestInteractionProfileBindings(
    XrInstance, const XrInteractionProfileSuggestedBinding* bindings) {
  g_mock.rec.suggestCalls++;
  g_mock.rec.lastSuggestCount = bindings ? bindings->countSuggestedBindings : 0;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockAttachSessionActionSets(XrSession,
                                                const XrSessionActionSetsAttachInfo* info) {
  g_mock.rec.attachCalls++;
  g_mock.rec.lastAttachCount = info ? info->countActionSets : 0;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockSyncActions(XrSession, const XrActionsSyncInfo* info) {
  g_mock.rec.syncCalls++;
  g_mock.rec.lastSyncCount = info ? info->countActiveActionSets : 0;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockGetActionStatePose(XrSession, const XrActionStateGetInfo* info,
                                           XrActionStatePose* state) {
  g_mock.rec.getActionStatePoseCalls++;
  g_mock.rec.lastPoseAction = info ? info->action : XR_NULL_HANDLE;
  if (state) {
    state->type = XR_TYPE_ACTION_STATE_POSE;
    state->isActive = XR_TRUE;
  }
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockLocateSpace(XrSpace, XrSpace, XrTime, XrSpaceLocation* location) {
  g_mock.rec.locateSpaceCalls++;
  // Report the gaze the test asked for: invert the symmetric projection.
  const float tx = 2.f * g_mock.gazeU - 1.f;
  const float ty = 1.f - 2.f * g_mock.gazeV;
  const float len = std::sqrt(tx * tx + ty * ty + 1.f);
  const XrVector3f direction{tx / len, ty / len, -1.f / len};
  if (location) {
    location->type = XR_TYPE_SPACE_LOCATION;
    location->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                              XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
                              XR_SPACE_LOCATION_POSITION_VALID_BIT |
                              XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    location->pose.orientation = quaternionFromForward(direction);
    location->pose.position = {0.f, 0.f, 0.f};
  }
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockWaitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState* state) {
  g_mock.rec.waitFrameCalls++;
  if (state) {
    state->type = XR_TYPE_FRAME_STATE;
    state->predictedDisplayTime = g_mock.displayTime;
    state->predictedDisplayPeriod = 11000000;
    state->shouldRender = XR_TRUE;
  }
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockBeginFrame(XrSession, const XrFrameBeginInfo*) {
  g_mock.rec.beginFrameCalls++;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockEndFrame(XrSession, const XrFrameEndInfo*) {
  g_mock.rec.endFrameCalls++;
  return XR_SUCCESS;
}

XrResult XRAPI_CALL mockLocateViews(XrSession, const XrViewLocateInfo*, XrViewState* state,
                                    uint32_t capacity, uint32_t* count, XrView* views) {
  g_mock.rec.locateViewsCalls++;
  const uint32_t total = 2;
  if (count) *count = total;
  if (state) {
    state->type = XR_TYPE_VIEW_STATE;
    state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT |
                            XR_VIEW_STATE_ORIENTATION_TRACKED_BIT |
                            XR_VIEW_STATE_POSITION_VALID_BIT |
                            XR_VIEW_STATE_POSITION_TRACKED_BIT;
  }
  if (!views) return XR_SUCCESS;
  if (capacity < total) return XR_ERROR_SIZE_INSUFFICIENT;
  for (uint32_t i = 0; i < total; ++i) {
    views[i].type = XR_TYPE_VIEW;
    views[i].pose.orientation.w = 1.f;
    // The two eyes look from slightly different positions along X.
    views[i].pose.position = {i == 0 ? -0.03f : 0.03f, 0.f, 0.f};
    views[i].fov = g_mock.eyeFov;
  }
  return XR_SUCCESS;
}

}  // extern "C"

// The layer resolves its dispatch with the loader-provided GIPA.
XrResult XRAPI_CALL mockGetInstanceProcAddr(XrInstance instance, const char* name,
                                            PFN_xrVoidFunction* function);

struct MockFn {
  const char* name;
  PFN_xrVoidFunction fn;
};

const MockFn kMockFns[] = {
    {"xrEnumerateInstanceExtensionProperties",
     reinterpret_cast<PFN_xrVoidFunction>(mockEnumerateInstanceExtensionProperties)},
    {"xrEnumerateApiLayerProperties",
     reinterpret_cast<PFN_xrVoidFunction>(mockEnumerateApiLayerProperties)},
    {"xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction>(mockDestroyInstance)},
    {"xrGetInstanceProperties", reinterpret_cast<PFN_xrVoidFunction>(mockGetInstanceProperties)},
    {"xrGetSystem", reinterpret_cast<PFN_xrVoidFunction>(mockGetSystem)},
    {"xrGetSystemProperties", reinterpret_cast<PFN_xrVoidFunction>(mockGetSystemProperties)},
    {"xrEnumerateViewConfigurationViews",
     reinterpret_cast<PFN_xrVoidFunction>(mockEnumerateViewConfigurationViews)},
    {"xrStringToPath", reinterpret_cast<PFN_xrVoidFunction>(mockStringToPath)},
    {"xrCreateSession", reinterpret_cast<PFN_xrVoidFunction>(mockCreateSession)},
    {"xrDestroySession", reinterpret_cast<PFN_xrVoidFunction>(mockDestroySession)},
    {"xrBeginSession", reinterpret_cast<PFN_xrVoidFunction>(mockBeginSession)},
    {"xrEndSession", reinterpret_cast<PFN_xrVoidFunction>(mockEndSession)},
    {"xrCreateSwapchain", reinterpret_cast<PFN_xrVoidFunction>(mockCreateSwapchain)},
    {"xrEnumerateSwapchainImages",
     reinterpret_cast<PFN_xrVoidFunction>(mockEnumerateSwapchainImages)},
    {"xrDestroySwapchain", reinterpret_cast<PFN_xrVoidFunction>(mockDestroySwapchain)},
    {"xrCreateActionSet", reinterpret_cast<PFN_xrVoidFunction>(mockCreateActionSet)},
    {"xrCreateAction", reinterpret_cast<PFN_xrVoidFunction>(mockCreateAction)},
    {"xrCreateActionSpace", reinterpret_cast<PFN_xrVoidFunction>(mockCreateActionSpace)},
    {"xrCreateReferenceSpace", reinterpret_cast<PFN_xrVoidFunction>(mockCreateReferenceSpace)},
    {"xrDestroySpace", reinterpret_cast<PFN_xrVoidFunction>(mockDestroySpace)},
    {"xrDestroyAction", reinterpret_cast<PFN_xrVoidFunction>(mockDestroyAction)},
    {"xrDestroyActionSet", reinterpret_cast<PFN_xrVoidFunction>(mockDestroyActionSet)},
    {"xrSuggestInteractionProfileBindings",
     reinterpret_cast<PFN_xrVoidFunction>(mockSuggestInteractionProfileBindings)},
    {"xrAttachSessionActionSets",
     reinterpret_cast<PFN_xrVoidFunction>(mockAttachSessionActionSets)},
    {"xrSyncActions", reinterpret_cast<PFN_xrVoidFunction>(mockSyncActions)},
    {"xrGetActionStatePose", reinterpret_cast<PFN_xrVoidFunction>(mockGetActionStatePose)},
    {"xrLocateSpace", reinterpret_cast<PFN_xrVoidFunction>(mockLocateSpace)},
    {"xrWaitFrame", reinterpret_cast<PFN_xrVoidFunction>(mockWaitFrame)},
    {"xrBeginFrame", reinterpret_cast<PFN_xrVoidFunction>(mockBeginFrame)},
    {"xrEndFrame", reinterpret_cast<PFN_xrVoidFunction>(mockEndFrame)},
    {"xrLocateViews", reinterpret_cast<PFN_xrVoidFunction>(mockLocateViews)},
};

XrResult XRAPI_CALL mockGetInstanceProcAddr(XrInstance instance, const char* name,
                                            PFN_xrVoidFunction* function) {
  if (!name || !function) return XR_ERROR_VALIDATION_FAILURE;
  *function = nullptr;
  if (strcmp(name, "xrGetInstanceProcAddr") == 0) {
    *function = reinterpret_cast<PFN_xrVoidFunction>(mockGetInstanceProcAddr);
    return XR_SUCCESS;
  }
  for (const MockFn& fn : kMockFns) {
    if (strcmp(fn.name, name) == 0) {
      *function = fn.fn;
      return XR_SUCCESS;
    }
  }
  (void)instance;
  return XR_ERROR_FUNCTION_UNSUPPORTED;
}

XrResult XRAPI_CALL mockCreateApiLayerInstance(const XrInstanceCreateInfo* info,
                                               const XrApiLayerCreateInfo* layerInfo,
                                               XrInstance* instance) {
  g_mock.rec.sawInstanceCreate = true;
  g_mock.rec.eyeGazeExtensionRequested = false;
  for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
    if (strcmp(info->enabledExtensionNames[i], XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0) {
      g_mock.rec.eyeGazeExtensionRequested = true;
    }
  }
  (void)layerInfo;
  if (instance) *instance = g_mock.instance;
  return XR_SUCCESS;
}

// ---------------------------------------------------------------------------
// Minimal Vulkan side
// ---------------------------------------------------------------------------

#define VK_CHECK(expr)                              \
  do {                                              \
    VkResult vk_r = (expr);                         \
    if (vk_r != VK_SUCCESS) {                       \
      fprintf(stderr, "Vulkan call failed (%d): %s\n", (int)vk_r, #expr); \
      exit(1);                                      \
    }                                               \
  } while (0)

struct Vk {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice gpu = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  uint32_t queueFamily = 0;
  VkQueue queue = VK_NULL_HANDLE;
  VkCommandPool pool = VK_NULL_HANDLE;

  VkImage target = VK_NULL_HANDLE;  // stands in for the swapchain image (2 layers)
  VkDeviceMemory targetMemory = VK_NULL_HANDLE;
  VkImageView targetView = VK_NULL_HANDLE;
  VkBuffer readback = VK_NULL_HANDLE;
  VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
  void* readbackMapped = nullptr;

  VkShaderModule vert = VK_NULL_HANDLE;
  VkShaderModule frag = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;

  uint32_t width = 1024, height = 1024;
  PFN_vkCmdBeginRendering beginRendering = nullptr;
  PFN_vkCmdEndRendering endRendering = nullptr;
};

uint32_t memoryType(VkPhysicalDevice gpu, uint32_t bits, VkMemoryPropertyFlags flags) {
  VkPhysicalDeviceMemoryProperties props{};
  vkGetPhysicalDeviceMemoryProperties(gpu, &props);
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
    if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) return i;
  }
  fail("no suitable memory type");
}

void initVulkanInstance(Vk& vk) {
  VkApplicationInfo app{};
  app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  app.pApplicationName = "vrsfov xr_vk_e2e";
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo instanceInfo{};
  instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  instanceInfo.pApplicationInfo = &app;
  VK_CHECK(vkCreateInstance(&instanceInfo, nullptr, &vk.instance));

  uint32_t gpuCount = 0;
  vkEnumeratePhysicalDevices(vk.instance, &gpuCount, nullptr);
  if (!gpuCount) fail("no Vulkan device");
  std::vector<VkPhysicalDevice> gpus(gpuCount);
  vkEnumeratePhysicalDevices(vk.instance, &gpuCount, gpus.data());
  vk.gpu = gpus[0];
  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(vk.gpu, &props);
  printf("Vulkan device: %s\n", props.deviceName);

  uint32_t qfCount = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(vk.gpu, &qfCount, nullptr);
  std::vector<VkQueueFamilyProperties> qfs(qfCount);
  vkGetPhysicalDeviceQueueFamilyProperties(vk.gpu, &qfCount, qfs.data());
  for (uint32_t i = 0; i < qfCount; ++i) {
    if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      vk.queueFamily = i;
      break;
    }
  }

}

// Created after the device, and again after a late activation: only pipelines
// created while the layer is active carry our fragment shading rate state.
void createGraphicsPipeline(Vk& vk) {
  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vk.vert;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = vk.frag;
  stages[1].pName = "main";
  VkPipelineVertexInputStateCreateInfo vertexInput{};
  vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
  inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkViewport viewport{0.f, 0.f, (float)vk.width, (float)vk.height, 0.f, 1.f};
  VkRect2D scissor{{0, 0}, {vk.width, vk.height}};
  VkPipelineViewportStateCreateInfo viewportState{};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.pViewports = &viewport;
  viewportState.scissorCount = 1;
  viewportState.pScissors = &scissor;
  VkPipelineRasterizationStateCreateInfo raster{};
  raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  raster.polygonMode = VK_POLYGON_MODE_FILL;
  raster.cullMode = VK_CULL_MODE_NONE;
  raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  raster.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo multisample{};
  multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState blendAttachment{};
  blendAttachment.blendEnable = VK_FALSE;
  blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                   VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo blend{};
  blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  blend.attachmentCount = 1;
  blend.pAttachments = &blendAttachment;
  VkPipelineRenderingCreateInfo rendering{};
  rendering.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
  rendering.colorAttachmentCount = 1;
  VkFormat format = VK_FORMAT_R8_UINT;
  rendering.pColorAttachmentFormats = &format;
  VkGraphicsPipelineCreateInfo pipelineInfo{};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineInfo.pNext = &rendering;
  pipelineInfo.stageCount = 2;
  pipelineInfo.pStages = stages;
  pipelineInfo.pVertexInputState = &vertexInput;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &raster;
  pipelineInfo.pMultisampleState = &multisample;
  pipelineInfo.pColorBlendState = &blend;
  pipelineInfo.layout = vk.layout;
  VK_CHECK(vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                     &vk.pipeline));
}

// Records and frees an empty command buffer. For a device created before the
// XrInstance this is what wakes the layer up (the vkBeginCommandBuffer hook).
void triggerLayerActivation(Vk& vk) {
  VkCommandBufferAllocateInfo alloc{};
  alloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  alloc.commandPool = vk.pool;
  alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  alloc.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VK_CHECK(vkAllocateCommandBuffers(vk.device, &alloc, &cmd));
  VkCommandBufferBeginInfo beginInfo{};
  beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  VK_CHECK(vkBeginCommandBuffer(cmd, &beginInfo));
  VK_CHECK(vkEndCommandBuffer(cmd));
  vkFreeCommandBuffers(vk.device, vk.pool, 1, &cmd);
}

void initVulkanResources(Vk& vk) {
  // No FSR features are chained on purpose: the layer must add them.
  VkPhysicalDeviceVulkan13Features f13{};
  f13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  f13.dynamicRendering = VK_TRUE;
  VkDeviceQueueCreateInfo queueInfo{};
  queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  queueInfo.queueFamilyIndex = vk.queueFamily;
  queueInfo.queueCount = 1;
  float priority = 1.0f;
  queueInfo.pQueuePriorities = &priority;
  VkDeviceCreateInfo deviceInfo{};
  deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  deviceInfo.pNext = &f13;
  deviceInfo.queueCreateInfoCount = 1;
  deviceInfo.pQueueCreateInfos = &queueInfo;
  VK_CHECK(vkCreateDevice(vk.gpu, &deviceInfo, nullptr, &vk.device));
  vkGetDeviceQueue(vk.device, vk.queueFamily, 0, &vk.queue);

  vk.beginRendering =
      reinterpret_cast<PFN_vkCmdBeginRendering>(vkGetDeviceProcAddr(vk.device, "vkCmdBeginRendering"));
  vk.endRendering =
      reinterpret_cast<PFN_vkCmdEndRendering>(vkGetDeviceProcAddr(vk.device, "vkCmdEndRendering"));
  if (!vk.beginRendering || !vk.endRendering) fail("dynamic rendering entry points missing");

  VkCommandPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  poolInfo.queueFamilyIndex = vk.queueFamily;
  VK_CHECK(vkCreateCommandPool(vk.device, &poolInfo, nullptr, &vk.pool));

  // Target image: two layers, like an OpenXR stereo swapchain.
  VkImageCreateInfo imageInfo{};
  imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imageInfo.imageType = VK_IMAGE_TYPE_2D;
  imageInfo.format = VK_FORMAT_R8_UINT;
  imageInfo.extent = {vk.width, vk.height, 1};
  imageInfo.mipLevels = 1;
  imageInfo.arrayLayers = 2;
  imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
  imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
  imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VK_CHECK(vkCreateImage(vk.device, &imageInfo, nullptr, &vk.target));
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(vk.device, vk.target, &req);
  VkMemoryAllocateInfo allocate{};
  allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate.allocationSize = req.size;
  allocate.memoryTypeIndex = memoryType(vk.gpu, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VK_CHECK(vkAllocateMemory(vk.device, &allocate, nullptr, &vk.targetMemory));
  VK_CHECK(vkBindImageMemory(vk.device, vk.target, vk.targetMemory, 0));

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = vk.target;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;  // layer 0 only
  viewInfo.format = VK_FORMAT_R8_UINT;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VK_CHECK(vkCreateImageView(vk.device, &viewInfo, nullptr, &vk.targetView));

  VkBufferCreateInfo bufferInfo{};
  bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bufferInfo.size = (VkDeviceSize)vk.width * vk.height * 2;
  bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VK_CHECK(vkCreateBuffer(vk.device, &bufferInfo, nullptr, &vk.readback));
  vkGetBufferMemoryRequirements(vk.device, vk.readback, &req);
  allocate.allocationSize = req.size;
  allocate.memoryTypeIndex =
      memoryType(vk.gpu, req.memoryTypeBits,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VK_CHECK(vkAllocateMemory(vk.device, &allocate, nullptr, &vk.readbackMemory));
  VK_CHECK(vkBindBufferMemory(vk.device, vk.readback, vk.readbackMemory, 0));
  VK_CHECK(vkMapMemory(vk.device, vk.readbackMemory, 0, VK_WHOLE_SIZE, 0, &vk.readbackMapped));

  VkShaderModuleCreateInfo moduleInfo{};
  moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  moduleInfo.codeSize = kFullscreenVertSpvSize;
  moduleInfo.pCode = kFullscreenVertSpv;
  VK_CHECK(vkCreateShaderModule(vk.device, &moduleInfo, nullptr, &vk.vert));
  moduleInfo.codeSize = kShadingRateFragSpvSize;
  moduleInfo.pCode = kShadingRateFragSpv;
  VK_CHECK(vkCreateShaderModule(vk.device, &moduleInfo, nullptr, &vk.frag));

  VkPipelineLayoutCreateInfo layoutInfo{};
  layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  VK_CHECK(vkCreatePipelineLayout(vk.device, &layoutInfo, nullptr, &vk.layout));

  createGraphicsPipeline(vk);
}

// Renders one frame into layer 0 of the target and returns the rate map.
struct RateMap {
  std::vector<uint8_t> codes;
  uint32_t width = 0, height = 0;
};

RateMap renderAndReadRateMap(Vk& vk, uint32_t texel) {
  VkCommandBufferAllocateInfo cmdInfo{};
  cmdInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cmdInfo.commandPool = vk.pool;
  cmdInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cmdInfo.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VK_CHECK(vkAllocateCommandBuffers(vk.device, &cmdInfo, &cmd));
  VkCommandBufferBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  VK_CHECK(vkBeginCommandBuffer(cmd, &begin));

  VkRenderingAttachmentInfo color{};
  color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
  color.imageView = vk.targetView;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.clearValue = {{{0, 0, 0, 0}}};
  VkRenderingInfo rendering{};
  rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
  rendering.renderArea = {{0, 0}, {vk.width, vk.height}};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &color;
  vk.beginRendering(cmd, &rendering);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipeline);
  vkCmdDraw(cmd, 3, 1, 0, 0);
  vk.endRendering(cmd);

  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = vk.target;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
  barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.imageExtent = {vk.width, vk.height, 1};
  vkCmdCopyImageToBuffer(cmd, vk.target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.readback, 1, &copy);

  VK_CHECK(vkEndCommandBuffer(cmd));
  VkSubmitInfo submit{};
  submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd;
  VK_CHECK(vkQueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE));
  VK_CHECK(vkQueueWaitIdle(vk.queue));
  vkFreeCommandBuffers(vk.device, vk.pool, 1, &cmd);

  const uint8_t* pixels = static_cast<const uint8_t*>(vk.readbackMapped);
  RateMap map;
  map.width = (vk.width + texel - 1) / texel;
  map.height = (vk.height + texel - 1) / texel;
  map.codes.resize((size_t)map.width * map.height);
  for (uint32_t ty = 0; ty < map.height; ++ty) {
    for (uint32_t tx = 0; tx < map.width; ++tx) {
      const uint32_t px = std::min(tx * texel + texel / 2, vk.width - 1);
      const uint32_t py = std::min(ty * texel + texel / 2, vk.height - 1);
      map.codes[(size_t)ty * map.width + tx] = pixels[(size_t)py * vk.width + px];
    }
  }
  return map;
}

// Centroid of the full-rate (1x1) texels, in [0,1] of the eye image.
bool foveaCentroid(const RateMap& map, float* u, float* v) {
  double sumX = 0, sumY = 0;
  uint64_t count = 0;
  for (uint32_t y = 0; y < map.height; ++y) {
    for (uint32_t x = 0; x < map.width; ++x) {
      if (map.codes[(size_t)y * map.width + x] != 0) continue;
      sumX += ((double)x + 0.5) / map.width;
      sumY += ((double)y + 0.5) / map.height;
      ++count;
    }
  }
  if (!count) return false;
  *u = (float)(sumX / count);
  *v = (float)(sumY / count);
  return true;
}


// The Vulkan loader loads one copy of the layer; the XR side has to talk to the
// very same object or the two halves would not share SharedState. Find it in
// /proc/self/maps (after the Vulkan instance is created) and open that path.
std::string findLoadedLayerPath() {
  FILE* maps = fopen("/proc/self/maps", "r");
  if (!maps) return {};
  char line[4096];
  std::string found;
  while (fgets(line, sizeof(line), maps)) {
    const std::string text(line);
    const size_t pos = text.find("libvrsfov.so");
    if (pos == std::string::npos) continue;
    size_t start = text.find('/');
    if (start == std::string::npos) continue;
    size_t end = text.find_first_of(" \n", pos);
    if (end == std::string::npos) end = text.size();
    found = text.substr(start, end - start);
    break;
  }
  fclose(maps);
  return found;
}

int failed = 0;
void check(bool ok, const char* what) {
  printf("  %s %s\n", ok ? "[ok]  " : "[FAIL]", what);
  if (!ok) ++failed;
}

}  // namespace

int main(int argc, char** argv) {
  const char* layerPathArg = argc > 1 ? argv[1] : nullptr;
  // M8 fallback path: a runtime without eye gaze must degrade to fixed foveation.
  const bool noEyeGaze = getenv("VRSFOV_E2E_NO_EYE_GAZE") != nullptr;
  // M11: the other order of initialisation - Vulkan graphics (device included)
  // before OpenXR, as VTOL VR does. The layer must activate late.
  const bool deviceFirst = getenv("VRSFOV_E2E_VK_DEVICE_FIRST") != nullptr;
  // The layer must suggest its own gaze binding: most apps never mention the
  // eye-gaze interaction profile at all.
  const bool appSuggestsGaze = getenv("VRSFOV_E2E_NO_APP_GAZE_SUGGEST") == nullptr;
  const bool appOwnsGazeBinding = getenv("VRSFOV_E2E_APP_OWNS_GAZE_BINDING") != nullptr;
  g_mock.supportsEyeGaze = !noEyeGaze;
  printf("mode: %s%s%s%s\n", noEyeGaze ? "runtime without eye gaze" : "eye-tracked",
         deviceFirst ? ", Vulkan device before the XR instance" : "",
         appSuggestsGaze ? "" : ", app never suggests eye gaze",
         appOwnsGazeBinding ? ", app owns the gaze binding" : "");

  // Creating the Vulkan instance makes the loader load the layer.
  Vk vk;
  initVulkanInstance(vk);

  // In the device-first case the device exists before there is any XrInstance.
  if (deviceFirst) initVulkanResources(vk);

  std::string layerPath = findLoadedLayerPath();
  if (layerPath.empty()) {
    layerPath = layerPathArg ? layerPathArg : "";
    fprintf(stderr, "warning: layer library not found in /proc/self/maps\n");
  }
  if (layerPath.empty()) fail("no layer library to load");
  printf("layer (as loaded by the Vulkan loader): %s\n", layerPath.c_str());

  void* handle = dlopen(layerPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
  if (!handle) fail(dlerror());
  if (layerPathArg && layerPath != layerPathArg) {
    printf("note: dlopen'ed %s instead of %s so that both halves share one object\n",
           layerPath.c_str(), layerPathArg);
  }
  auto negotiate = reinterpret_cast<PFN_xrNegotiateLoaderApiLayerInterface>(
      dlsym(handle, "xrNegotiateLoaderApiLayerInterface"));
  if (!negotiate) fail("xrNegotiateLoaderApiLayerInterface not exported");

  // --- The "loader" negotiates our API layer ------------------------------
  XrNegotiateLoaderInfo loaderInfo{};
  loaderInfo.structType = XR_LOADER_INTERFACE_STRUCT_LOADER_INFO;
  loaderInfo.structVersion = XR_LOADER_INFO_STRUCT_VERSION;
  loaderInfo.structSize = sizeof(XrNegotiateLoaderInfo);
  loaderInfo.minInterfaceVersion = 1;
  loaderInfo.maxInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
  loaderInfo.minApiVersion = XR_MAKE_VERSION(1, 0, 0);
  loaderInfo.maxApiVersion = XR_CURRENT_API_VERSION;
  XrNegotiateApiLayerRequest request{};
  request.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST;
  request.structVersion = XR_API_LAYER_INFO_STRUCT_VERSION;
  request.structSize = sizeof(XrNegotiateApiLayerRequest);
  XrResult result = negotiate(&loaderInfo, "XR_APILAYER_VRSFOV_foveated_rendering", &request);
  check(result == XR_SUCCESS, "layer negotiation succeeds");
  check(request.getInstanceProcAddr != nullptr && request.createApiLayerInstance != nullptr,
        "layer returned its entry points");

  // --- Instance creation through the layer --------------------------------
  XrApiLayerNextInfo nextInfo{};
  nextInfo.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_NEXT_INFO;
  nextInfo.structVersion = XR_API_LAYER_NEXT_INFO_STRUCT_VERSION;
  nextInfo.structSize = sizeof(XrApiLayerNextInfo);
  strncpy(nextInfo.layerName, "mock runtime", XR_MAX_API_LAYER_NAME_SIZE - 1);
  nextInfo.nextGetInstanceProcAddr = mockGetInstanceProcAddr;
  nextInfo.nextCreateApiLayerInstance = mockCreateApiLayerInstance;
  nextInfo.next = nullptr;
  XrApiLayerCreateInfo layerCreateInfo{};
  layerCreateInfo.structType = XR_LOADER_INTERFACE_STRUCT_API_LAYER_CREATE_INFO;
  layerCreateInfo.structVersion = XR_API_LAYER_CREATE_INFO_STRUCT_VERSION;
  layerCreateInfo.structSize = sizeof(XrApiLayerCreateInfo);
  layerCreateInfo.nextInfo = &nextInfo;

  g_mock.eyeFov = symmetricFov(45.f * 3.14159265f / 180.f);

  XrApplicationInfo appInfo{};
  strncpy(appInfo.applicationName, "xr_vk_e2e", XR_MAX_APPLICATION_NAME_SIZE - 1);
  strncpy(appInfo.engineName, "none", XR_MAX_ENGINE_NAME_SIZE - 1);
  appInfo.apiVersion = XR_CURRENT_API_VERSION;
  XrInstanceCreateInfo instanceCreate{};
  instanceCreate.type = XR_TYPE_INSTANCE_CREATE_INFO;
  instanceCreate.applicationInfo = appInfo;
  XrInstance instance = XR_NULL_HANDLE;
  result = request.createApiLayerInstance(&instanceCreate, &layerCreateInfo, &instance);
  check(result == XR_SUCCESS, "instance created through the layer");
  check(g_mock.rec.sawInstanceCreate, "the mock runtime saw the instance create info");
  check(g_mock.rec.eyeGazeExtensionRequested != noEyeGaze,
        noEyeGaze ? "layer did not ask for an unsupported extension"
                  : "layer appended XR_EXT_eye_gaze_interaction (runtime supports it)");

  // GetProcAddr must return our hooks for hooked names.
  auto gipa = request.getInstanceProcAddr;
  PFN_xrVoidFunction fn = nullptr;
  check(XR_SUCCEEDED(gipa(instance, "xrCreateSession", &fn)) && fn != nullptr &&
            reinterpret_cast<void*>(fn) != reinterpret_cast<void*>(mockCreateSession),
        "xrCreateSession is hooked");
  check(XR_SUCCEEDED(gipa(instance, "xrLocateSpace", &fn)) && fn != nullptr,
        "xrLocateSpace is resolvable");

  auto xrGetSystem = static_cast<PFN_xrGetSystem>(nullptr);
  gipa(instance, "xrGetSystem", reinterpret_cast<PFN_xrVoidFunction*>(&xrGetSystem));
  XrSystemGetInfo systemInfo{};
  systemInfo.type = XR_TYPE_SYSTEM_GET_INFO;
  systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  XrSystemId systemId = XR_NULL_SYSTEM_ID;
  check(XR_SUCCEEDED(xrGetSystem(instance, &systemInfo, &systemId)) && g_mock.rec.getSystemCalls == 1,
        "xrGetSystem forwarded to the runtime");

  // --- Vulkan device: must become active because the XR instance exists -----
  if (!deviceFirst) {
    initVulkanResources(vk);
    check(true, "Vulkan device created after the XR instance (layer must activate)");
  } else {
    check(true, "Vulkan device created before the XR instance (layer must activate late)");
  }

  // --- Session, views, swapchain ------------------------------------------
  XrGraphicsBindingVulkanKHR binding{};
  binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
  binding.instance = vk.instance;
  binding.physicalDevice = vk.gpu;
  binding.device = vk.device;
  binding.queueFamilyIndex = vk.queueFamily;
  binding.queueIndex = 0;
  XrSessionCreateInfo sessionCreate{};
  sessionCreate.type = XR_TYPE_SESSION_CREATE_INFO;
  sessionCreate.next = &binding;
  sessionCreate.systemId = systemId;
  XrSession session = XR_NULL_HANDLE;
  auto xrCreateSession = static_cast<PFN_xrCreateSession>(nullptr);
  gipa(instance, "xrCreateSession", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateSession));
  check(XR_SUCCEEDED(xrCreateSession(instance, &sessionCreate, &session)) &&
            g_mock.rec.createSessionCalls == 1,
        "session created and forwarded");

  auto xrBeginSession = static_cast<PFN_xrBeginSession>(nullptr);
  gipa(instance, "xrBeginSession", reinterpret_cast<PFN_xrVoidFunction*>(&xrBeginSession));
  XrSessionBeginInfo beginInfo{};
  beginInfo.type = XR_TYPE_SESSION_BEGIN_INFO;
  beginInfo.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  check(XR_SUCCEEDED(xrBeginSession(session, &beginInfo)), "session begins");

  if (deviceFirst) {
    // The layer activates on the next vkBeginCommandBuffer after the instance
    // exists. Pipelines created before that have no VRS state and cannot honour an
    // attachment, so recreate it - a real game creates its pipelines lazily, while
    // rendering, which is exactly why late activation helps it.
    triggerLayerActivation(vk);
    vkDeviceWaitIdle(vk.device);
    vkDestroyPipeline(vk.device, vk.pipeline, nullptr);
    vk.pipeline = VK_NULL_HANDLE;
    createGraphicsPipeline(vk);
    check(vk.pipeline != VK_NULL_HANDLE, "pipeline recreated after late activation");
  }

  auto xrEnumerateViews = static_cast<PFN_xrEnumerateViewConfigurationViews>(nullptr);
  gipa(instance, "xrEnumerateViewConfigurationViews",
       reinterpret_cast<PFN_xrVoidFunction*>(&xrEnumerateViews));
  XrViewConfigurationView views[2]{};
  views[0].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
  views[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
  uint32_t viewCount = 0;

  check(XR_SUCCEEDED(xrEnumerateViews(instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                      2, &viewCount, views)) &&
            viewCount == 2 && views[0].recommendedImageRectWidth == g_mock.recommendedWidth,
        "recommended eye size enumerated");

  // The runtime hands out our Vulkan image as the swapchain image.
  g_mock.swapchainImages[0] = vk.target;
  g_mock.swapchainImages[1] = vk.target;
  XrSwapchainCreateInfo swapchainCreate{};
  swapchainCreate.type = XR_TYPE_SWAPCHAIN_CREATE_INFO;
  swapchainCreate.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  swapchainCreate.format = VK_FORMAT_R8_UINT;
  swapchainCreate.width = vk.width;
  swapchainCreate.height = vk.height;
  swapchainCreate.arraySize = 2;
  swapchainCreate.sampleCount = 1;
  XrSwapchain swapchain = XR_NULL_HANDLE;
  auto xrCreateSwapchain = static_cast<PFN_xrCreateSwapchain>(nullptr);
  gipa(instance, "xrCreateSwapchain", reinterpret_cast<PFN_xrVoidFunction*>(&xrCreateSwapchain));
  check(XR_SUCCEEDED(xrCreateSwapchain(session, &swapchainCreate, &swapchain)),
        "swapchain created");

  auto xrEnumerateSwapchainImages = static_cast<PFN_xrEnumerateSwapchainImages>(nullptr);
  gipa(instance, "xrEnumerateSwapchainImages",
       reinterpret_cast<PFN_xrVoidFunction*>(&xrEnumerateSwapchainImages));
  XrSwapchainImageVulkanKHR swapchainImages[2] = {
      {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR, nullptr, VK_NULL_HANDLE},
      {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR, nullptr, VK_NULL_HANDLE}};
  uint32_t imageCount = 0;
  check(XR_SUCCEEDED(xrEnumerateSwapchainImages(
            swapchain, 2, &imageCount, reinterpret_cast<XrSwapchainImageBaseHeader*>(swapchainImages))) &&
            imageCount == 2,
        "swapchain images enumerated (the layer registers them)");

  // --- Action sets: the app suggests/attaches/syncs, the layer merges -------
  auto xrSuggest = static_cast<PFN_xrSuggestInteractionProfileBindings>(nullptr);
  gipa(instance, "xrSuggestInteractionProfileBindings",
       reinterpret_cast<PFN_xrVoidFunction*>(&xrSuggest));
  XrActionSuggestedBinding appBinding{};
  appBinding.action = reinterpret_cast<XrAction>(0x5000);
  appBinding.binding = appOwnsGazeBinding ? g_mock.gazeBindingPath : static_cast<XrPath>(0x99);
  XrInteractionProfileSuggestedBinding suggestion{};
  suggestion.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
  suggestion.interactionProfile = g_mock.gazeProfilePath;  // the profile the gaze uses
  suggestion.countSuggestedBindings = 1;
  suggestion.suggestedBindings = &appBinding;
  const uint32_t expectedSuggest = noEyeGaze ? 1u : (appOwnsGazeBinding ? 1u : 2u);
  if (!appSuggestsGaze) {
    // The layer had to suggest the binding itself when the session was created.
    check(!noEyeGaze && g_mock.rec.suggestCalls >= 1 && g_mock.rec.lastSuggestCount == 1,
          "layer suggested the gaze binding itself (the app never mentions eye gaze)");
  } else {
    check(XR_SUCCEEDED(xrSuggest(instance, &suggestion)) &&
              g_mock.rec.lastSuggestCount == expectedSuggest,
          noEyeGaze ? "app suggestion left untouched (no gaze action set)"
                    : (appOwnsGazeBinding
                           ? "the app's binding is left alone (no duplicate path)"
                           : "gaze binding merged into the app's suggestion for the shared profile"));
  }

  auto xrAttach = static_cast<PFN_xrAttachSessionActionSets>(nullptr);
  gipa(instance, "xrAttachSessionActionSets", reinterpret_cast<PFN_xrVoidFunction*>(&xrAttach));
  XrActionSet appSet = g_mock.appActionSet;
  XrSessionActionSetsAttachInfo attach{};
  attach.type = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
  attach.countActionSets = 1;
  attach.actionSets = &appSet;
  const uint32_t expectedAttach = noEyeGaze ? 1u : 2u;
  check(XR_SUCCEEDED(xrAttach(session, &attach)) && g_mock.rec.lastAttachCount == expectedAttach,
        noEyeGaze ? "only the app's action set is attached"
                  : "gaze action set attached together with the app's");

  auto xrSync = static_cast<PFN_xrSyncActions>(nullptr);
  gipa(instance, "xrSyncActions", reinterpret_cast<PFN_xrVoidFunction*>(&xrSync));
  auto xrWaitFrame = static_cast<PFN_xrWaitFrame>(nullptr);
  gipa(instance, "xrWaitFrame", reinterpret_cast<PFN_xrVoidFunction*>(&xrWaitFrame));
  auto xrBeginFrame = static_cast<PFN_xrBeginFrame>(nullptr);
  gipa(instance, "xrBeginFrame", reinterpret_cast<PFN_xrVoidFunction*>(&xrBeginFrame));
  auto xrLocateViews = static_cast<PFN_xrLocateViews>(nullptr);
  gipa(instance, "xrLocateViews", reinterpret_cast<PFN_xrVoidFunction*>(&xrLocateViews));
  auto xrEndFrame = static_cast<PFN_xrEndFrame>(nullptr);
  gipa(instance, "xrEndFrame", reinterpret_cast<PFN_xrVoidFunction*>(&xrEndFrame));

  XrFrameWaitInfo waitInfo{};
  waitInfo.type = XR_TYPE_FRAME_WAIT_INFO;
  XrFrameBeginInfo frameBegin{};
  frameBegin.type = XR_TYPE_FRAME_BEGIN_INFO;

  // A typical app frame: wait, sync actions, locate views, begin, render, end.
  auto runFrame = [&]() {
    XrFrameState frameState{};
    frameState.type = XR_TYPE_FRAME_STATE;
    if (XR_FAILED(xrWaitFrame(session, &waitInfo, &frameState))) fail("xrWaitFrame");

    XrActiveActionSet active{};
    active.actionSet = appSet;
    XrActionsSyncInfo syncInfo{};
    syncInfo.type = XR_TYPE_ACTIONS_SYNC_INFO;
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &active;
    if (XR_FAILED(xrSync(session, &syncInfo))) fail("xrSyncActions");
    if (g_mock.rec.lastSyncCount != (noEyeGaze ? 1u : 2u)) {
      fail(noEyeGaze ? "the app's action sets must not be replaced"
                     : "gaze action set was not synced with the app's");
    }

    XrViewLocateInfo locateInfo{};
    locateInfo.type = XR_TYPE_VIEW_LOCATE_INFO;
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = frameState.predictedDisplayTime;
    XrViewState viewState{};
    viewState.type = XR_TYPE_VIEW_STATE;
    XrView appViews[2]{};
    appViews[0].type = XR_TYPE_VIEW;
    appViews[1].type = XR_TYPE_VIEW;
    uint32_t located = 0;
    if (XR_FAILED(xrLocateViews(session, &locateInfo, &viewState, 2, &located, appViews)) ||
        located != 2) {
      fail("xrLocateViews");
    }
    if (XR_FAILED(xrBeginFrame(session, &frameBegin))) fail("xrBeginFrame");

    XrCompositionLayerProjectionView projectionViews[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
      projectionViews[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
      projectionViews[i].pose = appViews[i].pose;
      projectionViews[i].fov = appViews[i].fov;
      projectionViews[i].subImage.swapchain = swapchain;
      projectionViews[i].subImage.imageRect.offset = {0, 0};
      projectionViews[i].subImage.imageRect.extent = {(int32_t)vk.width, (int32_t)vk.height};
      projectionViews[i].subImage.imageArrayIndex = i;
    }
    XrCompositionLayerProjection projection{};
    projection.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    projection.viewCount = 2;
    projection.views = projectionViews;
    const XrCompositionLayerBaseHeader* layers[1] = {
        reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
    XrFrameEndInfo endInfo{};
    endInfo.type = XR_TYPE_FRAME_END_INFO;
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = 1;
    endInfo.layers = layers;
    if (XR_FAILED(xrEndFrame(session, &endInfo))) fail("xrEndFrame");
  };

  runFrame();
  check(g_mock.rec.endFrameCalls == 1, "xrEndFrame with the projection slots");
  check(g_mock.rec.locateViewsCalls >= 2, "the layer located the eyes itself");
  if (noEyeGaze) {
    check(g_mock.rec.createActionSetCalls == 0 && g_mock.rec.getActionStatePoseCalls == 0,
          "no gaze action set and no pose query without eye gaze support");
  } else {
    check(g_mock.rec.getActionStatePoseCalls >= 1 && g_mock.rec.locateSpaceCalls >= 1,
          "gaze pose queried from the runtime");
    check(g_mock.rec.lastPoseAction == (appOwnsGazeBinding ? appBinding.action : g_mock.gazeAction),
          appOwnsGazeBinding ? "the app's own gaze action is used when it owns the binding"
                             : "the layer's own gaze action is used");
  }

  // --- The Vulkan half must foveate at the gaze the runtime reported -------
  vkDeviceWaitIdle(vk.device);
  RateMap map = renderAndReadRateMap(vk, 8);
  float cu = 0, cv = 0;
  check(foveaCentroid(map, &cu, &cv), "rate map has a full-rate fovea");
  printf("  center gaze (%.3f, %.3f) -> fovea centroid (%.3f, %.3f)\n", g_mock.gazeU, g_mock.gazeV,
         cu, cv);
  if (noEyeGaze) {
    check(std::fabs(cu - 0.5f) < 0.05f && std::fabs(cv - 0.5f) < 0.05f,
          "fallback: fixed foveation in the centre of the eye image");
  } else {
    check(std::fabs(cu - g_mock.gazeU) < 0.05f && std::fabs(cv - g_mock.gazeV) < 0.05f,
          "fovea is at the gaze reported by the runtime");
  }

  // Move the gaze and run another frame: the fovea must follow.
  g_mock.gazeU = 0.25f;
  g_mock.gazeV = 0.30f;
  g_mock.displayTime += 11000000;
  runFrame();
  vkDeviceWaitIdle(vk.device);

  RateMap map2 = renderAndReadRateMap(vk, 8);
  float cu2 = 0, cv2 = 0;
  check(foveaCentroid(map2, &cu2, &cv2), "rate map still has a full-rate fovea");
  printf("  moved gaze (%.3f, %.3f) -> fovea centroid (%.3f, %.3f)\n", g_mock.gazeU, g_mock.gazeV,
         cu2, cv2);
  if (noEyeGaze) {
    check(std::fabs(cu2 - 0.5f) < 0.05f && std::fabs(cv2 - 0.5f) < 0.05f,
          "fallback: fovea stays in the centre when the runtime reports a gaze");
  } else {
    check(std::fabs(cu2 - g_mock.gazeU) < 0.05f && std::fabs(cv2 - g_mock.gazeV) < 0.05f,
          "fovea follows the new gaze");
  }

  printf("\n%s\n", failed == 0 ? "PASS" : "FAIL");
  return failed == 0 ? 0 : 1;
}
