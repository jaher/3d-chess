// SDL2 + Emscripten driver for the WebAssembly build.
//
// Replaces main.cpp + the GTK-bound parts of game_state.cpp. The renderer,
// chess rules, FEN/UCI helpers, and types are shared with the desktop build.
//
// Notes:
//   - Title-bar updates become writes to the HTML #chess-status div via EM_JS.
//   - Tick callbacks become inline animation updates inside the main loop.
//   - The AI engine is async via ai_player_web.cpp + web/stockfish-bridge.js;
//     we kick off requests with web_request_*() and poll web_ai::*_ready slots
//     each frame.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <emscripten.h>
#include <emscripten/em_macros.h>
#include <emscripten/html5.h>
#include <SDL2/SDL.h>
#include <GLES3/gl3.h>

#include "../board_renderer.h"
#include "../challenge.h"
#include "../chess_rules.h"
#include "../chess_types.h"
#include "../linalg.h"
#include "../stl_model.h"
#include "../ai_player.h"

// ---------------------------------------------------------------------------
// Bridge into web/ai_player_web.cpp
// ---------------------------------------------------------------------------
extern void web_request_ai_move(const std::string& fen, int movetime_ms);
extern void web_request_eval(const std::string& fen, int movetime_ms, int score_index);

namespace web_ai {
    extern bool        move_ready;
    extern std::string move_uci;
    extern bool eval_ready;
    extern int  eval_cp;
    extern int  eval_index;
}

// ---------------------------------------------------------------------------
// HTML status updates (replaces gtk_window_set_title)
// ---------------------------------------------------------------------------
EM_JS(void, set_status, (const char* s), {
    var el = document.getElementById('chess-status');
    if (el) el.textContent = UTF8ToString(s);
});

static void set_status_str(const std::string& s) { set_status(s.c_str()); }

// ---------------------------------------------------------------------------
// SDL / GL state
// ---------------------------------------------------------------------------
static SDL_Window* g_window = nullptr;
static SDL_GLContext g_gl_ctx = nullptr;
static int g_width = 1024;
static int g_height = 768;

// Camera (same as desktop main.cpp)
static float g_rot_x = 30.0f;
static float g_rot_y = 180.0f;
static float g_zoom  = 12.0f;

// Game state (we own this directly instead of going through game_state.cpp,
// which is GTK-bound).
static GameState g_state;
static StlModel  g_loaded_models[PIECE_COUNT];

// Mode and menu/challenge state (mirrors the desktop globals).
static GameMode g_mode = MODE_MENU;
static std::vector<PhysicsPiece> g_menu_pieces;
static double g_menu_start_time = 0;   // seconds
static double g_menu_last_update = 0;
static int g_menu_hover = 0;

static std::vector<std::string> g_challenge_files;
static std::vector<std::string> g_challenge_names;
static int g_challenge_select_hover = -1;
static Challenge g_current_challenge;
static int g_challenge_moves_made = 0;
static bool g_challenge_solved = false;
static bool g_challenge_next_hover = false;
static std::vector<std::vector<std::string>> g_challenge_solutions;
static bool g_challenge_show_summary = false;

// Glass shatter transition
static bool g_transition_active = false;
static int g_transition_pending_next = -1;
static double g_transition_start_time = 0;
static const float g_transition_duration = 1.3f;

// AI animation timing — game_state.cpp uses g_get_monotonic_time() in
// microseconds; we use emscripten_get_now() in milliseconds and convert.
static double g_ai_anim_start_sec = 0;

// Drag state
static bool g_dragging = false;
static double g_last_mouse_x = 0, g_last_mouse_y = 0;
static double g_press_x = 0, g_press_y = 0;

// ---------------------------------------------------------------------------
// FEN string helper for AI requests
// ---------------------------------------------------------------------------
static std::string current_fen(bool white_turn) {
    BoardSquare board[8][8];
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            board[r][c] = {-1, false};
    for (const auto& p : g_state.pieces)
        if (p.alive)
            board[p.row][p.col] = {p.type, p.is_white};
    return board_to_fen(board, white_turn,
        g_state.castling.white_king_moved, g_state.castling.black_king_moved,
        g_state.castling.white_rook_a_moved, g_state.castling.white_rook_h_moved,
        g_state.castling.black_rook_a_moved, g_state.castling.black_rook_h_moved);
}

