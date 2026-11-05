# 06 — Shading-rate pattern and eye-target classification

All code here is pure C++ (no Vulkan calls) so it can be unit tested.

## 1. Pattern buffer generation (pattern.cpp)

```cpp
uint8_t encodeRate(Rate r);   // (log2(w) << 2) | log2(h)
std::vector<uint8_t> buildPattern(uint32_t ew, uint32_t eh,   // eye rect size in texels
                                  const PatternParams& p);    // radii, rates, hscale, debugMode
```
Buffer size `(2*ew) x (2*eh)`, row-major, row pitch `2*ew`. Texel `(x, y)`:
```
dx = (x + 0.5) - ew        // texel-center distance from fovea center, in texels
dy = (y + 0.5) - eh
nx = 2 * dx / ew           // normalized: the eye image spans [-1, 1]
ny = 2 * dy / eh
d  = sqrt((nx / hscale)^2 + ny^2)
rate = d < inner ? innerRate : d < outer ? middleRate : outerRate
debugMode 1: rate = outerRate everywhere
debugMode 2: inverted: d < inner ? outerRate : innerRate   (fovea blurry -> verifies tracking)
```
`inner/outer` = eye-tracked or fixed radii depending on `PatternParams::eyeTracked`
(so the cache key also includes that bool). Rates are the device-clamped ones (04 §5).

Unit tests: center texel = innerRate code; corner = outerRate code; symmetry; debug modes.

## 2. Gaze → copy region (pattern.cpp)

For a region with rect in pixels `(px, py, pw, ph)`, gaze `(u, v)`, layer `l`, texel `t`, and
S image size `(sw, sh)`:
```
rx = px / t                       (floor)
ry = py / t
rw = min(ceil((px + pw) / t), sw) - rx
rh = min(ceil((py + ph) / t), sh) - ry
ew = rw, eh = rh                  // pattern key
gx = round(u * rw), gy = round(v * rh)
sx = clamp(ew - gx, 0, ew)        // source window start inside P (window size rw x rh)
sy = clamp(eh - gy, 0, eh)
VkBufferImageCopy {
  bufferOffset      = sy * (2*ew) + sx,
  bufferRowLength   = 2*ew,
  bufferImageHeight = 2*eh,
  imageSubresource  = {COLOR, mip 0, baseArrayLayer l, layerCount 1},
  imageOffset       = {rx, ry, 0},
  imageExtent       = {rw, rh, 1} }
```
Check: destination texel j gets `P[sx + j]`, whose distance from P's center is `j - gx`. ✓
Unit test the offset math, including u=0, u=1 and side-by-side halves where `px` is not a
multiple of `t` (adjacent regions may overlap by one texel column; later copy wins — fine).

## 3. Eye-target classification (classify.cpp)

Input:
```cpp
struct TargetDesc {
  uint32_t width, height;            // framebuffer size, or renderArea offset+extent
  uint32_t viewMask;                 // 0 if no multiview
  uint32_t layers;                   // framebuffer layers / rendering layerCount
  bool hasColor;
  struct Att { uint64_t vkImage; uint32_t baseLayer, layerCount; };
  std::vector<Att> colorAttachments; // resolved via image-view map
  uint64_t vkDevice;
};
struct Region { uint32_t layer; int32_t x, y, w, h; float u, v; };
struct FoveationPlan { std::vector<Region> regions; const char* reason; };
FoveationPlan planForTarget(const TargetDesc&, const Snapshot&, uint32_t rateImageLayers);
bool isEyeCandidateSize(uint32_t w, uint32_t h, const Snapshot&);
```

Preconditions → empty plan (neutral) if any fails:
- `config.enabled`, `snapshot.sessionVkDevice == vkDevice`, `sessionRunning`.
- `hasColor` (depth-only passes have no colour image to sharpen).
- Eye data valid and fresh; if stale or invalid, use `u = v = 0.5` with the fixed
  foveation radii.

Then, in order (first match wins):

**A. XR swapchain image.** If any color attachment's VkImage is in `swapchainByVkImage`:
for each `layer l` of the rate image (or 0 if 1 layer), compute the swapchain array index
`ai = att.baseLayer + (multiview ? l : 0)`; for every `ProjectionSlot` with the same swapchain
and `arrayIndex == ai`, add `Region{l, slot.rect, eyes[slot.eye].u/v}`. If multiview and rate
image has 1 layer: use one region per slot with the averaged gaze of both eyes on layer 0.
If no slot matched (no xrEndFrame yet) fall through to B.

**B. Multiview stereo.** `popcount(viewMask) >= 2` and size is an eye candidate: for
`l in {0,1}`: `Region{l, full rect, eyes[l]}` (if rate image has 1 layer: single region with
averaged gaze).

**C. Side-by-side.** `isEyeCandidateSize(w/2, h)` and not `isEyeCandidateSize(w,h)`: left
half → eye 0, right half → eye 1. Checked before the single-eye case, because a double-wide
target also looks eye-sized as a whole and one centre would land between the eyes.

**D. Single eye-sized target.** `isEyeCandidateSize(w,h)`: one region, full rect, gaze =
average of the two eyes' (u, v). (We cannot know which eye; the per-eye gaze points are
close.)

**E.** Otherwise empty plan (reason "not an eye target").

`isEyeCandidateSize(w, h, snap)`: reference sizes = recommended eye size plus every color
swapchain's per-eye size (`width`, and `width/2` if the swapchain is used side-by-side per slot
rects). Candidate if for any reference `(rw, rh)`:
`|(w/h) / (rw/rh) - 1| <= sizeTolerance` and `w >= minSizeRatio * rw` and `w <= 4 * rw`.
If no reference sizes are known yet → true.

Log each distinct `(w, h, decision, reason)` once at INFO via `LOG_ONCE`.

Unit tests: table-driven — multiview 1832x1920 viewMask 0b11 → 2 regions; 3664x1920 → SBS;
2048x2048 shadow map with Quest-like recommended 1832x1920 (aspect 1.0 vs 0.954) → no;
depth-only → no; swapchain image with slot rects → correct eye & rect; stale gaze modes.
