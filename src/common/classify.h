// Eye-target classification: is this render target / render area an eye render,
// and where is each eye's fovea inside it?
//
// Pure C++ (no Vulkan calls), unit tested (06 §3).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/shared_state.h"

namespace vrsfov {

struct TargetAtt {
  uint64_t vkImage = 0;
  uint32_t baseLayer = 0;
  uint32_t layerCount = 1;
};

struct TargetDesc {
  uint32_t width = 0, height = 0;  // framebuffer size, or renderArea offset+extent
  uint32_t viewMask = 0;           // 0 when the target is not multiview
  uint32_t layers = 1;             // framebuffer layers / rendering layerCount
  bool hasColor = false;
  std::vector<TargetAtt> colorAttachments;  // resolved from image views
  uint64_t vkDevice = 0;
  // Where width/height come from: "framebuffer" for a render pass, "render area"
  // for dynamic rendering (which is the renderArea, not the attachment's size -
  // small values mean a sub-rectangle update, not a small target).
  const char* source = "framebuffer";
};

struct Region {
  uint32_t layer = 0;
  int32_t x = 0, y = 0;
  uint32_t w = 0, h = 0;
  float u = 0.5f, v = 0.5f;
};

struct FoveationPlan {
  std::vector<Region> regions;
  std::string reason;  // human readable, includes the numbers that decided it
  // The regions came from matching an XR swapchain image (the final eye image the
  // compositor displays). False for intermediate targets.
  bool swapchainTarget = false;
  // The target's colour image belongs to a known XR swapchain. Unlike
  // swapchainTarget this does not need the per-frame projection slots, so it does
  // not flip from frame to frame when the app renders an image before the frame
  // that submitted it has ended.
  bool swapchainImageTarget = false;
  // The target is smaller than the display's eye size: an upscale source
  // (VTOL renders 1810 and upscales to 3620), not the displayed eye image.
  bool scaledTarget = false;
};

// Empty plan == neutral (full rate).
FoveationPlan planForTarget(const TargetDesc& target, const Snapshot& snapshot,
                            uint32_t rateImageLayers);

// 06 §3: does (w, h) look like an eye render at the current recommended size?
bool isEyeCandidateSize(uint32_t w, uint32_t h, const Snapshot& snapshot);

// Resolves both eyes' gaze points with the same validity/freshness fallback that
// planForTarget uses (a stale or invalid sample becomes the centre). Used by the
// VRSFOV_PAINT_GAZE debug marker so a layered pass without multiview can paint
// each eye at its own gaze.
void resolveEyeGaze(const Snapshot& snapshot, float& leftU, float& leftV, float& rightU,
                    float& rightV, bool& eyeTracked);

}  // namespace vrsfov
