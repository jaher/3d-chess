#pragma once

#include "board_renderer.h"  // PhysicsPiece, SummaryEntry
#include "challenge.h"       // Challenge
#include "cloth_flag.h"      // ClothFlag
#include "time_control.h"    // TimeControl, TIME_CONTROLS
#include "chess_types.h"     // GameState, GameMode
#include "game_state.h"

#include <array>
#include <cstdint>
#include <functional>
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
    // `game_id` is the index of the GameInstance in `a.games` that
    // initiated the request — round-tripped so we can route the
    // response back to the right game when the user has multiple
    // parallel games in flight.
    // 'fen' is a NUL-terminated C string valid only for the duration of
    // the call.
    void (*trigger_ai_move)(const char* fen, int movetime_ms, int game_id);

    // Kick off an async position-eval request. Result comes back via
    // app_eval_ready(). score_index is the index in
    // a.games[game_id].game.score_history to overwrite when the eval
    // arrives; game_id selects which instance.
    void (*trigger_eval)(const char* fen, int movetime_ms,
                         int score_index, int game_id);

    // Set the running Stockfish engine's UCI_Elo. Takes effect on the
    // next `go`, not any currently-running search. Safe to call from
    // the UI thread. On desktop the value is also latched so it's
    // applied during the first engine handshake when the subprocess
    // hasn't been spawned yet.
    void (*set_ai_elo)(int elo);

    // Request a clean app shutdown. Desktop: gtk_main_quit(), so
    // main() returns and the post-loop cleanup chain runs (voice
    // shutdown, BLE bridge shutdown, audio shutdown, etc.). Web:
    // no-op, since closing a tab from inside WASM isn't natural.
    // Must be safe to call from a UI event handler (the GTK click
    // path); it just signals — no blocking work.
    void (*request_quit)(void);

    // Kick off an async chess.com puzzle fetch. daily=true → the
    // /pub/puzzle "Puzzle of the Day" endpoint, false → /pub/puzzle/
    // random for the next-puzzle flow. Must return immediately. The
    // platform calls app_puzzle_ready() with the parsed JSON body
    // when the fetch completes (or with an empty body on failure).
    // May be nullptr on platforms that don't support outbound HTTP
    // (e.g. tests) — callers must null-check before invoking.
    void (*trigger_puzzle_fetch)(bool daily);
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
    KEY_S,   // toggles cartoon outline in a live game / challenge
};

// ===========================================================================
// Per-game state container
// ===========================================================================
//
// The "Multi-game" mode in pregame lets the user run up to 4 parallel
// games against Stockfish, played sequentially (one at a time, the
// app rotates the active game after each AI reply). Most of the
// codebase already treats GameState as per-game (move_history,
// snapshots, score_history live there), but a few bits of game-
// specific state used to dangle off AppState directly. This struct
// gathers those into a per-instance bundle so we can hold an array
// of them.
//
// For N=1 the bundle is functionally identical to "the live game"
// it replaces — every existing call site uses cur(a) / cur_gs(a)
// helpers below to read/write whichever instance is active.
struct GameInstance {
    GameState game;

    // Per-game clock state — only the active game's clock ticks
    // each frame; the rest are paused. clock_last_tick_us == 0
    // means "paused, re-latch on the next active tick" (same
    // semantics as pre-refactor AppState).
    int64_t white_ms_left      = 0;
    int64_t black_ms_left      = 0;
    int64_t clock_last_tick_us = 0;
    int     prev_white_turn    = -1;  // -1 unset, 0 black, 1 white

    // Per-game eval / hint cache. Each instance has its own
    // Stockfish bestmove history; the hint feature keys off the
    // active instance's prev_eval_best_uci to brand "Best" moves.
    std::string last_eval_best_uci;
    std::string prev_eval_best_uci;
    bool hint_request_pending = false;
    bool hint_confirm_pending = false;

