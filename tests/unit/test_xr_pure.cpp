#include "test_framework.h"

#include "openxr/xr_pure.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <string>
#include <type_traits>
#include <vector>

#include <unistd.h>

using namespace vrsfov;

namespace {

// OpenXR handle types are either opaque pointers or uint64 atomics; tests only
// need distinguishable values.
template <class T>
T fakeHandle(uintptr_t value) {
  if constexpr (std::is_pointer<T>::value) {
    return reinterpret_cast<T>(value);
  } else {
    return static_cast<T>(value);
  }
}

}  // namespace

VRSFOV_TEST(xr_pure_vulkan_binding) {
  VkDevice device = fakeHandle<VkDevice>(0x1234);
  VkPhysicalDevice gpu = fakeHandle<VkPhysicalDevice>(0x5678);
  XrGraphicsBindingVulkanKHR binding{};
  binding.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR;
  binding.device = device;
  binding.physicalDevice = gpu;
  binding.queueFamilyIndex = 3;

  GraphicsBindingInfo info = findVulkanBinding(&binding);
  CHECK(info.present);
  CHECK_EQ(info.vkDevice, (uint64_t)0x1234);
  CHECK_EQ(info.vkPhysicalDevice, (uint64_t)0x5678);
  CHECK_EQ(info.queueFamilyIndex, 3u);

  // The Vulkan2 struct uses the same type value.
  XrGraphicsBindingVulkan2KHR binding2{};
  binding2.type = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR;
  binding2.device = device;
  GraphicsBindingInfo info2 = findVulkanBinding(&binding2);
  CHECK(info2.present);
  CHECK_EQ(info2.vkDevice, (uint64_t)0x1234);

  XrSessionCreateInfo session{};
  session.type = XR_TYPE_SESSION_CREATE_INFO;
  session.next = &binding;
  CHECK(findVulkanBinding(session.next).present);

  // Anything that is not a Vulkan binding is ignored.
  CHECK(!findVulkanBinding(nullptr).present);
  XrInstanceCreateInfo notABinding{};
  notABinding.type = XR_TYPE_INSTANCE_CREATE_INFO;
  CHECK(!findVulkanBinding(&notABinding).present);
}

VRSFOV_TEST(xr_pure_projection_slots) {
  XrSwapchain left = fakeHandle<XrSwapchain>(0xAA);
  XrSwapchain right = fakeHandle<XrSwapchain>(0xBB);

  XrCompositionLayerProjectionView views[2]{};
  views[0].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
  views[0].subImage.swapchain = left;
  views[0].subImage.imageRect = {{0, 0}, {1832, 1920}};
  views[0].subImage.imageArrayIndex = 0;
  views[1].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
  views[1].subImage.swapchain = right;
  views[1].subImage.imageRect = {{1832, 0}, {1832, 1920}};
  views[1].subImage.imageArrayIndex = 1;

  XrCompositionLayerProjection projection{};
  projection.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
  projection.space = XR_NULL_HANDLE;
  projection.viewCount = 2;
  projection.views = views;

  XrFrameEndInfo end{};
  end.type = XR_TYPE_FRAME_END_INFO;
  end.layerCount = 1;
  const XrCompositionLayerBaseHeader* layers[1] = {
      reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
  end.layers = layers;

  SlotList slots;
  CHECK(extractProjectionSlots(end, slots));
  CHECK_EQ(slots.size(), 2u);
  CHECK_EQ(slots[0].swapchain, (uint64_t)0xAA);
  CHECK_EQ(slots[0].arrayIndex, 0u);
  CHECK_EQ(slots[0].eye, 0u);
  CHECK_EQ(slots[0].w, 1832);
  CHECK_EQ(slots[0].h, 1920);
  CHECK_EQ(slots[1].swapchain, (uint64_t)0xBB);
  CHECK_EQ(slots[1].x, 1832);
  CHECK_EQ(slots[1].eye, 1u);

  // A single-view projection layer is not the main scene stereo layer.
  projection.viewCount = 1;
  CHECK(!extractProjectionSlots(end, slots));
  CHECK_EQ(slots.size(), 0u);

  // Other layer types are ignored.
  XrCompositionLayerQuad quad{};
  quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
  const XrCompositionLayerBaseHeader* quadLayers[1] = {
      reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad)};
  end.layers = quadLayers;
  CHECK(!extractProjectionSlots(end, slots));
}

VRSFOV_TEST(xr_pure_gaze_suggestion_merge) {
  EyeGazePaths paths;
  paths.profile = fakeHandle<XrPath>(7);
  paths.binding = fakeHandle<XrPath>(9);

  XrActionSuggestedBinding existing[1]{};
  existing[0].action = fakeHandle<XrAction>(0x1);
  existing[0].binding = fakeHandle<XrPath>(20);
  XrInteractionProfileSuggestedBinding in{};
  in.type = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
  in.interactionProfile = paths.profile;
  in.countSuggestedBindings = 1;
  in.suggestedBindings = existing;

  std::vector<XrActionSuggestedBinding> out;
  mergeEyeGazeSuggestions(in, paths, out);
  CHECK_EQ(out.size(), 2u);  // the app's plus ours
  CHECK_EQ(out[1].binding, paths.binding);

  // A different profile is left alone.
  XrInteractionProfileSuggestedBinding other = in;
  other.interactionProfile = fakeHandle<XrPath>(99);
  mergeEyeGazeSuggestions(other, paths, out);
  CHECK_EQ(out.size(), 1u);

  // An app that already suggests our binding path is not duplicated.
  existing[0].binding = paths.binding;
  mergeEyeGazeSuggestions(in, paths, out);
  CHECK_EQ(out.size(), 1u);
}