// ---------------------------------------------------------------------------
// Status / title strings (mirrors game_state.cpp)
// ---------------------------------------------------------------------------
static void update_status() {
    if (g_state.ai_thinking) {
        set_status_str("AI thinking...");
        return;
    }
    if (g_state.game_over) {
        set_status_str("3D Chess — " + g_state.game_result);
        return;
    }
    float score = evaluate_position(g_state);
    char buf[160];
    const char* turn = g_state.white_turn ? "White" : "Black";
    bool check = is_in_check(g_state, g_state.white_turn);
    if (std::abs(score) < 0.1f) {
        std::snprintf(buf, sizeof(buf), "3D Chess — %s to move%s — Even",
                      turn, check ? " (CHECK)" : "");
    } else if (score > 0) {
        std::snprintf(buf, sizeof(buf), "3D Chess — %s to move%s — White +%.1f",
                      turn, check ? " (CHECK)" : "", score);
    } else {
        std::snprintf(buf, sizeof(buf), "3D Chess — %s to move%s — Black +%.1f",
                      turn, check ? " (CHECK)" : "", -score);
    }
    set_status(buf);
}

static void update_analysis_status() {
    int total = static_cast<int>(g_state.snapshots.size()) - 1;
    char buf[160];
    if (g_state.analysis_index == 0) {
        std::snprintf(buf, sizeof(buf),
                      "ANALYSIS — Starting position [0/%d] — Left/Right to navigate, Esc to exit",
                      total);
    } else {
        const auto& snap = g_state.snapshots[g_state.analysis_index];
        int move_num = (g_state.analysis_index + 1) / 2;
        const char* side = (g_state.analysis_index % 2 == 1) ? "White" : "Black";
        std::snprintf(buf, sizeof(buf),
                      "ANALYSIS — Move %d. %s %s [%d/%d] — Left/Right, Esc to exit",
                      move_num, side, snap.last_move.c_str(),
                      g_state.analysis_index, total);
    }
    set_status(buf);
}

// ---------------------------------------------------------------------------
// Game lifecycle (lifted from game_state.cpp::game_reset)
// ---------------------------------------------------------------------------
static void game_local_reset() {
    g_state.pieces = build_starting_position();
    g_state.white_turn = true;
    g_state.castling = CastlingRights();
    g_state.selected_col = -1;
    g_state.selected_row = -1;
    g_state.valid_moves.clear();
    g_state.game_over = false;
    g_state.game_result.clear();
    g_state.ai_thinking = false;
    g_state.ai_animating = false;
    g_state.analysis_mode = false;
    g_state.analysis_index = 0;
    g_state.move_history.clear();
    g_state.score_history.clear();
    g_state.snapshots.clear();
    g_state.rebuild_grid();
    g_state.score_history.push_back(evaluate_position(g_state));
    g_state.take_snapshot();
}

static void game_local_enter_analysis() {
    if (g_state.ai_thinking || g_state.ai_animating) return;
    g_state.analysis_mode = true;
    g_state.analysis_index = static_cast<int>(g_state.snapshots.size()) - 1;
    g_state.live_pieces = g_state.pieces;
    g_state.live_white_turn = g_state.white_turn;
    g_state.live_castling = g_state.castling;
    g_state.selected_col = g_state.selected_row = -1;
    g_state.valid_moves.clear();
}

static void game_local_exit_analysis() {
    g_state.analysis_mode = false;
    g_state.pieces = g_state.live_pieces;
    g_state.white_turn = g_state.live_white_turn;
    g_state.castling = g_state.live_castling;
    g_state.rebuild_grid();
}

// ---------------------------------------------------------------------------
// Trigger the AI: kick off an async Stockfish move + eval request.
// ---------------------------------------------------------------------------
static void trigger_ai() {
    g_state.ai_thinking = true;
    set_status("3D Chess — AI thinking...");
    std::string fen = current_fen(false);
    int movetime = 800;
    if (const char* v = std::getenv("CHESS_AI_MOVETIME_MS")) movetime = std::atoi(v);
    if (movetime <= 0) movetime = 800;
    std::printf("AI thinking... FEN: %s\n", fen.c_str());
    web_request_ai_move(fen, movetime);
}