    // Per-game pending move announcement — set at execute_move
    // time, drained in app_eval_ready so the move text + the
    // classification phrase land as one combined utterance.
    std::string pending_move_speech;
    bool        pending_move_speech_was_human = false;
    std::string pending_move_classification;
};

// ===========================================================================
// Shared UI-level application state
// ===========================================================================
struct AppState {
    // Active and (optionally) inactive game instances. With the
    // user-selected pregame_game_count = 1, the vector has one
    // entry and behaves identically to the pre-refactor world.
    // active_game indexes into this vector for the user's current
    // turn. Helpers `cur(a)` / `cur_gs(a)` (defined below the
    // struct) abbreviate the common access pattern.
    std::vector<GameInstance> games{1};
    int                       active_game = 0;

    // Camera
    float rot_x = 30.0f;
    float rot_y = 180.0f;
    float zoom  = 12.0f;

    bool   dragging = false;
    double last_mouse_x = 0, last_mouse_y = 0;
    double press_x = 0, press_y = 0;
    // Monotonic timestamp of the press that produced press_x/press_y.
    int64_t press_time_us = 0;

    // Rolling reference used by the menu drag-to-fling gesture. The
    // cursor position and timestamp here are advanced during motion
    // only after a minimum gap, so release velocity is always taken
    // from the last short segment of the drag rather than the whole
    // gesture. That way a slow drag followed by a fast flick still
    // throws the piece hard.
    double fling_sample_x = 0, fling_sample_y = 0;
    int64_t fling_sample_time_us = 0;

    // Mode machine + menu
    GameMode mode = MODE_MENU;
    std::vector<PhysicsPiece> menu_pieces;
    int64_t menu_start_time_us = 0;
    int64_t menu_last_update_us = 0;
    int menu_hover = 0;
    // Index of the piece currently held by the cursor in the menu,
    // or -1 if no grab is in flight. Set on the first motion after
    // press (app_press has no width/height to hit-test with), cleared
    // on release. Used to light up the cartoon outline on the
    // grabbed piece's mesh.
    int menu_grabbed_piece = -1;

    // Options screen hover: 0=none, 1=back, 2=outline toggle.
    int options_hover = 0;

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

    // Mistake feedback: set when the starter exhausts max_moves on a
    // mate_in_N puzzle without delivering checkmate. Triggers a
    // horizontal board-shake animation and a "Try Again" button that
    // reloads the puzzle's starting position.
    bool    challenge_mistake = false;
    int64_t challenge_mistake_start_us = 0;
    bool    challenge_try_again_hover = false;
    // Current horizontal shake offset (view-space units). Zero unless
    // a mistake animation is in flight.
    float   board_shake_x = 0.0f;
    // Generic shake trigger — set to now_us(a) by anything that wants
    // a brief "that was wrong" wobble (e.g. an invalid Chessnut Move
    // hand-move). The mistake-shake ticker also reads this so any
    // mode can trigger the same animation.
    int64_t board_shake_start_us = 0;

    // Glass-shatter transition between challenge puzzles
    bool  transition_active = false;
    int   transition_pending_next = -1;
    int64_t transition_start_time_us = 0;

    // ─────────────────────────────────────────────────────────────────────
    // Chess.com Daily / Random puzzle (MODE_PUZZLE) state.
    //
    // The starter FEN is loaded into games[0]; the player makes
    // moves through the same handle_board_click path used in
    // MODE_PLAYING and Stockfish always responds with its
    // best-move from the resulting position — no validation or
    // mistake feedback. The puzzle resolves when the position is
    // decisive (game-over OR cp ≥ 500 in the user's favour after
    // they've played at least one move) and we auto-advance to a
    // random next puzzle.
    // ─────────────────────────────────────────────────────────────────────
    std::string puzzle_title;            // displayed at the top of the screen
    std::string puzzle_url;              // chess.com URL (informational)
    std::string puzzle_starting_fen;     // FEN for the current puzzle
    bool puzzle_solved = false;
    bool puzzle_loading = false;         // request in flight
    bool puzzle_load_failed = false;     // last fetch returned no FEN

