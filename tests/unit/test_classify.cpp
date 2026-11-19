#include "test_framework.h"

#include "common/classify.h"

#include <memory>

using namespace vrsfov;

namespace {

Snapshot makeSnapshot() {
  Snapshot s;
  s.sessionVkDevice = 0x1234;
  s.sessionRunning = true;
  s.publishTimeNs = nowNs();
  s.recommendedW = 1832;
  s.recommendedH = 1920;
  s.eyes[0] = EyeView{true, 0.25f, 0.30f, true};
  s.eyes[1] = EyeView{true, 0.75f, 0.70f, true};
  s.config = std::make_shared<Config>();
  s.slots = std::make_shared<const SlotList>();
  s.swapchainByVkImage = std::make_shared<const SwapchainMap>();
  return s;
}

TargetDesc makeTarget(uint32_t w, uint32_t h, bool color = true) {
  TargetDesc t;
  t.width = w;
  t.height = h;
  t.hasColor = color;
  t.vkDevice = 0x1234;
  return t;
}

}  // namespace

VRSFOV_TEST(classify_multiview_stereo) {
  Snapshot s = makeSnapshot();
  TargetDesc t = makeTarget(1832, 1920);
  t.viewMask = 0b11;
  t.layers = 2;

  FoveationPlan plan = planForTarget(t, s, 2);
  CHECK_EQ(plan.regions.size(), 2u);
  CHECK_EQ(plan.regions[0].layer, 0u);
  CHECK_EQ(plan.regions[1].layer, 1u);
  CHECK_NEAR(plan.regions[0].u, 0.25, 1e-6);
  CHECK_NEAR(plan.regions[1].u, 0.75, 1e-6);
  CHECK_EQ(plan.regions[0].w, 1832u);
  CHECK_EQ(plan.regions[0].h, 1920u);

  // A single-layer rate image collapses both eyes onto layer 0 with the
  // averaged gaze.
  FoveationPlan single = planForTarget(t, s, 1);
  CHECK_EQ(single.regions.size(), 1u);
  CHECK_EQ(single.regions[0].layer, 0u);
  CHECK_NEAR(single.regions[0].u, 0.5, 1e-6);
  CHECK_NEAR(single.regions[0].v, 0.5, 1e-6);
}

// Some engines keep their intermediate eye images upside down and flip them when
// copying to the swapchain (VTOL VR). gaze_yflip_eye_images mirrors the gaze for
// such layered targets - but not for swapchain images or single-layer targets.
VRSFOV_TEST(classify_mirrors_intermediate_eye_images) {
  Snapshot s = makeSnapshot();
  s.eyes[0] = EyeView{true, 0.25f, 0.20f, true};
  s.eyes[1] = EyeView{true, 0.75f, 0.30f, true};  // average v = 0.25
  Config cfg;
  cfg.gazeYFlipEyeImages = true;
  s.config = std::make_shared<const Config>(cfg);

  TargetDesc layered = makeTarget(916, 960);  // half the 1832x1920 display size
  layered.layers = 2;
  FoveationPlan plan = planForTarget(layered, s, 1);
  CHECK_EQ(plan.regions.size(), 1u);
  CHECK(plan.scaledTarget);
  CHECK(!plan.swapchainTarget);
  CHECK_NEAR(plan.regions[0].v, 0.75, 1e-6);  // mirrored: 1 - 0.25

  // A display-sized target is not mirrored: it is in the display convention.
  TargetDesc display = makeTarget(1832, 1920);
  display.layers = 2;
  FoveationPlan full = planForTarget(display, s, 1);
  CHECK(!full.scaledTarget);
  CHECK_NEAR(full.regions[0].v, 0.25, 1e-6);

  // A single-layer target is the final image, not an intermediate stereo array.
  TargetDesc single = makeTarget(916, 960);
  single.layers = 1;
  FoveationPlan plain = planForTarget(single, s, 1);
  CHECK_NEAR(plain.regions[0].v, 0.25, 1e-6);

  // Off by default.
  Config off;
  s.config = std::make_shared<const Config>(off);
  FoveationPlan unmirrored = planForTarget(layered, s, 1);
  CHECK_NEAR(unmirrored.regions[0].v, 0.25, 1e-6);
}