static void trigger_eval(int score_index) {
    std::string fen = current_fen(g_state.white_turn);
    int movetime = 150;
    if (const char* v = std::getenv("CHESS_EVAL_MOVETIME_MS")) movetime = std::atoi(v);
    if (movetime <= 0) movetime = 150;
    web_request_eval(fen, movetime, score_index);
}

// ---------------------------------------------------------------------------
// Screen-to-board picking (verbatim from main.cpp::screen_to_board)
// ---------------------------------------------------------------------------
static bool screen_to_board(double mx, double my, int width, int height,
                            int& out_col, int& out_row) {
    float deg2rad = static_cast<float>(M_PI) / 180.0f;
    float aspect = static_cast<float>(width) / static_cast<float>(height);
    Mat4 view = mat4_multiply(
        mat4_translate(0, 0, -g_zoom),
        mat4_multiply(mat4_rotate_x(g_rot_x * deg2rad),
                      mat4_multiply(mat4_rotate_y(g_rot_y * deg2rad),
                                    mat4_translate(0, -BOARD_Y, 0))));
    Mat4 proj = mat4_perspective(45.0f * deg2rad, aspect, 0.1f, 100.0f);
    Mat4 inv_vp = mat4_inverse(mat4_multiply(proj, view));

    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;

    Vec4 nw = mat4_mul_vec4(inv_vp, {ndc_x, ndc_y, -1, 1});
    Vec4 fw = mat4_mul_vec4(inv_vp, {ndc_x, ndc_y,  1, 1});
    if (std::abs(nw.w) < 1e-10f || std::abs(fw.w) < 1e-10f) return false;

    float np[3] = {nw.x/nw.w, nw.y/nw.w, nw.z/nw.w};
    float fp[3] = {fw.x/fw.w, fw.y/fw.w, fw.z/fw.w};
    float d[3] = {fp[0]-np[0], fp[1]-np[1], fp[2]-np[2]};
    if (std::abs(d[1]) < 1e-10f) return false;

    float t = (BOARD_Y - np[1]) / d[1];
    if (t < 0) return false;

    out_col = static_cast<int>(std::floor((np[0] + t*d[0]) / SQ + 4.0f));
    out_row = static_cast<int>(std::floor((np[2] + t*d[2]) / SQ + 4.0f));
    return in_bounds(out_col, out_row);
}

// ---------------------------------------------------------------------------
// Board click handling (mirrors main.cpp::handle_board_click)
// ---------------------------------------------------------------------------
static void handle_board_click(double mx, double my) {
    bool is_challenge = (g_mode == MODE_CHALLENGE);
    bool is_normal_game = (g_mode == MODE_PLAYING);

    if (is_normal_game) {
        if (g_state.ai_thinking || g_state.ai_animating || g_state.analysis_mode ||
            !g_state.white_turn || g_state.game_over)
            return;
    } else if (is_challenge) {
        if (g_state.game_over || g_challenge_solved) return;
        int max_moves = g_current_challenge.max_moves;
        if (max_moves > 0) {
            bool starter_to_move = (g_state.white_turn == g_current_challenge.starts_white);
            if (starter_to_move && g_challenge_moves_made >= max_moves) return;
        }
    }

    int col, row;
    if (!screen_to_board(mx, my, g_width, g_height, col, row)) {
        g_state.selected_col = g_state.selected_row = -1;
        g_state.valid_moves.clear();
        return;
    }

    if (g_state.selected_col >= 0) {
        for (const auto& [mc, mr] : g_state.valid_moves) {
            if (mc == col && mr == row) {
                bool was_starter = (g_state.white_turn == g_current_challenge.starts_white);
                execute_move(g_state, g_state.selected_col, g_state.selected_row, col, row);
                g_state.selected_col = g_state.selected_row = -1;
                g_state.valid_moves.clear();

                if (is_challenge) {
                    if (was_starter) g_challenge_moves_made++;
                    if (!g_state.move_history.empty() && g_state.snapshots.size() >= 2) {
                        int pi = g_current_challenge.current_index;
                        if (pi >= 0 && pi < static_cast<int>(g_challenge_solutions.size())) {
                            const auto& before = g_state.snapshots[g_state.snapshots.size() - 2];
                            std::string alg = uci_to_algebraic(before, g_state.move_history.back());
                            g_challenge_solutions[pi].push_back(alg);
                        }
                    }
                    if (g_state.game_over) {
                        bool solved = false;
                        if (g_current_challenge.starts_white &&
                            g_state.game_result.find("White wins") != std::string::npos) solved = true;
                        if (!g_current_challenge.starts_white &&
                            g_state.game_result.find("Black wins") != std::string::npos) solved = true;
                        if (solved) g_challenge_solved = true;
                    }
                } else {
                    update_status();
                    trigger_eval(static_cast<int>(g_state.score_history.size()) - 1);
                    if (!g_state.white_turn && !g_state.game_over) trigger_ai();
                }
                return;
            }
        }
    }

    int idx = g_state.grid[row][col];
    if (idx >= 0 && g_state.pieces[idx].is_white == g_state.white_turn) {
        g_state.selected_col = col;
        g_state.selected_row = row;
        g_state.valid_moves = generate_legal_moves(g_state, col, row);
        g_state.anim_start_time = static_cast<int64_t>(emscripten_get_now() * 1000.0);
    } else {
        g_state.selected_col = g_state.selected_row = -1;
        g_state.valid_moves.clear();
    }
}