    // Canonical solution line parsed from the chess.com PGN at
    // load time. The puzzle play flow walks this list as long as
    // the user follows it: a matching user move advances the
    // index and the AI's reply is the next entry, played as a
    // canned animation that bypasses Stockfish entirely. The
    // moment the user diverges, `puzzle_on_book` flips to false
    // and we fall back to a regular Stockfish bestmove for every
    // subsequent AI reply (the rest of the solution line is no
    // longer applicable). Empty list = PGN missing or unparseable
    // — in that case we go straight to Stockfish from move 1.
    std::vector<std::string> puzzle_solution_uci;
    size_t puzzle_solution_index = 0;
    bool   puzzle_on_book        = false;

    // AI animation — start time stored in microseconds, mirrors what the
    // renderer reads from gs.ai_anim_start. Separate from gs.ai_anim_start
    // because platforms measure in different units internally.
    int64_t ai_anim_start_us = 0;

    // Pre-game setup screen state. Persists across menu <-> pregame
    // navigation so the user's last choices are remembered for the
    // duration of the session.
    bool human_plays_white = true;
    int  stockfish_elo     = 1400;
    bool slider_dragging   = false;
    int  pregame_hover     = 0;
    // Number of parallel games to start (1..4). Only the start-
    // game flow honours this; 2-player and challenge modes force
    // N=1. Default 1 so first-time users get the original single-
    // board UX. Drives `games.size()` after `app_enter_game`.
    int  pregame_game_count = 1;
    // Hover index for the games-count [1][2][3][4] row in the
    // pregame screen. 0 = none, 1..4 = button hovered. Visual
    // only — the actual selection lives in pregame_game_count.
    int  pregame_game_count_hover = 0;

    // Two-player (hot-seat) mode. Set by clicking the menu's
    // "Multiplayer" button (only visible when chessnut_connected),
    // cleared by the regular "Start Game" path. The pregame UI
    // hides the Elo slider when this is on; the in-game click
    // handler accepts moves for whichever side's turn it is and
    // skips Stockfish dispatch.
    bool two_player_mode = false;

    // Pregame time-control dropdown state. Default = Classical
    // (30+30) so the first game out of the box has a clock without
    // forcing the user to interact with the dropdown.
    TimeControl time_control    = TC_CLASSICAL;
    bool        pregame_tc_open = false;
    // -1 = none, 0..TC_COUNT-1 = row index (only meaningful when the
    // dropdown is open), -2 = collapsed-head hover.
    int         pregame_tc_hover = -1;

    // Clock-on/clock-off is a global setting (driven by the
    // pregame time-control choice); per-game clock state
    // (ms_left / last_tick / prev_white_turn) lives on each
    // GameInstance so parallel games each have their own
    // remaining time. Only the active instance's clock ticks
    // per frame.
    bool    clock_enabled       = false;

    // Hover flag for the "Back to Menu" button drawn on top of the
    // game-over / analysis overlay. Only meaningful in MODE_PLAYING
    // while game.game_over or game.analysis_mode is true.
    bool endgame_menu_hover = false;

    // Hover flag for the "Continue Playing" button drawn above "Back
    // to Menu" while in analysis mode.
    bool continue_playing_hover = false;

    // Withdraw flag — the wavy white cloth on a brown stick in the
    // bottom-right corner of MODE_PLAYING. Clicking it opens the
    // confirmation modal; "Yes" drops the user into analysis mode.
    ClothFlag flag;
    int64_t   flag_last_update_us = 0;
    int64_t   flag_start_us = 0;

    // Withdraw confirmation modal state.
    // withdraw_hover: 0=none, 1=Yes, 2=No
    bool withdraw_confirm_open = false;
    int  withdraw_hover = 0;

