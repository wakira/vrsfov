// OpenXR API layer: negotiation, instance creation and GetProcAddr routing (03 §1).
#include "common/export.h"
#include "common/guard.h"
#include "common/log.h"
#include "common/shared_state.h"
#include "openxr/xr_dispatch.h"
#include "openxr/xr_hooks.h"
#include "openxr/xr_pure.h"

#include <openxr/openxr_loader_negotiation.h>

#include <cstring>
#include <string>
#include <vector>

using namespace vrsfov;

extern "C" {

VRSFOV_EXPORT XRAPI_ATTR XrResult XRAPI_CALL
xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loaderInfo, const char* layerName,
                                   XrNegotiateApiLayerRequest* request) {
  log::init();
  LOG_INFO("OpenXR layer negotiation requested (layer '%s')", layerName ? layerName : "");
  if (!loaderInfo || !request) return XR_ERROR_INITIALIZATION_FAILED;
  if (loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO) {
    return XR_ERROR_INITIALIZATION_FAILED;
  }
  if (loaderInfo->structVersion != XR_LOADER_INFO_STRUCT_VERSION) {
    return XR_ERROR_INITIALIZATION_FAILED;
  }
  if (loaderInfo->structSize != sizeof(XrNegotiateLoaderInfo)) {
    return XR_ERROR_INITIALIZATION_FAILED;
  }
  if (!(loaderInfo->minInterfaceVersion <= XR_CURRENT_LOADER_API_LAYER_VERSION &&
        loaderInfo->maxInterfaceVersion >= XR_CURRENT_LOADER_API_LAYER_VERSION)) {
    return XR_ERROR_INITIALIZATION_FAILED;
  }
  if (!(loaderInfo->minApiVersion <= XR_CURRENT_API_VERSION &&
        loaderInfo->maxApiVersion >= XR_CURRENT_API_VERSION)) {
    return XR_ERROR_INITIALIZATION_FAILED;
  }

  request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
  request->layerApiVersion = XR_CURRENT_API_VERSION;
  request->getInstanceProcAddr = vrsfov::vrsfov_xrGetInstanceProcAddr;
  request->createApiLayerInstance = vrsfov::vrsfov_xrCreateApiLayerInstance;
  return XR_SUCCESS;
}

}  // extern "C"