// ---------------------------------------------------------------------------
// Mode transitions
// ---------------------------------------------------------------------------
static void start_menu();
static void start_game();
static void start_challenge_select();
static void start_challenge(int index);
static void load_challenge_puzzle(int puzzle_index);
static void reset_challenge_puzzle();

static void start_menu() {
    g_mode = MODE_MENU;
    menu_init_physics(g_menu_pieces);
    g_menu_start_time = emscripten_get_now() / 1000.0;
    g_menu_last_update = g_menu_start_time;
    set_status("3D Chess");
}

static void start_game() {
    g_mode = MODE_PLAYING;
    game_local_reset();
    g_rot_x = 30.0f;
    g_rot_y = 180.0f;
    g_zoom  = 12.0f;
    update_status();
}

static void start_challenge_select() {
    g_mode = MODE_CHALLENGE_SELECT;
    g_challenge_files = list_challenge_files("challenges");
    g_challenge_names.clear();
    for (const auto& f : g_challenge_files) {
        Challenge c = load_challenge(f);
        g_challenge_names.push_back(c.name);
    }
    g_challenge_select_hover = -1;
    set_status("Select Challenge");
}

static void load_challenge_puzzle(int puzzle_index) {
    if (puzzle_index < 0 || puzzle_index >= static_cast<int>(g_current_challenge.fens.size()))
        return;
    g_current_challenge.current_index = puzzle_index;
    g_challenge_moves_made = 0;
    g_challenge_solved = false;
    g_challenge_next_hover = false;
    ParsedFEN parsed = parse_fen(g_current_challenge.fens[puzzle_index]);
    if (parsed.valid) apply_fen_to_state(g_state, parsed);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Challenge: %s [%d/%d]",
                  g_current_challenge.name.c_str(),
                  puzzle_index + 1,
                  static_cast<int>(g_current_challenge.fens.size()));
    set_status(buf);
}

static void reset_challenge_puzzle() {
    int idx = g_current_challenge.current_index;
    if (idx >= 0 && idx < static_cast<int>(g_challenge_solutions.size()))
        g_challenge_solutions[idx].clear();
    load_challenge_puzzle(idx);
}

static void start_challenge(int index) {
    if (index < 0 || index >= static_cast<int>(g_challenge_files.size())) return;
    g_current_challenge = load_challenge(g_challenge_files[index]);
    if (g_current_challenge.fens.empty()) return;
    g_mode = MODE_CHALLENGE;
    g_challenge_solutions.assign(g_current_challenge.fens.size(), {});
    g_challenge_show_summary = false;
    load_challenge_puzzle(0);
}

// ---------------------------------------------------------------------------
// SDL event dispatch
// ---------------------------------------------------------------------------
static void on_button_press(const SDL_MouseButtonEvent& e) {
    if (e.button == SDL_BUTTON_LEFT) {
        g_dragging = true;
        g_last_mouse_x = g_press_x = e.x;
        g_last_mouse_y = g_press_y = e.y;
    }
}

