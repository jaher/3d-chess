#pragma once

#include "time_control.h"

// Pre-game setup screen: side toggle + time-control dropdown +
// (optional) Stockfish ELO slider. tc_hover: -2 = head hovered,
// -1 = none, 0..TC_COUNT-1 = row hovered. `hide_elo_slider` is
// true for two-player (Chessnut hot-seat) mode where there's no
// AI opponent to choose a strength for.
void renderer_draw_pregame(bool human_plays_white,
                           int elo, int elo_min, int elo_max,
                           TimeControl time_control,
                           bool dropdown_open,
                           int tc_hover,
                           bool hide_elo_slider,
                           int width, int height,
                           int hover);

// Returns 0=none, 1=Start, 2=Back, 3=Toggle button, 4=Slider area,
// 5=Dropdown head, 6=Dropdown row. Hit-tests on the ELO slider
// region only when hide_elo_slider is false.
int pregame_hit_test(double mx, double my, int width, int height,
                     bool dropdown_open, bool hide_elo_slider,
                     int* out_tc_index);
