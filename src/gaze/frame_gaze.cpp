// Steam Frame standalone gaze: read the per-eye centre published by
// vrsfov-gaze over /dev/shm. Nothing in the game's process talks to SteamVR.
#include "vulkan/vk_dispatch.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "common/log.h"
#include "common/shared_state.h"
#include "gaze/gaze_shm.h"

namespace vrsfov {

void updateStandaloneGaze(DeviceData& data) {
  if (!envFlag("VRSFOV_OPENVR_GAZE")) return;
  const int64_t now = nowNs();

  // Tell the publisher a game is rendering. It stays disconnected while this is
  // stale, which keeps it out of the boot ordering race entirely.
  {
    static int64_t lastWantNs = 0;
    if (now - lastWantNs > 1000LL * 1000 * 1000) {
      lastWantNs = now;
      int want = open(kGazeWantPath, O_WRONLY | O_CREAT, 0600);
      if (want >= 0) {
        const char c = '1';
        (void)!write(want, &c, 1);
        close(want);
      }
    }
  }

  // The publisher samples at a fixed 60 Hz; a 4 ms floor plus the seqlock check
  // below means one snapshot per published sample, however often a frame records
  // command buffers.
  constexpr int64_t kPollFloorNs = 4LL * 1000 * 1000;
  if (now - data.lastGazeCheckNs < kPollFloorNs) return;
  data.lastGazeCheckNs = now;

  static int fd = -1;
  static const GazeShm* map = nullptr;
  static int64_t lastOpenAttempt = 0;
  static bool have = false;
  static float lu = 0.5f, lv = 0.5f, ru = 0.5f, rv = 0.5f;
  static float lastLoggedU = -1.f, lastLoggedV = -1.f;
  static int64_t lastLogNs = 0;

  if (!map) {
    if (now - lastOpenAttempt < 1000LL * 1000 * 1000) return;
    lastOpenAttempt = now;
    fd = shm_open(kGazeShmName, O_RDONLY, 0);
    if (fd >= 0) {
      void* p = mmap(nullptr, sizeof(GazeShm), PROT_READ, MAP_SHARED, fd, 0);
      if (p != MAP_FAILED) {
        map = static_cast<const GazeShm*>(p);
        LOG_INFO("standalone gaze: publisher found (%s)", kGazeShmName);
      } else {
        close(fd);
        fd = -1;
      }
    }
    if (!map) {
      LOG_ONCE(INFO, "standalone-gaze-no-publisher",
               "standalone gaze: no publisher at %s (start vrsfov-gaze); fixed foveation",
               kGazeShmName);
      return;
    }
  }

  static uint64_t lastSeq = 0;
  GazeShm rec{};
  if (!gazeShmRead(map, rec) || !rec.valid || now - rec.monoNs > kGazeStaleNs) {
    LOG_ONCE(DEBUG, "standalone-gaze-no-data",
             "standalone gaze: no fresh sample; keeping the last centre");
    return;
  }
  if (rec.seq == lastSeq) return;
  lastSeq = rec.seq;

  float gu0 = rec.u0, gv0 = rec.v0, gu1 = rec.u1, gv1 = rec.v1;
  if (data.localConfig.gazeYFlip) {
    gv0 = 1.f - gv0;
    gv1 = 1.f - gv1;
  }

  float s = data.localConfig.smoothing;
  if (s < 0.f) s = 0.f;
  if (s > 0.99f) s = 0.99f;
  if (!have) {
    lu = gu0, lv = gv0, ru = gu1, rv = gv1;
    have = true;
  } else {
    lu = s * lu + (1.f - s) * gu0;
    lv = s * lv + (1.f - s) * gv0;
    ru = s * ru + (1.f - s) * gu1;
    rv = s * rv + (1.f - s) * gv1;
  }

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
           "standalone gaze: eye tracking active (publisher), centre left (%.3f, %.3f) "
           "right (%.3f, %.3f)",
           (double)lu, (double)lv, (double)ru, (double)rv);

  const float du = (lu > lastLoggedU ? lu - lastLoggedU : lastLoggedU - lu);
  const float dv = (lv > lastLoggedV ? lv - lastLoggedV : lastLoggedV - lv);
  if (now - lastLogNs >= 1000LL * 1000 * 1000 && (du > 0.03f || dv > 0.03f)) {
    lastLogNs = now;
    lastLoggedU = lu;
    lastLoggedV = lv;
    LOG_INFO("standalone gaze: left (%.3f, %.3f) right (%.3f, %.3f)", (double)lu, (double)lv,
             (double)ru, (double)rv);
  }
}

}  // namespace vrsfov
