#pragma once

// Options screen: reached from the main menu, currently exposes a
// single toggle for the cartoon-outline post-process used by
// gameplay. Future settings would live here too.

// hover: 0=none, 1=back, 2=outline toggle, 3=continuous-voice toggle.
// continuous_voice_supported: when false (e.g. web build) the
// continuous-voice row is not drawn at all.
void renderer_draw_options(bool cartoon_outline_enabled,
                           bool voice_continuous_enabled,
                           bool continuous_voice_supported,
                           int width, int height,
                           int hover);

// Returns 0=none, 1=back, 2=outline toggle, 3=continuous-voice
// toggle based on mouse position. Pass continuous_voice_supported =
// false to disable the third row's hit zone.
int options_hit_test(double mx, double my, int width, int height,
                     bool continuous_voice_supported);
