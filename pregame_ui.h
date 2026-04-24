#pragma once

#include "time_control.h"

// Pre-game setup screen: side toggle + time-control dropdown +
// Stockfish ELO slider. tc_hover: -2 = head hovered, -1 = none,
// 0..TC_COUNT-1 = row hovered (only meaningful when dropdown_open).
void renderer_draw_pregame(bool human_plays_white,
                           int elo, int elo_min, int elo_max,
                           TimeControl time_control,
                           bool dropdown_open,
                           int tc_hover,
                           int width, int height,
                           int hover);

// Returns 0=none, 1=Start, 2=Back, 3=Toggle button, 4=Slider area,
// 5=Dropdown head, 6=Dropdown row. If the return is 6 and
// out_tc_index is non-null, writes the hovered row index there.
int pregame_hit_test(double mx, double my, int width, int height,
                     bool dropdown_open, int* out_tc_index);