// A swapchain image rendered before the frame that submitted the previous image
// ended has no matching projection slot yet. It must still be treated as the
// final eye image, otherwise foveation and the paint marker switch on and off on
// alternate frames (VTOL under AFW does exactly this).
VRSFOV_TEST(classify_swapchain_image_without_slots) {
  Snapshot s = makeSnapshot();
  SwapchainInfo info;
  info.swapchain = 0x55;
  info.width = 1832;
  info.height = 1920;
  info.arraySize = 2;
  SwapchainMap map;
  map[0xABC] = info;
  s.swapchainByVkImage = std::make_shared<const SwapchainMap>(map);
  s.slots = std::make_shared<const SlotList>();  // nothing submitted yet

  TargetDesc t = makeTarget(1832, 1920);
  t.layers = 2;
  t.colorAttachments.push_back(TargetAtt{0xABC, 0, 2});
  FoveationPlan plan = planForTarget(t, s, 1);
  CHECK(plan.swapchainImageTarget);
  CHECK(plan.swapchainTarget);
  CHECK_EQ(plan.regions.size(), 1u);
  CHECK_EQ(plan.regions[0].w, 1832u);
  CHECK_EQ(plan.regions[0].h, 1920u);

  // A plain intermediate of the same size is not a swapchain image.
  FoveationPlan intermediate = planForTarget(makeTarget(1832, 1920), s, 1);
  CHECK(!intermediate.swapchainImageTarget);
}

VRSFOV_TEST(classify_side_by_side) {
  Snapshot s = makeSnapshot();
  TargetDesc t = makeTarget(3664, 1920);
  FoveationPlan plan = planForTarget(t, s, 1);
  CHECK_EQ(plan.regions.size(), 2u);
  CHECK_EQ(plan.regions[0].x, 0);
  CHECK_EQ(plan.regions[0].w, 1832u);
  CHECK_EQ(plan.regions[1].x, 1832);
  CHECK_NEAR(plan.regions[0].u, 0.25, 1e-6);
  CHECK_NEAR(plan.regions[1].u, 0.75, 1e-6);
}

VRSFOV_TEST(classify_eye_sized_single_target) {
  Snapshot s = makeSnapshot();
  TargetDesc t = makeTarget(1832, 1920);
  FoveationPlan plan = planForTarget(t, s, 1);
  CHECK_EQ(plan.regions.size(), 1u);
  CHECK_NEAR(plan.regions[0].u, 0.5, 1e-6);  // average of both eyes
  CHECK_NEAR(plan.regions[0].v, 0.5, 1e-6);
}

VRSFOV_TEST(classify_rejects_shadow_map) {
  Snapshot s = makeSnapshot();
  TargetDesc t = makeTarget(2048, 2048);
  FoveationPlan plan = planForTarget(t, s, 1);
  CHECK_EQ(plan.regions.size(), 0u);
  CHECK(!isEyeCandidateSize(2048, 2048, s));
  CHECK(isEyeCandidateSize(1832, 1920, s));
  CHECK(isEyeCandidateSize(916, 960, s));   // >= 0.5 * recommended width
  CHECK(!isEyeCandidateSize(400, 420, s));  // too small
}

VRSFOV_TEST(classify_rejects_depth_only) {
  Snapshot s = makeSnapshot();
  TargetDesc t = makeTarget(1832, 1920, /*color=*/false);
  FoveationPlan plan = planForTarget(t, s, 1);
  CHECK_EQ(plan.regions.size(), 0u);
}

