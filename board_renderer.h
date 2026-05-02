#pragma once

#include "challenge_ui.h"   // challenge-screen draws + SummaryEntry
#include "chess_types.h"
#include "cloth_flag.h"
#include "menu_input.h"     // menu_hit_test / menu_piece_hit_test / menu_throw_piece
#include "menu_physics.h"   // PhysicsPiece + menu_init_physics + menu_update_physics
#include "options_ui.h"     // renderer_draw_options + options_hit_test
#include "pregame_ui.h"     // renderer_draw_pregame + pregame_hit_test
#include "shatter_transition.h"  // renderer_capture_frame / renderer_draw_shatter
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

// "Pieces missing" modal — drawn on top of MODE_PLAYING when the
// physical Chessnut Move board reports squares that should hold
// pieces are empty. Single "Exit to Menu" button; auto-closes
// from the app side when the board re-agrees with the digital
// state. squares_msg is a comma-separated list (e.g. "e8, a7")
// or empty if all pieces are missing.
void renderer_draw_chessnut_missing_modal(const std::string& squares_msg,
                                          bool exit_hover);
bool chessnut_missing_exit_button_hit_test(double mx, double my,
                                           int width, int height);

// Hit-test for the withdraw confirmation modal. Writes to *which:
// 0 = background / neither button, 1 = Yes, 2 = No.
bool withdraw_confirm_hit_test(double mx, double my,
                               int width, int height, int* which);

// PhysicsPiece / menu_init_physics / menu_update_physics live in
// menu_physics.h, transitively included above.

// cartoon_outline=true reuses the scene post-process shader to draw a
// cartoon-style contour around the rendered pieces, used by the menu
// grab gesture to highlight which piece the cursor is holding.
void renderer_draw_menu(const std::vector<PhysicsPiece>& pieces,
                        int width, int height, float time,
                        int hover_button,  // 0=none, 1..5 (see menu_input.h)
                        bool cartoon_outline,
                        bool chessnut_connected);

// menu_hit_test / menu_piece_hit_test / menu_throw_piece live in
// menu_input.h, transitively included above.

// Challenge-screen draws (select / overlay / next-button / try-again /
// summary) + SummaryEntry live in challenge_ui.h, transitively
// included above.

// renderer_draw_pregame and pregame_hit_test live in pregame_ui.h,
// transitively included above.

// renderer_capture_frame / renderer_draw_shatter live in
// shatter_transition.h, transitively included above.
