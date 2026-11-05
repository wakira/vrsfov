// PC standalone gaze: SteamVR's own eye-tracked foveation centre, read directly
// from libopenvr_api.so in the game's process.
#include "vulkan/vk_dispatch.h"

#include <algorithm>
#include <cmath>

#include "common/log.h"
#include "common/openvr_gaze.h"
#include "common/shared_state.h"

namespace vrsfov {

void updateStandaloneGaze(DeviceData& data) {
  if (!envFlag("VRSFOV_OPENVR_GAZE")) return;
  const int64_t now = nowNs();
  constexpr int64_t kPollFloorNs = 8LL * 1000 * 1000;  // 120 Hz
  if (now - data.lastGazeCheckNs < kPollFloorNs) return;
  data.lastGazeCheckNs = now;

  static bool prepared = false;
  static bool available = false;
  if (!prepared) {
    prepared = true;
    std::string report;
    available = openvrGazePrepare(report);
    LOG_INFO("standalone gaze (VRSFOV_OPENVR_GAZE): %s", report.c_str());
  }
  if (!available) return;

  float u0 = 0.f, v0 = 0.f, u1 = 0.f, v1 = 0.f;
  if (!openvrGazeRead(u0, v0, u1, v1)) {
    LOG_ONCE(DEBUG, "standalone-gaze-no-data",
             "standalone gaze: no eye-tracked centre this poll; keeping the last one");
    return;
  }

  // Some stacks compose the eye image flipped vertically relative to the gaze
  // convention; gaze_yflip mirrors v to match the image the rate map describes.
  if (data.localConfig.gazeYFlip) {
    v0 = 1.f - v0;
    v1 = 1.f - v1;
  }
  u0 = std::min(1.f, std::max(0.f, u0 + data.localConfig.gazeOffsetX));
  u1 = std::min(1.f, std::max(0.f, u1 + data.localConfig.gazeOffsetX));
  v0 = std::min(1.f, std::max(0.f, v0 + data.localConfig.gazeOffsetY));
  v1 = std::min(1.f, std::max(0.f, v1 + data.localConfig.gazeOffsetY));

  // Per-eye EMA. A jump larger than 0.15 restarts the filter instead of dragging
  // the fovea across the image for seconds.
  static bool have = false;
  static float lu = 0.5f, lv = 0.5f, ru = 0.5f, rv = 0.5f;
  const float s = data.localConfig.smoothing;
  auto smooth = [&](float previous, float sample) {
    if (!have || s <= 0.f || std::fabs(sample - previous) > 0.15f) return sample;
    return previous + (1.f - s) * (sample - previous);
  };
  lu = smooth(lu, u0);
  lv = smooth(lv, v0);
  ru = smooth(ru, u1);
  rv = smooth(rv, v1);
  have = true;

  // Per eye, because the snapshot carries the gaze per eye: the classifier then
  // averages for a single-target pass and uses each eye's own centre for per-eye
  // and side-by-side targets.
  SharedState::get().update([&](Snapshot& snap) {
    snap.eyes[0] = EyeView{true, lu, lv, true};
    snap.eyes[1] = EyeView{true, ru, rv, true};
    snap.publishTimeNs = nowNs();
  });
  Stats& stats = SharedState::get().stats;
  stats.eyeTracked.store(1);
  stats.gazeU.store(lu);
  stats.gazeV.store(lv);
  LOG_ONCE(INFO, "standalone-gaze-active",
           "standalone gaze: eye tracking active (OpenVR source), centre left (%.3f, %.3f) "
           "right (%.3f, %.3f)",
           (double)lu, (double)lv, (double)ru, (double)rv);
}

}  // namespace vrsfov