static void on_button_release(const SDL_MouseButtonEvent& e) {
    if (e.button != SDL_BUTTON_LEFT) return;
    g_dragging = false;
    int w = g_width, h = g_height;

    if (g_mode == MODE_MENU) {
        int btn = menu_hit_test(e.x, e.y, w, h);
        if (btn == 1) start_game();
        else if (btn == 2) { /* quit no-op in browser */ }
        else if (btn == 3) start_challenge_select();
    } else if (g_mode == MODE_CHALLENGE_SELECT) {
        int idx = challenge_select_hit_test(e.x, e.y, w, h,
                                             static_cast<int>(g_challenge_names.size()));
        if (idx == -2) start_menu();
        else if (idx >= 0) start_challenge(idx);
    } else if (g_mode == MODE_CHALLENGE && g_challenge_show_summary) {
        start_menu();
    } else if (g_mode == MODE_CHALLENGE && g_challenge_solved && !g_transition_active) {
        if (next_button_hit_test(e.x, e.y, w, h)) {
            int next = g_current_challenge.current_index + 1;
            if (next < static_cast<int>(g_current_challenge.fens.size())) {
                g_transition_pending_next = next;
            } else {
                g_challenge_show_summary = true;
            }
        }
    } else {
        double dx = e.x - g_press_x, dy = e.y - g_press_y;
        if (dx*dx + dy*dy < 25.0) handle_board_click(e.x, e.y);
    }
}

static void on_motion(const SDL_MouseMotionEvent& e) {
    int w = g_width, h = g_height;
    if (g_mode == MODE_MENU) {
        g_menu_hover = menu_hit_test(e.x, e.y, w, h);
        return;
    }
    if (g_mode == MODE_CHALLENGE_SELECT) {
        g_challenge_select_hover = challenge_select_hit_test(
            e.x, e.y, w, h, static_cast<int>(g_challenge_names.size()));
        return;
    }
    if (g_mode == MODE_CHALLENGE && g_challenge_solved) {
        g_challenge_next_hover = next_button_hit_test(e.x, e.y, w, h);
    }
    if (g_dragging) {
        g_rot_y += static_cast<float>(e.x - g_last_mouse_x) * 0.3f;
        g_rot_x += static_cast<float>(e.y - g_last_mouse_y) * 0.3f;
        if (g_rot_x < 5.0f) g_rot_x = 5.0f;
        if (g_rot_x > 89.0f) g_rot_x = 89.0f;
        g_last_mouse_x = e.x;
        g_last_mouse_y = e.y;
    }
}

static void on_scroll(const SDL_MouseWheelEvent& e) {
    g_zoom -= static_cast<float>(e.y) * 0.5f;
    if (g_zoom < 3.0f)  g_zoom = 3.0f;
    if (g_zoom > 40.0f) g_zoom = 40.0f;
}

static void on_key(const SDL_KeyboardEvent& e) {
    SDL_Keycode k = e.keysym.sym;

    if (g_mode == MODE_CHALLENGE) {
        if (k == SDLK_ESCAPE) { reset_challenge_puzzle(); return; }
        if (k == SDLK_m)      { start_menu(); return; }
        return;
    }
    if (g_mode == MODE_CHALLENGE_SELECT) {
        if (k == SDLK_ESCAPE) start_menu();
        return;
    }

    if (g_state.analysis_mode) {
        if (k == SDLK_LEFT && g_state.analysis_index > 0) {
            g_state.analysis_index--;
            g_state.restore_snapshot(g_state.analysis_index);
            update_analysis_status();
        } else if (k == SDLK_RIGHT &&
                   g_state.analysis_index < static_cast<int>(g_state.snapshots.size()) - 1) {
            g_state.analysis_index++;
            g_state.restore_snapshot(g_state.analysis_index);
            update_analysis_status();
        } else if (k == SDLK_ESCAPE) {
            game_local_exit_analysis();
            update_status();
        }
    } else {
        if (k == SDLK_a || k == SDLK_LEFT || k == SDLK_RIGHT) {
            if (!g_state.ai_thinking && !g_state.ai_animating && g_state.snapshots.size() > 1) {
                game_local_enter_analysis();
                if (k == SDLK_LEFT && g_state.analysis_index > 0) g_state.analysis_index--;
                g_state.restore_snapshot(g_state.analysis_index);
                update_analysis_status();
            }
        }
    }
}