VRSFOV_TEST(classify_swapchain_slot) {
  Snapshot s = makeSnapshot();
  auto swapchains = std::make_shared<SwapchainMap>();
  SwapchainInfo info;
  info.swapchain = 77;
  info.width = 1832;
  info.height = 1920;
  info.arraySize = 2;
  (*swapchains)[0xABC] = info;
  s.swapchainByVkImage = swapchains;
  auto slots = std::make_shared<SlotList>();
  slots->push_back(ProjectionSlot{77, 1, 0, 0, 1832, 1920, 1});
  s.slots = slots;

  TargetDesc t = makeTarget(1832, 1920);
  t.colorAttachments.push_back(TargetAtt{0xABC, 1, 1});
  FoveationPlan plan = planForTarget(t, s, 1);
  CHECK_EQ(plan.regions.size(), 1u);
  CHECK_NEAR(plan.regions[0].u, 0.75, 1e-6);  // right eye
  CHECK_NEAR(plan.regions[0].v, 0.70, 1e-6);
  CHECK_EQ(plan.regions[0].w, 1832u);

  // A swapchain image without any matching projection slot falls through to the
  // size-based heuristics.
  auto emptySlots = std::make_shared<SlotList>();
  s.slots = emptySlots;
  plan = planForTarget(t, s, 1);
  CHECK_EQ(plan.regions.size(), 1u);
  CHECK_NEAR(plan.regions[0].u, 0.5, 1e-6);  // averaged gaze
}

VRSFOV_TEST(classify_stale_gaze_falls_back_to_centre) {
  Snapshot s = makeSnapshot();
  s.publishTimeNs = nowNs() - 500 * 1000 * 1000;  // 500 ms ago
  TargetDesc t = makeTarget(1832, 1920);

  FoveationPlan plan = planForTarget(t, s, 1);
  CHECK_EQ(plan.regions.size(), 1u);
  CHECK_NEAR(plan.regions[0].u, 0.5, 1e-6);
  CHECK_NEAR(plan.regions[0].v, 0.5, 1e-6);

  s.eyes[0].valid = false;
  s.eyes[1].valid = false;
  plan = planForTarget(t, s, 1);
  CHECK_EQ(plan.regions.size(), 1u);
  CHECK_NEAR(plan.regions[0].u, 0.5, 1e-6);
}

VRSFOV_TEST(classify_synthetic_any_device) {
  // A synthesized snapshot (standalone/fake-eye) describes the whole process, so
  // it must foveate every device in it - a game and SteamVR's vrclient each create
  // one, and tying the snapshot to the last one left the game unfoveated.
  Snapshot s = makeSnapshot();
  s.synthetic = true;
  s.anyDevice = true;
  s.sessionVkDevice = 0x1111;  // a different device than the target's
  TargetDesc t = makeTarget(1832, 1920);
  t.vkDevice = 0x2222;
  CHECK_EQ(planForTarget(t, s, 1).regions.size(), 1u);
}

VRSFOV_TEST(classify_preconditions) {
  Snapshot s = makeSnapshot();
  TargetDesc t = makeTarget(1832, 1920);

  TargetDesc otherDevice = t;
  otherDevice.vkDevice = 0x9999;
  CHECK_EQ(planForTarget(otherDevice, s, 1).regions.size(), 0u);

  Snapshot notRunning = s;
  notRunning.sessionRunning = false;
  CHECK_EQ(planForTarget(t, notRunning, 1).regions.size(), 0u);

  Config off;
  off.enabled = false;
  Snapshot disabled = s;
  disabled.config = std::make_shared<Config>(off);
  CHECK_EQ(planForTarget(t, disabled, 1).regions.size(), 0u);
}

VRSFOV_TEST(classify_known_size_heuristic_with_no_reference) {
  // No recommended size and no swapchains: everything is a candidate.
  Snapshot s = makeSnapshot();
  s.recommendedW = s.recommendedH = 0;
  CHECK(isEyeCandidateSize(1920, 1080, s));
  TargetDesc t = makeTarget(1920, 1080);
  CHECK_EQ(planForTarget(t, s, 1).regions.size(), 1u);
}
