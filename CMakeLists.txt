cmake_minimum_required(VERSION 3.16)

project(vrsfov VERSION 0.1.0 LANGUAGES CXX)

# VRSFov: one shared core with two targets.
#
#   -D VRSFOV_PLATFORM=pc     Vulkan layer + OpenXR API layer, direct OpenVR gaze
#   -D VRSFOV_PLATFORM=frame  Vulkan layer only, gaze from the vrsfov-gaze
#                             publisher over /dev/shm
#
# The Vulkan core and the common modules are the same code in both builds; only
# the activation default, gaze source and OpenXR half differ.

set(VRSFOV_PLATFORM "pc" CACHE STRING "Target platform: pc or frame")
set_property(CACHE VRSFOV_PLATFORM PROPERTY STRINGS pc frame)
if(NOT VRSFOV_PLATFORM STREQUAL "pc" AND NOT VRSFOV_PLATFORM STREQUAL "frame")
  message(FATAL_ERROR "VRSFOV_PLATFORM must be 'pc' or 'frame'")
endif()
set(VRSFOV_FRAME FALSE)
if(VRSFOV_PLATFORM STREQUAL "frame")
  set(VRSFOV_FRAME TRUE)
endif()

option(VRSFOV_BUILD_TESTS "Build the unit tests" ON)
option(VRSFOV_BUILD_GPU_TESTS "Build the GPU end-to-end test app" ON)
option(VRSFOV_USE_SYSTEM_HEADERS "Use system Vulkan/OpenXR headers instead of FetchContent" OFF)
# The Nix dev shell exports this, so `cmake -B build` works there as documented.
if(DEFINED ENV{VRSFOV_USE_SYSTEM_HEADERS} AND NOT VRSFOV_USE_SYSTEM_HEADERS)
  set(VRSFOV_USE_SYSTEM_HEADERS $ENV{VRSFOV_USE_SYSTEM_HEADERS} CACHE BOOL "" FORCE)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Dependencies (headers only)
# ---------------------------------------------------------------------------
if(VRSFOV_USE_SYSTEM_HEADERS)
  find_path(VRSFOV_VULKAN_INCLUDE_DIR vulkan/vulkan.h
    HINTS ENV VRSFOV_VULKAN_INCLUDE_DIR PATH_SUFFIXES include)
  # Optional: the SteamVR-native gaze source. Only the header is needed;
  # libopenvr_api.so is loaded at runtime with dlopen.
  find_path(VRSFOV_OPENVR_INCLUDE_DIR openvr/openvr.h
    HINTS ENV VRSFOV_OPENVR_INCLUDE_DIR PATH_SUFFIXES include)
  if(NOT VRSFOV_VULKAN_INCLUDE_DIR)
    message(FATAL_ERROR "vulkan/vulkan.h not found (set VRSFOV_VULKAN_INCLUDE_DIR)")
  endif()
  find_file(VRSFOV_VK_LAYER_HEADER vulkan/vk_layer.h HINTS ${VRSFOV_VULKAN_INCLUDE_DIR} NO_DEFAULT_PATH)
  if(NOT VRSFOV_VK_LAYER_HEADER)
    message(FATAL_ERROR "vulkan/vk_layer.h not found in ${VRSFOV_VULKAN_INCLUDE_DIR}")
  endif()
  if(NOT VRSFOV_FRAME)
    find_path(VRSFOV_OPENXR_INCLUDE_DIR openxr/openxr.h
      HINTS ENV VRSFOV_OPENXR_INCLUDE_DIR PATH_SUFFIXES include)
    if(NOT VRSFOV_OPENXR_INCLUDE_DIR)
      message(FATAL_ERROR "openxr/openxr.h not found (set VRSFOV_OPENXR_INCLUDE_DIR)")
    endif()
    find_file(VRSFOV_XR_NEGOTIATION_HEADER openxr/openxr_loader_negotiation.h
      HINTS ${VRSFOV_OPENXR_INCLUDE_DIR} NO_DEFAULT_PATH)
    if(NOT VRSFOV_XR_NEGOTIATION_HEADER)
      message(FATAL_ERROR "openxr/openxr_loader_negotiation.h not found in ${VRSFOV_OPENXR_INCLUDE_DIR}")
    endif()
  endif()
else()
  include(FetchContent)
  # SOURCE_SUBDIR points at a directory without a CMakeLists.txt on purpose: we
  # only want the headers, never to build the loader / test suites.
  FetchContent_Declare(vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG v1.3.280
    SOURCE_SUBDIR header-only)
  if(NOT VRSFOV_FRAME)
    FetchContent_Declare(openxr_sdk
      GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
      GIT_TAG release-1.0.34
      SOURCE_SUBDIR header-only)
    FetchContent_MakeAvailable(vulkan_headers openxr_sdk)
    set(VRSFOV_OPENXR_INCLUDE_DIR ${openxr_sdk_SOURCE_DIR}/include)
  else()
    FetchContent_MakeAvailable(vulkan_headers)
  endif()
  set(VRSFOV_VULKAN_INCLUDE_DIR ${vulkan_headers_SOURCE_DIR}/include)
