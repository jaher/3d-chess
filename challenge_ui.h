#pragma once

#include <string>
#include <vector>

// Challenge select screen.
void renderer_draw_challenge_select(const std::vector<std::string>& challenge_names,
                                    int width, int height, int hover_index);
// Returns -2=back, -1=none, 0..N-1=challenge index.
int challenge_select_hit_test(double mx, double my, int width, int height,
                              const std::vector<std::string>& challenge_names);

// Challenge in-game overlay (drawn on top of the regular game render
// while a challenge puzzle is in progress).
void renderer_draw_challenge_overlay(const std::string& challenge_name,
                                     int puzzle_index, int total_puzzles,
                                     int moves_made, int max_moves,
                                     bool starts_white,
                                     int width, int height);

// Next-puzzle button (drawn when a challenge puzzle is solved).
void renderer_draw_next_button(int width, int height, bool hover);
bool next_button_hit_test(double mx, double my, int width, int height);

// Try-again button (drawn when the player made a mistake on a
// mate-in-N puzzle). Click resets the puzzle to its starting FEN.
void renderer_draw_try_again_button(int width, int height, bool hover);
bool try_again_button_hit_test(double mx, double my, int width, int height);

// Summary table at the end of a challenge — shows the user's moves
// per puzzle.
struct SummaryEntry {
    std::string puzzle_name;
    std::vector<std::string> moves;  // algebraic notation per move
};
void renderer_draw_challenge_summary(const std::string& challenge_name,
                                     const std::vector<SummaryEntry>& entries,
                                     int width, int height);
