#pragma once

#include "chess_types.h"
#include "cloth_flag.h"
#include "stl_model.h"
#include "time_control.h"

#include <string>
#include <vector>

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
// in the game-over overlay; it's ignored when !gs.game_over && !gs.analysis_mode.
// continue_playing_hover controls the "Continue Playing" button's
// hover tint in the analysis overlay; ignored when !gs.analysis_mode.
// flag is the live cloth state for the withdraw flag; drawn only when
// draw_flag is true (i.e. a live game is in progress and no modal is open).
// withdraw_confirm_open draws the modal confirmation dialog on top of
// everything; withdraw_hover controls its button tints.
// draw_clock/clock_ms_remaining/clock_side_is_white drive the top-
// centre clock widget (drawn only while a timed live game is in
// progress).
void renderer_draw(GameState& gs, int width, int height,
                   float rot_x, float rot_y, float zoom,
                   bool human_plays_white,
                   bool endgame_menu_hover,
                   bool continue_playing_hover,
                   const ClothFlag* flag, bool draw_flag,
                   bool withdraw_confirm_open, int withdraw_hover,
                   bool draw_clock,
                   int64_t clock_ms_remaining,
                   bool clock_side_is_white,
                   bool cartoon_outline,
                   float shake_x = 0.0f);

// Hit-test for the "Back to Menu" button drawn in renderer_draw's
// game-over / analysis overlay. Returns true when (mx, my) falls
// inside the button's NDC rectangle. Only meaningful while the overlay
// is visible (gs.game_over or gs.analysis_mode).
bool endgame_menu_button_hit_test(double mx, double my,
                                  int width, int height);

// Hit-test for the "Continue Playing" button drawn above Back to
// Menu while gs.analysis_mode is true. Returns false in other modes.
bool analysis_continue_button_hit_test(double mx, double my,
                                       int width, int height);

// Hit-test for the withdraw flag in the bottom-right corner. Uses
// the deformed cloth's bounding box + a small NDC padding for
// ergonomics, so the click region tracks the rippling cloth.
bool flag_hit_test(const ClothFlag& flag,
                   double mx, double my, int width, int height);

// Hit-test for the withdraw confirmation modal. Writes to *which:
// 0 = background / neither button, 1 = Yes, 2 = No.
bool withdraw_confirm_hit_test(double mx, double my,
                               int width, int height, int* which);

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
// cartoon_outline=true reuses the scene post-process shader to draw a
// cartoon-style contour around the rendered pieces, used by the menu
// grab gesture to highlight which piece the cursor is holding.
void renderer_draw_menu(const std::vector<PhysicsPiece>& pieces,
                        int width, int height, float time,
                        int hover_button, // 0=none, 1=start, 2=quit
                        bool cartoon_outline = false);

// Returns 0=none, 1=start, 2=quit, 3=challenges based on mouse position
int menu_hit_test(double mx, double my, int width, int height);

// Ray-pick a menu piece under the cursor. Returns the index of the
// nearest-hit piece (the one closest to the camera), or -1 on miss.
// `time` must match the value passed to renderer_draw_menu on the
// same frame so the camera transform lines up with what's on screen.
int menu_piece_hit_test(const std::vector<PhysicsPiece>& pieces,
                        double mx, double my, int width, int height,
                        float time);

// Apply a drag-to-fling impulse to a menu piece. The cursor delta
// from press to release is unprojected at the piece's depth, giving
// a world-space velocity proportional to how fast the user flicked
// the cursor. Spin magnitude scales with throw speed.
void menu_throw_piece(PhysicsPiece& p,
                      double press_mx, double press_my,
                      double release_mx, double release_my,
                      float dt,
                      int width, int height, float time);

// Challenge select screen
void renderer_draw_challenge_select(const std::vector<std::string>& challenge_names,
                                    int width, int height, int hover_index);
// Returns -2=back, -1=none, 0..N-1=challenge index
int challenge_select_hit_test(double mx, double my, int width, int height,
                              const std::vector<std::string>& challenge_names);

// Pre-game setup screen: side toggle + Stockfish ELO slider + new
// time-control dropdown. tc_hover: -2 = head hovered, -1 = none,
// 0..TC_COUNT-1 = row hovered (only meaningful if dropdown_open).
void renderer_draw_pregame(bool human_plays_white,
                           int elo, int elo_min, int elo_max,
                           TimeControl time_control,
                           bool dropdown_open,
                           int tc_hover,
                           int width, int height,
                           int hover);
// Returns 0=none, 1=Start, 2=Back, 3=Toggle button, 4=Slider area,
// 5=Dropdown head, 6=Dropdown row (out_tc_index receives the row
// index when this function returns 6; caller may pass nullptr).
int pregame_hit_test(double mx, double my, int width, int height,
                     bool dropdown_open, int* out_tc_index);

// Challenge in-game overlay
void renderer_draw_challenge_overlay(const std::string& challenge_name,
                                     int puzzle_index, int total_puzzles,
                                     int moves_made, int max_moves,
                                     bool starts_white,
                                     int width, int height);

// Next puzzle button (drawn when challenge solved). Returns true if hit.
void renderer_draw_next_button(int width, int height, bool hover);
bool next_button_hit_test(double mx, double my, int width, int height);

// Try-again button (drawn when the player made a mistake on a
// mate-in-N puzzle). Click resets the puzzle to its starting FEN.
void renderer_draw_try_again_button(int width, int height, bool hover);
bool try_again_button_hit_test(double mx, double my, int width, int height);

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