endif()

find_library(VRSFOV_OPENVR_LIBRARY openvr_api
  HINTS ENV VRSFOV_OPENVR_LIBRARY_DIR ${VRSFOV_OPENVR_INCLUDE_DIR}/../lib
        ${VRSFOV_OPENVR_INCLUDE_DIR}/../../lib
  PATH_SUFFIXES lib)

if(NOT VRSFOV_OPENVR_INCLUDE_DIR)
  # Single header, pinned; downloaded rather than fetched as a project because we
  # only need the declarations (the library is resolved at runtime).
  set(VRSFOV_OPENVR_HEADER_DIR ${CMAKE_BINARY_DIR}/third_party/openvr)
  set(VRSFOV_OPENVR_HEADER ${VRSFOV_OPENVR_HEADER_DIR}/openvr/openvr.h)
  if(NOT EXISTS ${VRSFOV_OPENVR_HEADER})
    file(MAKE_DIRECTORY ${VRSFOV_OPENVR_HEADER_DIR}/openvr)
    file(DOWNLOAD
      https://raw.githubusercontent.com/ValveSoftware/openvr/v2.15.6/headers/openvr.h
      ${VRSFOV_OPENVR_HEADER} STATUS VRSFOV_OPENVR_DOWNLOAD)
    list(GET VRSFOV_OPENVR_DOWNLOAD 0 VRSFOV_OPENVR_DOWNLOAD_CODE)
    if(NOT VRSFOV_OPENVR_DOWNLOAD_CODE EQUAL 0)
      message(WARNING "could not download openvr.h; the OpenVR gaze source will be unavailable")
    endif()
  endif()
  if(EXISTS ${VRSFOV_OPENVR_HEADER})
    set(VRSFOV_OPENVR_INCLUDE_DIR ${VRSFOV_OPENVR_HEADER_DIR})
  endif()
endif()

if(NOT VRSFOV_FRAME AND NOT VRSFOV_OPENVR_INCLUDE_DIR)
  message(FATAL_ERROR "openvr/openvr.h not found (set VRSFOV_OPENVR_INCLUDE_DIR); the PC target "
                      "needs it for the OpenXR gaze fallback")
endif()
if(VRSFOV_FRAME AND NOT VRSFOV_OPENVR_INCLUDE_DIR)
  message(WARNING "OpenVR headers not found; vrsfov-gaze will not be built")
endif()

# ---------------------------------------------------------------------------
# The shared library
# ---------------------------------------------------------------------------
set(VRSFOV_COMMON_SOURCES
  src/common/log.cpp
  src/common/config.cpp
  src/common/shared_state.cpp
  src/common/pattern.cpp
  src/common/classify.cpp
  src/common/gaze_projection.cpp
  src/common/renderpass_convert.cpp
  src/common/spirv_scan.cpp)

set(VRSFOV_VULKAN_SOURCES
  src/vulkan/vk_layer.cpp
  src/vulkan/vk_dispatch.cpp
  src/vulkan/vk_device.cpp
  src/vulkan/vk_resources.cpp
  src/vulkan/vk_renderpass.cpp
  src/vulkan/vk_framebuffer.cpp
  src/vulkan/vk_pipeline.cpp
  src/vulkan/vk_commands.cpp)

set(VRSFOV_OPENXR_SOURCES
  src/openxr/xr_layer.cpp
  src/openxr/xr_dispatch.cpp
  src/openxr/xr_session.cpp
  src/openxr/xr_gaze.cpp
  src/openxr/xr_pure.cpp)

set(VRSFOV_LIB_SOURCES ${VRSFOV_COMMON_SOURCES} ${VRSFOV_VULKAN_SOURCES})
if(VRSFOV_FRAME)
  list(APPEND VRSFOV_LIB_SOURCES src/gaze/frame_gaze.cpp)
else()
  list(APPEND VRSFOV_LIB_SOURCES ${VRSFOV_OPENXR_SOURCES} src/gaze/pc_gaze.cpp)
  if(VRSFOV_OPENVR_INCLUDE_DIR)
    list(APPEND VRSFOV_LIB_SOURCES src/common/openvr_gaze.cpp)
  endif()
endif()

add_library(vrsfov SHARED ${VRSFOV_LIB_SOURCES})

target_compile_features(vrsfov PRIVATE cxx_std_17)
set_target_properties(vrsfov PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON
  OUTPUT_NAME vrsfov)

