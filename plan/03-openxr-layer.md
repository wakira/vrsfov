# 03 — OpenXR API layer

## 1. Negotiation & instance creation (xr_layer.cpp)

```cpp
extern "C" XRAPI_ATTR XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(
    const XrNegotiateLoaderInfo* loaderInfo, const char* layerName,
    XrNegotiateApiLayerRequest* req);
```
- Validate `loaderInfo->structType == XR_LOADER_INTERFACE_STRUCT_LOADER_INFO`, versions
  overlap `XR_CURRENT_LOADER_API_LAYER_VERSION` and `XR_CURRENT_API_VERSION` range.
- Fill `req->layerInterfaceVersion`, `req->layerApiVersion = XR_CURRENT_API_VERSION`,
  `req->getInstanceProcAddr = vrsfov_xrGetInstanceProcAddr`,
  `req->createApiLayerInstance = vrsfov_xrCreateApiLayerInstance`.

`vrsfov_xrCreateApiLayerInstance(const XrInstanceCreateInfo* ci, const XrApiLayerCreateInfo* li, XrInstance* out)`:
1. `PFN_xrGetInstanceProcAddr nextGipa = li->nextInfo->nextGetInstanceProcAddr;`
2. Load config (needs app name: `ci->applicationInfo.applicationName`). If `!enabled`, still
   chain normally but install no behavior (pure passthrough; store a flag).
3. Query runtime extensions: `nextGipa(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties")`,
   call it (two-call idiom). If `XR_EXT_eye_gaze_interaction` is present, `eyeTracking` is on,
   and the app did not already enable it, build a new extension-name array = app's + it.
4. Copy `XrApiLayerCreateInfo` → `XrApiLayerCreateInfo next = *li; next.nextInfo = li->nextInfo->next;`
   and call `li->nextInfo->nextCreateApiLayerInstance(&modifiedCi, &next, out)`.
   If it fails with the extra extension, retry once without it.
5. On success: create `InstanceData` (map `XrInstance → InstanceData`, guarded by mutex), resolve
   all needed next functions via `nextGipa(*out, name)` into `XrDispatch`, record whether we
   enabled eye gaze ourselves, `SharedState::liveXrInstances++`, log runtime name
   (`xrGetInstanceProperties`).

`vrsfov_xrGetInstanceProcAddr(instance, name, fn)`: if `name` is one of the hooked functions
below, return our function (only if the next layer also resolves it — call next first; if
next fails, return its error). Otherwise forward to next.

Handle → data lookups: `XrSession → SessionData*`, `XrSwapchain → SwapchainData`,
each holding the owning `InstanceData*` for dispatch. Use `std::unordered_map` + `std::mutex`.

## 2. Hooked functions

| Function | Behavior |
|---|---|
| `xrDestroyInstance` | call next, erase data, `liveXrInstances--` |
| `xrEnumerateViewConfigurationViews` | call next; if PRIMARY_STEREO and output filled, store `recommendedImageRectWidth/Height` of view 0 in snapshot |
| `xrCreateSession` | see §3 |
| `xrDestroySession` | destroy our gaze space/view space/action set, clear snapshot session fields, call next |
| `xrBeginSession` / `xrEndSession` | call next; set `sessionRunning` true/false |
| `xrCreateSwapchain` | call next; store `SwapchainInfo` (w,h,arraySize,format,usageFlags) |
| `xrEnumerateSwapchainImages` | call next; if images non-null and type is `XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR` and swapchain usage has `COLOR_ATTACHMENT` or `DEPTH_STENCIL_ATTACHMENT`, register each `VkImage → SwapchainInfo` |
| `xrDestroySwapchain` | unregister its VkImages, call next |
| `xrSuggestInteractionProfileBindings` | §4 |
| `xrAttachSessionActionSets` | §4 |
| `xrSyncActions` | §4 |
| `xrWaitFrame` | call next; store `predictedDisplayTime`; config hot-reload check |
| `xrLocateViews` | call next; then `updateGaze(session, displayTime)` (§5) if not yet done for this displayTime |
| `xrBeginFrame` | call next; if gaze not yet updated for last predicted time, `updateGaze` |
| `xrEndFrame` | record projection slots (§6), stats logging, call next |

## 3. xrCreateSession

1. Walk `createInfo->next`. Find `XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR` (same value is used by
   `XrGraphicsBindingVulkan2KHR`). If absent → mark session "inactive" (OpenGL etc.), passthrough.
2. Call next. On success record `VkDevice`, `VkPhysicalDevice`, `queueFamilyIndex` →
   snapshot `sessionVkDevice`.
