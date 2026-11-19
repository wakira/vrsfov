// Configuration: defaults, INI file with per-app/per-exe sections, environment
// overrides, validation. Pure apart from reading the file system.
#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// The Steam Frame build has no OpenXR half, so standalone foveation is the
// default there. CMake sets this to 1 for VRSFOV_PLATFORM=frame.
#ifndef VRSFOV_DEFAULT_STANDALONE
#define VRSFOV_DEFAULT_STANDALONE 0
#endif

namespace vrsfov {

struct Rate {
  uint8_t w = 1, h = 1;
};

bool operator==(const Rate& a, const Rate& b);
inline bool operator!=(const Rate& a, const Rate& b) { return !(a == b); }

struct Config {
  bool enabled = true;
  bool eyeTracking = true;
  // Radii in normalized eye-image units (the image spans [-1,1] in x and y).
  // Defaults reproduce Valve's FDM `lo` preset (a .295 x .286 rectangle at the
  // centre, one coarse band outside): our ellipse is 0.286 with hscale 1.03, and
  // outerRadius stays below the corner distance so the outer band does not
  // appear. `preset` can replace the geometry; explicit keys still win.
  float innerRadius = 0.286f, outerRadius = 0.95f;            // eye-tracked
  float fixedInnerRadius = 0.286f, fixedOuterRadius = 0.95f;  // fixed foveation
  Rate innerRate{1, 1}, middleRate{2, 2}, outerRate{2, 2};
  float horizontalScale = 1.03f;
  float fixedOffsetX = 0.0f, fixedOffsetY = 0.0f;  // fixed-mode gaze offset, tangent units
  float smoothing = 0.0f;                          // 0 = none, 0.9 = heavy (EMA factor)
  // Some stacks present the eye image flipped vertically relative to the gaze
  // convention (composition flip). Mirror v after projection.
  bool gazeYFlip = false;
  // Some engines render the eye images into an intermediate layered target
  // upside down and flip them when copying to the swapchain (VTOL VR does this).
  // The fovea is then at 1 - v in that target, so both the foveation and the
  // paint marker must be mirrored for it. Applies to layered eye targets that are
  // not XR swapchain images; opt-in because it is app specific.
  bool gazeYFlipEyeImages = false;
  // Named geometry preset matching Valve's FDM profiles: lo | med | hi.
  std::string preset = "lo";
  uint32_t texelSize = 0;  // 0 = auto
  float sizeTolerance = 0.02f;
  float minSizeRatio = 0.5f;
  // Generic fixed foveation for Vulkan apps without an OpenXR session (OpenVR
  // games, plain Vulkan apps): no per-eye knowledge, targets are matched by size.
  bool standalone = VRSFOV_DEFAULT_STANDALONE != 0;
  // Expected per-eye render size. 0 = auto: for an OpenVR game with
  // VRSFOV_OPENVR_GAZE=1, the out-of-process publisher's recommended size is
  // used when it becomes available; otherwise any colour target is eligible.
  uint32_t eyeWidth = 0, eyeHeight = 0;
  int debugMode = 0;  // 0 off, 1 = outer rate everywhere, 2 = inverted
  // Debug overlay: draw a thin band at each radius so the fovea can be found by
  // eye. It is a shading-rate band, not colour: the ring shows up as a line of
  // different sharpness. 0 off, 1 both, 2 inner only, 3 outer only.
  uint32_t drawRadii = 0;
  uint32_t ringWidth = 2;  // band width in rate-image texels
  // Debug visual marker: clear the inner foveal box to solid green at the end of
  // each foveated pass so the gaze area is unmistakably visible in-game.
  bool paintGaze = false;
  std::string logLevel = "info";
  uint64_t generation = 0;  // bumped on every reload
};

struct IniSection {
  std::string name;
  std::vector<std::pair<std::string, std::string>> entries;
};

// Parses INI text: [section], key = value, ';' and '#' comments, trimmed.
std::vector<IniSection> parseIni(const std::string& text);

// "1x1", "2x2", "4x4", "2x1", "1x2", "2x4", "4x2" -> Rate. Returns false and
// leaves `out` untouched when the text is not one of the seven valid rates.
bool parseRate(const std::string& text, Rate& out);
std::string rateToString(const Rate& r);

// "WxH" (for example "2432x2304") -> width/height. Returns false when the text
// is not of that form or either side is zero.
bool parseSize(const std::string& text, uint32_t& width, uint32_t& height);

// $XDG_CONFIG_HOME/vrsfov/settings.ini, defaulting to ~/.config/...
std::string defaultConfigPath();
// mtime in ns, or 0 when the file does not exist.
int64_t fileMtimeNs(const std::string& path);

// Basenames of /proc/self/exe and of every /proc/self/cmdline token (this covers
// Proton, where argv[0] is a Windows path and the real exe is elsewhere).
std::vector<std::string> processExeNames();

// True when the env var is set to something that is not "", "0", "false" or "no".
bool envFlag(const char* name);

// Loads defaults, then the config file (global -> exe -> app), then env
// overrides (VRSFOV_<UPPER_SNAKE_KEY>), then validates. Only the sections
// matching `appName` / `exeName` are applied. `generation` is stored as-is.
// Applies the resulting log level to the logging module.
Config loadConfig(const std::string& appName, const std::string& exeName, uint64_t generation = 0,
                  const std::string& path = defaultConfigPath());

// Same, but matching every candidate process name returned by processExeNames().
Config loadConfigForProcess(const std::string& appName, uint64_t generation = 0,
                            const std::string& path = defaultConfigPath());

void validateConfig(Config& c);

// One-line description of the effective settings, for the log.
std::string configSummary(const Config& c);

// Applies one key/value pair (snake_case key). Unknown keys are ignored with a
// warning. Exposed for tests.
void applyConfigKey(Config& c, const std::string& key, const std::string& value,
                    const std::string& origin);

}  // namespace vrsfov