// ---------------------------------------------------------------------------
// AI move animation lifecycle (mirrors game_state.cpp::on_ai_move_ready /
// on_ai_anim_tick, but inline in the per-frame loop).
// ---------------------------------------------------------------------------
static void start_ai_animation_from_uci(const std::string& uci) {
    int fc, fr, tc, tr;
    bool legal = false;
    if (!uci.empty() && parse_uci_move(uci, fc, fr, tc, tr)) {
        // Sanity check: source has a black piece, dest is in legal moves.
        int idx = g_state.grid[fr][fc];
        if (idx >= 0 && !g_state.pieces[idx].is_white) {
            auto legal_moves = generate_legal_moves(g_state, fc, fr);
            for (const auto& [mc, mr] : legal_moves) {
                if (mc == tc && mr == tr) { legal = true; break; }
            }
        }
    }

    if (!legal) {
        // Fallback: pick a random legal black move.
        std::vector<std::pair<std::pair<int,int>, std::pair<int,int>>> all_legal;
        for (const auto& p : g_state.pieces) {
            if (!p.alive || p.is_white) continue;
            auto moves = generate_legal_moves(g_state, p.col, p.row);
            for (const auto& [mc, mr] : moves)
                all_legal.push_back({{p.col, p.row}, {mc, mr}});
        }
        if (all_legal.empty()) {
            g_state.ai_thinking = false;
            update_status();
            return;
        }
        int idx = static_cast<int>(static_cast<int64_t>(emscripten_get_now()) % all_legal.size());
        auto& [from, to] = all_legal[idx];
        fc = from.first;  fr = from.second;
        tc = to.first;    tr = to.second;
        std::printf("AI fallback: %s\n", move_to_uci(fc, fr, tc, tr).c_str());
    } else {
        std::printf("AI plays: %s (legal)\n", move_to_uci(fc, fr, tc, tr).c_str());
    }

    g_state.ai_from_col = fc;
    g_state.ai_from_row = fr;
    g_state.ai_to_col = tc;
    g_state.ai_to_row = tr;
    g_state.ai_animating = true;
    g_ai_anim_start_sec = emscripten_get_now() / 1000.0;
}

static void tick_ai_animation() {
    if (!g_state.ai_animating) return;
    double now = emscripten_get_now() / 1000.0;
    float elapsed = static_cast<float>(now - g_ai_anim_start_sec);
    if (elapsed < g_state.ai_anim_duration) {
        // Update the anim_start_time field the renderer reads.
        g_state.ai_anim_start = static_cast<int64_t>(g_ai_anim_start_sec * 1e6);
        return;
    }
    g_state.ai_animating = false;
    execute_move(g_state, g_state.ai_from_col, g_state.ai_from_row,
                 g_state.ai_to_col, g_state.ai_to_row);
    g_state.ai_thinking = false;
    update_status();
    trigger_eval(static_cast<int>(g_state.score_history.size()) - 1);
}

