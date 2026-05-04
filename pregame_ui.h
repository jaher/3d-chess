#pragma once

#include "time_control.h"

// Pre-game setup screen: side toggle + time-control dropdown +
// (optional) Stockfish ELO slider. tc_hover: -2 = head hovered,
// -1 = none, 0..TC_COUNT-1 = row hovered. `hide_elo_slider` is
// true for two-player (Chessnut hot-seat) mode where there's no
// AI opponent to choose a strength for.
//
// game_count is the currently-selected number of parallel games
// (1..4). game_count_hover is the [1][2][3][4] row hover index
// (0 = none, 1..4 = button hovered). hide_game_count is true when
// the row should not be drawn / hit-tested (2-player or chessnut
// path forces single board).
void renderer_draw_pregame(bool human_plays_white,
                           int elo, int elo_min, int elo_max,
                           TimeControl time_control,
                           bool dropdown_open,
                           int tc_hover,
                           bool hide_elo_slider,
                           int game_count,
                           int game_count_hover,
                           bool hide_game_count,
                           int width, int height,
                           int hover);

// Returns 0=none, 1=Start, 2=Back, 3=Toggle button, 4=Slider area,
// 5=Dropdown head, 6=Dropdown row, 7..10=Games-count button (N = code-6).
// Hit-tests on the ELO slider region only when hide_elo_slider is
// false; on the games-count row only when hide_game_count is false.
int pregame_hit_test(double mx, double my, int width, int height,
                     bool dropdown_open,
                     bool hide_elo_slider,
                     bool hide_game_count,
                     int* out_tc_index);
