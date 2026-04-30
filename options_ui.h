#pragma once

// Options screen: reached from the main menu, currently exposes a
// single toggle for the cartoon-outline post-process used by
// gameplay. Future settings would live here too.

// hover: 0=none, 1=back, 2=outline, 3=continuous-voice, 4=chessnut.
// *_supported flags hide the row entirely on platforms where it
// can't work (e.g. continuous-voice on Firefox web, chessnut on
// the web build).
void renderer_draw_options(bool cartoon_outline_enabled,
                           bool voice_continuous_enabled,
                           bool continuous_voice_supported,
                           bool chessnut_enabled,
                           bool chessnut_supported,
                           int width, int height,
                           int hover);

// Returns 0=none, 1=back, 2=outline, 3=continuous-voice, 4=chessnut
// based on mouse position. Pass *_supported=false to skip the
// corresponding hit zone.
int options_hit_test(double mx, double my, int width, int height,
                     bool continuous_voice_supported,
                     bool chessnut_supported);
