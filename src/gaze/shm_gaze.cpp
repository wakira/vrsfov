// Reader for the out-of-process vrsfov-gaze publisher. Used as the last gaze
// fallback on PC when neither the native OpenXR action nor an in-process OpenVR
// client has data (some games go inactive during actual gameplay).
#include "gaze/shm_gaze.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "common/log.h"
#include "common/shared_state.h"
#include "gaze/gaze_shm.h"

namespace vrsfov {

namespace {

// Tell the publisher a game is rendering. On-demand connection keeps it from
// becoming the SteamVR session manager at boot.
void touchWant(int64_t now) {
  static int64_t lastWantNs = 0;
  if (now - lastWantNs < 1000LL * 1000 * 1000) return;
  lastWantNs = now;
  const int fd = open(kGazeWantPath, O_WRONLY | O_CREAT, 0600);
  if (fd >= 0) {
    const char c = '1';
    (void)!write(fd, &c, 1);
    close(fd);
  }
}

}  // namespace

bool readShmGaze(ShmGazeSample& out) {
  const int64_t now = nowNs();
  touchWant(now);

  static int fd = -1;
  static const GazeShm* map = nullptr;
  static int64_t lastOpenAttempt = 0;
  if (!map) {
    if (now - lastOpenAttempt < 1000LL * 1000 * 1000) return false;
    lastOpenAttempt = now;
    fd = shm_open(kGazeShmName, O_RDONLY, 0);
    if (fd >= 0) {
      void* p = mmap(nullptr, sizeof(GazeShm), PROT_READ, MAP_SHARED, fd, 0);
      if (p != MAP_FAILED) {
        map = static_cast<const GazeShm*>(p);
        LOG_INFO("gaze publisher found (%s)", kGazeShmName);
      } else {
        close(fd);
        fd = -1;
      }
    }
    if (!map) {
      LOG_ONCE(DEBUG, "shm-gaze-no-publisher",
               "no gaze publisher at %s; fixed/previous source remains", kGazeShmName);
      return false;
    }
  }

  GazeShm rec{};
  if (!gazeShmRead(map, rec) || !rec.valid || now - rec.monoNs > kGazeStaleNs) {
    LOG_ONCE(DEBUG, "shm-gaze-no-data", "gaze publisher has no fresh sample");
    return false;
  }
  out.u0 = rec.u0;
  out.v0 = rec.v0;
  out.u1 = rec.u1;
  out.v1 = rec.v1;
  out.monoNs = rec.monoNs;
  return true;
}

}  // namespace vrsfov
