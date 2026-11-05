# Unit tests: pure logic only, no Vulkan/OpenXR calls.
set(VRSFOV_TEST_COMMON_SOURCES
  ${CMAKE_SOURCE_DIR}/src/common/config.cpp
  ${CMAKE_SOURCE_DIR}/src/common/log.cpp
  ${CMAKE_SOURCE_DIR}/src/common/shared_state.cpp
  ${CMAKE_SOURCE_DIR}/src/common/pattern.cpp
  ${CMAKE_SOURCE_DIR}/src/common/classify.cpp
  ${CMAKE_SOURCE_DIR}/src/common/gaze_projection.cpp
  ${CMAKE_SOURCE_DIR}/src/common/overlay_texture.cpp
  ${CMAKE_SOURCE_DIR}/src/common/renderpass_convert.cpp
  ${CMAKE_SOURCE_DIR}/src/common/spirv_scan.cpp)
if(NOT VRSFOV_FRAME)
  list(APPEND VRSFOV_TEST_COMMON_SOURCES ${CMAKE_SOURCE_DIR}/src/openxr/xr_pure.cpp)
endif()

set(VRSFOV_UNIT_SOURCES
  unit/test_main.cpp
  unit/test_config.cpp
  unit/test_pattern.cpp
  unit/test_classify.cpp
  unit/test_gaze_projection.cpp
  unit/test_gaze_math.cpp
  unit/test_gaze_shm.cpp
  unit/test_overlay_texture.cpp
  unit/test_renderpass_convert.cpp
  unit/test_spirv_scan.cpp)
if(NOT VRSFOV_FRAME)
  list(APPEND VRSFOV_UNIT_SOURCES unit/test_xr_pure.cpp)
endif()

add_executable(vrsfov_unit_tests ${VRSFOV_UNIT_SOURCES} ${VRSFOV_TEST_COMMON_SOURCES})
target_include_directories(vrsfov_unit_tests PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${VRSFOV_VULKAN_INCLUDE_DIR})
if(NOT VRSFOV_FRAME)
  target_include_directories(vrsfov_unit_tests PRIVATE ${VRSFOV_OPENXR_INCLUDE_DIR})
endif()
target_compile_options(vrsfov_unit_tests PRIVATE -Wall -Wextra)

add_test(NAME unit COMMAND vrsfov_unit_tests)
set_tests_properties(unit PROPERTIES
  ENVIRONMENT "XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-state;XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config")

