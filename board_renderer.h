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

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
// CHESS_GLES — the OpenGL ES family (web / Android / iOS), a superset of
// __EMSCRIPTEN__. These targets render with OpenGL ES 3.0; desktop Linux /
// macOS use full GL via libepoxy. Defining it is behaviour-preserving for the
// existing Linux/web/macOS-desktop/iOS builds (each still selects the SAME GL
// header below) — it only adds the Android (__ANDROID__) branch.
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
#  ifndef CHESS_GLES
#    define CHESS_GLES 1
#  endif
#endif
#if defined(CHESS_GLES) && defined(__APPLE__) && TARGET_OS_IPHONE
// iOS / iPadOS: OpenGL ES 3.0 via Apple's OpenGLES.framework (no epoxy on iOS).
#include <OpenGLES/ES3/gl.h>
#include <OpenGLES/ES3/glext.h>
#elif defined(CHESS_GLES)
// web (emscripten) + Android NDK: OpenGL ES 3.0 system header.
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

// Initialize GL resources (call from on_realize).
//
// On the web this loads only what the main menu draws (piece meshes,
// fonts, shaders); the board/clock/table meshes, the retro piece set and
// the splat backdrops arrive later through the renderer_load_* calls
// below as their asset packages download. On desktop everything is on
// local disk, so renderer_init still loads the lot up front.
void renderer_init(StlModel loaded_models[PIECE_COUNT]);

// Deferred asset installs. Each is idempotent and safe to call once the
// matching files exist in the filesystem; until then the renderer simply
// draws without them (empty meshes are skipped, missing textures read as
// 0, a missing splat cloud falls back to the plain clear).
void renderer_load_game_assets();    // board + clock + table
void renderer_load_retro_assets();   // retro PC piece set (environment 1)

// One piece of the game asset set at a time. Each part costs several
// JPEG/PNG decodes plus mesh uploads, so the web build installs them on
// separate frames rather than freezing the menu animation for the lot.
// part: 0 = board, 1 = clock, 2 = table.
void renderer_load_game_asset_part(int part);

// Progress panel drawn over the pregame screen when the player starts a
// game before its assets have finished downloading. `progress` is 0..1.
void renderer_draw_asset_loading_overlay(const char* label, float progress);

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
// Multi-game flow: (sub_x, sub_y) is the lower-left corner of the
// sub-viewport in default-FB pixel coords; (width, height) are the
// sub-viewport dims. For single-game (N=1) callers pass (0, 0, w, h)
// — identical to the previous behaviour. For 2x2 grid callers pass
// per-quadrant rects from viewport_for_game().
// Call exactly once at the start of every render frame, BEFORE any
// renderer_draw call. Invalidates per-frame caches inside the
// renderer (currently the splat-backdrop colour texture). In multi-
// game mode this lets the first board pay the splat cost while the
// remaining boards reuse the cached colour texture, since every
// board shares the same camera (rot_x/rot_y/zoom) and the room is
// shake-immune by design.
extern "C" void renderer_begin_frame();
void renderer_set_jelly_pieces(bool enabled);
void renderer_dbg_jelly_ior(float ior);

// Switch the active Gaussian-splat environment behind the chessboard.
// Re-reads the SPZ from disk, re-uploads the packed splat textures,
// resets the per-environment cache flags (the GL compute upload gate
// and the per-frame splat-bg cache), and updates the bbox used to
// centre the splat cloud on the chess world. The numeric value comes
// from AppState::Environment (cast to int) — kept loose-typed here so
// board_renderer.h doesn't need to include app_state.h. Returns true
// on success; false (and leaves the previous splats loaded) when the
// chosen environment's SPZ couldn't be loaded.
bool renderer_set_environment(int env_kind);

// Debug/GIF harness: force a retro piece type's animated part to a fixed
// animation phase (seconds), bypassing selection. type < 0 clears it.
void renderer_dbg_animate(int type, float t_seconds);

