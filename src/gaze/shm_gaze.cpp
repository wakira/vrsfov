// Reader for the out-of-process vrsfov-gaze publisher. The layer only ever
// reads a shared-memory record; no OpenVR client is created in the game process.
#include "gaze/shm_gaze.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "common/log.h"
#include "common/shared_state.h"
#include "gaze/gaze_shm.h"

namespace vrsfov {

namespace {

// Tell the publisher a game is rendering. On-demand connection keeps it from
// becoming the SteamVR session manager at boot.
void touchWant(int64_t now) {
  static std::atomic<int64_t> lastWantNs{0};
  int64_t last = lastWantNs.load(std::memory_order_relaxed);
  if (now - last < 1000LL * 1000 * 1000) return;
  if (!lastWantNs.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;
  const int fd = open(kGazeWantPath, O_WRONLY | O_CREAT, 0600);
  if (fd >= 0) {
    const char c = '1';
    (void)!write(fd, &c, 1);
    close(fd);
  }
}

// True when a shared library ending in `needle` is mapped into this process.
bool mappedLibrary(const char* needle) {
  FILE* f = fopen("/proc/self/maps", "r");
  if (!f) return false;
  char line[4096];
  const size_t needleLen = strlen(needle);
  bool found = false;
  while (fgets(line, sizeof(line), f)) {
    char* path = strchr(line, '/');
    if (!path) continue;
    std::string p(path);
    while (!p.empty() && (p.back() == '\n' || p.back() == '\r' || p.back() == ' ')) {
      p.pop_back();
    }
    const size_t cut = p.find(" (deleted)");
    if (cut != std::string::npos) p.resize(cut);
    if (p.size() >= needleLen && p.compare(p.size() - needleLen, needleLen, needle) == 0) {
      found = true;
      break;
    }
  }
  fclose(f);
  return found;
}

}  // namespace

bool shmGazePublisherPresent() {
  return access((std::string("/dev/shm") + kGazeShmName).c_str(), R_OK) == 0;
}

bool openvrClientMappedInProcess() {
  // vrclient.so is loaded by SteamVR's OpenVR client (and by the OpenXR runtime
  // when SteamVR is the OpenXR runtime). Only the former is an OpenVR game; the
  // callers additionally require no XrInstance and standalone mode.
  static std::atomic<int> mapped{-1};  // -1 unknown, 0 no, 1 yes
  if (mapped.load(std::memory_order_acquire) == 1) return true;

  static std::atomic<int64_t> lastCheckNs{0};
  const int64_t now = nowNs();
  const int64_t last = lastCheckNs.load(std::memory_order_relaxed);
  if (now - last < 1000LL * 1000 * 1000) return false;
  lastCheckNs.store(now, std::memory_order_relaxed);

  const bool found = mappedLibrary("vrclient.so") ||
                     mappedLibrary("libopenvr_api.so") ||
                     mappedLibrary("vrclient.dll") ||
                     mappedLibrary("openvr_api.dll");
  mapped.store(found ? 1 : 0, std::memory_order_release);
  return found;
}

bool openxrLoaderMappedInProcess() {
  static std::atomic<int> mapped{-1};
  if (mapped.load(std::memory_order_acquire) == 1) return true;

  static std::atomic<int64_t> lastCheckNs{0};
  const int64_t now = nowNs();
  const int64_t last = lastCheckNs.load(std::memory_order_relaxed);
  if (now - last < 1000LL * 1000 * 1000) return false;
  lastCheckNs.store(now, std::memory_order_relaxed);

  const bool found = mappedLibrary("libopenxr_loader.so") ||
                     mappedLibrary("openxr_loader.dll");
  mapped.store(found ? 1 : 0, std::memory_order_release);
  return found;
}

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
  if (!gazeShmRead(map, rec) || now - rec.monoNs > kGazeStaleNs) {
    LOG_ONCE(DEBUG, "shm-gaze-no-data", "gaze publisher has no fresh sample");
    return false;
  }
  out.u0 = rec.u0;
  out.v0 = rec.v0;
  out.u1 = rec.u1;
  out.v1 = rec.v1;
  out.monoNs = rec.monoNs;
  out.seq = rec.seq;
  out.valid = rec.valid != 0;
  out.everValid = rec.everValid != 0;
  out.recommendedW = rec.recommendedW;
  out.recommendedH = rec.recommendedH;
  return true;
}

}  // namespace vrsfov
