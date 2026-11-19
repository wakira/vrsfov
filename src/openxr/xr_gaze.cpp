// Eye gaze interaction: action set attachment, action sync, action system
// merging and the gaze -> snapshot update (03 §4-§5).
#include <atomic>
#include <vector>

#include "common/guard.h"
#include "common/log.h"
#include "common/shared_state.h"
#include "openxr/xr_dispatch.h"
#include "openxr/xr_hooks.h"
#include "openxr/xr_pure.h"

using namespace vrsfov;

namespace vrsfov {

namespace {

constexpr const char* kEyeGazeProfile = "/interaction_profiles/ext/eye_gaze_interaction";
constexpr const char* kEyeGazeBinding = "/user/eyes_ext/input/gaze_ext/pose";

EyeGazePaths gazePaths(XrInstanceData& instance) {
  EyeGazePaths paths;
  if (!instance.d.StringToPath) return paths;
  if (XR_FAILED(instance.d.StringToPath(instance.instance, kEyeGazeProfile, &paths.profile))) {
    paths.profile = XR_NULL_PATH;
  }
  if (XR_FAILED(instance.d.StringToPath(instance.instance, kEyeGazeBinding, &paths.binding))) {
    paths.binding = XR_NULL_PATH;
  }
  return paths;
}

Quat quatFromXr(const XrQuaternionf& q) { return Quat{q.x, q.y, q.z, q.w}; }

// Suggests the gaze profile once for every gaze action this layer owns. The app
// may have suggested the same profile itself; those bindings are preserved and
// ours are merged in. If the app already owns the binding path, the OpenXR spec
// forbids adding a second binding for it, so our action cannot be bound.
bool suggestGazeProfile(XrInstanceData& instance) {
  if (!instance.d.SuggestInteractionProfileBindings || instance.ownGazeActions.empty()) {
    return false;
  }
  const EyeGazePaths paths = gazePaths(instance);
  if (paths.profile == XR_NULL_PATH || paths.binding == XR_NULL_PATH) return false;

  std::vector<XrActionSuggestedBinding> merged = instance.appGazeBindings;
  bool appOwnsBinding = false;
  for (const XrActionSuggestedBinding& b : merged) {
    if (b.binding == paths.binding) appOwnsBinding = true;
  }
  bool added = false;
  if (!appOwnsBinding) {
    for (XrAction action : instance.ownGazeActions) {
      if (action == XR_NULL_HANDLE) continue;
      merged.push_back(XrActionSuggestedBinding{action, paths.binding});
      added = true;
    }
  }
  if (merged.empty()) return false;

  XrInteractionProfileSuggestedBinding suggestion{};
  suggestion.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
  suggestion.interactionProfile = paths.profile;
  suggestion.countSuggestedBindings = (uint32_t)merged.size();
  suggestion.suggestedBindings = merged.data();
  const XrResult result =
      instance.d.SuggestInteractionProfileBindings(instance.instance, &suggestion);
  if (XR_FAILED(result)) {
    LOG_WARN("suggesting %s failed (%d); eye tracking will stay unavailable", kEyeGazeProfile,
             (int)result);
    return false;
  }
  if (appOwnsBinding) {
    LOG_ONCE(INFO, "gaze-binding-taken",
             "the app already suggests %s; our gaze action cannot use the same binding, so "
             "eye tracking stays unavailable", kEyeGazeBinding);
  } else if (added) {
    LOG_DEBUG("suggested %s for %zu gaze action(s)", kEyeGazeProfile,
              instance.ownGazeActions.size());
    for (XrSessionData* session : xrSessionsForInstance(&instance)) {
      if (session->gazeAction != XR_NULL_HANDLE) session->gazeBindingsSuggested = true;
    }
  }
  return true;
}

void publishEyes(XrSessionData& session, const float u[2], const float v[2], bool valid,
                 bool eyeTracked) {
  auto config = std::make_shared<const Config>(session.instance->config);
  const uint64_t device = session.hasVulkanBinding ? session.vkDevice : 0;
  const bool running = session.running;
  SharedState::get().update([&](Snapshot& snap) {
    snap.sessionVkDevice = device;
    snap.sessionRunning = running;
    for (int i = 0; i < 2; ++i) {
      snap.eyes[i].valid = valid;
      snap.eyes[i].u = u[i];
      snap.eyes[i].v = v[i];
      snap.eyes[i].eyeTracked = eyeTracked;
    }
    snap.config = config;
    snap.publishTimeNs = nowNs();
  });

  Stats& stats = SharedState::get().stats;
  stats.eyeTracked.store(eyeTracked ? 1 : 0);
  stats.gazeU.store(u[0]);
  stats.gazeV.store(v[0]);
}

}  // namespace

bool xrSuggestGazeProfile(XrInstanceData& instance) { return suggestGazeProfile(instance); }

XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrSuggestInteractionProfileBindings(
    XrInstance instance, const XrInteractionProfileSuggestedBinding* suggestedBindings) {
  VRSFOV_GUARD_BEGIN
  XrInstanceData* data = findXrInstance(instance);
  if (!data || !data->d.SuggestInteractionProfileBindings || !suggestedBindings) {
    return XR_ERROR_HANDLE_INVALID;
  }
  if (!data->enabled) return data->d.SuggestInteractionProfileBindings(instance, suggestedBindings);

  const EyeGazePaths paths = gazePaths(*data);
  if (paths.profile == XR_NULL_PATH || suggestedBindings->interactionProfile != paths.profile) {
    return data->d.SuggestInteractionProfileBindings(instance, suggestedBindings);
  }

  // Remember the app's own bindings for this profile. The call replaces earlier
  // suggestions, so if our action already exists the merged list is suggested
  // again; if it does not (apps may suggest before creating a session), the
  // stored list is used when our action is created.
  data->appGazeBindings.assign(suggestedBindings->suggestedBindings,
                               suggestedBindings->suggestedBindings +
                                   suggestedBindings->countSuggestedBindings);
  for (const XrActionSuggestedBinding& binding : data->appGazeBindings) {
    if (binding.binding != paths.binding) continue;
    data->appGazeAction = binding.action;
    for (XrSessionData* session : xrSessionsForInstance(data)) {
      session->appGazeAction = binding.action;
    }
    LOG_INFO("the app owns %s; using its gaze action instead of binding a second one",
             kEyeGazeBinding);
    break;
  }
  if (data->ownGazeActions.empty()) {
    LOG_DEBUG("app suggested %s before our gaze action exists; remembering it", kEyeGazeProfile);
    return data->d.SuggestInteractionProfileBindings(instance, suggestedBindings);
  }

  LOG_DEBUG("merging our gaze binding into the app's %s suggestion", kEyeGazeProfile);
  if (suggestGazeProfile(*data)) return XR_SUCCESS;
  return data->d.SuggestInteractionProfileBindings(instance, suggestedBindings);
  VRSFOV_GUARD_END(XR_ERROR_INITIALIZATION_FAILED)
}

XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrAttachSessionActionSets(
    XrSession session, const XrSessionActionSetsAttachInfo* attachInfo) {
  VRSFOV_GUARD_BEGIN
  XrSessionData* data = findXrSession(session);
  if (!data || !data->instance || !data->instance->d.AttachSessionActionSets || !attachInfo) {
    return XR_ERROR_HANDLE_INVALID;
  }
  XrInstanceData& instance = *data->instance;
  if (!instance.enabled || data->gazeSet == XR_NULL_HANDLE) {
    return instance.d.AttachSessionActionSets(session, attachInfo);
  }

  std::vector<XrActionSet> sets(attachInfo->countActionSets);
  for (uint32_t i = 0; i < attachInfo->countActionSets; ++i) sets[i] = attachInfo->actionSets[i];
  sets.push_back(data->gazeSet);

  XrSessionActionSetsAttachInfo merged = *attachInfo;
  merged.countActionSets = (uint32_t)sets.size();
  merged.actionSets = sets.data();

  XrResult result = instance.d.AttachSessionActionSets(session, &merged);
  if (XR_FAILED(result)) {
    LOG_WARN("attaching the gaze action set failed (%d); falling back to fixed foveation",
             (int)result);
    result = instance.d.AttachSessionActionSets(session, attachInfo);
    if (XR_SUCCEEDED(result)) data->gazeUnavailable = true;
    return result;
  }
  data->gazeSetAttached = true;
  LOG_INFO("gaze action set attached to session %p", reinterpret_cast<void*>(session));
  return result;
  VRSFOV_GUARD_END(XR_ERROR_INITIALIZATION_FAILED)
}

XRAPI_ATTR XrResult XRAPI_CALL vrsfov_xrSyncActions(XrSession session,
                                                   const XrActionsSyncInfo* syncInfo) {
  VRSFOV_GUARD_BEGIN
  XrSessionData* data = findXrSession(session);
  if (!data || !data->instance || !data->instance->d.SyncActions) return XR_ERROR_HANDLE_INVALID;
  XrInstanceData& instance = *data->instance;
  if (!instance.enabled || !data->gazeSetAttached || !syncInfo) {
    return instance.d.SyncActions(session, syncInfo);
  }

  std::vector<XrActiveActionSet> sets = mergeActiveActionSets(*syncInfo, data->gazeSet);
  XrActionsSyncInfo merged = *syncInfo;
  merged.countActiveActionSets = (uint32_t)sets.size();
  merged.activeActionSets = sets.data();
  XrResult result = instance.d.SyncActions(session, &merged);
  if (XR_SUCCEEDED(result)) data->syncedSinceLastGaze = true;
  return result;
  VRSFOV_GUARD_END(XR_ERROR_INITIALIZATION_FAILED)
}

void xrUpdateGaze(XrSessionData& session, XrTime displayTime) {
  XrInstanceData& instance = *session.instance;
  XrDispatch& d = instance.d;
  const Config& config = instance.config;
  session.lastGazeTime = displayTime;

  if (session.viewSpace == XR_NULL_HANDLE || !d.LocateViews) {
    return;  // the snapshot goes stale and the Vulkan half falls back
  }

  // 1. locate the eyes in VIEW space.
  XrViewLocateInfo locate{};
  locate.type = XR_TYPE_VIEW_LOCATE_INFO;
  locate.viewConfigurationType = session.viewConfig;
  locate.displayTime = displayTime;
  locate.space = session.viewSpace;
  XrViewState state{};
  state.type = XR_TYPE_VIEW_STATE;
  XrView views[2]{};
  views[0].type = XR_TYPE_VIEW;
  views[1].type = XR_TYPE_VIEW;
  uint32_t viewCount = 0;
  const XrResult locateResult =
      d.LocateViews(session.session, &locate, &state, 2, &viewCount, views);
  if (XR_SUCCEEDED(locateResult) && viewCount >= 2 &&
      (state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
    session.eyes[0] = XrSessionData::CachedEye{views[0].pose, views[0].fov, true};
    session.eyes[1] = XrSessionData::CachedEye{views[1].pose, views[1].fov, true};
    session.haveViewCache = true;
  }

  // 2. gaze direction in VIEW space: eye-tracked when possible, else fixed.
  //    Every way of failing says so once, so "eye tracking unavailable" in the
  //    stats line can be told apart from "our layer never asked for gaze".
  //    If the app already owns /user/eyes_ext/input/gaze_ext/pose, OpenXR forbids
  //    a second binding for that path, so use the app's action instead.
  bool useAppAction = session.appGazeAction != XR_NULL_HANDLE;
  if (useAppAction && session.appGazeSpace == XR_NULL_HANDLE) {
    if (!d.CreateActionSpace) {
      useAppAction = false;
    } else {
      XrActionSpaceCreateInfo spaceInfo{};
      spaceInfo.type = XR_TYPE_ACTION_SPACE_CREATE_INFO;
      spaceInfo.action = session.appGazeAction;
      spaceInfo.poseInActionSpace.orientation.w = 1.0f;
      const XrResult r =
          d.CreateActionSpace(session.session, &spaceInfo, &session.appGazeSpace);
      if (XR_FAILED(r)) {
        LOG_ONCE(WARN, "gaze-app-space-failed",
                 "xrCreateActionSpace for the app's gaze action failed (%d); using fixed foveation",
                 (int)r);
        useAppAction = false;
      } else {
        LOG_INFO("using the app's own gaze action %p", reinterpret_cast<void*>(session.appGazeAction));
      }
    }
  }
  const XrAction gazeAction = useAppAction ? session.appGazeAction : session.gazeAction;
  const XrSpace gazeSpace = useAppAction ? session.appGazeSpace : session.gazeSpace;

  bool eyeTracked = false;
  Vec3 direction;
  if (!config.eyeTracking) {
    LOG_ONCE(INFO, "gaze-off-in-config",
             "eye tracking unavailable: eye_tracking = 0 in the config; using fixed foveation");
  } else if (!useAppAction && session.gazeSet == XR_NULL_HANDLE) {
    LOG_ONCE(INFO, "gaze-no-action-set",
             "eye tracking unavailable: no gaze action set was created (the session log says why: "
             "eye_tracking = 0, or %s was not enabled, or setting it up failed); using fixed "
             "foveation", XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
  } else if (!useAppAction && session.gazeUnavailable) {
    LOG_ONCE(INFO, "gaze-action-unavailable",
             "eye tracking unavailable: the gaze action set was not created (the reason is logged "
             "when the session is set up); using fixed foveation");
  } else if (!useAppAction && !session.gazeSetAttached) {
    LOG_ONCE(INFO, "gaze-not-attached",
             "eye tracking unavailable: the app has not attached our gaze action set; using "
             "fixed foveation");
  } else if (!useAppAction && !session.gazeBindingsSuggested) {
    LOG_ONCE(INFO, "gaze-not-suggested",
             "eye tracking unavailable: our gaze action has no binding (the app already owns %s, "
             "or suggesting it failed); using fixed foveation", kEyeGazeBinding);
  } else if (!session.syncedSinceLastGaze) {
    LOG_ONCE(INFO, "gaze-not-synced",
             "eye tracking unavailable: the app has not synced actions since the last gaze update "
             "(normal on the very first frames - our gaze action set is merged into the app's "
             "xrSyncActions call; if this is the only reason ever logged, the app never calls "
             "xrSyncActions); using fixed foveation");
  } else if (gazeSpace == XR_NULL_HANDLE || !d.GetActionStatePose || !d.LocateSpace) {
    LOG_ONCE(INFO, "gaze-no-entrypoints",
             "eye tracking unavailable: the runtime provided no eye-gaze space or entry points; "
             "using fixed foveation");
  } else {
    XrActionStateGetInfo getInfo{};
    getInfo.type = XR_TYPE_ACTION_STATE_GET_INFO;
    getInfo.action = gazeAction;
    XrActionStatePose poseState{};
    poseState.type = XR_TYPE_ACTION_STATE_POSE;
    XrSpaceLocation location{};
    location.type = XR_TYPE_SPACE_LOCATION;
    const XrResult stateResult = d.GetActionStatePose(session.session, &getInfo, &poseState);
    if (XR_FAILED(stateResult)) {
      LOG_ONCE(WARN, "gaze-getstate-failed",
               "xrGetActionStatePose(gaze) failed (%d); using fixed foveation", (int)stateResult);
    } else if (poseState.isActive != XR_TRUE) {
      LOG_ONCE(INFO, "gaze-inactive",
               "eye tracking unavailable: the runtime reports the gaze action as inactive, i.e. "
               "nothing is tracking the eyes (no eye tracker, or this runtime does not expose it "
               "to the app); using fixed foveation");
    } else {
      const XrResult locateResult =
          d.LocateSpace(gazeSpace, session.viewSpace, displayTime, &location);
      if (XR_FAILED(locateResult)) {
        LOG_ONCE(WARN, "gaze-locate-failed", "xrLocateSpace(gaze) failed (%d); using fixed foveation",
                 (int)locateResult);
      } else if (!(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        LOG_ONCE(INFO, "gaze-not-valid",
                 "eye tracking unavailable: the gaze pose is not valid yet (flags 0x%x); using "
                 "fixed foveation", (unsigned)location.locationFlags);
      } else if (!(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)) {
        LOG_ONCE(INFO, "gaze-not-tracked",
                 "eye tracking unavailable: the gaze pose is valid but not tracked (flags 0x%x); "
                 "using fixed foveation", (unsigned)location.locationFlags);
      } else {
        direction = gazeDirectionFromPose(location.pose.orientation);
        eyeTracked = true;
      }
    }
  }
  session.syncedSinceLastGaze = false;

  // OpenXR applications always use the runtime's native eye-gaze action. The
  // out-of-process OpenVR publisher is for OpenVR games only and is never
  // consulted from this path.
  if (!eyeTracked) {
    direction = fixedGazeDirection(config.fixedOffsetX, config.fixedOffsetY);
  }

  const bool tracking = eyeTracked;
  if (tracking != session.eyeTrackedLogged) {
    session.eyeTrackedLogged = tracking;
    LOG_INFO("eye tracking %s",
             tracking ? "active" : "unavailable (using fixed foveation)");
  }

  // 3. project into each eye image, smooth and publish.
  float u[2] = {0.5f, 0.5f};
  float v[2] = {0.5f, 0.5f};
  bool valid = session.haveViewCache;
  const auto snap = SharedState::get().snapshot();
  FovTan usedFov[2] = {fovTanFromXr(session.eyes[0].fov), fovTanFromXr(session.eyes[1].fov)};
  for (int i = 0; i < 2; ++i) {
    if (!session.eyes[i].valid) {
      valid = false;
      continue;
    }
    // The fov the app submits with its projection layer is what the compositor
    // uses to map the eye image onto the display, and it is what the app rendered
    // with. Prefer it over the runtime's recommended fov: some apps submit a
    // different fov (a crop, another aspect, or a vertically flipped image), and
    // the recommended fov then puts the fovea and the paint marker in the wrong
    // place in that eye.
    if (snap && snap->submittedView[i].fov.valid) {
      const ViewFov& f = snap->submittedView[i].fov;
      usedFov[i] = fovToTan(f.left, f.right, f.up, f.down);
    }
    const GazeUv projected = projectGaze(direction, quatFromXr(session.eyes[i].pose.orientation),
                                         usedFov[i]);
    const float projectedV = config.gazeYFlip ? 1.f - projected.v : projected.v;
    session.smoothU[i] = smoothGazeSample(session.smoothU[i], projected.u, config.smoothing,
                                          &session.haveSmoothU[i]);
    session.smoothV[i] = smoothGazeSample(session.smoothV[i], projectedV, config.smoothing,
                                          &session.haveSmoothV[i]);
    u[i] = session.smoothU[i];
    v[i] = session.smoothV[i];
  }

  // Diagnostic for VRSFOV_PAINT_GAZE: once a second, report the projected per-eye
  // gaze together with the per-eye view data it came from. A vertically swapped
  // fov or a rotated eye pose on one eye shows up here, and it is what makes the
  // two painted boxes drift apart on pitch while staying merged at the centre.
  if (config.paintGaze && session.haveViewCache) {
    static std::atomic<int64_t> lastGazeLogNs{0};
    const int64_t nowLog = nowNs();
    int64_t last = lastGazeLogNs.load(std::memory_order_relaxed);
    if (nowLog - last > 1000LL * 1000 * 1000 &&
        lastGazeLogNs.compare_exchange_strong(last, nowLog, std::memory_order_relaxed)) {
      LOG_INFO("gaze project: L=(%.3f,%.3f) R=(%.3f,%.3f) "
               "fov L[%.3f %.3f %.3f %.3f] R[%.3f %.3f %.3f %.3f]",
               (double)u[0], (double)v[0], (double)u[1], (double)v[1],
               (double)usedFov[0].left, (double)usedFov[0].right, (double)usedFov[0].up,
               (double)usedFov[0].down, (double)usedFov[1].left, (double)usedFov[1].right,
               (double)usedFov[1].up, (double)usedFov[1].down);
      LOG_INFO("gaze pose: L[%.3f %.3f %.3f %.3f] R[%.3f %.3f %.3f %.3f]", 
               (double)session.eyes[0].pose.orientation.x,
               (double)session.eyes[0].pose.orientation.y,
               (double)session.eyes[0].pose.orientation.z,
               (double)session.eyes[0].pose.orientation.w,
               (double)session.eyes[1].pose.orientation.x,
               (double)session.eyes[1].pose.orientation.y,
               (double)session.eyes[1].pose.orientation.z,
               (double)session.eyes[1].pose.orientation.w);
      if (snap && snap->submittedView[0].fov.valid) {
        LOG_INFO("gaze submitted fov: L[%.3f %.3f %.3f %.3f] R[%.3f %.3f %.3f %.3f]",
                 (double)snap->submittedView[0].fov.left, (double)snap->submittedView[0].fov.right,
                 (double)snap->submittedView[0].fov.up, (double)snap->submittedView[0].fov.down,
                 (double)snap->submittedView[1].fov.left, (double)snap->submittedView[1].fov.right,
                 (double)snap->submittedView[1].fov.up, (double)snap->submittedView[1].fov.down);
        LOG_INFO("gaze submitted pose: L[%.3f %.3f %.3f %.3f] R[%.3f %.3f %.3f %.3f]",
                 (double)snap->submittedView[0].qx, (double)snap->submittedView[0].qy,
                 (double)snap->submittedView[0].qz, (double)snap->submittedView[0].qw,
                 (double)snap->submittedView[1].qx, (double)snap->submittedView[1].qy,
                 (double)snap->submittedView[1].qz, (double)snap->submittedView[1].qw);
      }
    }
  }

  publishEyes(session, u, v, valid, tracking);
}

}  // namespace vrsfov