# ---------------------------------------------------------------------------
# GPU end-to-end test: renders with the layer injected and checks the rate map.
# ---------------------------------------------------------------------------
if(VRSFOV_BUILD_GPU_TESTS)
  add_executable(vk_vrs_test vk_vrs_test/main.cpp)
  target_include_directories(vk_vrs_test PRIVATE
    ${VRSFOV_VULKAN_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/vk_vrs_test)
  target_compile_options(vk_vrs_test PRIVATE -Wall -Wextra)

  # The test app (not the layer!) links the Vulkan loader.
  find_package(PkgConfig QUIET)
  set(VRSFOV_LOADER_FOUND FALSE)
  if(PkgConfig_FOUND)
    pkg_check_modules(VULKAN_LOADER QUIET vulkan)
  endif()
  if(VULKAN_LOADER_FOUND)
    target_link_libraries(vk_vrs_test PRIVATE ${VULKAN_LOADER_LIBRARIES})
    target_include_directories(vk_vrs_test PRIVATE ${VULKAN_LOADER_INCLUDE_DIRS})
    target_link_directories(vk_vrs_test PRIVATE ${VULKAN_LOADER_LIBRARY_DIRS})
    set(VRSFOV_LOADER_FOUND TRUE)
  else()
    find_library(VRSFOV_VULKAN_LIBRARY NAMES vulkan vulkan-1)
    if(VRSFOV_VULKAN_LIBRARY)
      target_link_libraries(vk_vrs_test PRIVATE ${VRSFOV_VULKAN_LIBRARY})
      set(VRSFOV_LOADER_FOUND TRUE)
    endif()
  endif()
  if(NOT VRSFOV_LOADER_FOUND)
    message(WARNING "Vulkan loader not found; the GPU tests will not run")
  endif()

  # ---------------------------------------------------------------------------
  # Loader-protocol probe for the Vulkan layer: a layer must provide
  # vkCreateInstance for any instance argument, or the loader drops it and fails
  # instance creation (the Proton crash).
  # ---------------------------------------------------------------------------
  add_executable(vk_layer_probe vk_layer_probe/main.cpp)
  target_include_directories(vk_layer_probe PRIVATE ${VRSFOV_VULKAN_INCLUDE_DIR})
  target_compile_options(vk_layer_probe PRIVATE -Wall -Wextra)
  target_link_libraries(vk_layer_probe PRIVATE dl)
  add_test(NAME vk_layer_probe COMMAND vk_layer_probe ${CMAKE_BINARY_DIR}/libvrsfov.so)

  if(NOT VRSFOV_FRAME)
  # ---------------------------------------------------------------------------
  # OpenXR layer probe: loads the API layer through the OpenXR loader. It does
  # not need a runtime (negotiation happens first).
  # ---------------------------------------------------------------------------
  find_library(VRSFOV_OPENXR_LOADER_LIBRARY NAMES openxr_loader)
  if(VRSFOV_OPENXR_LOADER_LIBRARY)
    add_executable(xr_layer_probe xr_layer_probe/main.cpp)
    target_include_directories(xr_layer_probe PRIVATE ${VRSFOV_OPENXR_INCLUDE_DIR})
    target_compile_options(xr_layer_probe PRIVATE -Wall -Wextra)
    target_link_libraries(xr_layer_probe PRIVATE ${VRSFOV_OPENXR_LOADER_LIBRARY})
    add_test(NAME xr_layer_probe COMMAND xr_layer_probe)
    set_tests_properties(xr_layer_probe PROPERTIES
      ENVIRONMENT
        "XR_API_LAYER_PATH=${VRSFOV_XR_MANIFEST_DIR};VRSFOV_ENABLE=1;XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-state")
  else()
    message(WARNING "libopenxr_loader not found; the OpenXR probe will not run")
  endif()

  # ---------------------------------------------------------------------------
  # End-to-end: a mock OpenXR runtime drives our OpenXR half, and the Vulkan half
  # (loaded by the Vulkan loader) must foveate at the gaze it published.
  # ---------------------------------------------------------------------------
  if(VRSFOV_LOADER_FOUND)
    add_executable(xr_vk_e2e xr_vk_e2e/main.cpp)
    target_include_directories(xr_vk_e2e PRIVATE
      ${VRSFOV_VULKAN_INCLUDE_DIR}
      ${VRSFOV_OPENXR_INCLUDE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/vk_vrs_test)
    target_compile_options(xr_vk_e2e PRIVATE -Wall -Wextra)
    target_link_libraries(xr_vk_e2e PRIVATE ${VULKAN_LOADER_LIBRARIES} dl)
    if(VULKAN_LOADER_LIBRARY_DIRS)
      target_link_directories(xr_vk_e2e PRIVATE ${VULKAN_LOADER_LIBRARY_DIRS})
    endif()
    add_test(NAME xr_vk_e2e COMMAND xr_vk_e2e)
    set_tests_properties(xr_vk_e2e PROPERTIES
      ENVIRONMENT
        "VK_LAYER_PATH=${VRSFOV_VK_MANIFEST_DIR};VK_INSTANCE_LAYERS=VK_LAYER_VRSFOV_vrs;VRSFOV_ENABLE=1;XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-state;XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config;VRSFOV_INNER_RADIUS=0.15;VRSFOV_OUTER_RADIUS=0.30;VRSFOV_FIXED_INNER_RADIUS=0.15;VRSFOV_FIXED_OUTER_RADIUS=0.30")
    # Same test with a runtime that has no eye gaze: fixed foveation fallback.
    add_test(NAME xr_vk_e2e_no_eye_gaze COMMAND xr_vk_e2e)
    set_tests_properties(xr_vk_e2e_no_eye_gaze PROPERTIES
      ENVIRONMENT
        "VK_LAYER_PATH=${VRSFOV_VK_MANIFEST_DIR};VK_INSTANCE_LAYERS=VK_LAYER_VRSFOV_vrs;VRSFOV_ENABLE=1;VRSFOV_E2E_NO_EYE_GAZE=1;XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-state;XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config;VRSFOV_INNER_RADIUS=0.15;VRSFOV_OUTER_RADIUS=0.30;VRSFOV_FIXED_INNER_RADIUS=0.15;VRSFOV_FIXED_OUTER_RADIUS=0.30")
    # Vulkan device before the XrInstance (as VTOL VR initialises): the layer must
    # activate late and still foveate at the gaze the runtime reports.
    add_test(NAME xr_vk_e2e_device_first COMMAND xr_vk_e2e)
    set_tests_properties(xr_vk_e2e_device_first PROPERTIES
      ENVIRONMENT
        "VK_LAYER_PATH=${VRSFOV_VK_MANIFEST_DIR};VK_INSTANCE_LAYERS=VK_LAYER_VRSFOV_vrs;VRSFOV_ENABLE=1;VRSFOV_E2E_VK_DEVICE_FIRST=1;XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-state;XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config;VRSFOV_INNER_RADIUS=0.15;VRSFOV_OUTER_RADIUS=0.30;VRSFOV_FIXED_INNER_RADIUS=0.15;VRSFOV_FIXED_OUTER_RADIUS=0.30")
    # The app never suggests /interaction_profiles/ext/eye_gaze_interaction: the
    # layer must still bind its action and follow the runtime's gaze.
    add_test(NAME xr_vk_e2e_no_app_gaze_suggest COMMAND xr_vk_e2e)
    set_tests_properties(xr_vk_e2e_no_app_gaze_suggest PROPERTIES
      ENVIRONMENT
        "VK_LAYER_PATH=${VRSFOV_VK_MANIFEST_DIR};VK_INSTANCE_LAYERS=VK_LAYER_VRSFOV_vrs;VRSFOV_ENABLE=1;VRSFOV_E2E_NO_APP_GAZE_SUGGEST=1;XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-state;XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config;VRSFOV_INNER_RADIUS=0.15;VRSFOV_OUTER_RADIUS=0.30;VRSFOV_FIXED_INNER_RADIUS=0.15;VRSFOV_FIXED_OUTER_RADIUS=0.30")
    # The app owns /user/eyes_ext/input/gaze_ext/pose: OpenXR forbids a second
    # binding for that path, so the layer must drive the app's action.
    add_test(NAME xr_vk_e2e_app_owns_gaze_binding COMMAND xr_vk_e2e)
    set_tests_properties(xr_vk_e2e_app_owns_gaze_binding PROPERTIES
      ENVIRONMENT
        "VK_LAYER_PATH=${VRSFOV_VK_MANIFEST_DIR};VK_INSTANCE_LAYERS=VK_LAYER_VRSFOV_vrs;VRSFOV_ENABLE=1;VRSFOV_E2E_APP_OWNS_GAZE_BINDING=1;XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-state;XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config;VRSFOV_INNER_RADIUS=0.15;VRSFOV_OUTER_RADIUS=0.30;VRSFOV_FIXED_INNER_RADIUS=0.15;VRSFOV_FIXED_OUTER_RADIUS=0.30")
  endif()
  endif()


  # Layer loading is opt-in and configured through the environment. The layer
  # must be findable by the loader: either installed (scripts/install-pc.sh) or via
  # VK_LAYER_PATH pointing at the build directory.
  set(VRSFOV_GPU_TEST_ENV
    "VK_LAYER_PATH=${VRSFOV_VK_MANIFEST_DIR}"
    "VK_INSTANCE_LAYERS=VK_LAYER_VRSFOV_vrs"
    "VRSFOV_ENABLE=1"
    "VRSFOV_FORCE_ACTIVE=1"
    "VRSFOV_FAKE_EYE=1024x1024"
    "XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-state")
  foreach(mode dynamic rp1 rp2 multiview imageless nonEyeSize)
    add_test(NAME vk_vrs_test_${mode} COMMAND vk_vrs_test ${mode})
    set_tests_properties(vk_vrs_test_${mode} PROPERTIES ENVIRONMENT "${VRSFOV_GPU_TEST_ENV}")
  endforeach()

  # Standalone fixed foveation for apps without OpenXR (OpenVR games, plain
  # Vulkan apps): no FORCE_ACTIVE, no FAKE_EYE, just the config.
  set(VRSFOV_STANDALONE_ENV
    "VK_LAYER_PATH=${VRSFOV_VK_MANIFEST_DIR}"
    "VK_INSTANCE_LAYERS=VK_LAYER_VRSFOV_vrs"
    "VRSFOV_ENABLE=1"
    "VRSFOV_STANDALONE=1"
    "VRSFOV_EYE_SIZE=1024x1024"
    "VRSFOV_FIXED_INNER_RADIUS=0.25"
    "VRSFOV_FIXED_OUTER_RADIUS=0.45"
    "XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-state"
    "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/test-config")

  add_test(NAME vk_vrs_test_standalone_env COMMAND vk_vrs_test dynamic)
  set_tests_properties(vk_vrs_test_standalone_env PROPERTIES ENVIRONMENT "${VRSFOV_STANDALONE_ENV}")

  # draw_radii: the debug circles must appear in the rate map as a rate that is
  # neither the fovea rate nor the outer rate (the same path that puts the
  # circles in a game's eye image).
  set(VRSFOV_STANDALONE_RING_ENV ${VRSFOV_STANDALONE_ENV}
    "VRSFOV_DRAW_RADII=both" "VRSFOV_RING_WIDTH=2" "VRSFOV_EXPECT_RING=1")
  add_test(NAME vk_vrs_test_draw_radii COMMAND vk_vrs_test dynamic)
  set_tests_properties(vk_vrs_test_draw_radii PROPERTIES ENVIRONMENT "${VRSFOV_STANDALONE_RING_ENV}")

  # Same through the config file, in an [exe:vk_vrs_test] section, so the
  # per-executable sections and the Vulkan half's own config loading are covered.
  set(VRSFOV_STANDALONE_CONFIG_HOME ${CMAKE_BINARY_DIR}/test-config-standalone)
  file(MAKE_DIRECTORY ${VRSFOV_STANDALONE_CONFIG_HOME}/vrsfov)
  file(WRITE ${VRSFOV_STANDALONE_CONFIG_HOME}/vrsfov/settings.ini
    "; written by CMake for vk_vrs_test_standalone_file\n"
    "[global]\n"
    "enabled = 1\n"
    "[exe:vk_vrs_test]\n"
    "standalone = 1\n"
    "eye_size = 1024x1024\n"
    "fixed_inner_radius = 0.25\n"
    "fixed_outer_radius = 0.45\n")
  # Hot reload of the standalone config while a Vulkan-only app renders.
  add_test(NAME standalone_hot_reload
    COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/vk_vrs_test/standalone_hot_reload.sh)
  set_tests_properties(standalone_hot_reload PROPERTIES TIMEOUT 120
    ENVIRONMENT "VRSFOV_TEST_BIN=${CMAKE_BINARY_DIR}/tests/vk_vrs_test;VK_LAYER_PATH=${VRSFOV_VK_MANIFEST_DIR};VK_INSTANCE_LAYERS=VK_LAYER_VRSFOV_vrs")

  add_test(NAME vk_vrs_test_standalone_file COMMAND vk_vrs_test dynamic)
  set_tests_properties(vk_vrs_test_standalone_file PROPERTIES
    ENVIRONMENT
      "VK_LAYER_PATH=${VRSFOV_VK_MANIFEST_DIR};VK_INSTANCE_LAYERS=VK_LAYER_VRSFOV_vrs;VRSFOV_ENABLE=1;XDG_STATE_HOME=${CMAKE_BINARY_DIR}/test-state;XDG_CONFIG_HOME=${VRSFOV_STANDALONE_CONFIG_HOME}")
endif()