    // Cartoon-outline post-process toggle. Flipped by the 'S' key
    // during a live game or challenge. Off by default; persists
    // across games within a session until the user toggles again.
    bool cartoon_outline = false;

#ifndef __EMSCRIPTEN__
    // Voice input (push-to-talk on SPACE). Lazy init on first use so
    // we don't pay the model-load cost or prompt for mic permission
    // for users who never press space. ``voice_init_failed`` is
    // sticky across the session — once we've reported an init
    // failure (e.g. missing model file) we don't keep retrying.
    bool voice_initialized = false;
    bool voice_init_failed = false;
    bool voice_listening   = false;
#endif

    // Continuous (hands-free) voice mode. Off by default; toggled by
    // a row in the Options screen. Desktop drives a whisper.cpp
    // monitor thread; web drives the browser's SpeechRecognition
    // API (web/voice_web.cpp).
    bool voice_continuous_enabled = false;

    // Voice-output (TTS): announce every move (yours and the
    // opponent's) over the speaker. Desktop drives espeak-ng; web
    // drives the browser's speechSynthesis. ON by default so a
    // brand-new install gets the eyes-free experience without
    // hunting through Options. Flip OFF in Options if you'd rather
    // play silently. Voice phrases: "speak moves" / "announce
    // moves" / "toggle voice output".
    bool voice_tts_enabled = true;

    // Move hints (coach mode) — tri-state cycle:
    //   * Off       — no hint rings, no spoken hints.
    //   * Auto      — every user turn, surface Stockfish's
    //                 recommended move (yellow rings on
    //                 from+to squares + TTS "Hint: ...").
    //   * OnDemand  — silent until the user says "give me a hint"
    //                 (or "hint" / "best move"), at which point
    //                 the cached bestmove from the most recent
    //                 eval surfaces as a one-shot.
    // Off by default. Click the Options row to cycle Off → Auto
    // → OnDemand → Off; voice toggles via "move hints" / "hint
    // mode". The on-demand request itself is "hint" / "give me
    // a hint" / "best move" — distinct from the toggle phrases.
    enum class HintMode { Off, Auto, OnDemand };
    HintMode hint_mode = HintMode::Off;
    // Per-game state related to hints + pending move announcement
    // moved into GameInstance — see definitions there.

    // Chessnut Move physical board mirroring. Off by default;
    // toggled by a row in the Options screen. When on, the app
    // pushes the current FEN to the board on every state change.
    // Desktop drives this in-process via SimpleBLE; the web build
    // drives it via the browser's Web Bluetooth API
    // (web/chessnut_web.cpp). bridge_running is a desktop-only
    // signal (web has no subprocess) but kept ungated so the
    // shared apply_status code in app_state.cpp can compile in
    // both builds.
    bool chessnut_enabled        = false;
    bool chessnut_bridge_running = false;
    bool chessnut_connected      = false;
    // Monotonic timestamp (us) of the most recent reconnect attempt
    // while in the "enabled but disconnected" state. Used by
    // app_tick to throttle retries to one per kChessnutReconnectMs.
    int64_t chessnut_last_reconnect_us = 0;
    // Monotonic timestamp (us) of the most recent force-/soft-sync
    // we pushed to the firmware. While the firmware is mid-motion,
    // sensor frames don't match the digital position — we use this
    // to suppress the auto-reject behaviour during a settling
    // window so we don't fight our own motors.
    int64_t chessnut_last_sync_us = 0;
    // Most recent sensor grid the firmware reported, kept around so
    // each new frame can be diff'd against the *previous* sensor
    // state (delta detection) instead of against the digital game.
    // That way a ghost piece the firmware can't see (e.g. a Chessnut
    // Move piece with a flat ID-chip battery) becomes a stable
    // disagreement we ignore, rather than poisoning every move
    // attempt with a permanent extra diff.
    std::array<std::array<char, 8>, 8> chessnut_last_sensor_grid{};
    bool chessnut_sensor_baseline_set = false;
    // Modal popup that blocks game input when the physical board
    // disagrees with the digital state at game start. Three flavours:
    //   * Positioning — motors are mid-animation moving pieces into
    //     the starting position (or the user just hit Start and we
    //     haven't even heard from the firmware yet). Auto-closes
    //     once a stable sensor frame confirms the board matches.
    //   * Missing — pieces are unaccounted for (typically a Chessnut
    //     Move piece with a dead ID-chip battery, or pieces left
    //     off the board). chessnut_missing_squares_msg lists the
    //     squares.
    //   * WrongLayout — every piece is detected but they're not in
    //     the correct starting position. The user just needs to
    //     reset the layout.
    enum class ChessnutModalType { Positioning, Missing, WrongLayout };
    bool chessnut_missing_modal_open = false;
    ChessnutModalType chessnut_missing_modal_type = ChessnutModalType::Missing;
    std::string chessnut_missing_squares_msg;
    bool chessnut_missing_exit_hover = false;
    // Stockfish's first move (when the human picked black) is held
    // back while the Positioning modal is open — otherwise the AI
    // would advance the digital state while motors are still
    // arranging the starting position on the physical board, and
    // the firmware's view would diverge from the digital game.
    // Set in app_enter_game; consumed when the modal closes.
    bool chessnut_pending_ai_trigger = false;

