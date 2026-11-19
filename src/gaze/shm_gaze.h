#pragma once

#include <cstdint>

namespace vrsfov {

struct ShmGazeSample {
  float u0 = 0.5f, v0 = 0.5f;
  float u1 = 0.5f, v1 = 0.5f;
  int64_t monoNs = 0;
  uint64_t seq = 0;
  bool valid = false;     // this record has a usable gaze sample
  bool everValid = false; // the publisher has produced a sample at some point
  uint32_t recommendedW = 0, recommendedH = 0;
};

// True when the publisher's /dev/shm segment exists.
bool shmGazePublisherPresent();

// True once this process has an OpenVR runtime client mapped: native
// vrclient.so (or libopenvr_api.so) or Wine's vrclient.dll/openvr_api.dll.
// The publisher and the shared-memory gaze source must only be used for
// OpenVR applications, never for plain Vulkan or OpenXR applications.
bool openvrClientMappedInProcess();

// True when an OpenXR loader is already mapped. Used as an early OpenVR-game
// test before the first XrInstance exists: an OpenXR application must never
// contact the OpenVR publisher, even if it also maps vrclient.so.
bool openxrLoaderMappedInProcess();

// Reads the latest sample from the out-of-process vrsfov-gaze publisher. Also
// touches the "want" file so the publisher connects to SteamVR on demand.
// Returns false when there is no publisher, no fresh sample, or a torn write.
bool readShmGaze(ShmGazeSample& out);

}  // namespace vrsfov