// Display label for an environment id, used by the options-screen
// cycle row. Stable across runs; matches the spelling stored in
// settings.ini's `environment=` line. Returns a static const string
// — never null, falls back to the medieval label for out-of-range
// values.
const char* renderer_environment_label(int env_kind);

// Number of environments we know how to render. Used by the options
// row to wrap when the user clicks past the last one.
int renderer_environment_count();

// Re-render the chess scene directly into the shatter capture FBO.
// Used on web because the WebGL2 multisample default framebuffer
// doesn't reliably resolve via glBlitFramebuffer back to a user
// single-sample FBO across all implementations — the capture texture
// came back empty and the shatter shards rendered as transparent
// voids. Re-rendering into our own offscreen FBO sidesteps that
// quirk: the chess scene draws straight into the capture target
// using the existing renderer_draw pipeline. Cost: one extra
// renderer_draw per puzzle advance, which is fine for a one-shot
// transition event. Pass the same camera / lighting / splats_enabled
// inputs the live render is using so the captured snapshot matches
// what was just on screen.
void renderer_redraw_into_capture_fbo(GameState& gs, int width, int height,
                                       float rot_x, float rot_y, float zoom,
                                       bool human_plays_white,
                                       bool splats_enabled,
                                       float light_dir_x,
                                       float light_dir_y,
                                       float light_dir_z,
                                       bool light_positioning);

void renderer_draw(GameState& gs,
                   int sub_x, int sub_y,
                   int width, int height,
                   float rot_x, float rot_y, float zoom,
                   bool human_plays_white,
                   bool endgame_menu_hover,
                   bool continue_playing_hover,
                   const ClothFlag* flag, bool draw_flag,
                   bool withdraw_confirm_open, int withdraw_hover,
                   bool draw_clock,
                   int64_t clock_ms_remaining,
                   bool clock_side_is_white,
                   float shake_x = 0.0f,
                   const char* withdraw_confirm_title = "Withdraw from game?",
                   // Per-side cumulative thinking time so the 3D
                   // chess clock's needles can rotate monotonically
                   // (one full rev per real-time minute, sped up
                   // from a true minute-hand rate so the motion is
                   // visible on the small clock). Zero → needles
                   // stay at rest pose for non-playing screens.
                   // Cumulative rather than ms_left so Fischer
                   // increments don't reset the needles on each move.
                   int64_t white_thought_ms = 0,
                   int64_t black_thought_ms = 0,
                   // Per-side clock time REMAINING (ms) for the digital
                   // clock's LCD in the datacenter environment. The analog
                   // clock ignores these (its needles use thought time).
                   int64_t white_clock_ms = 0,
                   int64_t black_clock_ms = 0,
                   // Per-side lever blend (1.0 = fully UP / your
                   // turn, 0.0 = fully DOWN / just pressed). The
                   // renderer translates each silver lever along Y.
                   // Defaults: white UP, black DOWN — matches the
                   // pre-first-move state of an analog chess clock.
                   float white_lever_blend = 1.0f,
                   float black_lever_blend = 0.0f,
                   // P-key toggle: when true, the renderer uses the
                   // flat panorama skybox instead of the splat scene.
                   bool force_panorama_only = false,
                   // Scene light direction. Hardcoded defaults match
                   // the legacy values; AppState forwards the live
                   // values during the live-game render path so the
                   // D-key debug mode can aim the light interactively.
                   float light_dir_x = 0.4f,
                   float light_dir_y = 1.0f,
                   float light_dir_z = 0.6f,
                   // When true (D-mode active), the table mesh is
                   // added to the shadow pass and the splat backdrop
                   // samples the shadow map per-splat so the user can
                   // see the table's shadow projected onto the floor
                   // splats. Off by default — pure debug visualisation.
                   bool light_positioning = false);

