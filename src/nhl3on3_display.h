#pragma once

#include <cstdint>

namespace nhl3on3 {

// Applies aspect_ratio / render_quality to the SDK's video mode and draw-scale
// cvars. Must run before the guest first queries the video mode.
void ConfigureDisplay();

// Output aspect when it isn't 16:9 (ultrawide or portrait), else 0.
float UltrawideAspect();

// latency_probe: a controller state read just happened.
void NotePadRead();

// Portrait top-down camera: turns the sticks in a guest X_INPUT_STATE so they
// move players the way they point on screen. No-op otherwise.
void RemapPadSticks(uint8_t* base, uint32_t state);

// Top-down camera roof cut: the clip-space plane (kept where dot(plane, clip)
// >= 0) for 3D scene draws. False when it's off or no top-down match is on.
bool RoofClipPlane(float plane[4]);

// Wraps the GPU command processor's draw and swap (nhl3on3_widescreen.cpp).
// Call once the guest GPU is set up.
void InstallDrawHooks();

// Factors by which 2D draws are scaled (1 = not): x about the frame centre, y
// about frame row centre_y.
void UiSqueeze(float& x, float& y, float& centre_y);

}  // namespace nhl3on3