VRSFOV_TEST(xr_pure_active_action_sets_merge) {
  XrActiveActionSet appSets[2]{};
  appSets[0].actionSet = fakeHandle<XrActionSet>(0x1);
  appSets[1].actionSet = fakeHandle<XrActionSet>(0x2);
  XrActionsSyncInfo info{};
  info.type = XR_TYPE_ACTIONS_SYNC_INFO;
  info.countActiveActionSets = 2;
  info.activeActionSets = appSets;

  std::vector<XrActiveActionSet> merged =
      mergeActiveActionSets(info, fakeHandle<XrActionSet>(0x3));
  CHECK_EQ(merged.size(), 3u);
  CHECK_EQ(merged[2].actionSet, (XrActionSet)0x3);
  CHECK_EQ(merged[2].subactionPath, XR_NULL_PATH);

  // Without our set nothing changes.
  merged = mergeActiveActionSets(info, XR_NULL_HANDLE);
  CHECK_EQ(merged.size(), 2u);

  // Empty app list.
  XrActionsSyncInfo empty{};
  empty.type = XR_TYPE_ACTIONS_SYNC_INFO;
  merged = mergeActiveActionSets(empty, fakeHandle<XrActionSet>(0x3));
  CHECK_EQ(merged.size(), 1u);
}

VRSFOV_TEST(xr_pure_gaze_smoothing) {
  bool have = false;
  CHECK_NEAR(smoothGazeSample(0.5f, 0.9f, 0.9f, &have), 0.9f, 1e-6);  // first sample wins
  CHECK(have);
  // 0.9 factor = heavy smoothing: 10% of the way towards the new sample.
  CHECK_NEAR(smoothGazeSample(0.9f, 0.8f, 0.9f, &have), 0.89f, 1e-6);
  CHECK_NEAR(smoothGazeSample(0.89f, 0.8f, 0.0f, &have), 0.8f, 1e-6);  // no smoothing
  // A big jump is taken as-is (tracking re-acquired).
  CHECK_NEAR(smoothGazeSample(0.2f, 0.8f, 0.95f, &have), 0.8f, 1e-6);
}

VRSFOV_TEST(xr_pure_gaze_direction) {
  XrQuaternionf identity{};
  identity.w = 1.0f;
  Vec3 d = gazeDirectionFromPose(identity);
  CHECK_NEAR(d.x, 0.0, 1e-6);
  CHECK_NEAR(d.y, 0.0, 1e-6);
  CHECK_NEAR(d.z, -1.0, 1e-6);

  XrQuaternionf yaw{};
  yaw.y = std::sin(-22.5 * M_PI / 180.0 / 2.0);
  yaw.w = std::cos(-22.5 * M_PI / 180.0 / 2.0);
  d = gazeDirectionFromPose(yaw);
  CHECK(d.x > 0.3f);  // to the right

  d = fixedGazeDirection(0.0f, 0.0f);
  CHECK_NEAR(d.z, -1.0, 1e-6);
  d = fixedGazeDirection(0.5f, -0.25f);
  CHECK(d.x > 0.f && d.y < 0.f);
  CHECK_NEAR(length(d), 1.0, 1e-5);
}

VRSFOV_TEST(xr_pure_fov_tan) {
  XrFovf fov{};
  fov.angleLeft = -45.f * (float)M_PI / 180.f;
  fov.angleRight = 45.f * (float)M_PI / 180.f;
  fov.angleUp = 30.f * (float)M_PI / 180.f;
  fov.angleDown = -30.f * (float)M_PI / 180.f;
  FovTan tan = fovTanFromXr(fov);
  CHECK_NEAR(tan.left, -1.0, 1e-4);
  CHECK_NEAR(tan.right, 1.0, 1e-4);
  CHECK_NEAR(tan.up, std::tan(30.0 * M_PI / 180.0), 1e-4);
  CHECK_NEAR(tan.down, -std::tan(30.0 * M_PI / 180.0), 1e-4);

  // End to end: gaze straight ahead lands in the middle of the eye image.
  const GazeUv projected = projectGaze({0.f, 0.f, -1.f}, Quat{}, tan);
  CHECK(projected.valid);
  CHECK_NEAR(projected.u, 0.5, 1e-4);
  CHECK_NEAR(projected.v, 0.5, 1e-4);
}

VRSFOV_TEST(config_process_name_matching) {
  // loadConfigForProcess matches the basename of /proc/self/exe, which for this
  // test binary is vrsfov_unit_tests.
  const std::string path = "/tmp/vrsfov-proc-test-" + std::to_string((long)getpid()) + ".ini";
  {
    std::ofstream f(path);
    f << "[global]\nouter_rate = 2x2\n"
      << "[exe:vrsfov_unit_tests]\nouter_rate = 2x1\n";
  }
  Config config = loadConfigForProcess("no such app", 0, path);
  CHECK_EQ((int)config.outerRate.w, 2);
  CHECK_EQ((int)config.outerRate.h, 1);
  unlink(path.c_str());
}
