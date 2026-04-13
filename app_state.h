#pragma once

#include "board_renderer.h"  // PhysicsPiece, SummaryEntry
#include "challenge.h"       // Challenge
#include "chess_types.h"     // GameState, GameMode
#include "game_state.h"

#include <cstdint>
#include <string>
#include <vector>

// ===========================================================================
// Platform abstraction
// ===========================================================================
//
// The desktop (GTK) and WebAssembly (SDL2 + Emscripten) drivers share the
// bulk of the game's UI logic — mode machine, camera, click handling,
// challenge flow, shatter transition, render dispatch. Only these things
// differ between platforms:
//
//   - Event loop and event types                 (owned by the driver)
//   - Title bar / status display                 (set_status hook)
//   - Queue-a-redraw                             (queue_redraw hook)
//   - Monotonic time source                      (now_us hook)
//   - Async Stockfish dispatch                   (trigger_* hooks)
//
// Each platform fills in an AppPlatform struct of function pointers and
// passes it to app_init(). The shared app_state.cpp never touches GTK,
// SDL, Emscripten, or any threading primitive — it only calls through
// the hook table for anything that isn't pure C++.
struct AppPlatform {
    // Display the given text in the window title / HTML status element.
    void (*set_status)(const char* text);

    // Request a repaint. Desktop: gtk_widget_queue_draw. Web: no-op
    // because emscripten_set_main_loop redraws every frame anyway.
    void (*queue_redraw)(void);

    // Monotonic time, microseconds since an arbitrary fixed epoch. Used
    // for animation timing (AI move arc, shatter transition, menu dt).
    int64_t (*now_us)(void);

    // Kick off an async Stockfish move request. Must return immediately.
    // When the result arrives the platform calls app_ai_move_ready().
    // 'fen' is a NUL-terminated C string valid only for the duration of
    // the call.
    void (*trigger_ai_move)(const char* fen, int movetime_ms);

    // Kick off an async position-eval request. Result comes back via
    // app_eval_ready(). score_index is the index in gs.score_history
    // to overwrite when the eval arrives.
    void (*trigger_eval)(const char* fen, int movetime_ms, int score_index);
};

// ===========================================================================
// Keyboard abstraction
// ===========================================================================
// The handful of keys the game actually handles. Each driver translates
// its native key events (GDK/SDL) into one of these.
enum AppKey {
    KEY_UNKNOWN = 0,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_ESCAPE,
    KEY_A,
    KEY_M,
};

// ===========================================================================
// Shared UI-level application state
// ===========================================================================
struct AppState {
    // The chess position is owned here; platform code reads through a.game.
    GameState game;

    // Camera
    float rot_x = 30.0f;
    float rot_y = 180.0f;
    float zoom  = 12.0f;

    bool   dragging = false;
    double last_mouse_x = 0, last_mouse_y = 0;
    double press_x = 0, press_y = 0;

    // Mode machine + menu
    GameMode mode = MODE_MENU;
    std::vector<PhysicsPiece> menu_pieces;
    int64_t menu_start_time_us = 0;
    int64_t menu_last_update_us = 0;
    int menu_hover = 0;

    // Challenge mode
    std::vector<std::string> challenge_files;
    std::vector<std::string> challenge_names;
    int challenge_select_hover = -1;
    Challenge current_challenge;
    int challenge_moves_made = 0;
    bool challenge_solved = false;
    bool challenge_next_hover = false;
    std::vector<std::vector<std::string>> challenge_solutions;
    bool challenge_show_summary = false;

    // Glass-shatter transition between challenge puzzles
    bool  transition_active = false;
    int   transition_pending_next = -1;
    int64_t transition_start_time_us = 0;

    // AI animation — start time stored in microseconds, mirrors what the
    // renderer reads from gs.ai_anim_start. Separate from gs.ai_anim_start
    // because platforms measure in different units internally.
    int64_t ai_anim_start_us = 0;

    // Non-owning pointer to the platform's hook table.
    const AppPlatform* platform = nullptr;
};

// ===========================================================================
// Lifecycle
// ===========================================================================
void app_init(AppState& a, const AppPlatform* platform);

// Mode transitions
void app_enter_menu(AppState& a);
void app_enter_game(AppState& a);
void app_enter_challenge_select(AppState& a);
void app_enter_challenge(AppState& a, int index);
void app_load_challenge_puzzle(AppState& a, int puzzle_index);
void app_reset_challenge_puzzle(AppState& a);

// Re-emit the current title/status to the platform. Idempotent.
void app_refresh_status(AppState& a);

// ===========================================================================
// Input — platforms translate their native events into these calls.
// Coordinates are in the canvas/window's pixel space.
// ===========================================================================
void app_press(AppState& a, double mx, double my);
void app_release(AppState& a, double mx, double my, int width, int height);
void app_motion(AppState& a, double mx, double my, int width, int height);
void app_scroll(AppState& a, double delta);
void app_key(AppState& a, AppKey key);

// ===========================================================================
// Per-frame update and rendering
// ===========================================================================
// Advance menu physics, AI move animation, and shatter transition. Call
// this once per frame before app_render.
void app_tick(AppState& a);

// Dispatch to the appropriate renderer_draw_* call based on mode. The
// platform is responsible for SwapBuffers / SDL_GL_SwapWindow after this.
void app_render(AppState& a, int width, int height);

// ===========================================================================
// Async result delivery (platform calls these when Stockfish returns)
// ===========================================================================
// uci may be empty to indicate "no move produced" — in that case
// app_state picks a random legal black move as a fallback.
void app_ai_move_ready(AppState& a, const char* uci);
void app_eval_ready(AppState& a, int cp, int score_index);
