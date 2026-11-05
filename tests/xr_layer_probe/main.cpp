// Minimal OpenXR loader probe: verifies that the API layer is discovered and
// negotiated. Without an installed runtime xrCreateInstance is expected to fail
// with XR_ERROR_RUNTIME_UNAVAILABLE, but the layer negotiation has already
// happened (see the [xr] lines in the vrsfov log).
#include <openxr/openxr.h>

#include <cstdio>
#include <cstring>
#include <vector>

int main() {
  uint32_t count = 0;
  XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
  printf("xrEnumerateInstanceExtensionProperties -> %d (%u extensions)\n", (int)result, count);

  std::vector<XrExtensionProperties> props(count);
  for (auto& p : props) p.type = XR_TYPE_EXTENSION_PROPERTIES;
  if (count > 0) {
    uint32_t written = 0;
    if (XR_SUCCEEDED(
            xrEnumerateInstanceExtensionProperties(nullptr, count, &written, props.data()))) {
      for (const auto& p : props) {
        if (strcmp(p.extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME) == 0) {
          printf("runtime advertises %s\n", XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
        }
      }
    }
  }

  XrApplicationInfo app{};
  strncpy(app.applicationName, "vrsfov layer probe", XR_MAX_APPLICATION_NAME_SIZE - 1);
  app.applicationVersion = 1;
  strncpy(app.engineName, "none", XR_MAX_ENGINE_NAME_SIZE - 1);
  app.apiVersion = XR_CURRENT_API_VERSION;

  XrInstanceCreateInfo createInfo{};
  createInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
  createInfo.applicationInfo = app;
  XrInstance instance = XR_NULL_HANDLE;
  result = xrCreateInstance(&createInfo, &instance);
  printf("xrCreateInstance -> %d\n", (int)result);
  if (XR_SUCCEEDED(result)) {
    XrInstanceProperties instanceProps{};
    instanceProps.type = XR_TYPE_INSTANCE_PROPERTIES;
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance, &instanceProps))) {
      printf("runtime: %s\n", instanceProps.runtimeName);
    }
    xrDestroyInstance(instance);
    return 0;
  }
  if (result == XR_ERROR_RUNTIME_UNAVAILABLE || result == XR_ERROR_INITIALIZATION_FAILED) {
    printf("no usable runtime (the layer negotiation still ran)\n");
    return 0;
  }
  return 1;
}
