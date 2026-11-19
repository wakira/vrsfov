// Command buffer hooks: per-command-buffer state, dynamic rendering injection
// (05 §4) and the transfer-only rate image updates (05 §5).
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "common/classify.h"
#include "common/guard.h"
#include "common/log.h"
#include "common/pattern.h"
#include "common/shared_state.h"
#include "vulkan/vk_dispatch.h"
#include "vulkan/vk_hooks.h"
#include "vulkan/vk_resources.h"

using namespace vrsfov;

namespace vrsfov {
namespace {

const void* chainFind(const void* head, VkStructureType sType) {
  for (auto* node = reinterpret_cast<const VkBaseInStructure*>(head); node; node = node->pNext) {
    if (node->sType == sType) return node;
  }
  return nullptr;
}

uint32_t highestSetBit(uint32_t v) {
  uint32_t bit = 0;
  while (v >>= 1) ++bit;
  return bit;
}

void imageBarrier(VkCommandBuffer cmd, DeviceData& data, VkImage image, uint32_t layers,
                  VkImageLayout oldLayout, VkImageLayout newLayout, VkPipelineStageFlags srcStage,
                  VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
  VkImageMemoryBarrier barrier{};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
  barrier.srcAccessMask = srcAccess;
  barrier.dstAccessMask = dstAccess;
  data.d.CmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void clearRateImage(VkCommandBuffer cmd, DeviceData& data, RateImage& img) {
  if (!data.d.CmdClearColorImage) return;
  VkClearColorValue color{};
  color.uint32[0] = 0;
  VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, img.layers};
  data.d.CmdClearColorImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
}

// Does one region cover the whole rate image (all texels of its layer)?
bool regionCoversAll(const Region& r, uint32_t texel, const RateImage& img) {
  CopyRegion cr;
  if (!gazeCopyRegion(r.x, r.y, r.w, r.h, r.layer, texel, img.width, img.height, r.u, r.v, cr)) {
    return false;
  }
  return cr.imageX == 0 && cr.imageY == 0 && cr.imageW == img.width && cr.imageH == img.height;
}

// Hands the rate image over in FSR_OPTIMAL once its writes are complete.
//
// The destination stage is ALL_GRAPHICS plus the fragment shading rate
// attachment stage, and the access masks include the generic memory
// read/write bits, on purpose. Turnip (Steam Frame, Adreno 750) does not make
// the transfer writes visible to the attachment read when the dependency is
// expressed with VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR
// alone: only the tail of the rate image became visible and the top stayed at
// its previous (cleared) content, which put the fovea in the wrong place. The
// wider barrier is conservative, valid, and costs nothing in practice.
void rateImageReadyBarrier(VkCommandBuffer cmd, DeviceData& data, RateImage& img) {
  imageBarrier(cmd, data, img.image, img.layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR,
               VK_PIPELINE_STAGE_TRANSFER_BIT,
               VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
               VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT |
                   VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,
               VK_ACCESS_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR |
                   VK_ACCESS_MEMORY_READ_BIT);
}

// 05 §5.1: transition, clear when needed, copy the static pattern windows, and
// hand the image over in FSR_OPTIMAL. Transfer commands only.
void recordRateUpdate(VkCommandBuffer cmd, DeviceData& data, RateImage& img,
                      const FoveationPlan& plan, const Config& config, bool eyeTracked) {
  if (!data.d.CmdPipelineBarrier) return;
  const uint32_t t = data.caps.texel;

  imageBarrier(cmd, data, img.image, img.layers, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, 0,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

  bool fullyCovered = true;
  for (uint32_t layer = 0; layer < img.layers && fullyCovered; ++layer) {
    bool covered = false;
    for (const Region& r : plan.regions) {
      if (r.layer == layer && regionCoversAll(r, t, img)) {
        covered = true;
        break;
      }
    }
    if (!covered) fullyCovered = false;
  }

  if (!fullyCovered) {
    clearRateImage(cmd, data, img);
    if (!plan.regions.empty()) {
      imageBarrier(cmd, data, img.image, img.layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                   VK_ACCESS_TRANSFER_WRITE_BIT);
    }
  }

  if (data.d.CmdCopyBufferToImage && !plan.regions.empty()) {
    std::vector<VkBufferImageCopy> copies;
    const PatternBuffer* current = nullptr;
    auto flush = [&]() {
      if (!copies.empty() && current) {
        data.d.CmdCopyBufferToImage(cmd, current->buffer, img.image,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)copies.size(),
                                    copies.data());
      }
      copies.clear();
    };
    for (const Region& r : plan.regions) {
      CopyRegion cr;
      if (!gazeCopyRegion(r.x, r.y, r.w, r.h, r.layer, t, img.width, img.height, r.u, r.v, cr)) {
        continue;
      }
      LOG_ONCE(DEBUG, "rate-update-first",
               "first rate update: region layer %u rect (%d,%d) %ux%u gaze (%.3f,%.3f) -> "
               "image (%d,%d) %ux%u, source (%u,%u) of %ux%u",
               r.layer, r.x, r.y, r.w, r.h, (double)r.u, (double)r.v, cr.imageX, cr.imageY,
               cr.imageW, cr.imageH, cr.srcX, cr.srcY, 2 * cr.imageW, 2 * cr.imageH);
      const PatternBuffer* pattern = acquirePattern(data, cr.imageW, cr.imageH, eyeTracked, config);
      if (!pattern) continue;
      if (pattern != current) {
        flush();
        current = pattern;
      }
      copies.push_back(toVkBufferImageCopy(cr));
    }
    flush();
  }

  rateImageReadyBarrier(cmd, data, img);
}

// 05 §5.3: the shared neutral image must contain zeros before it is used as an
// attachment; do it once per command buffer (and again if the image was grown).
void ensureNeutralInitialized(VkCommandBuffer cmd, DeviceData& data, CmdState& state,
                              RateImage& img) {
  if (state.neutralInitialized && state.neutralInitImage == img.image) return;
  if (!data.d.CmdPipelineBarrier) return;
  imageBarrier(cmd, data, img.image, img.layers, VK_IMAGE_LAYOUT_UNDEFINED,
               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR, 0,
               VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
  clearRateImage(cmd, data, img);
  rateImageReadyBarrier(cmd, data, img);
  state.neutralInitialized = true;
  state.neutralInitImage = img.image;
}

void fillTargetFromRenderingInfo(DeviceData& data, const VkRenderingInfo* info, TargetDesc& desc) {
  desc.width = (uint32_t)info->renderArea.offset.x + info->renderArea.extent.width;
  desc.height = (uint32_t)info->renderArea.offset.y + info->renderArea.extent.height;
  desc.source = "render area";
  desc.viewMask = info->viewMask;
  desc.layers = info->layerCount;
  desc.vkDevice = (uint64_t)(uintptr_t)data.device;
  desc.hasColor = false;
  desc.colorAttachments.clear();
  if (!info->pColorAttachments) return;
  std::shared_lock<std::shared_mutex> lock(data.lock);
  for (uint32_t i = 0; i < info->colorAttachmentCount; ++i) {
    const VkRenderingAttachmentInfo& att = info->pColorAttachments[i];
    if (!att.imageView) continue;
    desc.hasColor = true;
    auto it = data.imageViews.find(att.imageView);
    if (it == data.imageViews.end()) {
      desc.colorAttachments.push_back(TargetAtt{0, 0, 1});
    } else {
      desc.colorAttachments.push_back(TargetAtt{(uint64_t)(uintptr_t)it->second.image,
                                                it->second.baseArrayLayer, it->second.layerCount});
    }
  }
}

uint32_t rateImageLayersFor(const DeviceData& data, const VkRenderingInfo* info) {
  if (info->viewMask != 0 && data.caps.layeredAttachments) {
    return highestSetBit(info->viewMask) + 1;
  }
  return 1;
}

std::vector<VkClearRect> buildPaintRects(const FoveationPlan& plan, const Snapshot& snap,
                                         const Config& config, bool eyeTracked, uint32_t viewMask,
                                         uint32_t layers, const VkRect2D& renderArea) {
  std::vector<VkClearRect> rects;
  if (!config.paintGaze || plan.regions.empty()) return rects;
  // Paint only the image the compositor displays (an XR swapchain image) when we
  // know the app's swapchains. Intermediates are copied into it - VTOL flips
  // vertically at every hop (1810 -> 3620 -> swapchain) - so painting them too
  // puts extra, mirrored markers into the final image. An upscale source is
  // skipped for the same reason.
  const bool haveSwapchains = snap.swapchainByVkImage && !snap.swapchainByVkImage->empty();
  if (haveSwapchains && !plan.swapchainImageTarget) return rects;
  if (plan.scaledTarget) return rects;
  const float radius = eyeTracked ? config.innerRadius : config.fixedInnerRadius;

  // Per-eye gaze, resolved once. The layered branch paints each eye at its own
  // gaze; the diagnostic below reports them so a gaze source that tracks the two
  // eyes differently is visible in the log.
  float lu = 0.5f, lv = 0.5f, ru = 0.5f, rv = 0.5f;
  bool tracked = false;
  resolveEyeGaze(snap, lu, lv, ru, rv, tracked);

  const int32_t raX0 = renderArea.offset.x;
  const int32_t raY0 = renderArea.offset.y;
  const int32_t raX1 = raX0 + (int32_t)renderArea.extent.width;
  const int32_t raY1 = raY0 + (int32_t)renderArea.extent.height;

  auto appendClamped = [&](const Region& r, uint32_t layer, float u, float v) {
    VkClearRect cr{};
    if (!gazeInnerBoxRect(r.x, r.y, r.w, r.h, layer, u, v, radius, config.horizontalScale, cr)) {
      return;
    }
    const int32_t x0 = std::clamp(cr.rect.offset.x, raX0, raX1);
    const int32_t y0 = std::clamp(cr.rect.offset.y, raY0, raY1);
    const int32_t x1 = std::clamp(cr.rect.offset.x + (int32_t)cr.rect.extent.width, raX0, raX1);
    const int32_t y1 = std::clamp(cr.rect.offset.y + (int32_t)cr.rect.extent.height, raY0, raY1);
    if (x1 > x0 && y1 > y0) {
      cr.rect.offset = {x0, y0};
      cr.rect.extent = {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)};
      rects.push_back(cr);
    }
  };

  if (viewMask != 0) {
    // Multiview: vkCmdClearAttachments broadcasts to all active views and
    // requires baseArrayLayer=0, layerCount=1.
    float u = 0.f, v = 0.f;
    for (const Region& r : plan.regions) {
      u += r.u;
      v += r.v;
    }
    u /= (float)plan.regions.size();
    v /= (float)plan.regions.size();
    appendClamped(plan.regions[0], 0, u, v);
  } else if (layers >= 2 && plan.regions.size() == 1) {
    // Both eyes in one layered target without multiview (VTOL VR): the plan has a
    // single region because one rate image layer is shared by every layer. Paint
    // each layer at its own eye's gaze so both eyes show the marker.
    const float gaze[2][2] = {{lu, lv}, {ru, rv}};
    for (uint32_t l = 0; l < layers && l < 2; ++l) {
      appendClamped(plan.regions[0], l, gaze[l][0], gaze[l][1]);
    }
  } else {
    for (const Region& r : plan.regions) {
      appendClamped(r, r.layer, r.u, r.v);
    }
  }

  // VRSFOV_PAINT_GAZE is a debug switch, so a once-a-second report of the gaze
  // and the rects is acceptable and is what makes a per-eye gaze problem (the
  // two boxes drifting apart on pitch) diagnosable from the log.
  static std::atomic<int64_t> lastPaintLogNs{0};
  const int64_t now = nowNs();
  int64_t last = lastPaintLogNs.load(std::memory_order_relaxed);
  if (now - last > 1000LL * 1000 * 1000 &&
      lastPaintLogNs.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
    std::string where;
    for (const VkClearRect& cr : rects) {
      char buf[72];
      snprintf(buf, sizeof(buf), " [layer %u %dx%d %ux%u]", cr.baseArrayLayer, cr.rect.offset.x,
               cr.rect.offset.y, cr.rect.extent.width, cr.rect.extent.height);
      where += buf;
    }
    LOG_INFO("paint gaze: viewMask=0x%x layers=%u regions=%zu tracked=%d "
             "eye L=(%.3f,%.3f) R=(%.3f,%.3f)%s",
             viewMask, layers, plan.regions.size(), (int)tracked, (double)lu, (double)lv,
             (double)ru, (double)rv, where.c_str());
  }
  return rects;
}

void paintGazeIfPending(VkCommandBuffer cmd, DeviceData& data) {
  std::vector<VkClearRect> rects;
  {
    std::unique_lock<std::shared_mutex> lock(data.lock);
    auto it = data.cmdStates.find(cmd);
    if (it == data.cmdStates.end() || it->second.pendingPaintRects.empty()) return;
    rects.swap(it->second.pendingPaintRects);
  }
  if (!data.d.CmdClearAttachments) return;
  SharedState::get().stats.paintRecords.fetch_add(1, std::memory_order_relaxed);

  // Green for layer 0 (left eye), red for layer 1 (right eye), so the two eyes
  // stay distinguishable while this debug switch is on.
  std::vector<VkClearRect> left, right;
  for (const VkClearRect& r : rects) {
    (r.baseArrayLayer == 1 ? right : left).push_back(r);
  }

  VkClearAttachment att{};
  att.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  att.colorAttachment = 0;
  LOG_ONCE(INFO, "paint-gaze-first",
           "VRSFOV_PAINT_GAZE active: painting %zu gaze box(es) at pass end (left/layer 0 "
           "green, right/layer 1 red; first rect (%d,%d) %ux%u layer %u)",
           rects.size(), rects[0].rect.offset.x, rects[0].rect.offset.y, rects[0].rect.extent.width,
           rects[0].rect.extent.height, rects[0].baseArrayLayer);

  auto clear = [&](std::vector<VkClearRect>& group, float r, float g, float b) {
    if (group.empty()) return;
    att.clearValue.color.float32[0] = r;
    att.clearValue.color.float32[1] = g;
    att.clearValue.color.float32[2] = b;
    att.clearValue.color.float32[3] = 1.0f;
    data.d.CmdClearAttachments(cmd, 1, &att, (uint32_t)group.size(), group.data());
  };
  clear(left, 0.0f, 1.0f, 0.0f);
  clear(right, 1.0f, 0.0f, 0.0f);
}

// Shared body for vkCmdBeginRendering and its KHR alias.
void beginRenderingImpl(VkCommandBuffer cmd, const VkRenderingInfo* info,
                        PFN_vkCmdBeginRendering next) {
  DeviceData* data = findDeviceForCommandBuffer(cmd);
  if (!data || !next || !info) {
    LOG_ONCE(DEBUG, "skip-beginRendering-nodevice", "dynamic rendering pass passed through: %s",
             !next ? "no next entry point" : (!data ? "unknown command buffer/device"
                                                   : "no rendering info"));
    if (next) next(cmd, info);
    return;
  }
  VRSFOV_GUARD_BEGIN
  if (!data->active) {
    LOG_ONCE(DEBUG, "skip-beginRendering-inactive",
             "dynamic rendering pass passed through: device not active");
    next(cmd, info);
    return;
  }
  if (chainFind(info->pNext, VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR) ||
      chainFind(info->pNext, VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT)) {
    next(cmd, info);  // the app manages VRS / density map itself
    return;
  }

  const bool resuming = (info->flags & VK_RENDERING_RESUMING_BIT) != 0;
  const bool suspending = (info->flags & VK_RENDERING_SUSPENDING_BIT) != 0;

  if (resuming) {
    VkImageView lastView = VK_NULL_HANDLE;
    {
      std::unique_lock<std::shared_mutex> lock(data->lock);
      auto it = data->cmdStates.find(cmd);
      if (it != data->cmdStates.end()) {
        lastView = it->second.lastAttachedView;
        it->second.pendingPaintRects = it->second.savedPaintRects;
      }
    }
    if (lastView == VK_NULL_HANDLE) {
      LOG_ONCE(DEBUG, "skip-beginRendering-resume",
               "dynamic rendering pass passed through: resumed without a recorded rate image");
      next(cmd, info);  // no attachment to resume with
      return;
    }
    VkRenderingFragmentShadingRateAttachmentInfoKHR fsr{};
    fsr.sType = VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR;
    fsr.imageView = lastView;
    fsr.imageLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
    fsr.shadingRateAttachmentTexelSize = {data->caps.texel, data->caps.texel};
    VkRenderingInfo copy = *info;
    fsr.pNext = const_cast<void*>(info->pNext);
    copy.pNext = &fsr;
    next(cmd, &copy);
    return;
  }

  auto snapshot = SharedState::get().snapshot();
  const Config& config = effectiveConfig(snapshot);
  static const Snapshot kEmptySnapshot;
  const Snapshot& snap = snapshot ? *snapshot : kEmptySnapshot;

  TargetDesc desc;
  fillTargetFromRenderingInfo(*data, info, desc);
  const uint32_t rateLayers = rateImageLayersFor(*data, info);

  SharedState::get().stats.passes.fetch_add(1, std::memory_order_relaxed);
  const FoveationPlan plan = planForTarget(desc, snap, rateLayers);
  for (const Region& r : plan.regions) {
    LOG_DEBUG("  region layer %u rect (%d,%d) %ux%u gaze (%.3f,%.3f)", r.layer, r.x, r.y, r.w, r.h,
              r.u, r.v);
  }
  if (plan.regions.empty()) {
    SharedState::get().stats.neutralPasses.fetch_add(1, std::memory_order_relaxed);
    next(cmd, info);
    return;
  }

  const uint32_t t = data->caps.texel;
  const uint32_t sw = (desc.width + t - 1) / t;
  const uint32_t sh = (desc.height + t - 1) / t;
  RateImage* img = acquireDynamicRateImage(*data, cmd, sw, sh, rateLayers);
  if (!img) {
    next(cmd, info);
    return;
  }

  const bool eyeTracked = snap.eyes[0].eyeTracked && snap.eyes[1].eyeTracked;
  recordRateUpdate(cmd, *data, *img, plan, config, eyeTracked);

  VkRenderingFragmentShadingRateAttachmentInfoKHR fsr{};
  fsr.sType = VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR;
  fsr.imageView = img->view;
  fsr.imageLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
  fsr.shadingRateAttachmentTexelSize = {t, t};
  VkRenderingInfo copy = *info;
  fsr.pNext = const_cast<void*>(info->pNext);
  copy.pNext = &fsr;

  if (suspending || config.paintGaze) {
    std::vector<VkClearRect> paintRects = buildPaintRects(
        plan, snap, config, eyeTracked, info->viewMask, info->layerCount, info->renderArea);
    std::unique_lock<std::shared_mutex> lock(data->lock);
    CmdState& st = data->cmdStates[cmd];
    if (suspending) {
      st.lastAttachedView = img->view;
      st.savedPaintRects = paintRects;
    }
    st.pendingPaintRects = std::move(paintRects);
  }

  SharedState::get().stats.foveatedPasses.fetch_add(1, std::memory_order_relaxed);
  LOG_ONCE(INFO, ("attach:" + std::to_string(desc.width) + "x" + std::to_string(desc.height) +
                  "x" + std::to_string(img->layers)).c_str(),
           "attaching rate image %ux%u texels x%u layer(s) to %ux%u viewMask=0x%x "
           "(%zu region(s), inner %s middle %s outer %s)",
           img->width, img->height, img->layers, desc.width, desc.height, desc.viewMask,
           plan.regions.size(), rateToString(data->caps.innerRate).c_str(),
           rateToString(data->caps.middleRate).c_str(), rateToString(data->caps.outerRate).c_str());
  LOG_DEBUG("dynamic rendering %ux%u viewMask=0x%x: %zu region(s), attaching rate image "
            "%ux%u texels x%u layer(s) (pattern rates: inner %s, middle %s, outer %s; "
            "debugMode %d)",
            desc.width, desc.height, desc.viewMask, plan.regions.size(), img->width, img->height,
            img->layers, rateToString(data->caps.innerRate).c_str(),
            rateToString(data->caps.middleRate).c_str(), rateToString(data->caps.outerRate).c_str(),
            config.debugMode);
  next(cmd, &copy);
  VRSFOV_GUARD_END_VOID
}

}  // namespace

VKAPI_ATTR VkResult VKAPI_CALL vrsfov_vkBeginCommandBuffer(
    VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo* pBeginInfo) {
  VRSFOV_GUARD_BEGIN
  DeviceData* data = findDeviceForCommandBuffer(commandBuffer);
  if (!data || !data->d.BeginCommandBuffer) {
    LOG_ERROR("vkBeginCommandBuffer: no device data");
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  VkResult result = data->d.BeginCommandBuffer(commandBuffer, pBeginInfo);
  if (result == VK_SUCCESS) {
    // A device created before the XrInstance activates here, long before the app
    // renders (throttled inside).
    if (!data->active) vrsfovMaybeActivate(*data);
    if (data->active) {
      std::unique_lock<std::shared_mutex> lock(data->lock);
      if (data->standalone) vrsfovMaybeReloadStandaloneConfig(*data);
      if (data->standalone) vrsfovMaybeUpdateStandaloneGaze(*data);
      data->cmdStates[commandBuffer] = CmdState{};
    }
  }
  return result;
  VRSFOV_GUARD_END(VK_ERROR_INITIALIZATION_FAILED)
}

VKAPI_ATTR void VKAPI_CALL vrsfov_vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool,
                                                      uint32_t commandBufferCount,
                                                      const VkCommandBuffer* pCommandBuffers) {
  VRSFOV_GUARD_BEGIN
  DeviceData* data = findDevice(device);
  if (!data || !data->d.FreeCommandBuffers) {
    LOG_ERROR("vkFreeCommandBuffers: no device data");
    return;
  }
  if (data->active) {
    std::unique_lock<std::shared_mutex> lock(data->lock);
    for (uint32_t i = 0; i < commandBufferCount; ++i) data->cmdStates.erase(pCommandBuffers[i]);
  }
  data->d.FreeCommandBuffers(device, commandPool, commandBufferCount, pCommandBuffers);
  VRSFOV_GUARD_END_VOID
}

VKAPI_ATTR void VKAPI_CALL vrsfov_vkCmdBeginRendering(VkCommandBuffer commandBuffer,
                                                     const VkRenderingInfo* pRenderingInfo) {
  DeviceData* data = findDeviceForCommandBuffer(commandBuffer);
  beginRenderingImpl(commandBuffer, pRenderingInfo, data ? data->d.CmdBeginRendering : nullptr);
}

VKAPI_ATTR void VKAPI_CALL vrsfov_vkCmdBeginRenderingKHR(VkCommandBuffer commandBuffer,
                                                        const VkRenderingInfo* pRenderingInfo) {
  DeviceData* data = findDeviceForCommandBuffer(commandBuffer);
  beginRenderingImpl(commandBuffer, pRenderingInfo, data ? data->d.CmdBeginRenderingKHR : nullptr);
}

namespace {

void endRenderingImpl(VkCommandBuffer cmd, PFN_vkCmdEndRendering next) {
  DeviceData* data = findDeviceForCommandBuffer(cmd);
  if (!data || !next) {
    if (next) next(cmd);
    return;
  }
  VRSFOV_GUARD_BEGIN
  if (data->active) paintGazeIfPending(cmd, *data);
  next(cmd);
  VRSFOV_GUARD_END_VOID
}

}  // namespace

VKAPI_ATTR void VKAPI_CALL vrsfov_vkCmdEndRendering(VkCommandBuffer commandBuffer) {
  DeviceData* data = findDeviceForCommandBuffer(commandBuffer);
  endRenderingImpl(commandBuffer, data ? data->d.CmdEndRendering : nullptr);
}

VKAPI_ATTR void VKAPI_CALL vrsfov_vkCmdEndRenderingKHR(VkCommandBuffer commandBuffer) {
  DeviceData* data = findDeviceForCommandBuffer(commandBuffer);
  endRenderingImpl(commandBuffer, data ? data->d.CmdEndRenderingKHR : nullptr);
}

namespace {

// Shared body for vkCmdBeginRenderPass and its v2/KHR variants (05 §3).
void beginRenderPassImpl(VkCommandBuffer cmd, const VkRenderPassBeginInfo* beginInfo,
                         const VkSubpassBeginInfo* subpassInfo, VkSubpassContents contents,
                         PFN_vkCmdBeginRenderPass next1, PFN_vkCmdBeginRenderPass2 next2) {
  DeviceData* data = findDeviceForCommandBuffer(cmd);
  if (!data || !beginInfo || (!next1 && !next2)) {
    LOG_ONCE(DEBUG, "skip-beginRenderPass-nodevice", "render pass passed through: %s",
             (!next1 && !next2) ? "no next entry point"
                                : (!data ? "unknown command buffer/device" : "no begin info"));
    if (next2) next2(cmd, beginInfo, subpassInfo);
    else if (next1) next1(cmd, beginInfo, contents);
    return;
  }
  auto callNext = [&](const VkRenderPassBeginInfo* info) {
    if (next2) next2(cmd, info, subpassInfo);
    else next1(cmd, info, contents);
  };

  VRSFOV_GUARD_BEGIN
  if (!data->active) {
    callNext(beginInfo);
    return;
  }

  FramebufferInfo fb;
  {
    std::shared_lock<std::shared_mutex> lock(data->lock);
    auto it = data->framebuffers.find(beginInfo->framebuffer);
    if (it == data->framebuffers.end() || !it->second.rp.modified || !it->second.image) {
      LOG_ONCE(DEBUG, "skip-beginRenderPass-fb",
               "render pass passed through: framebuffer %s", 
               it == data->framebuffers.end()
                   ? "not created through us"
                   : (!it->second.rp.modified ? "its render pass was not modified"
                                              : "no rate image"));
      callNext(beginInfo);
      return;
    }
    fb = it->second;
  }

  const VkRenderPassAttachmentBeginInfo* attachmentBegin = nullptr;
  std::vector<VkImageView> views;
  if (fb.imageless) {
    attachmentBegin = reinterpret_cast<const VkRenderPassAttachmentBeginInfo*>(
        chainFind(beginInfo->pNext, VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO));
    if (!attachmentBegin || !attachmentBegin->pAttachments ||
        attachmentBegin->attachmentCount < fb.rp.origAttachmentCount) {
      callNext(beginInfo);
      return;
    }
    views.assign(attachmentBegin->pAttachments,
                 attachmentBegin->pAttachments + attachmentBegin->attachmentCount);
  } else {
    views = fb.appViews;
  }

  auto snapshot = SharedState::get().snapshot();
  static const Snapshot kEmptySnapshot;
  const Snapshot& snap = snapshot ? *snapshot : kEmptySnapshot;
  const Config& config = effectiveConfig(snapshot);
  SharedState::get().stats.passes.fetch_add(1, std::memory_order_relaxed);

  if (fb.neutral) {
    // Nothing to foveate: make sure the shared neutral image contains zeros.
    std::unique_lock<std::shared_mutex> lock(data->lock);
    CmdState& state = data->cmdStates[cmd];
    ensureNeutralInitialized(cmd, *data, state, *fb.image);
    SharedState::get().stats.neutralPasses.fetch_add(1, std::memory_order_relaxed);
  } else {
    TargetDesc desc;
    desc.width = fb.width;
    desc.height = fb.height;
    desc.viewMask = fb.rp.viewMaskUnion;
    desc.layers = fb.layers;
    desc.hasColor = fb.rp.hasColor;
    desc.vkDevice = (uint64_t)(uintptr_t)data->device;
    {
      std::shared_lock<std::shared_mutex> lock(data->lock);
      for (VkImageView view : views) {
        auto it = data->imageViews.find(view);
        if (it == data->imageViews.end()) {
          desc.colorAttachments.push_back(TargetAtt{0, 0, 1});
        } else {
          desc.colorAttachments.push_back(TargetAtt{(uint64_t)(uintptr_t)it->second.image,
                                                    it->second.baseArrayLayer,
                                                    it->second.layerCount});
        }
      }
    }
    const FoveationPlan plan = planForTarget(desc, snap, fb.image->layers);
    const bool eyeTracked = snap.eyes[0].eyeTracked && snap.eyes[1].eyeTracked;
    recordRateUpdate(cmd, *data, *fb.image, plan, config, eyeTracked);
    if (plan.regions.empty()) {
      SharedState::get().stats.neutralPasses.fetch_add(1, std::memory_order_relaxed);
    } else {
      SharedState::get().stats.foveatedPasses.fetch_add(1, std::memory_order_relaxed);
      if (config.paintGaze) {
        std::vector<VkClearRect> paintRects = buildPaintRects(
            plan, snap, config, eyeTracked, fb.rp.viewMaskUnion, fb.layers,
            beginInfo->renderArea);
        std::unique_lock<std::shared_mutex> lock(data->lock);
        data->cmdStates[cmd].pendingPaintRects = std::move(paintRects);
      }
      LOG_DEBUG("render pass %ux%u viewMask=0x%x: %zu region(s), attaching rate image "
                "%ux%u texels x%u layer(s) (pattern rates: inner %s, middle %s, outer %s; "
                "debugMode %d)",
                desc.width, desc.height, desc.viewMask, plan.regions.size(), fb.image->width,
                fb.image->height, fb.image->layers, rateToString(data->caps.innerRate).c_str(),
                rateToString(data->caps.middleRate).c_str(),
                rateToString(data->caps.outerRate).c_str(), config.debugMode);
    }
  }

  if (fb.imageless) {
    // Patch-call-restore: append our view to the attachment list.
    auto* mutableBegin = const_cast<VkRenderPassAttachmentBeginInfo*>(attachmentBegin);
    const uint32_t savedCount = mutableBegin->attachmentCount;
    const VkImageView* savedViews = mutableBegin->pAttachments;
    views.push_back(fb.image->view);
    mutableBegin->attachmentCount = (uint32_t)views.size();
    mutableBegin->pAttachments = views.data();
    callNext(beginInfo);
    mutableBegin->attachmentCount = savedCount;
    mutableBegin->pAttachments = savedViews;
  } else {
    callNext(beginInfo);
  }
  VRSFOV_GUARD_END_VOID
}

}  // namespace

VKAPI_ATTR void VKAPI_CALL vrsfov_vkCmdBeginRenderPass(VkCommandBuffer commandBuffer,
                                                      const VkRenderPassBeginInfo* pRenderPassBegin,
                                                      VkSubpassContents contents) {
  DeviceData* data = findDeviceForCommandBuffer(commandBuffer);
  beginRenderPassImpl(commandBuffer, pRenderPassBegin, nullptr, contents,
                      data ? data->d.CmdBeginRenderPass : nullptr, nullptr);
}

VKAPI_ATTR void VKAPI_CALL vrsfov_vkCmdBeginRenderPass2(VkCommandBuffer commandBuffer,
                                                       const VkRenderPassBeginInfo* pRenderPassBegin,
                                                       const VkSubpassBeginInfo* pSubpassBeginInfo) {
  DeviceData* data = findDeviceForCommandBuffer(commandBuffer);
  beginRenderPassImpl(commandBuffer, pRenderPassBegin, pSubpassBeginInfo, VK_SUBPASS_CONTENTS_INLINE,
                      nullptr, data ? data->d.CmdBeginRenderPass2 : nullptr);
}

VKAPI_ATTR void VKAPI_CALL vrsfov_vkCmdBeginRenderPass2KHR(
    VkCommandBuffer commandBuffer, const VkRenderPassBeginInfo* pRenderPassBegin,
    const VkSubpassBeginInfo* pSubpassBeginInfo) {
  DeviceData* data = findDeviceForCommandBuffer(commandBuffer);
  beginRenderPassImpl(commandBuffer, pRenderPassBegin, pSubpassBeginInfo, VK_SUBPASS_CONTENTS_INLINE,
                      nullptr, data ? data->d.CmdBeginRenderPass2KHR : nullptr);
}

namespace {

void endRenderPassImpl(VkCommandBuffer cmd, const VkSubpassEndInfo* endInfo,
                       PFN_vkCmdEndRenderPass next1, PFN_vkCmdEndRenderPass2 next2) {
  DeviceData* data = findDeviceForCommandBuffer(cmd);
  if (!data || (!next1 && !next2)) {
    if (next2) next2(cmd, endInfo);
    else if (next1) next1(cmd);
    return;
  }
  VRSFOV_GUARD_BEGIN
  if (data->active) paintGazeIfPending(cmd, *data);
  if (next2) next2(cmd, endInfo);
  else next1(cmd);
  VRSFOV_GUARD_END_VOID
}

}  // namespace

VKAPI_ATTR void VKAPI_CALL vrsfov_vkCmdEndRenderPass(VkCommandBuffer commandBuffer) {
  DeviceData* data = findDeviceForCommandBuffer(commandBuffer);
  endRenderPassImpl(commandBuffer, nullptr, data ? data->d.CmdEndRenderPass : nullptr, nullptr);
}

VKAPI_ATTR void VKAPI_CALL vrsfov_vkCmdEndRenderPass2(VkCommandBuffer commandBuffer,
                                                     const VkSubpassEndInfo* pSubpassEndInfo) {
  DeviceData* data = findDeviceForCommandBuffer(commandBuffer);
  endRenderPassImpl(commandBuffer, pSubpassEndInfo, nullptr,
                    data ? data->d.CmdEndRenderPass2 : nullptr);
}

VKAPI_ATTR void VKAPI_CALL vrsfov_vkCmdEndRenderPass2KHR(VkCommandBuffer commandBuffer,
                                                        const VkSubpassEndInfo* pSubpassEndInfo) {
  DeviceData* data = findDeviceForCommandBuffer(commandBuffer);
  endRenderPassImpl(commandBuffer, pSubpassEndInfo, nullptr,
                    data ? data->d.CmdEndRenderPass2KHR : nullptr);
}

}  // namespace vrsfov
