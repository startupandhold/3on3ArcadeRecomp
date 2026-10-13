#pragma once

namespace nhl3on3 {

// Applies aspect_ratio / render_quality to the SDK's video mode and draw-scale
// cvars. Must run before the guest first queries the video mode.
void ConfigureDisplay();

}  // namespace nhl3on3
