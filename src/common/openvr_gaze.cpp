#include "common/openvr_gaze.h"

#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <unistd.h>

#include <openvr/openvr.h>

#include "common/gaze_math.h"
#include "common/log.h"

namespace vrsfov {
namespace {

using InitInternalFn = uint32_t (*)(vr::EVRInitError*, vr::EVRApplicationType);
using ShutdownInternalFn = void (*)();
using GetGenericInterfaceFn = void* (*)(const char*, vr::EVRInitError*);
using ErrorDescriptionFn = const char* (*)(vr::EVRInitError);

void* g_library = nullptr;
InitInternalFn g_init = nullptr;
ShutdownInternalFn g_shutdown = nullptr;
GetGenericInterfaceFn g_getInterface = nullptr;
ErrorDescriptionFn g_errorDescription = nullptr;  // optional, for readable logs
const char* g_mode = "2";
vr::IVRSystem* g_system = nullptr;
bool g_initialized = false;
bool g_prepared = false;
bool g_available = false;
std::string g_report;

struct ShutdownAtExit {
  ~ShutdownAtExit() { openvrGazeShutdown(); }
} g_shutdownGuard;

// Directory of the SteamVR install. In the publisher process vrclient.so is not
// mapped (there is no game in this process), so the usual SteamVR locations are
// used unless VRSFOV_STEAMVR_DIR names one explicitly.
std::string steamVrDirFromMaps() {
  FILE* f = fopen("/proc/self/maps", "r");
  if (f) {
    char line[4096];
    std::string found;
    const char* needle = "vrclient.so";
    const size_t needleLen = strlen(needle);
    while (fgets(line, sizeof(line), f)) {
      char* path = strchr(line, '/');
      if (!path) continue;
      std::string p(path);
      while (!p.empty() && (p.back() == '\n' || p.back() == '\r' || p.back() == ' ')) {
        p.pop_back();
      }
      const size_t cut = p.find(" (deleted)");
      if (cut != std::string::npos) p.resize(cut);
      if (p.size() >= needleLen && p.compare(p.size() - needleLen, needleLen, needle) == 0) {
        found = p;
        break;
      }
    }
    fclose(f);
    if (!found.empty()) {
      const size_t slash = found.find_last_of('/');
      if (slash != std::string::npos) return found.substr(0, slash);
    }
  }

  auto hasLibrary = [](const std::string& dir) {
    return !dir.empty() && access((dir + "/libopenvr_api.so").c_str(), R_OK) == 0;
  };
  if (const char* dir = getenv("VRSFOV_STEAMVR_DIR")) {
    if (hasLibrary(dir)) return dir;
  }
  const char* home = getenv("HOME");
#if defined(__aarch64__) || defined(_M_ARM64)
#define VRSFOV_STEAMVR_PRIMARY_BIN "linuxarm64"
#define VRSFOV_STEAMVR_SECONDARY_BIN "linux64"
#else
#define VRSFOV_STEAMVR_PRIMARY_BIN "linux64"
#define VRSFOV_STEAMVR_SECONDARY_BIN "linuxarm64"
#endif

  const char* relative[] = {
      "/.local/share/Steam/steamapps/common/SteamVR/bin/" VRSFOV_STEAMVR_PRIMARY_BIN,
      "/.local/share/Steam/steamapps/common/SteamVR/bin/" VRSFOV_STEAMVR_SECONDARY_BIN,
      "/.steam/steam/steamapps/common/SteamVR/bin/" VRSFOV_STEAMVR_PRIMARY_BIN,
      "/.steam/steam/steamapps/common/SteamVR/bin/" VRSFOV_STEAMVR_SECONDARY_BIN,
      "/.steam/root/steamapps/common/SteamVR/bin/" VRSFOV_STEAMVR_PRIMARY_BIN,
      "/.steam/root/steamapps/common/SteamVR/bin/" VRSFOV_STEAMVR_SECONDARY_BIN,
  };
  if (home) {
    for (const char* rel : relative) {
      const std::string dir = std::string(home) + rel;
      if (hasLibrary(dir)) return dir;
    }
  }
  for (const char* dir : {"/opt/steamvr/bin/" VRSFOV_STEAMVR_PRIMARY_BIN,
                          "/opt/steamvr/bin/" VRSFOV_STEAMVR_SECONDARY_BIN,
                          "/usr/lib/steamvr", "/usr/lib64/steamvr"}) {
    if (hasLibrary(dir)) return dir;
  }
#undef VRSFOV_STEAMVR_PRIMARY_BIN
#undef VRSFOV_STEAMVR_SECONDARY_BIN
  return {};
}

std::string initErrorText(vr::EVRInitError e) {
  const char* s = g_errorDescription ? g_errorDescription(e) : nullptr;
  return std::string(s ? s : "unknown error") + " (" + std::to_string((int)e) + ")";
}

void resetClient() {
  g_system = nullptr;
  if (g_initialized && g_shutdown) {
    g_shutdown();
    g_shutdown = nullptr;
  }
  g_initialized = false;
  g_init = nullptr;
  g_shutdown = nullptr;
  g_getInterface = nullptr;
  g_errorDescription = nullptr;
  if (g_library) {
    dlclose(g_library);
    g_library = nullptr;
  }
  g_available = false;
  g_prepared = false;  // allow a later reconnect
}

}  // namespace

bool openvrGazePrepare(std::string& report) {
  if (g_prepared) {
    report = g_report;
    return g_available;
  }
  g_prepared = true;

  const std::string dir = steamVrDirFromMaps();
  if (dir.empty()) {
    g_report = "SteamVR libopenvr_api.so was not found (set VRSFOV_STEAMVR_DIR)";
    report = g_report;
    resetClient();
    return false;
  }
  const std::string libPath = dir + "/libopenvr_api.so";
  g_library = dlopen(libPath.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!g_library) {
    const char* errMsg = dlerror();
    g_report = "cannot load " + libPath + ": " + (errMsg ? errMsg : "?");
    report = g_report;
    resetClient();
    return false;
  }
  g_init = reinterpret_cast<InitInternalFn>(dlsym(g_library, "VR_InitInternal"));
  g_shutdown = reinterpret_cast<ShutdownInternalFn>(dlsym(g_library, "VR_ShutdownInternal"));
  g_getInterface =
      reinterpret_cast<GetGenericInterfaceFn>(dlsym(g_library, "VR_GetGenericInterface"));
  g_errorDescription = reinterpret_cast<ErrorDescriptionFn>(
      dlsym(g_library, "VR_GetVRInitErrorAsEnglishDescription"));
  if (!g_init || !g_shutdown || !g_getInterface) {
    g_report = "libopenvr_api.so has no VR_InitInternal/VR_GetGenericInterface";
    report = g_report;
    resetClient();
    return false;
  }

  // Client type. A background client (the polite default: this process already
  // belongs to the VR application) is *refused* gaze data by SteamVR, so the
  // "Other" type - which does not claim to render a scene, so it does not disturb
  // the running application - is what actually gets the eye-tracked centre.
  // 1/background, 2/other (default), 3/overlay, 5/utility. VRApplication_OpenXRScene
  // is deliberately NOT offered: it is reserved for OpenXR itself, and asking for it
  // here (the frame after this was written) was followed by the headset dropping off
  // the link - one reserved client type too far.
  // VRSFOV_OPENVR_GAZE is the on/off switch (checked by the callers); the
  // client type has its own variable so "1" cannot accidentally mean
  // VRApplication_Background, which SteamVR refuses eye-tracked data to.
  const char* mode = getenv("VRSFOV_OPENVR_CLIENT");
  g_mode = mode && *mode ? mode : "2";
  vr::EVRApplicationType appType = vr::VRApplication_Other;
  if (!strcmp(g_mode, "1") || !strcmp(g_mode, "background")) {
    appType = vr::VRApplication_Background;
  } else if (!strcmp(g_mode, "3") || !strcmp(g_mode, "overlay")) {
    appType = vr::VRApplication_Overlay;
  } else if (!strcmp(g_mode, "5") || !strcmp(g_mode, "utility")) {
    appType = vr::VRApplication_Utility;
  }
  vr::EVRInitError err = vr::VRInitError_None;
  // VR_InitInternal returns a VR token, not an EVRInitError. The error is
  // reported through `err`; treating a nonzero token as failure leaves the
  // publisher stuck at "No Error (0)" forever.
  (void)g_init(&err, appType);
  if (err != vr::VRInitError_None) {
    g_report = "VR_InitInternal failed: " + initErrorText(err);
    report = g_report;
    resetClient();
    return false;
  }
  g_initialized = true;

  vr::EVRInitError ifaceErr = vr::VRInitError_None;
  void* iface = g_getInterface(vr::IVRSystem_Version, &ifaceErr);
  if (!iface) {
    g_report = std::string("the runtime does not provide ") + vr::IVRSystem_Version + " (" +
               initErrorText(ifaceErr) + ")";
    report = g_report;
    resetClient();
    return false;
  }
  g_system = static_cast<vr::IVRSystem*>(iface);

  char model[256] = {0};
  vr::ETrackedPropertyError pe = vr::TrackedProp_Success;
  g_system->GetStringTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                                           vr::Prop_ModelNumber_String, model, sizeof(model), &pe);
  pe = vr::TrackedProp_Success;
  const bool supports = g_system->GetBoolTrackedDeviceProperty(
      vr::k_unTrackedDeviceIndex_Hmd, vr::Prop_SupportsXrEyeGazeInteraction_Bool, &pe);

