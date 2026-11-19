// OpenVR gaze source for the out-of-process vrsfov-gaze publisher.
//
// Some desktop runtimes are inert on the OpenXR eye-gaze path even though the
// driver does have eye tracking: SteamVR has been observed reporting
// supportsEyeGazeInteraction, accepting and attaching the gaze action set, and
// then never activating the action. The same runtime exposes eye tracking
// through OpenVR's own API (IVRSystem::GetEyeTrackedFoveationCenter).
//
// This API is used only by the separate vrsfov-gaze process. The Vulkan layer
// reads the publisher's /dev/shm record and never creates an OpenVR client in
// the game's process.
#pragma once

#include <cstdint>
#include <string>

namespace vrsfov {

// Loads libopenvr_api.so, initialises an OpenVR client and probes the runtime
// once, filling `report` with everything that was found. Returns false when the
// library or the interface is unavailable. Only safe in the publisher process:
// this creates a SteamVR client.
bool openvrGazePrepare(std::string& report);

// Per-eye eye-tracked foveation centre as eye-image UV. False when the runtime
// has no centre for this client (eye tracking off, or not the VR app).
bool openvrGazeRead(float& u0, float& v0, float& u1, float& v1);

// Runtime-recommended per-eye render target size. False when the client is not
// connected or the runtime returns no usable size.
bool openvrGazeRecommendedSize(uint32_t& width, uint32_t& height);

// Tears down the publisher's own OpenVR client. Never called by the layer.
void openvrGazeShutdown();

}  // namespace vrsfov
