#pragma once

#include <cstdint>

namespace vrsfov {

struct ShmGazeSample {
  float u0 = 0.5f, v0 = 0.5f;
  float u1 = 0.5f, v1 = 0.5f;
  int64_t monoNs = 0;
};

// Reads the latest sample from the out-of-process vrsfov-gaze publisher. Also
// touches the "want" file so the publisher connects to SteamVR on demand.
// Returns false when there is no publisher, no fresh sample, or a torn write.
bool readShmGaze(ShmGazeSample& out);

}  // namespace vrsfov
