#pragma once

#include <cstdint>

// OpenXR headset output (Windows, Direct3D 12). Matches render in true 3D by
// alternating eyes: every other frame is drawn from the other eye's tracked
// pose, so each eye gets half the frame rate. Menus (frames without the match
// camera) show on a flat virtual screen.

namespace nhl3on3::vr {

// The vr setting is on (Windows only).
bool Enabled();

// One eye, as tracked in the headset's seated space: position in metres,
// orientation quaternion (x, y, z, w), and the symmetric view window the eye
// is drawn with (tangents of the half field of view).
struct EyeView {
  float position[3];
  float orientation[4];
  float tan_x, tan_y;
};

// Placement of the rink: metres per game unit, and the rink centre in the
// seated space.
void TableTransform(float& scale, float centre[3]);

// --- game thread ---

// The eye this frame is drawn for. False while the headset isn't tracking.
bool FrameEye(EyeView& out);

// The match camera was set from FrameEye this frame.
void NoteStereoFrame();

// End of a guest frame (VdSwap): queues the frame's eye for the GPU thread and
// picks the other eye for the next frame.
void OnGuestSwap();

// --- GPU thread ---

// Installs the presenter hook that sees each finished frame. Call once the
// guest GPU is set up.
void InstallPresenterHook();

// After the command processor has handled a swap: copies the frame to its
// eye (or to the menu screen) for the headset.
void OnHostSwap();

// 2D scale for the HUD during a stereo match (1 = not in one).
void HudSqueeze(float& x, float& y);

}  // namespace nhl3on3::vr
