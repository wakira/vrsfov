// Standalone gaze for OpenVR games: read the out-of-process vrsfov-gaze
// publisher from /dev/shm. Nothing in the game's process talks to OpenVR.
//
// This source is only used when the process is an OpenVR game:
//   - standalone mode is active (the caller checks that),
//   - there is no OpenXR instance, and
//   - vrclient.so is mapped (the game has an OpenVR client).
// Plain Vulkan apps and OpenXR apps never read the publisher.
#include "vulkan/vk_dispatch.h"

#include <cmath>
#include <cstdlib>

#include "common/log.h"
#include "common/shared_state.h"
#include "gaze/shm_gaze.h"

namespace vrsfov {

void updateStandaloneGaze(DeviceData& data) {
  const char* gazeEnv = getenv("VRSFOV_OPENVR_GAZE");
  if (gazeEnv && *gazeEnv && !envFlag("VRSFOV_OPENVR_GAZE")) return;

  // Make sure the out-of-process source is used for OpenVR games only. A
  // SteamVR OpenXR application also has vrclient.so mapped, so the instance
  // check is what keeps the OpenVR publisher out of the OpenXR path.
  if (SharedState::get().liveXrInstances.load() > 0) return;
  if (openxrLoaderMappedInProcess()) return;
  if (!openvrClientMappedInProcess()) return;

  const int64_t now = nowNs();
  constexpr int64_t kPollFloorNs = 4LL * 1000 * 1000;
  if (now - data.lastGazeCheckNs < kPollFloorNs) return;
  data.lastGazeCheckNs = now;

  ShmGazeSample sample;
  if (!readShmGaze(sample)) {
    LOG_ONCE(DEBUG, "standalone-gaze-no-publisher-sample",
             "standalone gaze: no fresh publisher sample; keeping the last centre");
    return;
  }

  // The publisher also reports the runtime's recommended per-eye size. Use it
  // as the classifier reference only when the config did not specify one; an
  // explicit eye_size (or VRSFOV_FAKE_EYE) always wins.
  const char* fakeEye = getenv("VRSFOV_FAKE_EYE");
  if (data.localConfig.eyeWidth == 0 && !(fakeEye && *fakeEye) && sample.recommendedW &&
      sample.recommendedH) {
    const auto snapshot = SharedState::get().snapshot();
    if (!snapshot || snapshot->recommendedW != sample.recommendedW ||
        snapshot->recommendedH != sample.recommendedH) {
      SharedState::get().update([&](Snapshot& snap) {
        snap.recommendedW = sample.recommendedW;
        snap.recommendedH = sample.recommendedH;
      });
      LOG_INFO("standalone size: using publisher recommended per-eye %ux%u (eye_size is auto)",
               sample.recommendedW, sample.recommendedH);
    }
  }

  // eye_tracking = 0 keeps the publisher's recommended size above but leaves
  // the fovea fixed. Useful for HMDs/runtimes that report a centre without real
  // eye tracking.
  if (!data.localConfig.eyeTracking) {
    LOG_ONCE(INFO, "standalone-gaze-off",
             "standalone gaze: eye_tracking=0; using fixed foveation");
    return;
  }

  if (!sample.valid) {
    LOG_ONCE(DEBUG, "standalone-gaze-publisher-no-data",
             "standalone gaze: publisher has no eye-tracked sample; fixed foveation remains");
    return;
  }

  // The publisher samples at 60 Hz. Do not apply the EMA repeatedly when Vulkan
  // records many command buffers between two samples.
  static uint64_t lastSequence = 0;
  if (sample.seq == lastSequence) return;
  lastSequence = sample.seq;

  float u0 = sample.u0, v0 = sample.v0, u1 = sample.u1, v1 = sample.v1;
  if (data.localConfig.gazeYFlip) {
    v0 = 1.f - v0;
    v1 = 1.f - v1;
  }

  static bool have = false;
  static float lu = 0.5f, lv = 0.5f, ru = 0.5f, rv = 0.5f;
  float s = data.localConfig.smoothing;
  if (s < 0.f) s = 0.f;
  if (s > 0.99f) s = 0.99f;
  auto smooth = [&](float previous, float sampleValue) {
    if (!have || s <= 0.f || std::fabs(sampleValue - previous) > 0.15f) return sampleValue;
    return previous + (1.f - s) * (sampleValue - previous);
  };
  lu = smooth(lu, u0);
  lv = smooth(lv, v0);
  ru = smooth(ru, u1);
  rv = smooth(rv, v1);
  have = true;

  SharedState::get().update([&](Snapshot& snap) {
    snap.eyes[0] = EyeView{true, lu, lv, true};
    snap.eyes[1] = EyeView{true, ru, rv, true};
    snap.publishTimeNs = nowNs();
  });
  Stats& stats = SharedState::get().stats;
  stats.eyeTracked.store(1);
  stats.gazeU.store(lu);
  stats.gazeV.store(lv);
  LOG_ONCE(INFO, "standalone-gaze-publisher-active",
           "standalone gaze: eye tracking active (publisher), centre left (%.3f, %.3f) "
           "right (%.3f, %.3f)",
           (double)lu, (double)lv, (double)ru, (double)rv);
}

}  // namespace vrsfov