    // BLE verbose-log toggle. Off by default; flipping it on in the
    // Options screen surfaces every notify frame the bridge sees as
    // a status-bar message of the form "BLE <uuid-suffix> <hex>".
    // Used for capturing raw frames from boards with unverified wire
    // formats (notably Phantom variants on different firmware
    // versions) without needing terminal access. Session-only
    // persistence — same precedent as cartoon_outline.
    bool ble_verbose_log = false;

    // Which physical-board protocol the connected device speaks.
    // The picker scan lists Chessnut Move devices and Phantom
    // Chessboards together (both are robotic boards with
    // app-driven motor commands). They have different wire
    // formats — the chessnut bridge speaks setMoveBoard / RGB
    // LED frames, the phantom bridge speaks ASCII MOVE_CMD
    // strings — so we route every per-move sync through whichever
    // bridge matches the device the user picked.
    enum class ChessnutBoardKind { Move, Phantom };
    ChessnutBoardKind chessnut_board_kind = ChessnutBoardKind::Move;

    // In-app device picker state. When the user toggles Chessnut
    // Move on without a cached MAC (or when they explicitly ask
    // to re-pair), we run a BLE scan and render the discovered
    // peripherals as clickable rows under the existing toggles.
    // Web build doesn't use this — the browser provides its own
    // picker via navigator.bluetooth.requestDevice.
    struct ChessnutScannedDevice {
        std::string address;
        std::string name;
    };
    bool chessnut_picker_open = false;
    bool chessnut_picker_scanning = false;
    std::vector<ChessnutScannedDevice> chessnut_devices;
    int chessnut_picker_hover = -1;  // index of the hovered row, -1 = none

    // Non-owning pointer to the platform's hook table.
    const AppPlatform* platform = nullptr;

    // Non-owning pointer to the driver-owned array of loaded STL
    // piece models (PIECE_COUNT entries, indexed by PieceType).
    // Used by the ray-vs-mesh hit test in handle_board_click so
    // clicking on a tall piece that leans over a neighbouring
    // square selects the piece the user can actually see, rather
    // than the square underneath the cursor. The driver assigns
    // this right after app_init. nullptr is a safe fallback —
    // pick_piece returns -1 and the code paths fall through to
    // the flat-plane click test.
    const StlModel* loaded_models = nullptr;
};

