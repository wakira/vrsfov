// Device creation / destruction, activation decision and capability detection.
#include "common/guard.h"
#include "common/log.h"
#include "common/shared_state.h"
#include "vulkan/vk_dispatch.h"
#include "vulkan/vk_hooks.h"

#include <vulkan/vk_layer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
using namespace vrsfov;

namespace vrsfov {
namespace {

const VkLayerDeviceCreateInfo* findLinkInfo(const void* pNext) {
  auto info = reinterpret_cast<const VkLayerDeviceCreateInfo*>(pNext);
  while (info) {
    if (info->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
        info->function == VK_LAYER_LINK_INFO) {
      return info;
    }
    info = reinterpret_cast<const VkLayerDeviceCreateInfo*>(info->pNext);
  }
  return nullptr;
}

void resolveDeviceDispatch(DeviceData& data) {
  VkDevice dev = data.device;
  PFN_vkGetDeviceProcAddr gdpa = data.nextGdpa;
  DeviceDispatch& t = data.d;
  t.GetDeviceProcAddr = gdpa;

#define VRSFOV_RESOLVE(field, name) \
  t.field = gdpa ? reinterpret_cast<decltype(t.field)>(gdpa(dev, name)) : nullptr
  VRSFOV_RESOLVE(DestroyDevice, "vkDestroyDevice");
  VRSFOV_RESOLVE(CreateImage, "vkCreateImage");
  VRSFOV_RESOLVE(DestroyImage, "vkDestroyImage");
  VRSFOV_RESOLVE(CreateImageView, "vkCreateImageView");
  VRSFOV_RESOLVE(DestroyImageView, "vkDestroyImageView");
  VRSFOV_RESOLVE(CreateBuffer, "vkCreateBuffer");
  VRSFOV_RESOLVE(DestroyBuffer, "vkDestroyBuffer");
  VRSFOV_RESOLVE(AllocateMemory, "vkAllocateMemory");
  VRSFOV_RESOLVE(FreeMemory, "vkFreeMemory");
  VRSFOV_RESOLVE(BindImageMemory, "vkBindImageMemory");
  VRSFOV_RESOLVE(BindBufferMemory, "vkBindBufferMemory");
  VRSFOV_RESOLVE(MapMemory, "vkMapMemory");
  VRSFOV_RESOLVE(GetImageMemoryRequirements, "vkGetImageMemoryRequirements");
  VRSFOV_RESOLVE(GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
  VRSFOV_RESOLVE(CreateRenderPass, "vkCreateRenderPass");
  VRSFOV_RESOLVE(CreateRenderPass2, "vkCreateRenderPass2");
  VRSFOV_RESOLVE(CreateRenderPass2KHR, "vkCreateRenderPass2KHR");
  VRSFOV_RESOLVE(DestroyRenderPass, "vkDestroyRenderPass");
  VRSFOV_RESOLVE(CreateFramebuffer, "vkCreateFramebuffer");
  VRSFOV_RESOLVE(DestroyFramebuffer, "vkDestroyFramebuffer");
  VRSFOV_RESOLVE(CreateGraphicsPipelines, "vkCreateGraphicsPipelines");
  VRSFOV_RESOLVE(CreateShaderModule, "vkCreateShaderModule");
  VRSFOV_RESOLVE(DestroyShaderModule, "vkDestroyShaderModule");
  VRSFOV_RESOLVE(BeginCommandBuffer, "vkBeginCommandBuffer");
  VRSFOV_RESOLVE(FreeCommandBuffers, "vkFreeCommandBuffers");
  VRSFOV_RESOLVE(CmdBeginRenderPass, "vkCmdBeginRenderPass");
  VRSFOV_RESOLVE(CmdBeginRenderPass2, "vkCmdBeginRenderPass2");
  VRSFOV_RESOLVE(CmdBeginRenderPass2KHR, "vkCmdBeginRenderPass2KHR");
  VRSFOV_RESOLVE(CmdBeginRendering, "vkCmdBeginRendering");
  VRSFOV_RESOLVE(CmdBeginRenderingKHR, "vkCmdBeginRenderingKHR");
  VRSFOV_RESOLVE(CmdPipelineBarrier, "vkCmdPipelineBarrier");
  VRSFOV_RESOLVE(CmdCopyBufferToImage, "vkCmdCopyBufferToImage");
  VRSFOV_RESOLVE(CmdClearColorImage, "vkCmdClearColorImage");
#undef VRSFOV_RESOLVE
}

// ---------------------------------------------------------------------------
// Capability helpers
// ---------------------------------------------------------------------------

template <class T>
T* findInChain(const void* head, VkStructureType sType) {
  auto node = reinterpret_cast<const VkBaseInStructure*>(head);
  while (node) {
    if (node->sType == sType) return const_cast<T*>(reinterpret_cast<const T*>(node));
    node = node->pNext;
  }
  return nullptr;
}

bool nameInList(uint32_t count, const char* const* names, const char* want) {
  if (!names) return false;
  for (uint32_t i = 0; i < count; ++i) {
    if (names[i] && strcmp(names[i], want) == 0) return true;
  }
  return false;
}

bool deviceExtensionEnabled(const VkDeviceCreateInfo* ci, const char* name) {
  return nameInList(ci->enabledExtensionCount, ci->ppEnabledExtensionNames, name);
}

bool deviceExtensionAvailable(InstanceData* inst, VkPhysicalDevice gpu, const char* name) {
  if (!inst || !inst->d.EnumerateDeviceExtensionProperties) return false;
  uint32_t count = 0;
  if (inst->d.EnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr) != VK_SUCCESS) {
    return false;
  }
  std::vector<VkExtensionProperties> props(count);
  if (count &&
      inst->d.EnumerateDeviceExtensionProperties(gpu, nullptr, &count, props.data()) != VK_SUCCESS) {
    return false;
  }
  for (const auto& p : props) {
    if (strcmp(p.extensionName, name) == 0) return true;
  }
  return false;
}

uint32_t pow2Floor(uint32_t v) {
  if (v == 0) return 1;
  while (v & (v - 1)) v &= v - 1;
  return v;
}

Rate clampRate(const Rate& want, const std::vector<VkExtent2D>& supported) {
  Rate best{1, 1};
  bool first = true;
  for (const VkExtent2D& s : supported) {
    if (s.width == 0 || s.height == 0) continue;
    if (s.width > want.w || s.height > want.h) continue;
    if (first || (uint32_t)s.width * s.height > (uint32_t)best.w * best.h) {
      best.w = (uint8_t)s.width;
      best.h = (uint8_t)s.height;
      first = false;
    }
  }
  return best;
}

struct Activation {
  bool active = false;
  // The device is capable (our extensions and features can be enabled) but there
  // is no XrInstance yet: the injections start as soon as one appears. Games that
  // initialise Vulkan graphics before OpenXR create their device first, and this
  // is what lets them foveate at all.
  bool pendingInstance = false;
  std::string reason;
};

// 04 §5 step 2: everything that can rule the device out before we touch the
// create info.
Activation evaluateActivation(DeviceData& dd, const VkDeviceCreateInfo* ci, const Config& cfg) {
  Activation a;
  SharedState& state = SharedState::get();

  if (!cfg.enabled) {
    a.reason = "disabled in config";
    return a;
  }
  // Diagnostic switch: the layer stays in the chain (loaded and negotiated) but
  // never activates, so "merely present" can be told apart from "active".
  if (envFlag("VRSFOV_DISABLE_VK")) {
    a.reason = "VRSFOV_DISABLE_VK";
    return a;
  }
  // OpenXR apps have an XrInstance; standalone mode (fixed foveation for any
  // Vulkan app) and the VRSFOV_FORCE_ACTIVE debug knob do not need one. This
  // is checked *after* the capability probes on purpose: a device created before
  // the XrInstance still gets our extensions and features enabled, and then
  // activates as soon as an instance appears (see vrsfovMaybeActivate).
  const bool hasInstance = state.liveXrInstances.load() > 0;
  const bool instanceRequired = !cfg.standalone && !envFlag("VRSFOV_FORCE_ACTIVE");

  InstanceData* inst = dd.instance;
  if (!inst || !inst->d.GetPhysicalDeviceFeatures2 || !inst->d.GetPhysicalDeviceProperties2) {
    a.reason = "instance dispatch incomplete";
    return a;
  }

  if (!deviceExtensionAvailable(inst, dd.physicalDevice, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)) {
    a.reason = "VK_KHR_fragment_shading_rate not available";
    return a;
  }
  const bool renderPass2 = inst->apiVersion >= VK_API_VERSION_1_2 ||
                           deviceExtensionAvailable(inst, dd.physicalDevice,
                                                    VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
  if (!renderPass2) {
    a.reason = "no VK_KHR_create_renderpass2 and API < 1.2";
    return a;
  }
  if (!inst->d.GetPhysicalDeviceFragmentShadingRatesKHR) {
    a.reason = "vkGetPhysicalDeviceFragmentShadingRatesKHR not resolvable";
    return a;
  }

  VkPhysicalDeviceFragmentShadingRateFeaturesKHR fsr{};
  fsr.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR;
  VkPhysicalDeviceFeatures2 features2{};
  features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  features2.pNext = &fsr;
  inst->d.GetPhysicalDeviceFeatures2(dd.physicalDevice, &features2);
  if (!fsr.pipelineFragmentShadingRate || !fsr.attachmentFragmentShadingRate) {
    a.reason = "pipeline/attachment fragment shading rate not supported";
    return a;
  }

  // Apps that already use NV shading rate image or EXT fragment density map must
  // be left alone: those are mutually exclusive with FSR.
  if (const auto* nv = findInChain<VkPhysicalDeviceShadingRateImageFeaturesNV>(
          ci->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADING_RATE_IMAGE_FEATURES_NV)) {
    if (nv->shadingRateImage == VK_TRUE) {
      a.reason = "app uses VK_NV_shading_rate_image";
      return a;
    }
  }
  if (const auto* fdm = findInChain<VkPhysicalDeviceFragmentDensityMapFeaturesEXT>(
          ci->pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT)) {
    if (fdm->fragmentDensityMap == VK_TRUE) {
      a.reason = "app uses VK_EXT_fragment_density_map";
      return a;
    }
  }

  VkFormatProperties fmt{};
  if (inst->d.GetPhysicalDeviceFormatProperties) {
    inst->d.GetPhysicalDeviceFormatProperties(dd.physicalDevice, VK_FORMAT_R8_UINT, &fmt);
  }
  if (!(fmt.optimalTilingFeatures & VK_FORMAT_FEATURE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR)) {
    a.reason = "R8_UINT is not a fragment shading rate attachment format";
    return a;
  }

  a.active = !instanceRequired || hasInstance;
  a.pendingInstance = instanceRequired && !hasInstance;
  if (a.pendingInstance) a.reason = "no OpenXR instance yet";
  return a;
}

void fillCaps(DeviceData& dd, const VkDeviceCreateInfo* ci, const Config& cfg) {
  Caps& c = dd.caps;
  InstanceData* inst = dd.instance;

  VkPhysicalDeviceFragmentShadingRatePropertiesKHR props{};
  props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR;
  VkPhysicalDeviceProperties2 props2{};
  props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
  props2.pNext = &props;
  inst->d.GetPhysicalDeviceProperties2(dd.physicalDevice, &props2);

  c.minTexel = props.minFragmentShadingRateAttachmentTexelSize;
  c.maxTexel = props.maxFragmentShadingRateAttachmentTexelSize;
  c.maxTexelAspect = std::max(1u, props.maxFragmentShadingRateAttachmentTexelSizeAspectRatio);
  c.layeredAttachments = props.layeredShadingRateAttachments == VK_TRUE;
  c.nonTrivialCombiners = props.fragmentShadingRateNonTrivialCombinerOps == VK_TRUE;
  c.withDepthStencilWrites = props.fragmentShadingRateWithShaderDepthStencilWrites == VK_TRUE;
  c.withShaderSampleMask = props.fragmentShadingRateWithShaderSampleMask == VK_TRUE;
  c.withSampleMask = props.fragmentShadingRateWithSampleMask == VK_TRUE;
  if (c.minTexel.width == 0 || c.minTexel.height == 0) c.minTexel = VkExtent2D{1, 1};
  if (c.maxTexel.width == 0 || c.maxTexel.height == 0) c.maxTexel = VkExtent2D{1, 1};

  uint32_t count = 0;
  inst->d.GetPhysicalDeviceFragmentShadingRatesKHR(dd.physicalDevice, &count, nullptr);
  std::vector<VkPhysicalDeviceFragmentShadingRateKHR> rates(count);
  for (auto& r : rates) {
    r.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR;
  }
  if (count) {
    inst->d.GetPhysicalDeviceFragmentShadingRatesKHR(dd.physicalDevice, &count, rates.data());
  }
  for (const auto& r : rates) {
    if (!(r.sampleCounts & VK_SAMPLE_COUNT_1_BIT)) continue;
    c.supportedRates.push_back(r.fragmentSize);
  }
  if (c.supportedRates.empty()) c.supportedRates.push_back(VkExtent2D{1, 1});

  // Square texels, power of two, inside the device range; 16 preferred.
  uint32_t lo = std::max(c.minTexel.width, c.minTexel.height);
  uint32_t hi = std::min(c.maxTexel.width, c.maxTexel.height);
  if (hi < lo) hi = lo;
  uint32_t want = cfg.texelSize ? cfg.texelSize : 16;
  want = std::min(std::max(want, lo), hi);
  c.texel = std::max(1u, pow2Floor(want));

  c.innerRate = clampRate(cfg.innerRate, c.supportedRates);
  c.middleRate = clampRate(cfg.middleRate, c.supportedRates);
  c.outerRate = clampRate(cfg.outerRate, c.supportedRates);

  VkPhysicalDeviceMemoryProperties mem{};
  if (inst->d.GetPhysicalDeviceMemoryProperties) {
    inst->d.GetPhysicalDeviceMemoryProperties(dd.physicalDevice, &mem);
  }
  c.memProps = mem;
  const VkMemoryPropertyFlags deviceLocal = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  const VkMemoryPropertyFlags hostCoherent =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags f = mem.memoryTypes[i].propertyFlags;
    if ((f & deviceLocal) == deviceLocal) {
      c.memoryTypeDeviceLocal = i;
      break;
    }
  }
  for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags f = mem.memoryTypes[i].propertyFlags;
    if ((f & hostCoherent) == hostCoherent) {
      c.memoryTypeHostVisibleCoherent = i;
      break;
    }
  }