target_include_directories(vrsfov PRIVATE
  ${VRSFOV_OPENVR_INCLUDE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${VRSFOV_VULKAN_INCLUDE_DIR}
  ${VRSFOV_OPENXR_INCLUDE_DIR})

target_compile_options(vrsfov PRIVATE -Wall -Wextra)
if(VRSFOV_FRAME)
  target_compile_definitions(vrsfov PRIVATE VRSFOV_PLATFORM_FRAME=1 VRSFOV_DEFAULT_STANDALONE=1)
else()
  target_compile_definitions(vrsfov PRIVATE VRSFOV_PLATFORM_FRAME=0 VRSFOV_DEFAULT_STANDALONE=0)
endif()

target_link_options(vrsfov PRIVATE
  -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map
  -Wl,-Bsymbolic
  -Wl,--no-undefined)

target_link_libraries(vrsfov PRIVATE dl pthread)

# Per-half log tags ([xr] / [vk]).
foreach(src IN LISTS VRSFOV_OPENXR_SOURCES)
  if(NOT VRSFOV_FRAME)
    set_source_files_properties(${src} PROPERTIES COMPILE_DEFINITIONS "VRSFOV_LOG_TAG=\"xr\"")
  endif()
endforeach()
foreach(src IN LISTS VRSFOV_VULKAN_SOURCES)
  set_source_files_properties(${src} PROPERTIES COMPILE_DEFINITIONS "VRSFOV_LOG_TAG=\"vk\"")
endforeach()

# ---------------------------------------------------------------------------
# Tools
# ---------------------------------------------------------------------------
if(NOT VRSFOV_FRAME AND VRSFOV_OPENVR_INCLUDE_DIR AND VRSFOV_OPENVR_LIBRARY)
  # A separate process that draws the foveal radii as a SteamVR overlay quad.
  add_executable(vrsfov-overlay
    tools/overlay.cpp
    src/common/config.cpp
    src/common/log.cpp
    src/common/overlay_texture.cpp)
  target_include_directories(vrsfov-overlay PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${VRSFOV_OPENVR_INCLUDE_DIR})
  target_link_libraries(vrsfov-overlay PRIVATE ${VRSFOV_OPENVR_LIBRARY})
  target_compile_options(vrsfov-overlay PRIVATE -Wall -Wextra)
elseif(NOT VRSFOV_FRAME)
  message(STATUS "vrsfov-overlay not built: OpenVR headers/library not found")
endif()

if(VRSFOV_FRAME AND VRSFOV_OPENVR_INCLUDE_DIR)
  # The only component that talks to SteamVR on the Frame. The layer reads the
  # /dev/shm record this process publishes.
  add_executable(vrsfov-gaze
    tools/gaze/main.cpp
    src/common/openvr_gaze.cpp
    src/common/log.cpp)
  target_include_directories(vrsfov-gaze PRIVATE
    ${VRSFOV_OPENVR_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_compile_options(vrsfov-gaze PRIVATE -Wall -Wextra)
  target_link_libraries(vrsfov-gaze PRIVATE dl pthread)
endif()

# ---------------------------------------------------------------------------
# Manifests / install
# ---------------------------------------------------------------------------
set(VRSFOV_LIBRARY_PATH ${CMAKE_BINARY_DIR}/libvrsfov.so)
set(VRSFOV_MANIFEST_DIR ${CMAKE_BINARY_DIR}/manifests)
set(VRSFOV_VK_MANIFEST_DIR ${VRSFOV_MANIFEST_DIR}/vulkan)
file(MAKE_DIRECTORY ${VRSFOV_VK_MANIFEST_DIR})
configure_file(manifests/VkLayer_vrsfov.json.in
  ${VRSFOV_VK_MANIFEST_DIR}/VkLayer_vrsfov.json @ONLY)

if(NOT VRSFOV_FRAME)
  set(VRSFOV_XR_MANIFEST_DIR ${VRSFOV_MANIFEST_DIR}/openxr)
  file(MAKE_DIRECTORY ${VRSFOV_XR_MANIFEST_DIR})
  configure_file(manifests/XrApiLayer_vrsfov.json.in
    ${VRSFOV_XR_MANIFEST_DIR}/XrApiLayer_vrsfov.json @ONLY)
endif()

set(VRSFOV_TEST_HOME ${CMAKE_BINARY_DIR}/test-home)
file(MAKE_DIRECTORY ${VRSFOV_TEST_HOME})

include(GNUInstallDirs)
install(TARGETS vrsfov LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/vrsfov)
if(TARGET vrsfov-gaze)
  install(TARGETS vrsfov-gaze RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(VRSFOV_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

message(STATUS "vrsfov: platform: ${VRSFOV_PLATFORM}")
message(STATUS "vrsfov: Vulkan headers: ${VRSFOV_VULKAN_INCLUDE_DIR}")
if(NOT VRSFOV_FRAME)
  message(STATUS "vrsfov: OpenXR headers: ${VRSFOV_OPENXR_INCLUDE_DIR}")
endif()
