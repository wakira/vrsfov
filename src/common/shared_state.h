// Process-wide state shared by the OpenXR half (writer) and the Vulkan half
// (reader) of the layer. Both halves live in the same shared object, so a
// function-local static singleton is enough; log its address at init to verify.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "common/config.h"

namespace vrsfov {

struct EyeView {  // per eye, per frame
  bool valid = false;
  float u = 0.5f, v = 0.5f;  // gaze in [0,1] of the eye image (v down, Vulkan)
  bool eyeTracked = false;   // false => fixed foveation radii are used
};

struct ProjectionSlot {  // from the last xrEndFrame
  uint64_t swapchain = 0;  // XrSwapchain as integer
  uint32_t arrayIndex = 0; // XrSwapchainSubImage.imageArrayIndex
  int32_t x = 0, y = 0, w = 0, h = 0;  // imageRect, pixels
  uint32_t eye = 0;                    // 0 = left, 1 = right
};

struct SwapchainInfo {
  uint64_t swapchain = 0;
  uint32_t width = 0, height = 0, arraySize = 0;
  int64_t format = 0;
};

// The fov an app submits for a view in its projection layer (radians). This is
// what the compositor uses to map the eye image onto the display, so it, not the
// runtime's recommended fov, defines how the image maps to view directions. Some
// apps submit something different from xrLocateViews' fov (a crop, a different
// aspect, or a vertically flipped image).
struct ViewFov {
  float left = 0.f, right = 0.f, up = 0.f, down = 0.f;
  bool valid = false;
};

// One view as the app submitted it: fov plus the orientation it rendered with.
struct SubmittedView {
  ViewFov fov;
  float qx = 0.f, qy = 0.f, qz = 0.f, qw = 1.f;  // app-submitted orientation
};

using SwapchainMap = std::unordered_map<uint64_t, SwapchainInfo>;
using SlotList = std::vector<ProjectionSlot>;

// Immutable once published.
struct Snapshot {
  uint64_t frameIndex = 0;
  uint64_t sessionVkDevice = 0;  // VkDevice as integer, 0 = none
  bool sessionRunning = false;
  uint32_t recommendedW = 0, recommendedH = 0;  // per-eye recommended size
  EyeView eyes[2];
  // App-submitted projection view data (index 0 left, 1 right); fov.valid=false
  // until the app has ended a frame.
  SubmittedView submittedView[2];
  // Rebuilt only when they change; null means "empty".
  std::shared_ptr<const SlotList> slots;
  std::shared_ptr<const SwapchainMap> swapchainByVkImage;
  std::shared_ptr<const Config> config;
  int64_t publishTimeNs = 0;  // CLOCK_MONOTONIC
  // True when the eye data is synthesized (standalone/fake-eye mode) rather than
  // published by a live OpenXR session: it never goes stale.
  bool synthetic = false;
  // True for synthesized snapshots: they describe the whole process, so every
  // VkDevice in it counts (a game and SteamVR's vrclient have separate devices).
  bool anyDevice = false;
};

// Foveation statistics, incremented by the Vulkan half.
struct Stats {
  std::atomic<uint64_t> frames{0};
  std::atomic<uint64_t> passes{0};
  std::atomic<uint64_t> foveatedPasses{0};
  std::atomic<uint64_t> neutralPasses{0};
  std::atomic<uint64_t> paintRecords{0};  // gaze boxes recorded into command buffers
  std::atomic<int> eyeTracked{0};
  std::atomic<float> gazeU{0.5f};
  std::atomic<float> gazeV{0.5f};
};

class SharedState {
 public:
  static SharedState& get();
  std::shared_ptr<const Snapshot> snapshot() const {
    return std::atomic_load_explicit(&snap_, std::memory_order_acquire);
  }
  void publish(std::shared_ptr<const Snapshot> s) {
    std::lock_guard<std::mutex> lock(writerMutex_);
    std::atomic_store_explicit(&snap_, std::move(s), std::memory_order_release);
  }
  // Copy the current snapshot (or a default one), mutate the copy, publish.
  template <class F>
  void update(F&& mutate) {
    std::lock_guard<std::mutex> lock(writerMutex_);
    auto cur = std::atomic_load_explicit(&snap_, std::memory_order_acquire);
    auto next = std::make_shared<Snapshot>(cur ? *cur : Snapshot{});
    if (!next->slots) next->slots = std::make_shared<const SlotList>();
    if (!next->swapchainByVkImage) next->swapchainByVkImage = std::make_shared<const SwapchainMap>();
    mutate(*next);
    std::atomic_store_explicit(&snap_, std::shared_ptr<const Snapshot>(next), std::memory_order_release);
  }

  std::atomic<int> liveXrInstances{0};
  Stats stats;

 private:
  SharedState();
  mutable std::mutex writerMutex_;
  std::shared_ptr<const Snapshot> snap_;
};

int64_t nowNs();  // CLOCK_MONOTONIC

// The config published by the OpenXR half, or a default-constructed one when no
// snapshot has been published yet.
const Config& effectiveConfig(const std::shared_ptr<const Snapshot>& snap);

}  // namespace vrsfov