  c.dynamicRenderingAvailable =
      dd.instance->apiVersion >= VK_API_VERSION_1_3 ||
      deviceExtensionAvailable(inst, dd.physicalDevice, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) ||
      deviceExtensionEnabled(ci, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
}

// Enables pipeline+attachment fragment shading rate on the create info: patch the
// app's feature struct in place when it exists (never duplicate an sType), else
// prepend our own. Restores/undoes on destruction.
class FsrFeaturePatch {
 public:
  void apply(VkDeviceCreateInfo& ci) {
    auto* existing = findInChain<VkPhysicalDeviceFragmentShadingRateFeaturesKHR>(
        ci.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR);
    if (existing) {
      patched_ = existing;
      savedPipeline_ = existing->pipelineFragmentShadingRate;
      savedAttachment_ = existing->attachmentFragmentShadingRate;
      existing->pipelineFragmentShadingRate = VK_TRUE;
      existing->attachmentFragmentShadingRate = VK_TRUE;
      mode_ = Mode::Patch;
    } else {
      ours_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR;
      ours_.pNext = const_cast<void*>(ci.pNext);
      ours_.pipelineFragmentShadingRate = VK_TRUE;
      ours_.attachmentFragmentShadingRate = VK_TRUE;
      ours_.primitiveFragmentShadingRate = VK_FALSE;
      ci.pNext = &ours_;
      mode_ = Mode::Prepend;
    }
  }

