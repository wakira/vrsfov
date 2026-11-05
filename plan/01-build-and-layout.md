# 01 — Repository layout, build, manifests, install

## 1. Layout

```
vrsfov/
  CMakeLists.txt
  exports.map                      # linker version script
  manifests/
    XrApiLayer_vrsfov.json.in  # OpenXR implicit layer
    VkLayer_vrsfov.json.in     # Vulkan implicit layer
  src/
    common/
      log.h / log.cpp              # leveled logging to file + stderr
      config.h / config.cpp        # INI parse, env overrides, hot reload
      shared_state.h / .cpp        # Snapshot publish/consume (see 02)
      xr_math.h                    # vec3/quat helpers (header-only)
      gaze_projection.h / .cpp     # gaze dir + eye pose/fov -> uv (pure, unit tested)
      pattern.h / pattern.cpp      # rate encoding, pattern buffer generation (pure)
      classify.h / classify.cpp    # eye-target heuristics -> FoveationPlan (pure)
    openxr/
      xr_layer.cpp                 # negotiation, xrGetInstanceProcAddr, create instance
      xr_dispatch.h                # next-function pointers per XrInstance
      xr_session.cpp               # session, swapchains, frame loop hooks
      xr_gaze.cpp                  # action set, bindings, attach/sync merge, locate
    vulkan/
      vk_layer.cpp                 # negotiation, GIPA/GDPA, instance create/destroy
      vk_dispatch.h / .cpp         # dispatch tables, key->data maps
      vk_device.cpp                # device create/destroy, capability detection
      vk_resources.h / .cpp        # S images, neutral image, pattern buffers, memory
      vk_renderpass.cpp            # v1->v2 conversion, FSR attachment injection
      vk_framebuffer.cpp           # framebuffer + image view tracking
      vk_pipeline.cpp              # pipeline combiner/flag injection, SPIR-V scan
      vk_commands.cpp              # BeginCommandBuffer, BeginRenderPass(2), BeginRendering
      renderpass_convert.h / .cpp  # pure v1->v2 conversion with owned storage (unit tested)
      spirv_scan.h / .cpp          # detect FragDepth/SampleMask/etc. (unit tested)
  tests/
    unit/                          # pure-function tests (doctest single header or plain asserts)
    vk_vrs_test/                   # GPU end-to-end test app (see 07)
  scripts/
    run-pc.sh / run-frame.sh
  README.md
```

## 2. Dependencies

- CMake >= 3.16, C++17 compiler (gcc >= 9 or clang >= 10).
- Headers only, fetched with `FetchContent` (pin tags):
  - `KhronosGroup/Vulkan-Headers` tag `v1.3.280` or newer (needs `VK_KHR_dynamic_rendering`,
    `VkRenderingFragmentShadingRateAttachmentInfoKHR`, `VkPipelineCreateFlags2CreateInfoKHR`).
  - `KhronosGroup/OpenXR-SDK` tag `release-1.0.34` or newer. Use `include/openxr/openxr.h`,
    `openxr_platform.h`, `openxr_loader_negotiation.h`. Do NOT build the loader
    (`FetchContent_Populate` + include dir only).
  - Allow `-DVRSFOV_USE_SYSTEM_HEADERS=ON` to use system headers instead.
- Link: `dl`, `pthread` only.
- Before including `openxr_platform.h`: `#include <vulkan/vulkan.h>` and
  `#define XR_USE_GRAPHICS_API_VULKAN`.

## 3. CMake essentials

```cmake
add_library(vrsfov SHARED ${SOURCES})
target_compile_features(vrsfov PRIVATE cxx_std_17)
set_target_properties(vrsfov PROPERTIES CXX_VISIBILITY_PRESET hidden
                                            VISIBILITY_INLINES_HIDDEN ON)
target_link_options(vrsfov PRIVATE
   -Wl,--version-script=${CMAKE_SOURCE_DIR}/exports.map -Wl,-Bsymbolic -Wl,--no-undefined)
target_link_libraries(vrsfov PRIVATE dl pthread)
# configure_file the two manifests with @VRSFOV_LIBRARY_PATH@ = absolute install path
# add_subdirectory(tests) when VRSFOV_BUILD_TESTS=ON (default ON); enable_testing()
```

`exports.map`:
```
{ global: xrNegotiateLoaderApiLayerInterface; vkNegotiateLoaderLayerInterfaceVersion;
  local: *; };
```
Compile with `-Wall -Wextra`. Keep C++ exceptions enabled, but **no exception may escape a C
entry point**. Every hook body is wrapped with macros
`VRSFOV_GUARD_BEGIN` / `VRSFOV_GUARD_END(errValue)` that expand to
`try { ... } catch (const std::exception& e) { LOG_ERROR(...); return errValue; } catch (...) {...}`.
For `void` Vulkan commands (vkCmd*), on internal error log once and fall through to calling the
next layer with the *original* arguments (fail open: the app must keep working).

## 4. Manifests

`XrApiLayer_vrsfov.json.in`
```json
{
  "file_format_version": "1.0.0",
  "api_layer": {
    "name": "XR_APILAYER_VRSFOV_foveated_rendering",
    "library_path": "@VRSFOV_LIBRARY_PATH@",
    "api_version": "1.0",
    "implementation_version": "1",
    "description": "VRS-based foveated rendering for Vulkan apps",
    "disable_environment": "VRSFOV_DISABLE",
    "enable_environment": "VRSFOV_ENABLE"
  }
}
```

`VkLayer_vrsfov.json.in`
```json
{
  "file_format_version": "1.2.0",
  "layer": {
    "name": "VK_LAYER_VRSFOV_vrs",
    "type": "GLOBAL",
    "library_path": "@VRSFOV_LIBRARY_PATH@",
    "api_version": "1.3.280",
    "implementation_version": "1",
    "description": "VRS injection for VRSFov",
    "enable_environment": { "VRSFOV_ENABLE": "1" },
    "disable_environment": { "VRSFOV_DISABLE": "1" }
  }
}
```

Both `library_path` values MUST be the identical absolute path (shared globals rely on it).

## 5. Run locations (no install)

The build tree is the install. The manifests stay under
`build-pc/manifests/{vulkan,openxr}` (or `build-frame/manifests/vulkan`), and
`scripts/run-pc.sh` / `run-frame.sh` set `VK_LAYER_PATH`, `VK_INSTANCE_LAYERS`,
`XR_API_LAYER_PATH`, `XR_ENABLE_API_LAYERS` and `VRSFOV_ENABLE`.
The config is `~/.config/vrsfov/settings.ini`; the log is
`~/.local/state/vrsfov/vrsfov.log`. Nothing is written to `/usr`.

Usage: `VRSFOV_ENABLE=1 ./app` or Steam launch options `VRSFOV_ENABLE=1 %command%`.