3. Create our VIEW reference space (`XR_REFERENCE_SPACE_TYPE_VIEW`, identity pose).
4. If eye gaze extension enabled: `xrGetSystemProperties` with chained
   `XrSystemEyeGazeInteractionPropertiesEXT`; if `supportsEyeGazeInteraction`:
   - `xrCreateActionSet` name `"vrsfov_gaze_set"`, localized `"VRSFov gaze"`, priority 0.
   - `xrCreateAction` name `"vrsfov_gaze"`, type `XR_ACTION_TYPE_POSE_INPUT`.
   - Suggest bindings: profile `/interaction_profiles/ext/eye_gaze_interaction`, binding
     `/user/eyes_ext/input/gaze_ext/pose` (use `xrStringToPath`). Remember we did this.
   - `xrCreateActionSpace` for the action, identity pose, subactionPath NULL.
   Errors here are non-fatal: log WARN, fall back to fixed foveation.

## 4. Action-system merging (xr_gaze.cpp)

- `xrSuggestInteractionProfileBindings`: if the app suggests for the eye-gaze profile, append
  our binding to its list (new array) — the call replaces earlier suggestions for that profile.
- `xrAttachSessionActionSets`: build a new array = app's sets + our set (if created), call
  next. On success mark `ourSetAttached = true`. If it fails with ours, retry with app's only
  and disable eye tracking (log WARN).
- `xrSyncActions`: if `ourSetAttached`, build `XrActionsSyncInfo` copy whose `activeActionSets`
  = app's + `{ourSet, XR_NULL_PATH}`; call next. Remember `lastSyncTime`.
- Apps that never attach action sets: eye tracking unavailable → fixed foveation. Log INFO once.
  (Do not attach on the app's behalf: attach can be called only once and would break apps
  that attach late.)

## 5. updateGaze(session, displayTime)

1. If not running, or app has not called `xrSyncActions` since last update → eye-tracked=false.
2. `xrLocateViews(viewConfig=PRIMARY_STEREO, displayTime, space=ourViewSpace)` via next
   dispatch → per-eye pose (in VIEW space) and `XrFovf`. Cache last valid result.
3. Gaze direction in VIEW space:
   - Eye tracked: `xrGetActionStatePose` (isActive) and `xrLocateSpace(gazeSpace, viewSpace, displayTime)`.
     Valid if `ORIENTATION_VALID_BIT && ORIENTATION_TRACKED_BIT`.
     `dView = rotate(gazePose.orientation, {0,0,-1})`.
   - Else fixed: `dView = normalize({fixedOffsetX, fixedOffsetY, -1})`.
4. For each eye i: `(u,v) = projectGaze(dView, eyePose[i].orientation, fov[i])` (§5.1).
   Apply smoothing (EMA per eye: `u = u_prev + (1 - smoothing) * (u - u_prev)`; reset on
   first valid sample or a jump > 0.15).
5. Publish snapshot with `eyes[i] = {valid, u, v, eyeTracked}`, `publishTimeNs = now`.

### 5.1 projectGaze (src/common/gaze_projection.cpp — pure, unit tested)

```
dEye = rotate(conjugate(qEye), dView)
if (dEye.z > -1e-4) -> invalid (looking sideways/backwards): return center fallback (0.5,0.5)
tx = dEye.x / -dEye.z ;  ty = dEye.y / -dEye.z
L = tan(fov.angleLeft); R = tan(fov.angleRight); U = tan(fov.angleUp); D = tan(fov.angleDown)
u = (tx - L) / (R - L)
v = (U - ty) / (U - D)          // v grows downward (Vulkan image space)
clamp u, v to [0,1]
```
Note `angleLeft` and `angleDown` are negative. Unit tests:
- symmetric ±45° FOV, identity orientations, dView=(0,0,-1) → (0.5, 0.5)
- dView rotated 22.5° to the right (yaw −22.5° about +Y) → u = (tan22.5 + 1)/2 ≈ 0.7071, v = 0.5
- dView pitched up 22.5° → v ≈ 0.2929
- asymmetric FOV L=−50°, R=40° straight ahead → u = tan50/(tan50+tan40) ≈ 0.587
- eye canted 10° outward handled via qEye.

## 6. xrEndFrame: projection slots

For each layer in `frameEndInfo->layers` with `type == XR_TYPE_COMPOSITION_LAYER_PROJECTION`
and `viewCount == 2`: for view i, record `ProjectionSlot{ swapchain = views[i].subImage.swapchain,
arrayIndex = views[i].subImage.imageArrayIndex, rect = views[i].subImage.imageRect, eye = i }`.
Use only the first projection layer (the main scene). If the slot list differs from the
published one, publish a new snapshot. Also increment `frameIndex`.

Note the Vulkan half uses *previous* frame's slots for the current frame — this is fine,
mappings are stable frame to frame.