  vr::HmdVector2_t l{}, r{};
  const bool haveCentre = g_system->GetEyeTrackedFoveationCenter(&l, &r);

  g_report = std::string("client type ") + g_mode + "; runtime " +
             (g_system->GetRuntimeVersion() ? g_system->GetRuntimeVersion() : "?") +
             ", HMD '" + (model[0] ? model : "?") + "', " + vr::IVRSystem_Version +
             ", Prop_SupportsXrEyeGazeInteraction=" + (supports ? "true" : "false") +
             (pe == vr::TrackedProp_Success ? "" : " (property unsupported)") +
             (haveCentre ? ", GetEyeTrackedFoveationCenter=ok"
                         : ", GetEyeTrackedFoveationCenter=no data");
  g_available = true;
  report = g_report;
  return true;
}

bool openvrGazeRead(float& u0, float& v0, float& u1, float& v1) {
  if (!g_available || !g_system) return false;
  vr::HmdVector2_t l{}, r{};
  if (!g_system->GetEyeTrackedFoveationCenter(&l, &r)) return false;
  openvrNdcToUv(l.v[0], l.v[1], u0, v0);
  openvrNdcToUv(r.v[0], r.v[1], u1, v1);
  return true;
}

bool openvrGazeRecommendedSize(uint32_t& width, uint32_t& height) {
  if (!g_available || !g_system) return false;
  uint32_t w = 0, h = 0;
  g_system->GetRecommendedRenderTargetSize(&w, &h);
  if (!w || !h) return false;
  width = w;
  height = h;
  return true;
}

void openvrGazeShutdown() { resetClient(); }

}  // namespace vrsfov
