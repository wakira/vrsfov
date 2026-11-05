#include "openxr/xr_pure.h"

#include <cmath>

namespace vrsfov {

namespace {

constexpr float kJumpThreshold = 0.15f;

template <class T>
const T* findNext(const void* pNext, XrStructureType type) {
  auto node = reinterpret_cast<const XrBaseInStructure*>(pNext);
  while (node) {
    if (node->type == type) return reinterpret_cast<const T*>(node);
    node = node->next;
  }
  return nullptr;
}

}  // namespace

GraphicsBindingInfo findVulkanBinding(const void* pNext) {
  GraphicsBindingInfo out;
  const auto* binding = findNext<XrGraphicsBindingVulkanKHR>(pNext, XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR);
  if (!binding) return out;
  out.present = true;
  out.vkDevice = reinterpret_cast<uint64_t>(binding->device);
  out.vkPhysicalDevice = reinterpret_cast<uint64_t>(binding->physicalDevice);
  out.queueFamilyIndex = binding->queueFamilyIndex;
  return out;
}

bool extractProjectionSlots(const XrFrameEndInfo& info, SlotList& out) {
  out.clear();
  if (!info.layers) return false;
  for (uint32_t i = 0; i < info.layerCount; ++i) {
    const XrCompositionLayerBaseHeader* base = info.layers[i];
    if (!base) continue;
    if (base->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
    const auto* projection = reinterpret_cast<const XrCompositionLayerProjection*>(base);
    if (projection->viewCount != 2 || !projection->views) continue;
    for (uint32_t v = 0; v < projection->viewCount; ++v) {
      const XrSwapchainSubImage& sub = projection->views[v].subImage;
      ProjectionSlot slot;
      slot.swapchain = reinterpret_cast<uint64_t>(sub.swapchain);
      slot.arrayIndex = sub.imageArrayIndex;
      slot.x = sub.imageRect.offset.x;
      slot.y = sub.imageRect.offset.y;
      slot.w = sub.imageRect.extent.width;
      slot.h = sub.imageRect.extent.height;
      slot.eye = v;
      out.push_back(slot);
    }
    return true;  // only the first projection layer
  }
  return false;
}

void mergeEyeGazeSuggestions(const XrInteractionProfileSuggestedBinding& in,
                             const EyeGazePaths& paths,
                             std::vector<XrActionSuggestedBinding>& out) {
  out.assign(in.suggestedBindings, in.suggestedBindings + in.countSuggestedBindings);
  if (paths.profile == XR_NULL_PATH || paths.binding == XR_NULL_PATH) return;
  if (in.interactionProfile != paths.profile) return;
  for (const auto& b : out) {
    if (b.binding == paths.binding) return;  // the app already said this
  }
  XrActionSuggestedBinding ours{};
  ours.action = XR_NULL_HANDLE;  // filled in by the caller
  ours.binding = paths.binding;
  out.push_back(ours);
}

std::vector<XrActiveActionSet> mergeActiveActionSets(const XrActionsSyncInfo& info,
                                                     XrActionSet ours) {
  std::vector<XrActiveActionSet> sets;
  sets.reserve(info.countActiveActionSets + 1);
  for (uint32_t i = 0; i < info.countActiveActionSets; ++i) sets.push_back(info.activeActionSets[i]);
  if (ours != XR_NULL_HANDLE) {
    XrActiveActionSet set{};
    set.actionSet = ours;
    set.subactionPath = XR_NULL_PATH;
    sets.push_back(set);
  }
  return sets;
}

float smoothGazeSample(float previous, float sample, float factor, bool* havePrevious) {
  if (!havePrevious || !*havePrevious) {
    if (havePrevious) *havePrevious = true;
    return sample;
  }
  if (std::fabs(sample - previous) > kJumpThreshold) return sample;  // tracking re-acquired
  const float f = factor < 0.f ? 0.f : (factor > 0.99f ? 0.99f : factor);
  return previous + (1.f - f) * (sample - previous);
}

Vec3 gazeDirectionFromPose(const XrQuaternionf& orientation) {
  const Quat q{orientation.x, orientation.y, orientation.z, orientation.w};
  return normalize(quatRotate(q, Vec3{0.f, 0.f, -1.f}));
}

Vec3 fixedGazeDirection(float tangentX, float tangentY) {
  return normalize(Vec3{tangentX, tangentY, -1.f});
}

FovTan fovTanFromXr(const XrFovf& fov) {
  FovTan out;
  out.left = std::tan(fov.angleLeft);
  out.right = std::tan(fov.angleRight);
  out.up = std::tan(fov.angleUp);
  out.down = std::tan(fov.angleDown);
  return out;
}

}  // namespace vrsfov
