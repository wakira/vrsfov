// vrsfov-overlay - draw the foveal radii as a SteamVR overlay quad.
//
// Why an overlay and not an in-pass draw: a composited quad works over any SteamVR
// application, OpenVR or OpenXR alike, and cannot disturb its rendering, its rate
// image or its Vulkan state. That is what fpsVR, OVR Advanced Settings and
// XSOverlay do, and it is the only one of the two patterns that is safe to run
// while someone is playing.
//
// Radius r (the layer's normalised eye-image units) maps to r * horizontalScale / 2
// of the quad's width and r / 2 of its height, because the quad spans exactly the
// eye's field of view and both the eye buffer and the quad are linear in the
// tangent of the view angle - so the ring lands on the real fovea boundary.
//
// Usage:
//   vrsfov-overlay [--inner R] [--outer R] [--width W] [--distance M]
//                 [--color-inner RRGGBB] [--color-outer RRGGBB] [--alpha A]
//                 [--eye left|right|average] [--gaze on|off] [--hscale S]
//                 [--flip-v] [--bgra] [--config PATH] [--once]
//
// Defaults for the radii and horizontal scale come from the layer's settings.ini,
// so the marker matches what the layer actually draws. Ctrl+C exits.
//
// If the ring appears mirrored vertically add --flip-v; if its colours look wrong
// add --bgra (the two conventions some runtimes use).
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <string>
#include <thread>
#include <chrono>

#include <openvr/openvr.h>

#include "common/config.h"
#include "common/overlay_texture.h"

