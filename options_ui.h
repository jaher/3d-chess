#pragma once

// Options screen: reached from the main menu, currently exposes a
// single toggle for the cartoon-outline post-process used by
// gameplay. Future settings would live here too.

void renderer_draw_options(bool cartoon_outline_enabled,
                           int width, int height,
                           int hover);  // 0=none, 1=back, 2=outline toggle

// Returns 0=none, 1=back, 2=outline toggle based on mouse position.
int options_hit_test(double mx, double my, int width, int height);
