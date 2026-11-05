#include "openxr/xr_dispatch.h"

#include <mutex>

namespace vrsfov {

namespace {

std::mutex g_lock;
std::unordered_map<XrInstance, std::unique_ptr<XrInstanceData>> g_instances;
std::unordered_map<XrSession, std::unique_ptr<XrSessionData>> g_sessions;
std::unordered_map<XrSwapchain, std::unique_ptr<XrSwapchainData>> g_swapchains;

}  // namespace

XrInstanceData* findXrInstance(XrInstance instance) {
  if (instance == XR_NULL_HANDLE) return nullptr;
  std::lock_guard<std::mutex> guard(g_lock);
  auto it = g_instances.find(instance);
  return it == g_instances.end() ? nullptr : it->second.get();
}

void addXrInstance(std::unique_ptr<XrInstanceData> data) {
  std::lock_guard<std::mutex> guard(g_lock);
  g_instances[data->instance] = std::move(data);
}

std::unique_ptr<XrInstanceData> removeXrInstance(XrInstance instance) {
  std::lock_guard<std::mutex> guard(g_lock);
  auto it = g_instances.find(instance);
  if (it == g_instances.end()) return nullptr;
  auto out = std::move(it->second);
  g_instances.erase(it);
  return out;
}

XrSessionData* findXrSession(XrSession session) {
  if (session == XR_NULL_HANDLE) return nullptr;
  std::lock_guard<std::mutex> guard(g_lock);
  auto it = g_sessions.find(session);
  return it == g_sessions.end() ? nullptr : it->second.get();
}

void addXrSession(std::unique_ptr<XrSessionData> data) {
  std::lock_guard<std::mutex> guard(g_lock);
  g_sessions[data->session] = std::move(data);
}

std::unique_ptr<XrSessionData> removeXrSession(XrSession session) {
  std::lock_guard<std::mutex> guard(g_lock);
  auto it = g_sessions.find(session);
  if (it == g_sessions.end()) return nullptr;
  auto out = std::move(it->second);
  g_sessions.erase(it);
  return out;
}

XrSwapchainData* findXrSwapchain(XrSwapchain swapchain) {
  if (swapchain == XR_NULL_HANDLE) return nullptr;
  std::lock_guard<std::mutex> guard(g_lock);
  auto it = g_swapchains.find(swapchain);
  return it == g_swapchains.end() ? nullptr : it->second.get();
}

void addXrSwapchain(std::unique_ptr<XrSwapchainData> data) {
  std::lock_guard<std::mutex> guard(g_lock);
  g_swapchains[data->swapchain] = std::move(data);
}

std::unique_ptr<XrSwapchainData> removeXrSwapchain(XrSwapchain swapchain) {
  std::lock_guard<std::mutex> guard(g_lock);
  auto it = g_swapchains.find(swapchain);
  if (it == g_swapchains.end()) return nullptr;
  auto out = std::move(it->second);
  g_swapchains.erase(it);
  return out;
}

std::vector<XrSessionData*> xrSessionsForInstance(XrInstanceData* instance) {
  std::vector<XrSessionData*> out;
  if (!instance) return out;
  std::lock_guard<std::mutex> guard(g_lock);
  for (auto& entry : g_sessions) {
    if (entry.second->instance == instance) out.push_back(entry.second.get());
  }
  return out;
}

}  // namespace vrsfov
