// Session, swapchain and frame-loop hooks (03 §2-§3, §6).
#include <algorithm>
#include <cstring>

#include "common/guard.h"
#include "common/log.h"
#include "common/shared_state.h"
#include "openxr/xr_dispatch.h"
#include "openxr/xr_hooks.h"
#include "openxr/xr_pure.h"

using namespace vrsfov;

namespace vrsfov {

namespace {

std::atomic<int64_t> g_lastStatsNs{0};

void xrPublishRecommendedSize(uint32_t w, uint32_t h) {
  SharedState::get().update([&](Snapshot& snap) {
    snap.recommendedW = w;
    snap.recommendedH = h;
  });
  LOG_INFO("recommended per-eye size %ux%u", w, h);
}

// 02 §3: a short summary every five seconds, driven by the frame loop.
void xrMaybeLogStats(XrSessionData& session) {
  const int64_t now = nowNs();
  int64_t last = g_lastStatsNs.load(std::memory_order_relaxed);
  if (last != 0 && now - last < 5LL * 1000 * 1000 * 1000) return;
  g_lastStatsNs.store(now, std::memory_order_relaxed);

  SharedState& state = SharedState::get();
  auto snap = state.snapshot();
  const double frames = (double)state.stats.frames.load(std::memory_order_relaxed);
  const double passes = (double)state.stats.passes.load(std::memory_order_relaxed);
  const double foveated = (double)state.stats.foveatedPasses.load(std::memory_order_relaxed);
  const double neutral = (double)state.stats.neutralPasses.load(std::memory_order_relaxed);
  LOG_INFO("stats: %llu frames, %.1f passes/frame (%.1f foveated, %.1f neutral), eye tracking %s, "
           "last gaze (%.3f, %.3f), session %s",
           (unsigned long long)frames, frames > 0 ? passes / frames : 0.0,
           frames > 0 ? foveated / frames : 0.0, frames > 0 ? neutral / frames : 0.0,
           state.stats.eyeTracked.load() ? "active" : "unavailable",
           (double)state.stats.gazeU.load(), (double)state.stats.gazeV.load(),
           session.running ? "running" : "stopped");
}

void destroyGazeObjects(XrSessionData& session) {
  XrInstanceData* instance = session.instance;
  if (!instance) return;
  if (session.appGazeSpace != XR_NULL_HANDLE && instance->d.DestroySpace) {
    instance->d.DestroySpace(session.appGazeSpace);
  }
  if (session.gazeSpace != XR_NULL_HANDLE && instance->d.DestroySpace) {
    instance->d.DestroySpace(session.gazeSpace);
  }
  if (session.gazeAction != XR_NULL_HANDLE) {
    auto& owned = instance->ownGazeActions;
    owned.erase(std::remove(owned.begin(), owned.end(), session.gazeAction), owned.end());
    if (instance->d.DestroyAction) instance->d.DestroyAction(session.gazeAction);
  }
  if (session.gazeSet != XR_NULL_HANDLE && instance->d.DestroyActionSet) {
    instance->d.DestroyActionSet(session.gazeSet);
  }
  if (session.viewSpace != XR_NULL_HANDLE && instance->d.DestroySpace) {
    instance->d.DestroySpace(session.viewSpace);
  }
  session.gazeSpace = XR_NULL_HANDLE;
  session.appGazeSpace = XR_NULL_HANDLE;
  session.appGazeAction = XR_NULL_HANDLE;
  session.gazeAction = XR_NULL_HANDLE;
  session.gazeSet = XR_NULL_HANDLE;
  session.viewSpace = XR_NULL_HANDLE;
  session.gazeSetAttached = false;
  session.gazeBindingsSuggested = false;
}

// 03 §3: the action set, the pose action and the gaze action space.
void createGazeObjects(XrSessionData& session) {
  XrInstanceData& instance = *session.instance;
  XrDispatch& d = instance.d;
  const Config& config = instance.config;
  session.appGazeAction = instance.appGazeAction;

  if (!config.eyeTracking) {
    LOG_INFO("eye tracking disabled in the config; using fixed foveation");
    return;
  }
  if (!instance.addedEyeGazeExtension) {
    LOG_INFO("XR_EXT_eye_gaze_interaction was not enabled; using fixed foveation");
    return;
  }
  if (session.instance->systemId == XR_NULL_SYSTEM_ID && d.GetSystem) {
    XrSystemId id = XR_NULL_SYSTEM_ID;
    XrSystemGetInfo getInfo{};
    getInfo.type = XR_TYPE_SYSTEM_GET_INFO;
    getInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (XR_SUCCEEDED(d.GetSystem(instance.instance, &getInfo, &id))) {
      instance.systemId = id;
    }
  }
  bool supportsEyeGaze = false;
  if (instance.systemId != XR_NULL_SYSTEM_ID && d.GetSystemProperties) {
    XrSystemEyeGazeInteractionPropertiesEXT gazeProps{};
    gazeProps.type = XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT;
    XrSystemProperties props{};
    props.type = XR_TYPE_SYSTEM_PROPERTIES;
    props.next = &gazeProps;
    if (XR_SUCCEEDED(d.GetSystemProperties(instance.instance, instance.systemId, &props))) {
      supportsEyeGaze = gazeProps.supportsEyeGazeInteraction == XR_TRUE;
    }
  }
  if (!supportsEyeGaze) {
    LOG_INFO("runtime does not support XR_EXT_eye_gaze_interaction; using fixed foveation");
    session.gazeUnavailable = true;
    return;
  }
  if (!d.CreateActionSet || !d.CreateAction || !d.CreateActionSpace || !d.StringToPath) {
    LOG_WARN("missing entry points for the gaze action set; using fixed foveation");
    session.gazeUnavailable = true;
    return;
  }

  XrActionSetCreateInfo setInfo{};
  setInfo.type = XR_TYPE_ACTION_SET_CREATE_INFO;
  strncpy(setInfo.actionSetName, "vrsfov_gaze_set", XR_MAX_ACTION_SET_NAME_SIZE - 1);
  strncpy(setInfo.localizedActionSetName, "VRSFov gaze", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
  setInfo.priority = 0;
  XrResult result = d.CreateActionSet(instance.instance, &setInfo, &session.gazeSet);
  if (XR_FAILED(result)) {
    LOG_WARN("xrCreateActionSet failed (%d); using fixed foveation", (int)result);
    session.gazeUnavailable = true;
    return;
  }

  XrActionCreateInfo actionInfo{};
  actionInfo.type = XR_TYPE_ACTION_CREATE_INFO;
  strncpy(actionInfo.actionName, "vrsfov_gaze", XR_MAX_ACTION_NAME_SIZE - 1);
  actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
  strncpy(actionInfo.localizedActionName, "VRSFov gaze", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
  result = d.CreateAction(session.gazeSet, &actionInfo, &session.gazeAction);
  if (XR_FAILED(result)) {
    LOG_WARN("xrCreateAction failed (%d); using fixed foveation", (int)result);
    session.gazeUnavailable = true;
    return;
  }

  XrActionSpaceCreateInfo spaceInfo{};
  spaceInfo.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
  spaceInfo.action = session.gazeAction;
  spaceInfo.poseInActionSpace.orientation.w = 1.0f;
  result = d.CreateActionSpace(session.session, &spaceInfo, &session.gazeSpace);
  if (XR_FAILED(result)) {
    LOG_WARN("xrCreateActionSpace failed (%d); using fixed foveation", (int)result);
    session.gazeUnavailable = true;
    return;
  }

  // Suggest our own binding. Most apps never mention the eye-gaze profile, and
  // an action without a binding always reports isActive=false. This is also why
  // this used to look like "the runtime's gaze action is inert".
  instance.ownGazeActions.push_back(session.gazeAction);
  if (xrSuggestGazeProfile(instance)) {
    session.gazeBindingsSuggested = true;
    LOG_INFO("eye gaze action set created and binding suggested; waiting for the app to attach "
             "action sets");
  } else {
    LOG_WARN("the gaze binding could not be suggested; eye tracking may stay unavailable");
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Config hot reload (02 §2)
// ---------------------------------------------------------------------------

void xrMaybeReloadConfig(XrInstanceData& instance) {
  const int64_t now = nowNs();
  if (now - instance.lastConfigCheckNs < 1000LL * 1000 * 1000) return;
  instance.lastConfigCheckNs = now;
  const int64_t mtime = fileMtimeNs(instance.configPath);
  if (mtime == 0 || mtime == instance.configMtime) return;
  instance.configMtime = mtime;

  const uint64_t generation = instance.config.generation + 1;
  Config reloaded = loadConfigForProcess(instance.appName, generation, instance.configPath);
  LOG_INFO("config reloaded (generation %llu): enabled=%d inner=%s middle=%s outer=%s "
           "radii %.2f/%.2f debug=%d",
           (unsigned long long)generation, (int)reloaded.enabled,
           rateToString(reloaded.innerRate).c_str(), rateToString(reloaded.middleRate).c_str(),
           rateToString(reloaded.outerRate).c_str(), reloaded.innerRadius, reloaded.outerRadius,
           reloaded.debugMode);
  instance.config = reloaded;
  auto config = std::make_shared<const Config>(instance.config);
  SharedState::get().update([&](Snapshot& snap) { snap.config = config; });
}

void xrPublishSessionState(XrSessionData& session, XrInstanceData& instance) {
  auto swapchains = std::make_shared<const SwapchainMap>(session.imagesByVkImage);
  auto config = std::make_shared<const Config>(instance.config);
  const uint64_t device = session.hasVulkanBinding ? session.vkDevice : 0;
  const bool running = session.running;
  SharedState::get().update([&](Snapshot& snap) {
    snap.sessionVkDevice = device;
    snap.sessionRunning = running;
    snap.swapchainByVkImage = swapchains;
    snap.config = config;
  });
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrEnumerateViewConfigurationViews(
    XrInstance instance, XrSystemId systemId, XrViewConfigurationType viewConfigurationType,
    uint32_t viewCapacityInput, uint32_t* viewCountOutput, XrViewConfigurationView* views) {
  VRSFOV_GUARD_BEGIN
  XrInstanceData* data = findXrInstance(instance);
  if (!data || !data->d.EnumerateViewConfigurationViews) return XR_ERROR_HANDLE_INVALID;
  XrResult result = data->d.EnumerateViewConfigurationViews(
      instance, systemId, viewConfigurationType, viewCapacityInput, viewCountOutput, views);
  if (XR_SUCCEEDED(result) && data->enabled && views && viewCountOutput && *viewCountOutput > 0 &&
      viewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
    xrPublishRecommendedSize(views[0].recommendedImageRectWidth,
                             views[0].recommendedImageRectHeight);
  }
  return result;
  VRSFOV_GUARD_END(XR_ERROR_INITIALIZATION_FAILED)
}

XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrCreateSession(XrInstance instance,
                                                     const XrSessionCreateInfo* createInfo,
                                                     XrSession* session) {
  VRSFOV_GUARD_BEGIN
  XrInstanceData* data = findXrInstance(instance);
  if (!data || !data->d.CreateSession) return XR_ERROR_HANDLE_INVALID;

  GraphicsBindingInfo binding = findVulkanBinding(createInfo->next);
  XrResult result = data->d.CreateSession(instance, createInfo, session);
  if (XR_FAILED(result)) return result;

  auto sessionData = std::make_unique<XrSessionData>();
  sessionData->session = *session;
  sessionData->instance = data;
  sessionData->hasVulkanBinding = binding.present;
  sessionData->vkDevice = binding.vkDevice;
  sessionData->vkPhysicalDevice = binding.vkPhysicalDevice;
  sessionData->queueFamilyIndex = binding.queueFamilyIndex;

  if (!binding.present) {
    LOG_WARN("session %p has no Vulkan graphics binding; foveation will not apply",
             reinterpret_cast<void*>(*session));
  } else {
    LOG_INFO("session %p created with VkDevice %p (queue family %u)",
             reinterpret_cast<void*>(*session), reinterpret_cast<void*>(binding.vkDevice),
             binding.queueFamilyIndex);
  }

  // Our own VIEW reference space, used to locate the eyes and the gaze.
  if (data->d.CreateReferenceSpace) {
    XrReferenceSpaceCreateInfo spaceInfo{};
    spaceInfo.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    XrResult spaceResult =
        data->d.CreateReferenceSpace(*session, &spaceInfo, &sessionData->viewSpace);
    if (XR_FAILED(spaceResult)) {
      LOG_WARN("xrCreateReferenceSpace(VIEW) failed (%d)", (int)spaceResult);
      sessionData->viewSpace = XR_NULL_HANDLE;
    }
  }

  XrSessionData* stored = sessionData.get();
  addXrSession(std::move(sessionData));
  xrPublishSessionState(*stored, *data);
  createGazeObjects(*stored);
  return XR_SUCCESS;
  VRSFOV_GUARD_END(XR_ERROR_INITIALIZATION_FAILED)
}

XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrDestroySession(XrSession session) {
  VRSFOV_GUARD_BEGIN
  XrSessionData* raw = findXrSession(session);
  if (!raw) {
    LOG_WARN("xrDestroySession: unknown session %p", reinterpret_cast<void*>(session));
    return XR_ERROR_HANDLE_INVALID;
  }
  XrInstanceData* instance = raw->instance;
  destroyGazeObjects(*raw);
  std::unique_ptr<XrSessionData> data = removeXrSession(session);

  SharedState::get().update([&](Snapshot& snap) {
    snap.sessionRunning = false;
    snap.sessionVkDevice = 0;
    snap.swapchainByVkImage = std::make_shared<const SwapchainMap>();
    snap.slots = std::make_shared<const SlotList>();
  });
  if (!instance || !instance->d.DestroySession) return XR_ERROR_HANDLE_INVALID;
  return instance->d.DestroySession(session);
  VRSFOV_GUARD_END(XR_ERROR_INITIALIZATION_FAILED)
}

XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrBeginSession(XrSession session,
                                                    const XrSessionBeginInfo* beginInfo) {
  VRSFOV_GUARD_BEGIN
  XrSessionData* data = findXrSession(session);
  if (!data || !data->instance || !data->instance->d.BeginSession) return XR_ERROR_HANDLE_INVALID;
  XrResult result = data->instance->d.BeginSession(session, beginInfo);
  if (XR_SUCCEEDED(result)) {
    data->running = true;
    if (beginInfo && beginInfo->primaryViewConfigurationType != 0) {
      data->viewConfig = beginInfo->primaryViewConfigurationType;
    }
    xrPublishSessionState(*data, *data->instance);
    LOG_INFO("session %p running", reinterpret_cast<void*>(session));
  }
  return result;
  VRSFOV_GUARD_END(XR_ERROR_INITIALIZATION_FAILED)
}

XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrEndSession(XrSession session) {
  VRSFOV_GUARD_BEGIN
  XrSessionData* data = findXrSession(session);
  if (!data || !data->instance || !data->instance->d.EndSession) return XR_ERROR_HANDLE_INVALID;
  XrResult result = data->instance->d.EndSession(session);
  if (XR_SUCCEEDED(result)) {
    data->running = false;
    xrPublishSessionState(*data, *data->instance);
    LOG_INFO("session %p stopped", reinterpret_cast<void*>(session));
  }
  return result;
  VRSFOV_GUARD_END(XR_ERROR_INITIALIZATION_FAILED)
}

XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrCreateSwapchain(XrSession session,
                                                       const XrSwapchainCreateInfo* createInfo,
                                                       XrSwapchain* swapchain) {
  VRSFOV_GUARD_BEGIN
  XrSessionData* sessionData = findXrSession(session);
  if (!sessionData || !sessionData->instance || !sessionData->instance->d.CreateSwapchain) {
    return XR_ERROR_HANDLE_INVALID;
  }
  XrResult result = sessionData->instance->d.CreateSwapchain(session, createInfo, swapchain);
  if (XR_FAILED(result)) return result;

  auto data = std::make_unique<XrSwapchainData>();
  data->swapchain = *swapchain;
  data->session = sessionData;
  data->width = createInfo->width;
  data->height = createInfo->height;
  data->arraySize = createInfo->arraySize;
  data->format = createInfo->format;
  data->registersImages =
      (createInfo->usageFlags & (XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                                 XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) != 0;
  const bool registers = data->registersImages;
  addXrSwapchain(std::move(data));
  LOG_DEBUG("swapchain %p %ux%u x%u format %lld usage 0x%llx (register=%d)",
            reinterpret_cast<void*>(*swapchain), createInfo->width, createInfo->height,
            createInfo->arraySize, (long long)createInfo->format,
            (unsigned long long)createInfo->usageFlags, (int)registers);
  return XR_SUCCESS;
  VRSFOV_GUARD_END(XR_ERROR_INITIALIZATION_FAILED)
}

XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrEnumerateSwapchainImages(XrSwapchain swapchain,
                                                                uint32_t imageCapacityInput,
                                                                uint32_t* imageCountOutput,
                                                                XrSwapchainImageBaseHeader* images) {
  VRSFOV_GUARD_BEGIN
  XrSwapchainData* data = findXrSwapchain(swapchain);
  if (!data || !data->session || !data->session->instance ||
      !data->session->instance->d.EnumerateSwapchainImages) {
    return XR_ERROR_HANDLE_INVALID;
  }
  XrResult result = data->session->instance->d.EnumerateSwapchainImages(
      swapchain, imageCapacityInput, imageCountOutput, images);
  if (XR_FAILED(result)) return result;

  if (data->registersImages && images && imageCountOutput && *imageCountOutput > 0 &&
      images->type == XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) {
    XrSessionData& session = *data->session;
    for (uint32_t i = 0; i < *imageCountOutput; ++i) {
      const auto* vkImage = reinterpret_cast<const XrSwapchainImageVulkanKHR*>(images + i);
      if (vkImage->type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR || !vkImage->image) continue;
      SwapchainInfo info;
      info.swapchain = reinterpret_cast<uint64_t>(swapchain);
      info.width = data->width;
      info.height = data->height;
      info.arraySize = data->arraySize;
      info.format = data->format;
      session.imagesByVkImage[reinterpret_cast<uint64_t>(vkImage->image)] = info;
    }
    xrPublishSessionState(session, *session.instance);
    LOG_DEBUG("registered %u swapchain image(s) of %p", *imageCountOutput,
              reinterpret_cast<void*>(swapchain));
  }
  return result;
  VRSFOV_GUARD_END(XR_ERROR_INITIALIZATION_FAILED)
}

XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrDestroySwapchain(XrSwapchain swapchain) {
  VRSFOV_GUARD_BEGIN
  XrSwapchainData* raw = findXrSwapchain(swapchain);
  if (!raw) {
    LOG_WARN("xrDestroySwapchain: unknown swapchain %p", reinterpret_cast<void*>(swapchain));
    return XR_ERROR_HANDLE_INVALID;
  }
  XrSessionData* session = raw->session;
  XrInstanceData* instance = session ? session->instance : nullptr;
  std::unique_ptr<XrSwapchainData> data = removeXrSwapchain(swapchain);

  if (session) {
    const uint64_t handle = reinterpret_cast<uint64_t>(swapchain);
    for (auto it = session->imagesByVkImage.begin(); it != session->imagesByVkImage.end();) {
      if (it->second.swapchain == handle) {
        it = session->imagesByVkImage.erase(it);
      } else {
        ++it;
      }
    }
    if (instance) xrPublishSessionState(*session, *instance);
  }
  if (!instance || !instance->d.DestroySwapchain) return XR_ERROR_HANDLE_INVALID;
  return instance->d.DestroySwapchain(swapchain);
  VRSFOV_GUARD_END(XR_ERROR_INITIALIZATION_FAILED)
}

XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrWaitFrame(XrSession session,
                                                 const XrFrameWaitInfo* frameWaitInfo,
                                                 XrFrameState* frameState) {
  VRSFOV_GUARD_BEGIN
  XrSessionData* data = findXrSession(session);
  if (!data || !data->instance || !data->instance->d.WaitFrame) return XR_ERROR_HANDLE_INVALID;
  XrInstanceData& instance = *data->instance;
  if (instance.enabled) xrMaybeReloadConfig(instance);
  return instance.d.WaitFrame(session, frameWaitInfo, frameState);
  VRSFOV_GUARD_END(XR_ERROR_INITIALIZATION_FAILED)
}

XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrBeginFrame(XrSession session,
                                                  const XrFrameBeginInfo* frameBeginInfo) {
  VRSFOV_GUARD_BEGIN
  XrSessionData* data = findXrSession(session);
  if (!data || !data->instance || !data->instance->d.BeginFrame) return XR_ERROR_HANDLE_INVALID;
  XrResult result = data->instance->d.BeginFrame(session, frameBeginInfo);
  if (XR_SUCCEEDED(result) && data->instance->enabled && data->running) {
    // The app may not call xrLocateViews every frame.
    if (data->lastGazeTime != 0 && data->lastGazeTime != data->predictedDisplayTime) {
      xrUpdateGaze(*data, data->predictedDisplayTime);
    }
  }
  return result;
  VRSFOV_GUARD_END(XR_ERROR_INITIALIZATION_FAILED)
}

XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrLocateViews(XrSession session,
                                                   const XrViewLocateInfo* viewLocateInfo,
                                                   XrViewState* viewState, uint32_t viewCapacityInput,
                                                   uint32_t* viewCountOutput, XrView* views) {
  VRSFOV_GUARD_BEGIN
  XrSessionData* data = findXrSession(session);
  if (!data || !data->instance || !data->instance->d.LocateViews) return XR_ERROR_HANDLE_INVALID;
  XrResult result =
      data->instance->d.LocateViews(session, viewLocateInfo, viewState, viewCapacityInput,
                                    viewCountOutput, views);
  if (XR_SUCCEEDED(result) && data->instance->enabled && data->running && viewLocateInfo) {
    data->predictedDisplayTime = viewLocateInfo->displayTime;
    if (data->lastGazeTime != data->predictedDisplayTime) {
      xrUpdateGaze(*data, data->predictedDisplayTime);
    }
  }
  return result;
  VRSFOV_GUARD_END(XR_ERROR_INITIALIZATION_FAILED)
}

XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrEndFrame(XrSession session,
                                                const XrFrameEndInfo* frameEndInfo) {
  VRSFOV_GUARD_BEGIN
  XrSessionData* data = findXrSession(session);
  if (!data || !data->instance || !data->instance->d.EndFrame) return XR_ERROR_HANDLE_INVALID;
  XrInstanceData& instance = *data->instance;

  if (instance.enabled && frameEndInfo) {
    SlotList slots;
    if (extractProjectionSlots(*frameEndInfo, slots)) {
      auto published = std::make_shared<const SlotList>(slots);
      SharedState::get().update([&](Snapshot& snap) {
        const SlotList& old = snap.slots ? *snap.slots : SlotList{};
        bool same = old.size() == slots.size();
        if (same) {
          for (size_t i = 0; i < slots.size(); ++i) {
            const ProjectionSlot& a = old[i];
            const ProjectionSlot& b = slots[i];
            if (a.swapchain != b.swapchain || a.arrayIndex != b.arrayIndex || a.eye != b.eye ||
                a.x != b.x || a.y != b.y || a.w != b.w || a.h != b.h) {
              same = false;
              break;
            }
          }
        }
        if (!same) {
          snap.slots = published;
          LOG_DEBUG("projection slots: %zu view(s) published", slots.size());
        }
        ++snap.frameIndex;
      });
    } else {
      SharedState::get().update([&](Snapshot& snap) { ++snap.frameIndex; });
    }
    SharedState::get().stats.frames.fetch_add(1, std::memory_order_relaxed);
    xrMaybeLogStats(*data);
  }
  return instance.d.EndFrame(session, frameEndInfo);
  VRSFOV_GUARD_END(XR_ERROR_INITIALIZATION_FAILED)
}

}  // namespace vrsfov
