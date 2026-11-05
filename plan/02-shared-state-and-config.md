# 02 — Shared state, config, logging

## 1. SharedState (src/common/shared_state.h)

A process-wide singleton (function-local static) used by both halves.

```cpp
struct EyeView {                 // per eye, per frame
  bool  valid = false;
  float u = 0.5f, v = 0.5f;      // gaze point in [0,1] of the eye image (v down, Vulkan)
  bool  eyeTracked = false;      // false => fixed foveation radii are used
};

struct ProjectionSlot {          // from last xrEndFrame
  uint64_t swapchain;            // XrSwapchain as integer
  uint32_t arrayIndex;           // XrSwapchainSubImage.imageArrayIndex
  int32_t  x, y, w, h;           // imageRect in pixels
  uint32_t eye;                  // 0 = left, 1 = right
};

struct SwapchainInfo {
  uint64_t swapchain;
  uint32_t width, height, arraySize;
  int64_t  format;
};

struct Snapshot {                                         // immutable once published
  uint64_t frameIndex = 0;
  uint64_t sessionVkDevice = 0;                           // VkDevice as integer, 0 = none
  bool     sessionRunning = false;
  uint32_t recommendedW = 0, recommendedH = 0;            // per-eye recommended size
  EyeView  eyes[2];
  std::vector<ProjectionSlot> slots;                      // last frame's projection views
  std::unordered_map<uint64_t, SwapchainInfo> swapchainByVkImage;  // VkImage -> swapchain
  std::shared_ptr<const Config> config;                   // see §2
  int64_t  publishTimeNs = 0;                             // CLOCK_MONOTONIC
};

class SharedState {
 public:
  static SharedState& get();
  std::shared_ptr<const Snapshot> snapshot() const;       // std::atomic_load
  void publish(std::shared_ptr<const Snapshot> s);        // std::atomic_store
  // mutation helper used only by the OpenXR half:
  template <class F> void update(F&& mutateCopy);         // copy current, mutate, publish (under writer mutex)
  std::atomic<int> liveXrInstances{0};
};
```

Rules:
- Only the OpenXR half writes. Writers serialize with a `std::mutex writerMutex`.
- The Vulkan half calls `snapshot()` once per hooked call and uses that pointer throughout.
- `swapchainByVkImage` changes rarely; to keep per-frame publish cheap store it as
  `std::shared_ptr<const std::unordered_map<...>>` inside Snapshot and only rebuild on
  swapchain create/destroy. Same for `slots` (rebuild only if changed vs previous frame).
- **Stale gaze**: Vulkan half treats eye data as invalid when
  `now - publishTimeNs > 200 ms` or `!sessionRunning`, and falls back to the fixed
  centre with the fixed-foveation radii.

## 2. Config (src/common/config.h)

```cpp
struct Rate { uint8_t w = 1, h = 1; };   // parsed from "1x1", "2x2", "4x4", "2x1", ...

struct Config {
  bool  enabled = true;
  bool  eyeTracking = true;          // use XR_EXT_eye_gaze_interaction if available
  // Radii in normalized eye-image units (image spans [-1,1] in x and y; see 06).
  float innerRadius = 0.286f,  outerRadius = 0.95f;   // eye-tracked
  float fixedInnerRadius = 0.286f, fixedOuterRadius = 0.95f; // fixed foveation
  Rate  innerRate{1,1}, middleRate{2,2}, outerRate{2,2};
  float horizontalScale = 1.03f;     // >1 widens the ellipse horizontally
  float fixedOffsetX = 0.0f, fixedOffsetY = 0.0f;   // fixed-mode gaze offset, tangent units
  float smoothing = 0.0f;            // 0 = none, 0.9 = heavy (EMA factor)
  bool  gazeYFlip = false;           // mirror v for flipped eye images
  std::string preset = "lo";         // lo | med | hi (Valve FDM geometry)
  uint32_t texelSize = 0;            // 0 = auto (16 clamped to device range)
  float sizeTolerance = 0.02f;       // aspect-ratio tolerance for eye candidates
  float minSizeRatio = 0.5f;         // candidate width >= ratio * recommended width
  int   debugMode = 0;               // 0 off, 1 = whole eye at outer rate, 2 = inverted (fovea coarse)
  std::string logLevel = "info";     // error|warn|info|debug|trace
  uint64_t generation = 0;           // bumped on every reload
};
```

File: `$XDG_CONFIG_HOME/vrsfov/settings.ini` (default `~/.config/vrsfov/settings.ini`).
Format:
```ini
[global]
enabled = 1
inner_radius = 0.35
outer_radius = 0.70
inner_rate = 1x1
middle_rate = 2x2
outer_rate = 4x4

[app:hello_xr]          ; XrApplicationInfo::applicationName, case-insensitive
outer_rate = 2x2

[exe:SomeGame.exe]      ; matched against basename of /proc/self/exe and of argv[0]
enabled = 0             ;   (Proton: argv[0] is the Windows exe path, read /proc/self/cmdline)
```
Key names = snake_case of the struct fields. Precedence (low→high): defaults, `[global]`,
`[exe:...]`, `[app:...]`, env vars `VRSFOV_<UPPER_SNAKE_KEY>` (e.g. `VRSFOV_OUTER_RATE=2x2`).

Validation: clamp radii to [0, 3], force `outerRadius >= innerRadius`, rates must be one of
the 7 valid ones (else keep default and warn). Rates are later clamped to the device's
supported rates (see 04 §5).

Hot reload: the OpenXR half checks the file `mtime` at most once per second inside
`xrWaitFrame`; on change, reparse, bump `generation`, publish. The Vulkan half rebuilds pattern
buffers lazily when it sees a new `generation` (old buffers are kept alive until device
destruction — they may still be referenced by in-flight command buffers).

Parser: write a tiny INI parser (sections, `key = value`, `;`/`#` comments, trim). No deps.
Unit test it.

## 3. Logging (src/common/log.h)

- Macros `LOG_ERROR/WARN/INFO/DEBUG/TRACE(fmt, ...)` printf-style.
- Output to `$XDG_STATE_HOME/vrsfov/vrsfov.log` (default `~/.local/state/...`),
  truncated at process start, plus stderr for WARN and above. Prefix with monotonic time,
  thread id, and `[xr]` / `[vk]`.
- `LOG_ONCE(key, ...)` helper (static `std::unordered_set<std::string>` under mutex) for
  per-render-target classification messages — never log per-frame at INFO.
- Every 5 seconds (checked in `xrEndFrame`) log stats at INFO: frames, foveated passes/frame,
  neutral passes/frame, eye-tracking state, last gaze uv. Counters are `std::atomic<uint64_t>`
  in SharedState incremented by the Vulkan half.
