#pragma once

namespace nhl3on3 {

// Applies aspect_ratio / render_quality to the SDK's video mode and draw-scale
// cvars. Must run before the guest first queries the video mode.
void ConfigureDisplay();

// Output aspect when it isn't 16:9 (ultrawide or portrait), else 0.
float UltrawideAspect();

// latency_probe: a controller state read just happened.
void NotePadRead();

// Wraps the GPU command processor's draw and swap (nhl3on3_widescreen.cpp).
// Call once the guest GPU is set up.
void InstallDrawHooks();

}  // namespace nhl3on3
