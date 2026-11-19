// Pure OpenXR-side helpers: no OpenXR/Vulkan calls, so they can be unit tested.
#pragma once

#include <vulkan/vulkan.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <vector>

#include "common/gaze_projection.h"
#include "common/shared_state.h"
#include "common/xr_math.h"

namespace vrsfov {

struct GraphicsBindingInfo {
  bool present = false;
  uint64_t vkDevice = 0;
  uint64_t vkPhysicalDevice = 0;
  uint32_t queueFamilyIndex = 0;
};

// Finds XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR (the same type value is used by
// XrGraphicsBindingVulkan2KHR) in a session create info pNext chain.
GraphicsBindingInfo findVulkanBinding(const void* pNext);

// 03 §6: the projection slots of the first two-view projection layer. When
// `views` is given, it receives the fov and orientation the app submitted for
// each view (index 0 left, 1 right): the image -> display mapping the compositor
// will use.
bool extractProjectionSlots(const XrFrameEndInfo& info, SlotList& out,
                            SubmittedView views[2] = nullptr);

// 03 §4: the eye gaze profile and the gaze pose binding paths.
struct EyeGazePaths {
  XrPath profile = XR_NULL_PATH;
  XrPath binding = XR_NULL_PATH;
};

// Appends our gaze binding to the app's suggestion for that profile. If the app
// already suggested the same binding, it is not duplicated.
void mergeEyeGazeSuggestions(const XrInteractionProfileSuggestedBinding& in,
                             const EyeGazePaths& paths,
                             std::vector<XrActionSuggestedBinding>& out);

// 03 §4: the app's active action sets plus ours.
std::vector<XrActiveActionSet> mergeActiveActionSets(const XrActionsSyncInfo& info,
                                                     XrActionSet ours);

// 02 §2: exponential moving average of a gaze coordinate. The first sample is
// taken as-is; a jump larger than 0.15 resets the history.
float smoothGazeSample(float previous, float sample, float factor, bool* havePrevious);

// 03 §5: gaze direction in view space.
Vec3 gazeDirectionFromPose(const XrQuaternionf& orientation);
Vec3 fixedGazeDirection(float tangentX, float tangentY);

FovTan fovTanFromXr(const XrFovf& fov);

}  // namespace vrsfov
