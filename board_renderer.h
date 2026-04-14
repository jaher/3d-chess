#pragma once

#include "chess_types.h"
#include "stl_model.h"

#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#else
#include <epoxy/gl.h>
#endif

// GPU-resident per-piece mesh handles. Moved out of chess_types.h so
// that the pure-logic layer doesn't have to pull in a GL header.
struct PieceGPU {
    GLuint vao = 0, vbo = 0;
    int num_vertices = 0;
};

// Initialize GL resources (call from on_realize)
void renderer_init(StlModel loaded_models[PIECE_COUNT]);

// Main render function (call from on_render).
// human_plays_white flips the score graph so the human's color is
// always at the bottom (light fill at bottom for white player, dark
// fill at bottom for black player).
// endgame_menu_hover controls the "Back to Menu" button's hover tint
// in the game-over overlay; it's ignored when !gs.game_over.
void renderer_draw(GameState& gs, int width, int height,
                   float rot_x, float rot_y, float zoom,
                   bool human_plays_white,
                   bool endgame_menu_hover);

// Hit-test for the "Back to Menu" button drawn in renderer_draw's
// game-over overlay. Returns true when (mx, my) falls inside the
// button's NDC rectangle. Only meaningful while gs.game_over is true.
bool endgame_menu_button_hit_test(double mx, double my,
                                  int width, int height);

// Menu screen
struct PhysicsPiece {
    PieceType type;
    float x, y, z;
    float vx, vy, vz;
    float rot_x, rot_y, rot_z;
    float spin_x, spin_y, spin_z;
    float scale;
};

void menu_init_physics(std::vector<PhysicsPiece>& pieces);
void menu_update_physics(std::vector<PhysicsPiece>& pieces, float dt);
void renderer_draw_menu(const std::vector<PhysicsPiece>& pieces,
                        int width, int height, float time,
                        int hover_button); // 0=none, 1=start, 2=quit

// Returns 0=none, 1=start, 2=quit, 3=challenges based on mouse position
int menu_hit_test(double mx, double my, int width, int height);

// Challenge select screen
void renderer_draw_challenge_select(const std::vector<std::string>& challenge_names,
                                    int width, int height, int hover_index);
// Returns -2=back, -1=none, 0..N-1=challenge index
int challenge_select_hit_test(double mx, double my, int width, int height, int num_challenges);

// Pre-game setup screen: side toggle + Stockfish ELO slider.
void renderer_draw_pregame(bool human_plays_white,
                           int elo, int elo_min, int elo_max,
                           int width, int height,
                           int hover);
// Returns 0=none, 1=Start, 2=Back, 3=Toggle button, 4=Slider area
int pregame_hit_test(double mx, double my, int width, int height);

// Challenge in-game overlay
void renderer_draw_challenge_overlay(const std::string& challenge_name,
                                     int puzzle_index, int total_puzzles,
                                     int moves_made, int max_moves,
                                     bool starts_white,
                                     int width, int height);

// Next puzzle button (drawn when challenge solved). Returns true if hit.
void renderer_draw_next_button(int width, int height, bool hover);
bool next_button_hit_test(double mx, double my, int width, int height);

// Glass shatter transition: capture the current frame then animate shards
void renderer_capture_frame(int width, int height);
void renderer_draw_shatter(float t, int width, int height);

// Summary table at end of challenge — shows user's solutions
struct SummaryEntry {
    std::string puzzle_name;
    std::vector<std::string> moves; // algebraic notation per move
};
void renderer_draw_challenge_summary(const std::string& challenge_name,
                                     const std::vector<SummaryEntry>& entries,
                                     int width, int height);