// Draws a thin white border around the current viewport. Called
// after renderer_draw in multi-game mode to mark the active board
// — the one that accepts moves and whose clock is ticking. Caller
// must have glViewport / glScissor set to the active sub-rect
// already; (sub_w, sub_h) drive the per-pixel border thickness in
// NDC so the frame stays the same width regardless of quadrant
// size.
void renderer_draw_active_frame(int sub_w, int sub_h);

// Draws a centered "Puzzle solved!" popup over a dim backdrop —
// shown briefly between the user completing the puzzle's solution
// line and the next random puzzle finishing its network fetch.
// No buttons; the puzzle screen auto-advances when the new puzzle
// arrives.
void renderer_draw_puzzle_solved_popup();

// Splits the window into sub-rects per game count.
//   N=1 → full screen.
//   N=2 → side-by-side halves (game 0 left, game 1 right).
//   N=3 → 2x2 grid, bottom-right empty (game 0 TL, 1 TR, 2 BL).
//   N=4 → full 2x2 grid (TL, TR, BL, BR in index order).
// Coordinates use the GL convention: (sub_x, sub_y) is the
// lower-left corner of the sub-rect in default-FB pixel coords.
void viewport_for_game(int game_idx, int N, int win_w, int win_h,
                       int& sub_x, int& sub_y,
                       int& sub_w, int& sub_h);

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

// Hit-test for the algebraic move list (top-right HUD). Returns the
// ply index (snapshot index) of a clicked move that has a "why?"
// explanation available (gs.why_reason non-empty) — i.e. a flagged
// mistake worth opening the panel on — or -1 for any other click.
// Mirrors draw_move_list's layout; (mx,my) are in the same coordinate
// space the other HUD hit-tests use (active sub-viewport, top-down
// pixels with width/height the sub-viewport dims).
int move_list_hit_test(double mx, double my, int width, int height,
                       const GameState& gs);

// Hit-test for the open "why?" panel's "x" close button. Returns 1 if
// it was clicked, or 0 otherwise. Coords use the same space as
// move_list_hit_test. Only meaningful when gs.why_ply >= 0.
int why_panel_hit_test(double mx, double my, int width, int height,
                       const GameState& gs);

// Hit-test for the withdraw flag in the bottom-right corner. Uses
// the deformed cloth's bounding box + a small NDC padding for
// ergonomics, so the click region tracks the rippling cloth.
bool flag_hit_test(const ClothFlag& flag,
                   double mx, double my, int width, int height);

// Board-doesn't-match-digital-state modal — drawn on top of
// MODE_PLAYING when the physical Chessnut Move board disagrees
// with the digital state at game start. Single "Exit to Menu"
// button; auto-closes from the app side when the disagreement
// resolves.
//
// kind selects the title + body wording:
//   Positioning → motors mid-animation, "Positioning pieces on
//                 the chessboard" + "Wait for the motors to
//                 finish".
//   Missing     → pieces unaccounted for; squares_msg is a comma-
//                 separated list (e.g. "e8, a7"). Empty string
//                 means "no pieces at all".
//   WrongLayout → every piece on the board but in the wrong
//                 position; squares_msg is ignored. The user
//                 needs to reset the layout to the starting
//                 position.
enum class ChessnutBoardModalKind { Positioning, Missing, WrongLayout };
void renderer_draw_chessnut_missing_modal(const std::string& squares_msg,
                                          ChessnutBoardModalKind kind,
                                          bool exit_hover);
bool chessnut_missing_exit_button_hit_test(double mx, double my,
                                           int width, int height);

// Hit-test for the withdraw confirmation modal. Writes to *which:
// 0 = background / neither button, 1 = Yes, 2 = No.
bool withdraw_confirm_hit_test(double mx, double my,
                               int width, int height, int* which);

// PhysicsPiece / menu_init_physics / menu_update_physics live in
// menu_physics.h, transitively included above.

void renderer_draw_menu(const std::vector<PhysicsPiece>& pieces,
                        int width, int height, float time,
                        int hover_button,  // 0=none, 1..5 (see menu_input.h)
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
