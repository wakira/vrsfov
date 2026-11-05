#include "common/classify.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "common/log.h"

namespace vrsfov {

namespace {

constexpr int64_t kStaleNs = 200 * 1000 * 1000;  // 200 ms

const Config& configOf(const Snapshot& snap) {
  static const Config kDefault{};
  return snap.config ? *snap.config : kDefault;
}

int popcount32(uint32_t v) {
  int n = 0;
  while (v) {
    n += (int)(v & 1u);
    v >>= 1;
  }
  return n;
}

struct EyeGaze {
  float u = 0.5f, v = 0.5f;
  bool tracked = false;
};

// Resolves the two eyes to usable gaze points. A stale or invalid sample falls
// back to the centre with the fixed-foveation radii.
void resolveGaze(const Snapshot& snap, int64_t nowNs, EyeGaze eyes[2]) {
  const bool fresh = snap.synthetic ||
                     (snap.publishTimeNs != 0 && (nowNs - snap.publishTimeNs) <= kStaleNs);
  for (int i = 0; i < 2; ++i) {
    const EyeView& e = snap.eyes[i];
    if (e.valid && fresh) {
      eyes[i].u = e.u;
      eyes[i].v = e.v;
      eyes[i].tracked = e.eyeTracked;
    } else {
      eyes[i].u = 0.5f;
      eyes[i].v = 0.5f;
      eyes[i].tracked = false;
    }
  }
}

EyeGaze averageGaze(const EyeGaze eyes[2]) {
  EyeGaze g;
  g.u = 0.5f * (eyes[0].u + eyes[1].u);
  g.v = 0.5f * (eyes[0].v + eyes[1].v);
  g.tracked = eyes[0].tracked && eyes[1].tracked;
  return g;
}

bool sizeMatches(uint32_t w, uint32_t h, uint32_t rw, uint32_t rh, const Config& cfg) {
  if (!w || !h || !rw || !rh) return false;
  const double aspect = (double)w / (double)h;
  const double refAspect = (double)rw / (double)rh;
  if (std::fabs(aspect / refAspect - 1.0) > (double)cfg.sizeTolerance) return false;
  return (double)w >= (double)cfg.minSizeRatio * (double)rw && (double)w <= 4.0 * (double)rw;
}

std::string sizeVerdict(uint32_t w, uint32_t h, const Snapshot& snap) {
  const Config& cfg = configOf(snap);
  const double aspect = h ? (double)w / (double)h : 0.0;
  std::string best;
  std::vector<std::pair<uint32_t, uint32_t>> refs;
  if (snap.recommendedW && snap.recommendedH) refs.emplace_back(snap.recommendedW, snap.recommendedH);
  if (snap.swapchainByVkImage) {
    for (const auto& entry : *snap.swapchainByVkImage) {
      if (entry.second.width && entry.second.height) {
        refs.emplace_back(entry.second.width, entry.second.height);
      }
    }
  }
  if (refs.empty()) return "no reference size known";

  char buf[220];
  for (const auto& ref : refs) {
    const double refAspect = (double)ref.first / (double)ref.second;
    const double deviation = std::fabs(aspect / refAspect - 1.0);
    if (deviation <= (double)cfg.sizeTolerance) {
      if ((double)w < (double)cfg.minSizeRatio * (double)ref.first) {
        snprintf(buf, sizeof(buf), "aspect %.3f matches %ux%u but width %u < %.2f x %u",
                 aspect, ref.first, ref.second, w, (double)cfg.minSizeRatio, ref.first);
      } else if ((double)w > 4.0 * (double)ref.first) {
        snprintf(buf, sizeof(buf), "aspect %.3f matches %ux%u but width %u > 4 x %u", aspect,
                 ref.first, ref.second, w, ref.first);
      } else {
        snprintf(buf, sizeof(buf), "matches %ux%u", ref.first, ref.second);
      }
    } else {
      snprintf(buf, sizeof(buf), "aspect %.3f vs %.3f (reference %ux%u), tolerance %.3f", aspect,
               refAspect, ref.first, ref.second, (double)cfg.sizeTolerance);
    }
    if (best.empty()) best = buf;
  }
  return best;
}

void logOnceDecision(const TargetDesc& t, const FoveationPlan& plan) {
  const std::string key = std::string("classify:") + std::to_string(t.width) + "x" +
                          std::to_string(t.height) + ":" + (t.source ? t.source : "") + ":" +
                          (plan.regions.empty() ? "neutral" : "foveated") + ":" + plan.reason;
  LOG_ONCE(INFO, key.c_str(),
           "render target %ux%u (%s, viewMask=0x%x layers=%u color=%d): %s (%s)", t.width, t.height,
           t.source ? t.source : "framebuffer", t.viewMask, t.layers, (int)t.hasColor,
           plan.regions.empty() ? "neutral" : "foveated", plan.reason.c_str());
}

}  // namespace

bool isEyeCandidateSize(uint32_t w, uint32_t h, const Snapshot& snap) {
  if (!w || !h) return false;
  const Config& cfg = configOf(snap);

  std::vector<std::pair<uint32_t, uint32_t>> refs;
  if (snap.recommendedW && snap.recommendedH) {
    refs.emplace_back(snap.recommendedW, snap.recommendedH);
  }
  if (snap.swapchainByVkImage) {
    for (const auto& entry : *snap.swapchainByVkImage) {
      const SwapchainInfo& sc = entry.second;
      if (sc.width && sc.height) refs.emplace_back(sc.width, sc.height);
      // Side-by-side usage: a slot rect that is at most half of the image width.
      if (snap.slots && sc.width >= 2) {
        for (const ProjectionSlot& slot : *snap.slots) {
          if (slot.swapchain == sc.swapchain && slot.w > 0 &&
              (uint32_t)slot.w * 2u <= sc.width) {
            refs.emplace_back(sc.width / 2, sc.height);
            break;
          }
        }
      }
    }
  }
  if (refs.empty()) return true;  // nothing known yet: do not rule anything out
  for (const auto& ref : refs) {
    if (sizeMatches(w, h, ref.first, ref.second, cfg)) return true;
  }
  return false;
}

FoveationPlan planForTarget(const TargetDesc& target, const Snapshot& snapshot,
                            uint32_t rateImageLayers) {
  FoveationPlan plan;
  const Config& cfg = configOf(snapshot);
  plan.reason = "not an eye target";

  if (!cfg.enabled) {
    plan.reason = "disabled";
    return plan;
  }
  if (!snapshot.anyDevice && snapshot.sessionVkDevice != target.vkDevice) {
    plan.reason = "other VkDevice";
    return plan;
  }
  if (!snapshot.sessionRunning) {
    plan.reason = "session not running";
    return plan;
  }
  if (!target.hasColor) {
    plan.reason = "depth-only";
    return plan;
  }

  EyeGaze eyes[2];
  resolveGaze(snapshot, nowNs(), eyes);

  // A. render target is an XR swapchain image.
  const uint32_t rateLayers = std::max(1u, rateImageLayers);
  if (snapshot.swapchainByVkImage && snapshot.slots) {
    for (const TargetAtt& att : target.colorAttachments) {
      auto it = snapshot.swapchainByVkImage->find(att.vkImage);
      if (it == snapshot.swapchainByVkImage->end()) continue;
      const uint32_t layers = (target.viewMask != 0) ? rateLayers : 1u;
      for (uint32_t l = 0; l < layers; ++l) {
        const uint32_t arrayIndex = att.baseLayer + (target.viewMask != 0 ? l : 0);
        for (const ProjectionSlot& slot : *snapshot.slots) {
          if (slot.swapchain != it->second.swapchain) continue;
          if (slot.arrayIndex != arrayIndex) continue;
          Region r;
          r.layer = l;
          r.x = slot.x;
          r.y = slot.y;
          r.w = (uint32_t)std::max(0, slot.w);
          r.h = (uint32_t)std::max(0, slot.h);
          if (target.viewMask != 0 && rateLayers == 1) {
            const EyeGaze avg = averageGaze(eyes);
            r.u = avg.u;
            r.v = avg.v;
          } else {
            const uint32_t eye = std::min<uint32_t>(slot.eye, 1u);
            r.u = eyes[eye].u;
            r.v = eyes[eye].v;
          }
          if (r.w && r.h) plan.regions.push_back(r);
        }
      }
    }
    if (!plan.regions.empty()) {
      plan.reason = "xr swapchain image";
      logOnceDecision(target, plan);
      return plan;
    }
  }

  // B. multiview stereo.
  if (popcount32(target.viewMask) >= 2 && isEyeCandidateSize(target.width, target.height, snapshot)) {
    if (rateLayers >= 2) {
      for (uint32_t l = 0; l < 2; ++l) {
        Region r;
        r.layer = l;
        r.x = 0;
        r.y = 0;
        r.w = target.width;
        r.h = target.height;
        r.u = eyes[l].u;
        r.v = eyes[l].v;
        plan.regions.push_back(r);
      }
    } else {
      const EyeGaze avg = averageGaze(eyes);
      Region r;
      r.layer = 0;
      r.x = 0;
      r.y = 0;
      r.w = target.width;
      r.h = target.height;
      r.u = avg.u;
      r.v = avg.v;
      plan.regions.push_back(r);
    }
    plan.reason = "multiview stereo";
    logOnceDecision(target, plan);
    return plan;
  }

  // C. side-by-side: both eyes in one image. Checked BEFORE the single-eye case:
  // a double-wide stereo target also "looks like" an eye target, and giving it
  // one centre puts the sharp area between the eyes (everything peripheral, no
  // clear area at all). With a known eye size the halves must match it and the
  // whole must not; without one, a ~2:1 aspect is the signature of a stereo pass.
  if (target.width >= 2) {
    const uint32_t halfW = target.width / 2;
    const bool halvesMatch = isEyeCandidateSize(halfW, target.height, snapshot);
    const bool wholeMatches = isEyeCandidateSize(target.width, target.height, snapshot);
    const double aspect = target.height ? (double)target.width / (double)target.height : 0.0;
    const bool looksTwoUp = (snapshot.recommendedW && snapshot.recommendedH)
                                ? (halvesMatch && !wholeMatches)
                                : (aspect >= 1.9 && aspect <= 2.1 && halfW >= 512);
    if (looksTwoUp) {
      const uint32_t leftW = target.width / 2;
      const uint32_t rightW = target.width - leftW;
      Region left;
      left.layer = 0;
      left.x = 0;
      left.y = 0;
      left.w = leftW;
      left.h = target.height;
      left.u = eyes[0].u;
      left.v = eyes[0].v;
      Region right;
      right.layer = 0;
      right.x = (int32_t)leftW;
      right.y = 0;
      right.w = rightW;
      right.h = target.height;
      right.u = eyes[1].u;
      right.v = eyes[1].v;
      plan.regions.push_back(left);
      plan.regions.push_back(right);
      plan.reason = "side-by-side";
      logOnceDecision(target, plan);
      return plan;
    }
  }

  // D. single eye-sized target.
  if (isEyeCandidateSize(target.width, target.height, snapshot)) {
    const EyeGaze avg = averageGaze(eyes);
    Region r;
    r.layer = 0;
    r.x = 0;
    r.y = 0;
    r.w = target.width;
    r.h = target.height;
    r.u = avg.u;
    r.v = avg.v;
    plan.regions.push_back(r);
    plan.reason = "eye-sized target";
    logOnceDecision(target, plan);
    return plan;
  }

  if (plan.regions.empty()) {
    plan.reason = "not an eye target (" + sizeVerdict(target.width, target.height, snapshot) + ")";
  }
  logOnceDecision(target, plan);
  return plan;
}

}  // namespace vrsfov