// ---------------------------------------------------------------------------
// Per-frame eval result handler
// ---------------------------------------------------------------------------
static void poll_eval_result() {
    if (!web_ai::eval_ready) return;
    web_ai::eval_ready = false;
    if (web_ai::eval_index < 0 ||
        web_ai::eval_index >= static_cast<int>(g_state.score_history.size())) {
        return;
    }
    int cp = web_ai::eval_cp;
    int mate_threshold = 30000 - 100;
    float pawn_units;
    if (cp >= mate_threshold) {
        pawn_units = 100.0f - static_cast<float>(30000 - cp);
    } else if (cp <= -mate_threshold) {
        pawn_units = -(100.0f - static_cast<float>(30000 + cp));
    } else {
        pawn_units = cp / 100.0f;
    }
    g_state.score_history[web_ai::eval_index] = pawn_units;
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
static void main_loop_iter() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_MOUSEBUTTONDOWN: on_button_press(ev.button); break;
            case SDL_MOUSEBUTTONUP:   on_button_release(ev.button); break;
            case SDL_MOUSEMOTION:     on_motion(ev.motion); break;
            case SDL_MOUSEWHEEL:      on_scroll(ev.wheel); break;
            case SDL_KEYDOWN:         on_key(ev.key); break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                    ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    g_width  = ev.window.data1;
                    g_height = ev.window.data2;
                }
                break;
        }
    }

    // AI move result
    if (web_ai::move_ready) {
        web_ai::move_ready = false;
        if (g_state.ai_thinking && !g_state.ai_animating) {
            start_ai_animation_from_uci(web_ai::move_uci);
        }
    }
    tick_ai_animation();
    poll_eval_result();

    // Menu physics tick
    if (g_mode == MODE_MENU) {
        double now = emscripten_get_now() / 1000.0;
        float dt = static_cast<float>(now - g_menu_last_update);
        g_menu_last_update = now;
        if (dt > 0.05f) dt = 0.05f;
        menu_update_physics(g_menu_pieces, dt);
    }

    // ---- Render ----
    glViewport(0, 0, g_width, g_height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    if (g_mode == MODE_MENU) {
        float t = static_cast<float>(emscripten_get_now() / 1000.0 - g_menu_start_time);
        renderer_draw_menu(g_menu_pieces, g_width, g_height, t, g_menu_hover);
    } else if (g_mode == MODE_CHALLENGE_SELECT) {
        renderer_draw_challenge_select(g_challenge_names, g_width, g_height,
                                        g_challenge_select_hover);
    } else if (g_mode == MODE_CHALLENGE && g_challenge_show_summary) {
        std::vector<SummaryEntry> entries;
        for (size_t i = 0; i < g_challenge_solutions.size(); i++) {
            SummaryEntry e;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "Puzzle %zu", i + 1);
            e.puzzle_name = buf;
            e.moves = g_challenge_solutions[i];
            entries.push_back(e);
        }
        renderer_draw_challenge_summary(g_current_challenge.name, entries,
                                        g_width, g_height);
    } else {
        bool save_game_over = false;
        std::string save_result;
        if (g_mode == MODE_CHALLENGE) {
            save_game_over = g_state.game_over;
            save_result = g_state.game_result;
            g_state.game_over = false;
            g_state.game_result.clear();
        }

        renderer_draw(g_state, g_width, g_height, g_rot_x, g_rot_y, g_zoom);

        if (g_mode == MODE_CHALLENGE) {
            g_state.game_over = save_game_over;
            g_state.game_result = save_result;

            renderer_draw_challenge_overlay(
                g_current_challenge.name,
                g_current_challenge.current_index,
                static_cast<int>(g_current_challenge.fens.size()),
                g_challenge_moves_made,
                g_current_challenge.max_moves,
                g_current_challenge.starts_white,
                g_width, g_height);
            if (g_challenge_solved && !g_transition_active && g_transition_pending_next < 0)
                renderer_draw_next_button(g_width, g_height, g_challenge_next_hover);

            if (g_transition_pending_next >= 0) {
                renderer_capture_frame(g_width, g_height);
                load_challenge_puzzle(g_transition_pending_next);
                g_transition_pending_next = -1;
                g_transition_active = true;
                g_transition_start_time = emscripten_get_now() / 1000.0;
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                renderer_draw(g_state, g_width, g_height, g_rot_x, g_rot_y, g_zoom);
                renderer_draw_challenge_overlay(
                    g_current_challenge.name,
                    g_current_challenge.current_index,
                    static_cast<int>(g_current_challenge.fens.size()),
                    g_challenge_moves_made,
                    g_current_challenge.max_moves,
                    g_current_challenge.starts_white,
                    g_width, g_height);
            }

            if (g_transition_active) {
                float t = static_cast<float>(emscripten_get_now() / 1000.0 - g_transition_start_time);
                if (t >= g_transition_duration) {
                    g_transition_active = false;
                } else {
                    renderer_draw_shatter(t, g_width, g_height);
                }
            }
        }
    }

    SDL_GL_SwapWindow(g_window);
}

// ---------------------------------------------------------------------------
// Main / startup
// ---------------------------------------------------------------------------
int main(int /*argc*/, char* /*argv*/[]) {
    // Load STL piece models from the preloaded virtual filesystem.
    // Sequential is fine — happens once, ~12 small files.
    std::printf("Loading models...\n");
    for (int i = 0; i < PIECE_COUNT; i++) {
        std::string path = std::string("/models/") + piece_filenames[i];
        g_loaded_models[i].load(path);
        std::printf("  %s: %zu triangles\n",
                    piece_filenames[i], g_loaded_models[i].triangle_count());
    }
    std::printf("All models loaded.\n");

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    g_window = SDL_CreateWindow("3D Chess",
                                 SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                 g_width, g_height,
                                 SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!g_window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    g_gl_ctx = SDL_GL_CreateContext(g_window);
    if (!g_gl_ctx) {
        std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return 1;
    }

    renderer_init(g_loaded_models);
    game_local_reset();
    start_menu();

    emscripten_set_main_loop(main_loop_iter, 0, 1);
    return 0;
}
