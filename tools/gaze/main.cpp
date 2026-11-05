// vrsfov-gaze: out-of-process eye-tracked foveation centre publisher.
//
// Connects to SteamVR as a non-scene client (VRApplication_Other by default),
// polls IVRSystem::GetEyeTrackedFoveationCenter and publishes the per-eye centre
// into /dev/shm/vrsfov-gaze. The vrsfov layer only reads that record, so no
// second SteamVR client ever lives inside the game's process.
//
// Connection is ON DEMAND: the layer touches /dev/shm/vrsfov-gaze.want while a
// game is rendering, and this process stays disconnected otherwise. That matters
// on the Frame: gamescope's OpenVR backend asserts (GetSupportedModifiers ->
// !modifiers.empty(), rendervulkan.cpp) if it becomes VR session manager after
// another VR app has already connected, which at boot meant a dead session and an
// autologin loop. Idle at boot, connect while a game runs.
//
// Usage: vrsfov-gaze [-q]        (-q: only log state changes)
// Env:   VRSFOV_OPENVR_CLIENT=2|3|5   client type (default Other)
//
// Sampling is a fixed 60 Hz: that is the rate SteamVR requests from the eye
// tracking service ("cv: Request eye tracking framerate: 60"), which is what the
// eye cameras run at, so polling faster only re-reads the same sample.
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <string>

#include "gaze/gaze_shm.h"
#include "common/openvr_gaze.h"

namespace {

int64_t monoNs() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

// st_mtime is wall clock: compare it against CLOCK_REALTIME, never against
// CLOCK_MONOTONIC (that made the want file look permanently fresh).
int64_t realNs() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

// True when the layer asked for gaze recently.
bool wantFresh(int64_t now, int64_t maxAgeNs) {
  struct stat st;
  if (stat(vrsfov::kGazeWantPath, &st) != 0) return false;
  const int64_t mtime = (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
  return now - mtime <= maxAgeNs;
}

}  // namespace

int main(int argc, char **argv) {
  bool quiet = false;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "-q")) quiet = true;
  }

  // The eye cameras run at 60 Hz and SteamVR asks the service for 60 Hz, so that
  // is the whole budget: faster polling returns the same sample.
  constexpr int kSampleHz = 60;

  int fd = shm_open(vrsfov::kGazeShmName, O_CREAT | O_RDWR, 0600);
  if (fd < 0) {
    perror("vrsfov-gaze: shm_open");
    return 2;
  }
  if (ftruncate(fd, (off_t)sizeof(vrsfov::GazeShm)) != 0) {
    perror("vrsfov-gaze: ftruncate");
    return 2;
  }
  auto *shm = static_cast<vrsfov::GazeShm *>(
      mmap(nullptr, sizeof(vrsfov::GazeShm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  if (shm == MAP_FAILED) {
    perror("vrsfov-gaze: mmap");
    return 2;
  }
  memset(shm, 0, sizeof(*shm));
  shm->magic = vrsfov::kGazeMagic;
  shm->version = vrsfov::kGazeVersion;
  const char *ct = getenv("VRSFOV_OPENVR_CLIENT");
  shm->clientType = (uint32_t)(ct && *ct ? atoi(ct) : 2);

  bool connected = false;
  bool lastValid = false;
  bool loggedFirstSample = false;
  int64_t lastLog = 0;
  int backoffMs = 500;
  int64_t lastStateLog = 0;

  // State transitions are always logged (they are rare and are the only way to
  // tell from the journal whether the publisher is idle or connected).
  fprintf(stderr, "vrsfov-gaze: idle until %s is fresh; sampling at %d Hz\n",
          vrsfov::kGazeWantPath, kSampleHz);

  for (;;) {
    const int64_t now = monoNs();
    const bool want = wantFresh(realNs(), 5LL * 1000 * 1000 * 1000);  // 5 s

    if (!want) {
      if (connected) {
        vrsfov::openvrGazeShutdown();
        connected = false;
        lastValid = false;
        // Publish a "not tracking" record so the layer stops holding a stale one.
        vrsfov::GazeShm v{};
        v.magic = vrsfov::kGazeMagic;
        v.version = vrsfov::kGazeVersion;
        v.clientType = shm->clientType;
        v.monoNs = now;
        vrsfov::gazeShmPublish(shm, v);
        fprintf(stderr, "vrsfov-gaze: disconnected (no game asked for gaze)\n");
      }
      usleep(250 * 1000);
      continue;
    }

    if (!connected) {
      std::string report;
      if (!vrsfov::openvrGazePrepare(report)) {
        if (now - lastStateLog > 30LL * 1000 * 1000 * 1000) {
          lastStateLog = now;
          fprintf(stderr, "vrsfov-gaze: connect failed: %s (retry in %d ms)\n", report.c_str(),
                  backoffMs);
        }
        usleep((useconds_t)backoffMs * 1000);
        backoffMs = backoffMs < 5000 ? backoffMs * 2 : 5000;
        continue;
      }
      connected = true;
      backoffMs = 500;
      fprintf(stderr, "vrsfov-gaze: connected: %s\n", report.c_str());
    }

    vrsfov::GazeShm v{};
    v.magic = vrsfov::kGazeMagic;
    v.version = vrsfov::kGazeVersion;
    v.clientType = shm->clientType;
    v.monoNs = now;

    float u0 = 0.f, v0 = 0.f, u1 = 0.f, v1 = 0.f;
    const bool ok = vrsfov::openvrGazeRead(u0, v0, u1, v1);
    v.valid = ok ? 1u : 0u;
    v.everValid = (ok || lastValid) ? 1u : 0u;
    if (ok) {
      v.u0 = u0, v.v0 = v0, v.u1 = u1, v.v1 = v1;
      lastValid = true;
    }

    vrsfov::gazeShmPublish(shm, v);

    if ((!loggedFirstSample && ok) || (!quiet && lastValid != ok && now - lastLog > 1000LL * 1000 * 1000)) {
      lastLog = now;
      if (ok) loggedFirstSample = true;
      fprintf(stderr, "vrsfov-gaze: %s L=(%.3f, %.3f) R=(%.3f, %.3f)\n",
              ok ? "tracking" : "no sample (headset off?)", (double)u0, (double)v0, (double)u1,
              (double)v1);
    }
    usleep((useconds_t)(1000000 / kSampleHz));
  }
}