  void restore(VkDeviceCreateInfo& ci) {
    if (mode_ == Mode::Patch && patched_) {
      patched_->pipelineFragmentShadingRate = savedPipeline_;
      patched_->attachmentFragmentShadingRate = savedAttachment_;
    } else if (mode_ == Mode::Prepend) {
      ci.pNext = ours_.pNext;
    }    mode_ = Mode::None;
  }

  ~FsrFeaturePatch() {
    if (mode_ == Mode::Patch && patched_) {
      patched_->pipelineFragmentShadingRate = savedPipeline_;
      patched_->attachmentFragmentShadingRate = savedAttachment_;
    }
  }

 private:
  enum class Mode { None, Patch, Prepend };
  Mode mode_ = Mode::None;
  VkPhysicalDeviceFragmentShadingRateFeaturesKHR* patched_ = nullptr;
  VkPhysicalDeviceFragmentShadingRateFeaturesKHR ours_{};
  VkBool32 savedPipeline_ = VK_FALSE;
  VkBool32 savedAttachment_ = VK_FALSE;
};

// Standalone / debug: synthesize the snapshot the OpenXR half would publish.
//   standalone = 1        fixed foveation for a Vulkan app without OpenXR
//   VRSFOV_FAKE_EYE=WxH   (debug) override the recommended eye size
//   VRSFOV_FAKE_GAZE=u,v  (debug) override the gaze point
void installSyntheticSnapshot(DeviceData& data, const Config& cfg) {
  const char* spec = getenv("VRSFOV_FAKE_EYE");
  const bool haveFakeEye = spec && *spec;
  if (!haveFakeEye && !cfg.standalone) return;

  uint32_t w = cfg.eyeWidth, h = cfg.eyeHeight;
  if (haveFakeEye && (sscanf(spec, "%ux%u", &w, &h) != 2 || w == 0 || h == 0)) {
    LOG_WARN("VRSFOV_FAKE_EYE='%s' is not WxH; ignoring", spec);
    w = cfg.eyeWidth;
    h = cfg.eyeHeight;
  }

  float u = 0.5f, v = 0.5f;
  bool haveGaze = false;
  if (const char* gaze = getenv("VRSFOV_FAKE_GAZE")) {
    if (sscanf(gaze, "%f,%f", &u, &v) == 2) {
      haveGaze = true;
    } else {
      LOG_WARN("VRSFOV_FAKE_GAZE='%s' is not u,v; using the center", gaze);
    }
  }
  if (!haveGaze) {
    // Fixed-mode gaze: the centre plus the configured offsets. The OpenXR half
    // maps them through the session FOV; here the 90 degree convention is used
    // (fixed_offset_x = 0.5 -> u = 0.75).
    u = std::min(1.f, std::max(0.f, 0.5f + 0.5f * cfg.fixedOffsetX));
    v = std::min(1.f, std::max(0.f, 0.5f - 0.5f * cfg.fixedOffsetY));
  }

  auto config = std::make_shared<const Config>(cfg);
  const uint64_t device = (uint64_t)(uintptr_t)data.device;
  SharedState::get().update([&](Snapshot& snap) {
    snap.sessionVkDevice = device;
    snap.sessionRunning = true;
    snap.recommendedW = w;
    snap.recommendedH = h;
    snap.eyes[0] = EyeView{true, u, v, false};
    snap.eyes[1] = EyeView{true, u, v, false};
    snap.config = config;
    snap.publishTimeNs = nowNs();
    snap.synthetic = true;
    snap.anyDevice = true;
  });

  LOG_INFO("synthetic snapshot (%s): recommended %ux%u, gaze (%.3f, %.3f), debugMode %d",
           haveFakeEye ? "VRSFOV_FAKE_EYE" : "standalone mode", w, h, u, v, cfg.debugMode);
  LOG_INFO("config (file + environment): %s", configSummary(cfg).c_str());
  if (w == 0 || h == 0) {
    LOG_WARN("no usable eye size: every colour target will be foveated at the centre "
             "(set eye_size = WxH, or VRSFOV_FAKE_EYE=WxH)");
  }
}

// Standalone mode has no OpenXR half to hot reload the config, so the Vulkan half
// does it while recording (throttled to once per second by the caller).
void reloadStandaloneConfig(DeviceData& data) {
  const int64_t now = nowNs();
  if (now - data.lastConfigCheckNs < 1000LL * 1000 * 1000) return;
  data.lastConfigCheckNs = now;
  const int64_t mtime = fileMtimeNs(data.configPath);
  if (mtime == 0 || mtime == data.configMtime) return;
  data.configMtime = mtime;

  const uint64_t generation = data.localConfig.generation + 1;
  Config reloaded = loadConfigForProcess(std::string(), generation, data.configPath);
  data.localConfig = reloaded;
  LOG_INFO("standalone config reloaded (generation %llu): %s", (unsigned long long)generation,
           configSummary(reloaded).c_str());
  if (!reloaded.standalone) {
    LOG_INFO("standalone mode turned off; foveation stops");
    SharedState::get().update([&](Snapshot& snap) {
      snap.sessionRunning = false;
      snap.synthetic = false;
      snap.anyDevice = false;
      snap.publishTimeNs = nowNs();
    });
    data.standalone = false;
    return;
  }
  installSyntheticSnapshot(data, reloaded);
}

// Standalone mode has no OpenXR half, so an OpenVR game can get its gaze straight
// from SteamVR when VRSFOV_OPENVR_GAZE is set: the per-eye eye-tracked
// foveation centre, published into the shared snapshot the classifier reads per
// eye. Opt-in, because it opens a second SteamVR client inside the game's
// process; the last value is kept when a poll has no sample, so the spot does not
// snap back to the centre.
//
// Polled at ~120 Hz (one update per sample the compositor can actually provide),
// not once a second: the eye-tracked centre has to follow the eyes, and a 1 s
// poll made it look like a fixed spot that occasionally jumped. `smoothing`
// applies an EMA exactly as it does for the OpenXR source.

}  // namespace

void vrsfovMaybeReloadStandaloneConfig(DeviceData& data) { reloadStandaloneConfig(data); }

// Late activation: a device that was created before the XrInstance. It already has
// our extensions and features enabled (see vrsfov_vkCreateDevice), so all that is
// missing is the go-ahead, and that is exactly the same condition as at creation
// time: an XrInstance must exist. Checked while recording, throttled to once a
// second - command buffers are recorded as the app starts up, so this catches an
// app that initialises Vulkan graphics before OpenXR (VTOL VR does).
void maybeActivate(DeviceData& data) {
  if (data.active || !data.pendingInstance) return;
  const int64_t now = nowNs();
  if (now - data.lastActivateCheckNs < 1000LL * 1000 * 1000) return;
  data.lastActivateCheckNs = now;
  if (SharedState::get().liveXrInstances.load() <= 0) return;

  data.active = true;
  data.pendingInstance = false;
  const Caps& c = data.caps;
  LOG_INFO("device %p ACTIVE (late: the OpenXR instance appeared after this device): texel %u, "
           "rates inner %s middle %s outer %s (supported %zu), layered=%d nonTrivialCombiners=%d "
           "dynamicRendering=%d; render passes, framebuffers and pipelines created before now stay "
           "unmodified",
           reinterpret_cast<void*>(data.device), c.texel, rateToString(c.innerRate).c_str(),
           rateToString(c.middleRate).c_str(), rateToString(c.outerRate).c_str(),
           c.supportedRates.size(), (int)c.layeredAttachments, (int)c.nonTrivialCombiners,
           (int)c.dynamicRenderingAvailable);
}

}  // namespace vrsfov