namespace vrsfov {

namespace {

// Kept so the diagnostic passthrough (VRSFOV_DISABLE_XR=1) can forward
// instead of failing, exactly like a layer that installs no hooks.
PFN_xrGetInstanceProcAddr g_nextLayerGipa = nullptr;

const NamedXrHook kHooks[] = {
    {"xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction>(vrsfov_xrDestroyInstance)},
    {"xrGetSystem", reinterpret_cast<PFN_xrVoidFunction>(vrsfov_xrGetSystem)},
    {"xrEnumerateViewConfigurationViews",
     reinterpret_cast<PFN_xrVoidFunction>(vrsfov_xrEnumerateViewConfigurationViews)},
    {"xrCreateSession", reinterpret_cast<PFN_xrVoidFunction>(vrsfov_xrCreateSession)},
    {"xrDestroySession", reinterpret_cast<PFN_xrVoidFunction>(vrsfov_xrDestroySession)},
    {"xrBeginSession", reinterpret_cast<PFN_xrVoidFunction>(vrsfov_xrBeginSession)},
    {"xrEndSession", reinterpret_cast<PFN_xrVoidFunction>(vrsfov_xrEndSession)},
    {"xrCreateSwapchain", reinterpret_cast<PFN_xrVoidFunction>(vrsfov_xrCreateSwapchain)},
    {"xrEnumerateSwapchainImages",
     reinterpret_cast<PFN_xrVoidFunction>(vrsfov_xrEnumerateSwapchainImages)},
    {"xrDestroySwapchain", reinterpret_cast<PFN_xrVoidFunction>(vrsfov_xrDestroySwapchain)},
    {"xrSuggestInteractionProfileBindings",
     reinterpret_cast<PFN_xrVoidFunction>(vrsfov_xrSuggestInteractionProfileBindings)},
    {"xrAttachSessionActionSets",
     reinterpret_cast<PFN_xrVoidFunction>(vrsfov_xrAttachSessionActionSets)},
    {"xrSyncActions", reinterpret_cast<PFN_xrVoidFunction>(vrsfov_xrSyncActions)},
    {"xrWaitFrame", reinterpret_cast<PFN_xrVoidFunction>(vrsfov_xrWaitFrame)},
    {"xrLocateViews", reinterpret_cast<PFN_xrVoidFunction>(vrsfov_xrLocateViews)},
    {"xrBeginFrame", reinterpret_cast<PFN_xrVoidFunction>(vrsfov_xrBeginFrame)},
    {"xrEndFrame", reinterpret_cast<PFN_xrVoidFunction>(vrsfov_xrEndFrame)},
};

bool hasExtension(const XrInstanceCreateInfo* ci, const char* name) {
  for (uint32_t i = 0; i < ci->enabledExtensionCount; ++i) {
    if (ci->enabledExtensionNames && ci->enabledExtensionNames[i] &&
        strcmp(ci->enabledExtensionNames[i], name) == 0) {
      return true;
    }
  }
  return false;
}

bool runtimeHasExtension(PFN_xrGetInstanceProcAddr gipa, const char* name) {
  if (!gipa) return false;
  PFN_xrVoidFunction raw = nullptr;
  if (XR_FAILED(gipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", &raw)) || !raw) {
    return false;
  }
  auto enumerate = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(raw);
  uint32_t count = 0;
  if (XR_FAILED(enumerate(nullptr, 0, &count, nullptr))) return false;
  std::vector<XrExtensionProperties> props(count);
  for (auto& p : props) p.type = XR_TYPE_EXTENSION_PROPERTIES;
  if (count && XR_FAILED(enumerate(nullptr, count, &count, props.data()))) return false;
  for (const auto& p : props) {
    if (strcmp(p.extensionName, name) == 0) return true;
  }
  return false;
}

void resolveDispatch(XrInstanceData& data) {
  XrInstance instance = data.instance;
  PFN_xrGetInstanceProcAddr gipa = data.d.GetInstanceProcAddr;
  auto resolve = [&](const char* name) -> PFN_xrVoidFunction {
    PFN_xrVoidFunction fn = nullptr;
    if (XR_FAILED(gipa(instance, name, &fn))) return nullptr;
    return fn;
  };

  data.d.DestroyInstance =
      reinterpret_cast<PFN_xrDestroyInstance>(resolve("xrDestroyInstance"));
  data.d.GetInstanceProperties =
      reinterpret_cast<PFN_xrGetInstanceProperties>(resolve("xrGetInstanceProperties"));
  data.d.GetSystem = reinterpret_cast<PFN_xrGetSystem>(resolve("xrGetSystem"));
  data.d.EnumerateViewConfigurationViews = reinterpret_cast<PFN_xrEnumerateViewConfigurationViews>(
      resolve("xrEnumerateViewConfigurationViews"));
  data.d.GetSystemProperties =
      reinterpret_cast<PFN_xrGetSystemProperties>(resolve("xrGetSystemProperties"));
  data.d.StringToPath = reinterpret_cast<PFN_xrStringToPath>(resolve("xrStringToPath"));
  data.d.CreateSession = reinterpret_cast<PFN_xrCreateSession>(resolve("xrCreateSession"));
  data.d.DestroySession = reinterpret_cast<PFN_xrDestroySession>(resolve("xrDestroySession"));
  data.d.BeginSession = reinterpret_cast<PFN_xrBeginSession>(resolve("xrBeginSession"));
  data.d.EndSession = reinterpret_cast<PFN_xrEndSession>(resolve("xrEndSession"));
  data.d.CreateSwapchain = reinterpret_cast<PFN_xrCreateSwapchain>(resolve("xrCreateSwapchain"));
  data.d.EnumerateSwapchainImages =
      reinterpret_cast<PFN_xrEnumerateSwapchainImages>(resolve("xrEnumerateSwapchainImages"));
  data.d.DestroySwapchain = reinterpret_cast<PFN_xrDestroySwapchain>(resolve("xrDestroySwapchain"));
  data.d.CreateActionSet = reinterpret_cast<PFN_xrCreateActionSet>(resolve("xrCreateActionSet"));
  data.d.DestroyActionSet = reinterpret_cast<PFN_xrDestroyActionSet>(resolve("xrDestroyActionSet"));
  data.d.CreateAction = reinterpret_cast<PFN_xrCreateAction>(resolve("xrCreateAction"));
  data.d.DestroyAction = reinterpret_cast<PFN_xrDestroyAction>(resolve("xrDestroyAction"));
  data.d.SuggestInteractionProfileBindings = reinterpret_cast<PFN_xrSuggestInteractionProfileBindings>(
      resolve("xrSuggestInteractionProfileBindings"));
  data.d.AttachSessionActionSets = reinterpret_cast<PFN_xrAttachSessionActionSets>(
      resolve("xrAttachSessionActionSets"));
  data.d.SyncActions = reinterpret_cast<PFN_xrSyncActions>(resolve("xrSyncActions"));
  data.d.GetActionStatePose =
      reinterpret_cast<PFN_xrGetActionStatePose>(resolve("xrGetActionStatePose"));
  data.d.CreateActionSpace =
      reinterpret_cast<PFN_xrCreateActionSpace>(resolve("xrCreateActionSpace"));
  data.d.DestroySpace = reinterpret_cast<PFN_xrDestroySpace>(resolve("xrDestroySpace"));
  data.d.LocateSpace = reinterpret_cast<PFN_xrLocateSpace>(resolve("xrLocateSpace"));
  data.d.CreateReferenceSpace =
      reinterpret_cast<PFN_xrCreateReferenceSpace>(resolve("xrCreateReferenceSpace"));
  data.d.LocateViews = reinterpret_cast<PFN_xrLocateViews>(resolve("xrLocateViews"));
  data.d.WaitFrame = reinterpret_cast<PFN_xrWaitFrame>(resolve("xrWaitFrame"));
  data.d.BeginFrame = reinterpret_cast<PFN_xrBeginFrame>(resolve("xrBeginFrame"));
  data.d.EndFrame = reinterpret_cast<PFN_xrEndFrame>(resolve("xrEndFrame"));
}

}  // namespace

PFN_xrVoidFunction hookedXrFn(const char* name) {
  for (const auto& hook : kHooks) {
    if (strcmp(hook.name, name) == 0) return hook.fn;
  }
  return nullptr;
}

XRAPI_ATTR XrResult XRAPI_CALL
vrsfov_xrCreateApiLayerInstance(const XrInstanceCreateInfo* createInfo,
                               const XrApiLayerCreateInfo* layerInfo, XrInstance* instance) {
  VRSFOV_GUARD_BEGIN
  if (!layerInfo || !layerInfo->nextInfo || !instance) return XR_ERROR_INITIALIZATION_FAILED;
  XrApiLayerNextInfo* nextInfo = layerInfo->nextInfo;
  PFN_xrGetInstanceProcAddr nextGipa = nextInfo->nextGetInstanceProcAddr;
  if (!nextGipa) return XR_ERROR_INITIALIZATION_FAILED;

  g_nextLayerGipa = nextGipa;
  if (envFlag("VRSFOV_DISABLE_XR")) {
    XrApiLayerCreateInfo passthrough = *layerInfo;
    passthrough.nextInfo = nextInfo->next;
    XrResult r = nextInfo->nextCreateApiLayerInstance(createInfo, &passthrough, instance);
    LOG_INFO("OpenXR hooks disabled (VRSFOV_DISABLE_XR=1): instance %p passed through (%d)",
             reinterpret_cast<void*>(instance ? *instance : XR_NULL_HANDLE), (int)r);
    return r;
  }

  const char* appName = createInfo->applicationInfo.applicationName;
  const std::string configPath = defaultConfigPath();
  Config config = loadConfigForProcess(appName, 0, configPath);
  LOG_INFO("config %s: %s (rates are the configured ones, the device clamps them later)",
           configPath.c_str(), configSummary(config).c_str());

  // Ask for the eye gaze extension when the runtime has it and the app has not
  // already enabled it (03 §1 step 3).
  XrInstanceCreateInfo modified = *createInfo;
  std::vector<const char*> extensions;
  bool addedEyeGaze = false;
  if (config.enabled && config.eyeTracking &&
      !hasExtension(createInfo, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) &&
      runtimeHasExtension(nextGipa, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)) {
    extensions.assign(createInfo->enabledExtensionNames,
                      createInfo->enabledExtensionNames + createInfo->enabledExtensionCount);
    extensions.push_back(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
    modified.enabledExtensionNames = extensions.data();
    modified.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    addedEyeGaze = true;
  }

  XrApiLayerCreateInfo next = *layerInfo;
  next.nextInfo = nextInfo->next;

  XrInstance created = XR_NULL_HANDLE;
  XrResult result = nextInfo->nextCreateApiLayerInstance(&modified, &next, &created);
  if (XR_FAILED(result) && addedEyeGaze) {
    LOG_WARN("instance creation failed with %s (%d); retrying without it",
             XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME, (int)result);
    addedEyeGaze = false;
    created = XR_NULL_HANDLE;
    result = nextInfo->nextCreateApiLayerInstance(createInfo, &next, &created);
  }
  if (XR_FAILED(result)) {
    LOG_WARN("xrCreateApiLayerInstance failed: %d", (int)result);
    return result;
  }

  auto data = std::make_unique<XrInstanceData>();
  data->instance = created;
  data->config = config;
  data->enabled = config.enabled;
  data->addedEyeGazeExtension = addedEyeGaze;
  data->appName = appName;
  data->configPath = configPath;
  data->configMtime = fileMtimeNs(configPath);
  data->d.GetInstanceProcAddr = nextGipa;
  resolveDispatch(*data);
  SharedState::get().liveXrInstances.fetch_add(1);
  addXrInstance(std::move(data));

  if (const XrInstanceData* stored = findXrInstance(created)) {
    if (stored->d.GetInstanceProperties) {
      XrInstanceProperties props{};
      props.type = XR_TYPE_INSTANCE_PROPERTIES;
      if (XR_SUCCEEDED(stored->d.GetInstanceProperties(created, &props))) {
        LOG_INFO("runtime: %s %u.%u.%u", props.runtimeName, XR_VERSION_MAJOR(props.runtimeVersion),
                 XR_VERSION_MINOR(props.runtimeVersion), XR_VERSION_PATCH(props.runtimeVersion));
      }
    }
  }
  LOG_INFO("instance %p created for app '%s' (enabled=%d eyeTracking=%d gazeExt=%d); "
           "SharedState @ %p",
           reinterpret_cast<void*>(created), appName, (int)config.enabled,
           (int)config.eyeTracking, (int)addedEyeGaze,
           static_cast<const void*>(&SharedState::get()));
  *instance = created;
  return XR_SUCCESS;
  VRSFOV_GUARD_END(XR_ERROR_INITIALIZATION_FAILED)
}

XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrGetInstanceProcAddr(XrInstance instance, const char* name,
                                                           PFN_xrVoidFunction* function) {
  if (!name || !function) return XR_ERROR_VALIDATION_FAILURE;
  *function = nullptr;
  if (strcmp(name, "xrGetInstanceProcAddr") == 0) {
    *function = reinterpret_cast<PFN_xrVoidFunction>(vrsfov_xrGetInstanceProcAddr);
    return XR_SUCCESS;
  }

  XrInstanceData* data = findXrInstance(instance);
  if (!data) {
    // Not one of our instances (or a pre-instance lookup): the loader falls back.
    return XR_ERROR_HANDLE_INVALID;
  }
  PFN_xrVoidFunction next = nullptr;
  XrResult result = data->d.GetInstanceProcAddr(instance, name, &next);
  if (XR_FAILED(result)) return result;
  PFN_xrVoidFunction hook = hookedXrFn(name);
  if (hook && next) {
    *function = hook;
    return XR_SUCCESS;
  }
  *function = next;
  return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrDestroyInstance(XrInstance instance) {
  VRSFOV_GUARD_BEGIN
  std::unique_ptr<XrInstanceData> data = removeXrInstance(instance);
  if (!data) {
    LOG_WARN("xrDestroyInstance: unknown instance %p", reinterpret_cast<void*>(instance));
    return XR_ERROR_HANDLE_INVALID;
  }
  SharedState::get().liveXrInstances.fetch_sub(1);
  XrResult result = XR_SUCCESS;
  if (data->d.DestroyInstance) result = data->d.DestroyInstance(instance);
  return result;
  VRSFOV_GUARD_END(XR_ERROR_INITIALIZATION_FAILED)
}

XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrGetSystem(XrInstance instance,
                                                 const XrSystemGetInfo* getInfo,
                                                 XrSystemId* systemId) {
  VRSFOV_GUARD_BEGIN
  XrInstanceData* data = findXrInstance(instance);
  if (!data || !data->d.GetSystem) return XR_ERROR_HANDLE_INVALID;
  XrResult result = data->d.GetSystem(instance, getInfo, systemId);
  if (XR_SUCCEEDED(result) && systemId && *systemId != XR_NULL_SYSTEM_ID) {
    data->systemId = *systemId;
  }
  return result;
  VRSFOV_GUARD_END(XR_ERROR_INITIALIZATION_FAILED)
}

}  // namespace vrsfov