// ===========================================================================
// Active-game accessors
// ===========================================================================
//
// Most of the codebase reads/writes "the live game" — for N=1 that's
// just `a.games[0]`; for N>1 it's whichever instance is currently
// the user's turn. These helpers abbreviate the access pattern so
// `a.game.move_history` becomes `cur_gs(a).move_history` and
// `a.white_ms_left` becomes `cur(a).white_ms_left`. Inline +
// reference-returning so there's no copy and no perf cost.
inline GameInstance& cur(AppState& a) {
    return a.games[a.active_game];
}
inline const GameInstance& cur(const AppState& a) {
    return a.games[a.active_game];
}
inline GameState& cur_gs(AppState& a) {
    return a.games[a.active_game].game;
}
inline const GameState& cur_gs(const AppState& a) {
    return a.games[a.active_game].game;
}

// ===========================================================================
// Lifecycle
// ===========================================================================
void app_init(AppState& a, const AppPlatform* platform);

// Mode transitions
void app_enter_menu(AppState& a);
void app_enter_pregame(AppState& a);
void app_enter_game(AppState& a);
void app_enter_challenge_select(AppState& a);
void app_enter_options(AppState& a);
void app_enter_challenge(AppState& a, int index);
void app_load_challenge_puzzle(AppState& a, int puzzle_index);
void app_reset_challenge_puzzle(AppState& a);
// Enter the chess.com puzzle screen and request the daily puzzle.
// On the first solve we auto-fetch /pub/puzzle/random for an
// indefinite session; ESC / "Back" from the screen returns to the
// menu.
void app_enter_puzzle(AppState& a);

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
void app_ai_move_ready(AppState& a, const char* uci, int game_id);
void app_eval_ready(AppState& a, int cp, int score_index,
                    const std::string& best_uci = std::string(),
                    int game_id = 0);

// Platform calls this with the JSON body returned by the chess.com
// /pub/puzzle endpoint (daily=true) or /pub/puzzle/random
// (daily=false). On parse failure a status hint is shown and the
// flow falls back to letting the user retry from the menu.
void app_puzzle_ready(AppState& a, const char* json_body, bool daily);

#ifndef __EMSCRIPTEN__
// Voice push-to-talk (desktop only — SDL2 + whisper.cpp). The
// driver wires these to SPACE key-down / key-up. app_voice_press
// triggers a lazy voice engine init on first use; app_voice_release
// stops capture and dispatches transcription on a worker thread,
// then calls back via `on_done` (which the driver should marshal
// onto the GUI thread). app_voice_apply_result is the GUI-thread
// tail that parses the utterance and applies the resulting move.
void app_voice_press(AppState& a);
void app_voice_release(
    AppState& a,
    std::function<void(const std::string& utterance,
                       const std::string& error)> on_done);
void app_voice_apply_result(AppState& a,
                            const std::string& utterance,
                            const std::string& error);

// Continuous (hands-free) voice toggle (desktop). On flips the
// voice_continuous_enabled flag, lazy-initialises the whisper.cpp
// engine, and starts the VAD monitor thread. Web has its own
// implementation in web/voice_web.cpp.
void app_voice_set_continuous(
    AppState& a, bool on,
    std::function<void(const std::string& utterance,
                       const std::string& error)> on_utterance,
    std::function<void(const std::string& partial)> on_partial);

// Release the voice engine on app exit. Idempotent.
void app_voice_shutdown(AppState& a);
#endif

// Chessnut Move board mirroring. Available on both desktop (BLE
// via SimpleBLE) and web (Web Bluetooth). The marshal callback is
// invoked when status lines arrive from the underlying transport;
// driver implementations are responsible for posting onto the GUI
// thread (g_idle_add on desktop / direct ccall on web — Emscripten
// runs everything single-threaded by default).
void app_chessnut_set_enabled(
    AppState& a, bool on,
    std::function<void(const std::string& status)> on_status);