namespace {

uint32_t parseHexColor(const char* text, uint32_t fallback) {
  if (!text) return fallback;
  std::string s(text);
  if (!s.empty() && s[0] == '#') s.erase(0, 1);
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s.erase(0, 2);
  if (s.size() != 6) return fallback;
  char* end = nullptr;
  const unsigned long v = strtoul(s.c_str(), &end, 16);
  if (end != s.c_str() + s.size()) return fallback;
  return (uint32_t)(v & 0xffffffu);
}

struct Options {
  float inner = -1.f;          // <0 = take from the config
  float outer = -1.f;
  float width = 0.01f;         // ring thickness, normalised radius units
  float hscale = -1.f;         // <0 = take from the config
  float distance = 1.0f;       // metres in front of the eyes
  float alpha = 0.85f;
  uint32_t colorInner = 0x00ff00;
  uint32_t colorOuter = 0xff8000;
  int eye = -1;                // -1 average, 0 left, 1 right
  bool gaze = true;
  bool flipV = false;
  bool bgra = false;
  bool once = false;
  int test = 0;                // 0 ring, 1 checker, 2 solid
  std::string configPath;
};

void printUsage() {
  printf(
      "vrsfov-overlay - draw the foveal radii as a SteamVR overlay quad\n"
      "\n"
      "  --inner R            inner radius, normalised eye-image units (default: config)\n"
      "  --outer R            outer radius (default: config)\n"
      "  --width W            ring thickness, same units (default 0.01)\n"
      "  --hscale S           horizontal scale (default: config)\n"
      "  --distance M         quad distance in metres (default 1.0)\n"
      "  --alpha A            overlay opacity 0..1 (default 0.85)\n"
      "  --color-inner RRGGBB (default 00ff00)\n"
      "  --color-outer RRGGBB (default ff8000)\n"
      "  --eye left|right|average  which eye's FOV the quad is sized for (default average)\n"
      "  --gaze on|off        move the ring to the eye-tracked fovea (default on)\n"
      "  --flip-v  --bgra     try these if the ring looks mirrored or its colour wrong\n"
      "  --config PATH        settings.ini to read the radii from (default: the layer's)\n"
      "  --once               create the overlays, update once, print status, exit\n"
      "\n"
      "SteamVR must be running. Exits on Ctrl+C.\n");
}

bool argMatch(const char* a, const char* name) { return strcmp(a, name) == 0; }

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    auto next = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : nullptr; };
    if (argMatch(a, "--help") || argMatch(a, "-h")) {
      printUsage();
      return 0;
    } else if (argMatch(a, "--inner")) {
      const char* v = next();
      if (v) opt.inner = (float)atof(v);
    } else if (argMatch(a, "--outer")) {
      const char* v = next();
      if (v) opt.outer = (float)atof(v);
    } else if (argMatch(a, "--width")) {
      const char* v = next();
      if (v) opt.width = (float)atof(v);
    } else if (argMatch(a, "--hscale")) {
      const char* v = next();
      if (v) opt.hscale = (float)atof(v);
    } else if (argMatch(a, "--distance")) {
      const char* v = next();
      if (v) opt.distance = (float)atof(v);
    } else if (argMatch(a, "--alpha")) {
      const char* v = next();
      if (v) opt.alpha = (float)atof(v);
    } else if (argMatch(a, "--color-inner")) {
      opt.colorInner = parseHexColor(next(), opt.colorInner);
    } else if (argMatch(a, "--color-outer")) {
      opt.colorOuter = parseHexColor(next(), opt.colorOuter);
    } else if (argMatch(a, "--eye")) {
      const char* v = next();
      if (v && strcmp(v, "left") == 0) opt.eye = 0;
      else if (v && strcmp(v, "right") == 0) opt.eye = 1;
      else opt.eye = -1;
    } else if (argMatch(a, "--gaze")) {
      const char* v = next();
      opt.gaze = !(v && (strcmp(v, "off") == 0 || strcmp(v, "0") == 0));
    } else if (argMatch(a, "--flip-v")) {
      opt.flipV = true;
    } else if (argMatch(a, "--bgra")) {
      opt.bgra = true;
    } else if (argMatch(a, "--once")) {
      opt.once = true;
    } else if (argMatch(a, "--test")) {
      const char* v = next();
      if (v && strcmp(v, "checker") == 0) opt.test = 1;
      else if (v && strcmp(v, "solid") == 0) opt.test = 2;
      else opt.test = 0;
    } else if (argMatch(a, "--config")) {
      const char* v = next();
      if (v) opt.configPath = v;
    } else {
      fprintf(stderr, "unknown argument '%s' (try --help)\n", a);
      return 2;
    }
  }

  // Radii and horizontal scale default to the layer's own settings.
  const std::string configPath =
      opt.configPath.empty() ? vrsfov::defaultConfigPath() : opt.configPath;
  const vrsfov::Config cfg = vrsfov::loadConfigForProcess("vrsfov-overlay", 0, configPath);
  const float inner = opt.inner >= 0.f ? opt.inner : cfg.innerRadius;
  const float outer = opt.outer >= 0.f ? opt.outer : cfg.outerRadius;
  const float hscale = opt.hscale > 0.f ? opt.hscale : cfg.horizontalScale;
  printf("config %s: radii %.3f/%.3f, horizontalScale %.2f\n", configPath.c_str(),
         (double)inner, (double)outer, (double)hscale);
  if (inner <= 0.f && outer <= 0.f) {
    fprintf(stderr, "nothing to draw: give --inner/--outer or put them in the config\n");
    return 2;
  }

  vr::EVRInitError err = vr::VRInitError_None;
  vr::IVRSystem* system = vr::VR_Init(&err, vr::VRApplication_Other);
  if (!system) {
    fprintf(stderr, "VR_Init failed: %s (%d)\n",
            vr::VR_GetVRInitErrorAsEnglishDescription(err), (int)err);
    return 1;
  }
  vr::IVROverlay* overlay = static_cast<vr::IVROverlay*>(
      vr::VR_GetGenericInterface(vr::IVROverlay_Version, &err));
  if (!overlay) {
    fprintf(stderr, "no IVROverlay interface: %s (%d)\n",
            vr::VR_GetVRInitErrorAsEnglishDescription(err), (int)err);
    vr::VR_Shutdown();
    return 1;
  }
  char hmd[256] = {0};
  vr::ETrackedPropertyError propErr = vr::TrackedProp_Success;
  system->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                                         vr::Prop_ModelNumber_String, hmd, sizeof(hmd), &propErr);
  printf("SteamVR %s, HMD '%s'\n", system->GetRuntimeVersion(), hmd[0] ? hmd : "?");

  // Quad geometry from the chosen eye's projection: the quad spans exactly that
  // eye's field of view at `distance`, so a normalised radius maps to a screen
  // fraction of the eye image.
  auto projection = [&](float& l, float& r, float& t, float& b) {
    if (opt.eye >= 0) {
      system->GetProjectionRaw(opt.eye == 0 ? vr::Eye_Left : vr::Eye_Right, &l, &r, &t, &b);
      return;
    }
    float l0, r0, t0, b0, l1, r1, t1, b1;
    system->GetProjectionRaw(vr::Eye_Left, &l0, &r0, &t0, &b0);
    system->GetProjectionRaw(vr::Eye_Right, &l1, &r1, &t1, &b1);
    l = 0.5f * (l0 + l1);
    r = 0.5f * (r0 + r1);
    t = 0.5f * (t0 + t1);
    b = 0.5f * (b0 + b1);
  };
  float pl = -1.f, pr = 1.f, pt = 1.f, pb = -1.f;
  projection(pl, pr, pt, pb);
  // Some runtimes (this one) report the vertical tangents sign-flipped, so take the
  // extents as absolute spans and remember which raw value is the top.
  const float xLeft = std::min(pl, pr), xRight = std::max(pl, pr);
  const float yTop = std::max(pt, pb), yBottom = std::min(pt, pb);
  const float quadW = opt.distance * (xRight - xLeft);
  const float quadH = opt.distance * (yTop - yBottom);
  printf("eye %s: projection l %.3f r %.3f t %.3f b %.3f -> quad %.3f x %.3f m at %.2f m\n",
         opt.eye < 0 ? "average" : (opt.eye == 0 ? "left" : "right"), (double)pl, (double)pr,
         (double)pt, (double)pb, (double)quadW, (double)quadH, (double)opt.distance);

  struct Ring {
    float radius;
    uint32_t rgb;
    vr::VROverlayHandle_t handle = vr::k_ulOverlayHandleInvalid;
  };
  Ring rings[2];
  size_t ringCount = 0;
  if (inner > 0.f) rings[ringCount++] = {inner, opt.colorInner, vr::k_ulOverlayHandleInvalid};
  if (outer > 0.f) rings[ringCount++] = {outer, opt.colorOuter, vr::k_ulOverlayHandleInvalid};

  for (size_t i = 0; i < ringCount; ++i) {
    char key[64];
    snprintf(key, sizeof(key), "vrsfov-overlay.%zu", i);
    if (overlay->CreateOverlay(key, key, &rings[i].handle) != vr::VROverlayError_None) {
      fprintf(stderr, "CreateOverlay failed for %s\n", key);
      vr::VR_Shutdown();
      return 1;
    }
    vrsfov::RingTextureParams p;
    // The texture's aspect must match the quad's, or the ring is stretched: the
    // quad spans the eye's field of view, whose aspect is quadW x quadH.
    p.pattern = opt.test == 1   ? vrsfov::TexturePattern::Checker
                : opt.test == 2 ? vrsfov::TexturePattern::Solid
                                : vrsfov::TexturePattern::Ring;
    p.width = 2048;
    p.height =
        (uint32_t)std::max(64.0f, std::min(4096.0f, roundf((float)p.width * quadH / quadW)));
    // radius r -> r * hscale / 2 of the width and r / 2 of the height, in texture
    // fractions where 1.0 is half the texture.
    p.radiusX = rings[i].radius * hscale * 0.5f;
    p.radiusY = rings[i].radius * 0.5f;
    p.thickness = opt.width * 0.5f;
    vrsfov::unpackColor(rings[i].rgb, opt.alpha, p.r, p.g, p.b, p.a);
    std::vector<uint8_t> tex = vrsfov::buildRingTexture(p);
    if (opt.bgra) {
      for (size_t t = 0; t + 3 < tex.size(); t += 4) std::swap(tex[t], tex[t + 2]);
    }
    if (opt.flipV) {
      const size_t stride = (size_t)p.width * 4;
      for (uint32_t y = 0; y < p.height / 2; ++y) {
        std::swap_ranges(tex.begin() + (size_t)y * stride, tex.begin() + (size_t)(y + 1) * stride,
                         tex.begin() + (size_t)(p.height - 1 - y) * stride);
      }
    }
    if (overlay->SetOverlayRaw(rings[i].handle, tex.data(), p.width, p.height, 4) !=
        vr::VROverlayError_None) {
      fprintf(stderr, "SetOverlayRaw failed (%ux%u RGBA)\n", p.width, p.height);
      vr::VR_Shutdown();
      return 1;
    }
    overlay->SetOverlayWidthInMeters(rings[i].handle, quadW);
    // Be explicit about the mapping: the whole texture, and square texels, so the
    // quad cannot be stretched or sampled outside [0,1] by a default.
    vr::VRTextureBounds_t bounds{};
    bounds.uMin = 0.f;
    bounds.vMin = 0.f;
    bounds.uMax = 1.f;
    bounds.vMax = 1.f;
    overlay->SetOverlayTextureBounds(rings[i].handle, &bounds);
    overlay->SetOverlayTexelAspect(rings[i].handle, 1.f);
    overlay->SetOverlayInputMethod(rings[i].handle, vr::VROverlayInputMethod_None);
    overlay->SetOverlayFlag(rings[i].handle, vr::VROverlayFlags_NoDashboardTab, true);
    printf("ring %zu: radius %.3f, colour %06x, thickness %.3f\n", i, (double)rings[i].radius,
           rings[i].rgb, (double)opt.width);
  }
  (void)quadH;

  // Place the rings: quad centred on the eye image's origin, then offset by the
  // gaze when we have one.
  auto update = [&](bool verbose) {
    float l, r, t, b;
    projection(l, r, t, b);
    float u = 0.5f, v = 0.5f;
    bool haveGaze = false;
    if (opt.gaze) {
      vr::HmdVector2_t nleft{}, nright{};
      if (system->GetEyeTrackedFoveationCenter(&nleft, &nright)) {
        const float nx = 0.5f * ((nleft.v[0] + nright.v[0]) * 0.5f + 1.0f);
        const float ny = 0.5f * ((nleft.v[1] + nright.v[1]) * 0.5f + 1.0f);
        u = nx;
        v = opt.flipV ? ny : 1.0f - ny;  // our v is measured downwards
        haveGaze = true;
      }
    }
    // u across the horizontal extent, v downwards from the top, at the quad's
    // distance. (l, r, t, b here are the raw values; the extents above are the
    // sign-normalised ones.)
    (void)l;
    (void)r;
    (void)t;
    (void)b;
    const float x = (xLeft + u * (xRight - xLeft)) * opt.distance;
    const float y = (yTop + v * (yBottom - yTop)) * opt.distance;
    for (size_t i = 0; i < ringCount; ++i) {
      vr::HmdMatrix34_t m{};
      m.m[0][0] = m.m[1][1] = m.m[2][2] = 1.f;
      m.m[0][3] = x;
      m.m[1][3] = y;
      m.m[2][3] = -opt.distance;
      overlay->SetOverlayTransformTrackedDeviceRelative(rings[i].handle,
                                                        vr::k_unTrackedDeviceIndex_Hmd, &m);
    }
    // Report on change, so a gaze that only becomes available once the headset is
    // being worn still shows up (this used to print once and stay quiet).
    static int lastState = -1;
    const int state = haveGaze ? 1 : 0;
    if (verbose || state != lastState) {
      lastState = state;
      printf("%s: gaze (%.3f, %.3f) -> quad offset (%.3f, %.3f) m\n",
             haveGaze ? "eye-tracked" : "fixed (no eye-tracked centre)", (double)u, (double)v,
             (double)x, (double)y);
      fflush(stdout);
    }
  };
  update(true);

  if (opt.once) {
    vr::VR_Shutdown();
    return 0;
  }

  printf("drawing; Ctrl+C to stop\n");
  for (;;) {
    update(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(8));
  }
  vr::VR_Shutdown();
  return 0;
}