namespace vrsfov {

void vrsfovMaybeActivate(DeviceData& data) { maybeActivate(data); }

void vrsfovMaybeUpdateStandaloneGaze(DeviceData& data) { updateStandaloneGaze(data); }

VKAPI_ATTR VkResult VKAPI_CALL vrsfov_vkCreateDevice(VkPhysicalDevice physicalDevice,
                                                    const VkDeviceCreateInfo* pCreateInfo,
                                                    const VkAllocationCallbacks* pAllocator,
                                                    VkDevice* pDevice) {
  VRSFOV_GUARD_BEGIN
  const VkLayerDeviceCreateInfo* link = findLinkInfo(pCreateInfo->pNext);
  if (!link || !link->u.pLayerInfo) {
    LOG_ERROR("vkCreateDevice: no loader link info in the pNext chain");
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  VkLayerDeviceCreateInfo* mutableLink = const_cast<VkLayerDeviceCreateInfo*>(link);
  PFN_vkGetInstanceProcAddr nextGipa = mutableLink->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr nextGdpa = mutableLink->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  mutableLink->u.pLayerInfo = mutableLink->u.pLayerInfo->pNext;
  if (!nextGipa || !nextGdpa) return VK_ERROR_INITIALIZATION_FAILED;

  InstanceData* instance = findInstanceByKey(dispatchKey(physicalDevice));
  VkInstance instanceHandle = instance ? instance->instance : VK_NULL_HANDLE;
  auto nextCreate = reinterpret_cast<PFN_vkCreateDevice>(nextGipa(instanceHandle, "vkCreateDevice"));
  if (!nextCreate) return VK_ERROR_INITIALIZATION_FAILED;

  g_lastNextGdpa = nextGdpa;

  auto data = std::make_unique<DeviceData>();
  data->physicalDevice = physicalDevice;
  data->instance = instance;
  data->nextGipa = nextGipa;
  data->nextGdpa = nextGdpa;

  // The OpenXR half publishes the config when it exists; without it (standalone
  // mode, plain Vulkan apps) the Vulkan half loads the file and environment
  // itself. [app:...] sections cannot match here (no applicationName).
  const std::string configPath = defaultConfigPath();
  Config localConfig = loadConfigForProcess(std::string(), 0, configPath);
  auto snapshot = SharedState::get().snapshot();
  const bool haveXr = SharedState::get().liveXrInstances.load() > 0;
  const Config& cfg = haveXr ? effectiveConfig(snapshot) : localConfig;

  data->localConfig = localConfig;
  data->standalone = localConfig.standalone;
  data->configPath = configPath;
  data->configMtime = fileMtimeNs(configPath);

  Activation activation = evaluateActivation(*data, pCreateInfo, cfg);
  VkDeviceCreateInfo createInfo = *pCreateInfo;
  std::vector<const char*> extensions;
  FsrFeaturePatch patch;
  // Capable: we can enable our extensions and features on this device. That is
  // done even when the XrInstance does not exist yet, because device features are
  // immutable - a device created before the instance could never start foveating
  // otherwise. Injection still only starts once there is something to publish.
  bool capable = activation.active || activation.pendingInstance;
  auto giveUp = [&](const char* reason) {
    activation.active = false;
    activation.pendingInstance = false;
    activation.reason = reason;
    capable = false;
  };
  if (capable) {
    fillCaps(*data, pCreateInfo, cfg);

    // Add the extensions we need to the app's list.
    extensions.assign(pCreateInfo->ppEnabledExtensionNames,
                      pCreateInfo->ppEnabledExtensionNames + pCreateInfo->enabledExtensionCount);
    auto addExtension = [&](const char* name) {
      if (nameInList((uint32_t)extensions.size(), extensions.data(), name)) return true;
      if (!deviceExtensionAvailable(instance, physicalDevice, name)) return false;
      extensions.push_back(name);
      return true;
    };
    if (!addExtension(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)) {
      giveUp("cannot enable VK_KHR_fragment_shading_rate");
    } else if (instance->apiVersion < VK_API_VERSION_1_2) {
      // VK_KHR_create_renderpass2 and its dependencies.
      if (!addExtension(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME) ||
          !addExtension(VK_KHR_MULTIVIEW_EXTENSION_NAME) ||
          !addExtension(VK_KHR_MAINTENANCE_2_EXTENSION_NAME)) {
        giveUp("cannot enable VK_KHR_create_renderpass2");
      }
    }
  }

  if (capable) {
    createInfo.ppEnabledExtensionNames = extensions.data();
    createInfo.enabledExtensionCount = (uint32_t)extensions.size();
    patch.apply(createInfo);
    VkResult result = nextCreate(physicalDevice, &createInfo, pAllocator, pDevice);
    patch.restore(createInfo);
    if (result != VK_SUCCESS) {
      // Retry once without our modifications; the app must keep working.
      LOG_WARN("vkCreateDevice with VRS extensions failed (%d); retrying without", (int)result);
      giveUp("device creation with VRS extensions failed");
      result = nextCreate(physicalDevice, pCreateInfo, pAllocator, pDevice);
      if (result != VK_SUCCESS) return result;
    }
  } else {
    VkResult result = nextCreate(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) return result;
  }

  data->device = *pDevice;
  resolveDeviceDispatch(*data);
  data->active = activation.active;
  data->pendingInstance = capable && activation.pendingInstance;
  if (data->active) installSyntheticSnapshot(*data, cfg);
  if (data->active) {
    const Caps& c = data->caps;
    LOG_INFO("device %p ACTIVE: texel %u, rates inner %s middle %s outer %s (supported %zu), "
             "layered=%d nonTrivialCombiners=%d dynamicRendering=%d",
             reinterpret_cast<void*>(*pDevice), c.texel, rateToString(c.innerRate).c_str(),
             rateToString(c.middleRate).c_str(), rateToString(c.outerRate).c_str(),
             c.supportedRates.size(), (int)c.layeredAttachments, (int)c.nonTrivialCombiners,
             (int)c.dynamicRenderingAvailable);
    if (instance && instance->d.GetPhysicalDeviceProperties) {
      VkPhysicalDeviceProperties props{};
      instance->d.GetPhysicalDeviceProperties(physicalDevice, &props);
      LOG_INFO("device name: %s (apiVersion %u.%u.%u)", props.deviceName,
               VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
               VK_API_VERSION_PATCH(props.apiVersion));
    }
  } else if (data->pendingInstance) {
    LOG_INFO("device %p capable but waiting for an OpenXR instance (%s): it will activate on the "
             "next command buffer after the instance appears; render passes, framebuffers and "
             "pipelines created before that stay unmodified",
             reinterpret_cast<void*>(*pDevice), activation.reason.c_str());
  } else {
    LOG_INFO("device %p created; layer inactive (%s)", reinterpret_cast<void*>(*pDevice),
             activation.reason.c_str());
  }
  addDevice(std::move(data));
  return VK_SUCCESS;
  VRSFOV_GUARD_END(VK_ERROR_INITIALIZATION_FAILED)
}

VKAPI_ATTR void VKAPI_CALL vrsfov_vkDestroyDevice(VkDevice device,
                                                 const VkAllocationCallbacks* pAllocator) {
  VRSFOV_GUARD_BEGIN
  std::unique_ptr<DeviceData> data = removeDevice(device);
  if (data) {
    // Free our rate images and pattern buffers before the device goes away.
    if (data->d.DestroyDevice) {
      data->res.destroyAll(data->d, device);
      data->d.DestroyDevice(device, pAllocator);
    } else {
      data->res.destroyAll(data->d, device);
    }
  } else {
    LOG_WARN("vkDestroyDevice: no device data for %p", reinterpret_cast<void*>(device));
  }
  VRSFOV_GUARD_END_VOID
}

}  // namespace vrsfov