void app_chessnut_apply_status(AppState& a, const std::string& status);
void app_chessnut_sync_board(AppState& a, bool force);
void app_chessnut_shutdown(AppState& a);
void app_chessnut_toggle_request(AppState& a);
bool app_chessnut_supported();

// Snapshot the current game position as a FEN string. Exposed on
// AppState so non-app_state.cpp callers (e.g. web/chessnut_web.cpp)
// can drive the bridge without re-implementing FEN serialisation.
std::string app_current_fen(const AppState& a);

// Persistence for user-visible toggles (cartoon outline, Chessnut
// Move enabled). Continuous voice is intentionally NOT persisted
// — it triggers a mic-permission prompt and is meant to be
// per-session-opt-in. Settings live in
// $XDG_CONFIG_HOME/3d_chess/settings.ini (or ~/.config/...).
// Web build no-ops both — localStorage would work but isn't
// wired here yet.
void app_settings_load(AppState& a);
void app_settings_save(const AppState& a);

// Light up the source + destination squares of the most-recently-
// played move on the physical board. No-op when the bridge isn't
// connected or no move has been played yet. Called after every
// successful sync so the LED pattern always tracks the latest move.
void app_chessnut_highlight_last_move(AppState& a);

// Inbound sensor-frame handler. The board pushes a 32-byte
// piece-placement frame on the 8262 notify channel whenever the
// physical state changes (piece picked up, put down, etc.). This
// decodes the frame, diffs it against the current digital
// position, and — if the diff is exactly one legal move — applies
// it as if the user had clicked it. Closes the loop: pick up a
// physical piece, the on-screen game follows.
//
// Public so chessnut_web.cpp's notification handler can reuse the
// same logic. `hex` is a string of 64 hex chars (32 bytes).
void app_chessnut_apply_sensor_frame(AppState& a, const std::string& hex);

// Open the device picker — clears the device list, kicks off a
// fresh BLE scan, and switches the Options screen into
// picker-rendering mode. Web build is a no-op (the browser owns
// the picker dialog).
void app_chessnut_open_picker(AppState& a);

// User clicked a device row — connect to that MAC, close the
// picker, cache the address for next time. Web no-op.
void app_chessnut_pick_device(AppState& a, const std::string& address);

// Cancel the picker without connecting (back arrow / explicit
// cancel button).
void app_chessnut_close_picker(AppState& a);

// Delete the cached MAC address (~/.cache/chessnut_bridge_address)
// so the next connect goes through the picker fresh. Wired to
// the "Forget" button in the picker header. Web no-op (the
// browser owns its own permission cache).
void app_chessnut_forget_cached_device(AppState& a);

// Continuous-mode driver bridge. Defined per-platform: main.cpp on
// desktop, web/voice_web.cpp on web. Wired to the Continuous voice
// row in the Options screen. Hides the start/stop plumbing
// (whisper.cpp threads or browser SpeechRecognition) behind a
// platform-neutral signature.
void app_voice_toggle_continuous_request(AppState& a);

// Reports whether continuous voice can be enabled at all on this
// build (and, for web, whether the browser supports
// SpeechRecognition). Defined per-platform; lets the Options UI
// hide the row when there's no chance of it working.
bool app_voice_continuous_supported();

// "Speak moves" toggle — flips the voice-output direction (TTS).
// Lazy-initialises voice_tts on first enable; idempotent.
void app_voice_toggle_speak_moves_request(AppState& a);

// GUI-thread tail for continuous-mode utterances and live partial
// transcripts. Both ungated so the web driver can call them too.
// _continuous_apply is the finalize-and-execute path (sibling of
// app_voice_apply_result on desktop); _apply_partial just surfaces
// the latest best-guess in the status bar. Both are no-ops when the
// toggle is off — handles the race where a worker delivers after
// the toggle flips.
void app_voice_continuous_apply(AppState& a,
                                const std::string& utterance,
                                const std::string& error);
void app_voice_continuous_apply_partial(AppState& a,
                                        const std::string& partial);
