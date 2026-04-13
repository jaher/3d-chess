#include "app_state.h"

#include "ai_player.h"
#include "chess_rules.h"
#include "linalg.h"

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// ===========================================================================
// Small helpers
// ===========================================================================
namespace {

int64_t now_us(const AppState& a) {
    return a.platform ? a.platform->now_us() : 0;
}

void set_status(const AppState& a, const char* text) {
    if (a.platform && a.platform->set_status) a.platform->set_status(text);
}

void queue_redraw(const AppState& a) {
    if (a.platform && a.platform->queue_redraw) a.platform->queue_redraw();
}

std::string current_fen(const GameState& gs, bool white_turn) {
    BoardSquare board[8][8];
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            board[r][c] = {-1, false};
    for (const auto& p : gs.pieces)
        if (p.alive)
            board[p.row][p.col] = {p.type, p.is_white};
    return board_to_fen(board, white_turn,
        gs.castling.white_king_moved,  gs.castling.black_king_moved,
        gs.castling.white_rook_a_moved, gs.castling.white_rook_h_moved,
        gs.castling.black_rook_a_moved, gs.castling.black_rook_h_moved);
}

int env_int(const char* name, int fallback) {
    if (const char* v = std::getenv(name)) {
        int n = std::atoi(v);
        if (n > 0) return n;
    }
    return fallback;
}

} // namespace

// ===========================================================================
// Status text formatting
// ===========================================================================
static void refresh_play_status(AppState& a) {
    const GameState& gs = a.game;
    if (gs.ai_thinking) {
        set_status(a, "3D Chess — AI thinking...");
        return;
    }
    if (gs.game_over) {
        std::string s = "3D Chess — " + gs.game_result;
        set_status(a, s.c_str());
        return;
    }
    float score = evaluate_position(gs);
    char buf[160];
    const char* turn = gs.white_turn ? "White" : "Black";
    bool check = is_in_check(const_cast<GameState&>(gs), gs.white_turn);
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
    set_status(a, buf);
}

static void refresh_analysis_status(AppState& a) {
    const GameState& gs = a.game;
    int total = static_cast<int>(gs.snapshots.size()) - 1;
    char buf[160];
    if (gs.analysis_index == 0) {
        std::snprintf(buf, sizeof(buf),
                      "ANALYSIS — Starting position [0/%d] — Left/Right to navigate, Esc to exit",
                      total);
    } else {
        const auto& snap = gs.snapshots[gs.analysis_index];
        int move_num = (gs.analysis_index + 1) / 2;
        const char* side = (gs.analysis_index % 2 == 1) ? "White" : "Black";
        std::snprintf(buf, sizeof(buf),
                      "ANALYSIS — Move %d. %s %s [%d/%d] — Left/Right, Esc to exit",
                      move_num, side, snap.last_move.c_str(),
                      gs.analysis_index, total);
    }
    set_status(a, buf);
}

void app_refresh_status(AppState& a) {
    if (a.game.analysis_mode) refresh_analysis_status(a);
    else                       refresh_play_status(a);
}

// ===========================================================================
// Async AI / eval dispatch (through the platform)
// ===========================================================================
static void trigger_ai(AppState& a) {
    a.game.ai_thinking = true;
    set_status(a, "3D Chess — AI thinking...");
    queue_redraw(a);
    std::string fen = current_fen(a.game, /*white_turn=*/false);
    int movetime = env_int("CHESS_AI_MOVETIME_MS", 800);
    if (a.platform && a.platform->trigger_ai_move)
        a.platform->trigger_ai_move(fen.c_str(), movetime);
}

static void trigger_eval(AppState& a, int score_index) {
    std::string fen = current_fen(a.game, a.game.white_turn);
    int movetime = env_int("CHESS_EVAL_MOVETIME_MS", 150);
    if (a.platform && a.platform->trigger_eval)
        a.platform->trigger_eval(fen.c_str(), movetime, score_index);
}

// ===========================================================================
// Screen-to-board picking
// ===========================================================================
static bool screen_to_board(const AppState& a, double mx, double my,
                            int width, int height, int& out_col, int& out_row) {
    float deg2rad = static_cast<float>(M_PI) / 180.0f;
    float aspect = static_cast<float>(width) / static_cast<float>(height);
    Mat4 view = mat4_multiply(
        mat4_translate(0, 0, -a.zoom),
        mat4_multiply(mat4_rotate_x(a.rot_x * deg2rad),
                      mat4_multiply(mat4_rotate_y(a.rot_y * deg2rad),
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

// ===========================================================================
// Board click handling
// ===========================================================================
static void handle_board_click(AppState& a, double mx, double my,
                               int width, int height) {
    GameState& gs = a.game;
    bool is_challenge = (a.mode == MODE_CHALLENGE);
    bool is_normal_game = (a.mode == MODE_PLAYING);

    if (is_normal_game) {
        if (gs.ai_thinking || gs.ai_animating || gs.analysis_mode ||
            !gs.white_turn || gs.game_over)
            return;
    } else if (is_challenge) {
        if (gs.game_over || a.challenge_solved) return;
        int max_moves = a.current_challenge.max_moves;
        if (max_moves > 0) {
            bool starter_to_move =
                (gs.white_turn == a.current_challenge.starts_white);
            if (starter_to_move && a.challenge_moves_made >= max_moves) return;
        }
    }

    int col, row;
    if (!screen_to_board(a, mx, my, width, height, col, row)) {
        gs.selected_col = gs.selected_row = -1;
        gs.valid_moves.clear();
        queue_redraw(a);
        return;
    }

    if (gs.selected_col >= 0) {
        for (const auto& [mc, mr] : gs.valid_moves) {
            if (mc == col && mr == row) {
                bool was_starter =
                    (gs.white_turn == a.current_challenge.starts_white);
                execute_move(gs, gs.selected_col, gs.selected_row, col, row);
                gs.selected_col = gs.selected_row = -1;
                gs.valid_moves.clear();
                queue_redraw(a);

                if (is_challenge) {
                    if (was_starter) a.challenge_moves_made++;
                    if (!gs.move_history.empty() && gs.snapshots.size() >= 2) {
                        int pi = a.current_challenge.current_index;
                        if (pi >= 0 &&
                            pi < static_cast<int>(a.challenge_solutions.size())) {
                            const auto& before =
                                gs.snapshots[gs.snapshots.size() - 2];
                            std::string alg = uci_to_algebraic(
                                before, gs.move_history.back());
                            a.challenge_solutions[pi].push_back(alg);
                        }
                    }
                    if (gs.game_over) {
                        bool solved = false;
                        if (a.current_challenge.starts_white &&
                            gs.game_result.find("White wins") != std::string::npos)
                            solved = true;
                        if (!a.current_challenge.starts_white &&
                            gs.game_result.find("Black wins") != std::string::npos)
                            solved = true;
                        if (solved) a.challenge_solved = true;
                    }
                } else {
                    app_refresh_status(a);
                    trigger_eval(
                        a, static_cast<int>(gs.score_history.size()) - 1);
                    if (!gs.white_turn && !gs.game_over)
                        trigger_ai(a);
                }
                return;
            }
        }
    }

    int idx = gs.grid[row][col];
    if (idx >= 0 && gs.pieces[idx].is_white == gs.white_turn) {
        gs.selected_col = col;
        gs.selected_row = row;
        gs.valid_moves = generate_legal_moves(gs, col, row);
        gs.anim_start_time = now_us(a);
    } else {
        gs.selected_col = gs.selected_row = -1;
        gs.valid_moves.clear();
    }
    queue_redraw(a);
}

// ===========================================================================
// Mode transitions
// ===========================================================================
void app_enter_menu(AppState& a) {
    a.mode = MODE_MENU;
    menu_init_physics(a.menu_pieces);
    a.menu_start_time_us  = now_us(a);
    a.menu_last_update_us = a.menu_start_time_us;
    set_status(a, "3D Chess");
    queue_redraw(a);
}

void app_enter_game(AppState& a) {
    a.mode = MODE_PLAYING;
    game_reset(a.game);
    a.rot_x = 30.0f;
    a.rot_y = 180.0f;
    a.zoom  = 12.0f;
    app_refresh_status(a);
    queue_redraw(a);
}

void app_enter_challenge_select(AppState& a) {
    a.mode = MODE_CHALLENGE_SELECT;
    a.challenge_files = list_challenge_files("challenges");
    a.challenge_names.clear();
    for (const auto& f : a.challenge_files) {
        Challenge c = load_challenge(f);
        a.challenge_names.push_back(c.name);
    }
    a.challenge_select_hover = -1;
    set_status(a, "Select Challenge");
    queue_redraw(a);
}

void app_load_challenge_puzzle(AppState& a, int puzzle_index) {
    if (puzzle_index < 0 ||
        puzzle_index >= static_cast<int>(a.current_challenge.fens.size()))
        return;
    a.current_challenge.current_index = puzzle_index;
    a.challenge_moves_made = 0;
    a.challenge_solved = false;
    a.challenge_next_hover = false;
    ParsedFEN parsed = parse_fen(a.current_challenge.fens[puzzle_index]);
    if (parsed.valid) apply_fen_to_state(a.game, parsed);
    char buf[160];
    std::snprintf(buf, sizeof(buf), "Challenge: %s [%d/%d]",
                  a.current_challenge.name.c_str(),
                  puzzle_index + 1,
                  static_cast<int>(a.current_challenge.fens.size()));
    set_status(a, buf);
    queue_redraw(a);
}

void app_reset_challenge_puzzle(AppState& a) {
    int idx = a.current_challenge.current_index;
    if (idx >= 0 &&
        idx < static_cast<int>(a.challenge_solutions.size()))
        a.challenge_solutions[idx].clear();
    app_load_challenge_puzzle(a, idx);
}

void app_enter_challenge(AppState& a, int index) {
    if (index < 0 ||
        index >= static_cast<int>(a.challenge_files.size())) return;
    a.current_challenge = load_challenge(a.challenge_files[index]);
    if (a.current_challenge.fens.empty()) return;
    a.mode = MODE_CHALLENGE;
    a.challenge_solutions.assign(a.current_challenge.fens.size(), {});
    a.challenge_show_summary = false;
    app_load_challenge_puzzle(a, 0);
}

// ===========================================================================
// Input
// ===========================================================================
void app_press(AppState& a, double mx, double my) {
    a.dragging = true;
    a.last_mouse_x = a.press_x = mx;
    a.last_mouse_y = a.press_y = my;
}

void app_release(AppState& a, double mx, double my, int width, int height) {
    a.dragging = false;

    if (a.mode == MODE_MENU) {
        int btn = menu_hit_test(mx, my, width, height);
        if (btn == 1)      app_enter_game(a);
        else if (btn == 2) {
#ifndef __EMSCRIPTEN__
            std::exit(0);  // Desktop quit
#endif
        }
        else if (btn == 3) app_enter_challenge_select(a);
        return;
    }

    if (a.mode == MODE_CHALLENGE_SELECT) {
        int idx = challenge_select_hit_test(
            mx, my, width, height,
            static_cast<int>(a.challenge_names.size()));
        if (idx == -2)      app_enter_menu(a);
        else if (idx >= 0)  app_enter_challenge(a, idx);
        return;
    }

    if (a.mode == MODE_CHALLENGE && a.challenge_show_summary) {
        app_enter_menu(a);
        return;
    }

    if (a.mode == MODE_CHALLENGE && a.challenge_solved && !a.transition_active) {
        if (next_button_hit_test(mx, my, width, height)) {
            int next = a.current_challenge.current_index + 1;
            if (next < static_cast<int>(a.current_challenge.fens.size())) {
                a.transition_pending_next = next;
            } else {
                a.challenge_show_summary = true;
            }
            queue_redraw(a);
        }
        return;
    }

    // Regular game / challenge board interaction: only treat as a click
    // if the pointer didn't move much between press and release.
    double dx = mx - a.press_x, dy = my - a.press_y;
    if (dx*dx + dy*dy < 25.0) handle_board_click(a, mx, my, width, height);
}

void app_motion(AppState& a, double mx, double my, int width, int height) {
    if (a.mode == MODE_MENU) {
        int h = menu_hit_test(mx, my, width, height);
        if (h != a.menu_hover) {
            a.menu_hover = h;
            queue_redraw(a);
        }
        return;
    }
    if (a.mode == MODE_CHALLENGE_SELECT) {
        int h = challenge_select_hit_test(
            mx, my, width, height,
            static_cast<int>(a.challenge_names.size()));
        if (h != a.challenge_select_hover) {
            a.challenge_select_hover = h;
            queue_redraw(a);
        }
        return;
    }
    if (a.mode == MODE_CHALLENGE && a.challenge_solved) {
        bool h_now = next_button_hit_test(mx, my, width, height);
        if (h_now != a.challenge_next_hover) {
            a.challenge_next_hover = h_now;
            queue_redraw(a);
        }
    }
    if (a.dragging) {
        a.rot_y += static_cast<float>(mx - a.last_mouse_x) * 0.3f;
        a.rot_x += static_cast<float>(my - a.last_mouse_y) * 0.3f;
        if (a.rot_x < 5.0f)  a.rot_x = 5.0f;
        if (a.rot_x > 89.0f) a.rot_x = 89.0f;
        a.last_mouse_x = mx;
        a.last_mouse_y = my;
        queue_redraw(a);
    }
}

void app_scroll(AppState& a, double delta) {
    a.zoom += static_cast<float>(delta) * 0.5f;
    if (a.zoom < 3.0f)  a.zoom = 3.0f;
    if (a.zoom > 40.0f) a.zoom = 40.0f;
    queue_redraw(a);
}

void app_key(AppState& a, AppKey key) {
    GameState& gs = a.game;

    if (a.mode == MODE_CHALLENGE) {
        if (key == KEY_ESCAPE) { app_reset_challenge_puzzle(a); return; }
        if (key == KEY_M)      { app_enter_menu(a); return; }
        return;
    }
    if (a.mode == MODE_CHALLENGE_SELECT) {
        if (key == KEY_ESCAPE) app_enter_menu(a);
        return;
    }

    if (gs.analysis_mode) {
        if (key == KEY_LEFT && gs.analysis_index > 0) {
            gs.analysis_index--;
            gs.restore_snapshot(gs.analysis_index);
            refresh_analysis_status(a);
            queue_redraw(a);
        } else if (key == KEY_RIGHT &&
                   gs.analysis_index <
                       static_cast<int>(gs.snapshots.size()) - 1) {
            gs.analysis_index++;
            gs.restore_snapshot(gs.analysis_index);
            refresh_analysis_status(a);
            queue_redraw(a);
        } else if (key == KEY_ESCAPE) {
            game_exit_analysis(gs);
            refresh_play_status(a);
            queue_redraw(a);
        }
        return;
    }

    if (key == KEY_A || key == KEY_LEFT || key == KEY_RIGHT) {
        if (!gs.ai_thinking && !gs.ai_animating && gs.snapshots.size() > 1) {
            game_enter_analysis(gs);
            if (key == KEY_LEFT && gs.analysis_index > 0)
                gs.analysis_index--;
            gs.restore_snapshot(gs.analysis_index);
            refresh_analysis_status(a);
            queue_redraw(a);
        }
    }
}

// ===========================================================================
// AI move animation + async result delivery
// ===========================================================================
static bool is_legal_ai_move(GameState& gs, int fc, int fr, int tc, int tr) {
    int idx = gs.grid[fr][fc];
    if (idx < 0 || gs.pieces[idx].is_white) return false;
    auto legal = generate_legal_moves(gs, fc, fr);
    for (const auto& [mc, mr] : legal)
        if (mc == tc && mr == tr) return true;
    return false;
}

static void start_ai_animation(AppState& a, int fc, int fr, int tc, int tr) {
    GameState& gs = a.game;
    gs.ai_from_col = fc;
    gs.ai_from_row = fr;
    gs.ai_to_col   = tc;
    gs.ai_to_row   = tr;
    gs.ai_animating = true;
    a.ai_anim_start_us = now_us(a);
    gs.ai_anim_start = a.ai_anim_start_us;
    queue_redraw(a);
}

static void ai_random_fallback(AppState& a) {
    GameState& gs = a.game;
    std::vector<std::pair<std::pair<int,int>, std::pair<int,int>>> all_legal;
    for (const auto& p : gs.pieces) {
        if (!p.alive || p.is_white) continue;
        auto moves = generate_legal_moves(gs, p.col, p.row);
        for (const auto& [mc, mr] : moves)
            all_legal.push_back({{p.col, p.row}, {mc, mr}});
    }
    if (all_legal.empty()) {
        gs.ai_thinking = false;
        app_refresh_status(a);
        queue_redraw(a);
        return;
    }
    int64_t t = now_us(a);
    int idx = static_cast<int>(static_cast<uint64_t>(t) % all_legal.size());
    auto& [from, to] = all_legal[idx];
    std::printf("AI fallback: %s\n",
                move_to_uci(from.first, from.second,
                            to.first,   to.second).c_str());
    start_ai_animation(a, from.first, from.second, to.first, to.second);
}

void app_ai_move_ready(AppState& a, const char* uci_c) {
    GameState& gs = a.game;

    // If we're not waiting for a move (e.g. user reset), drop the result.
    if (!gs.ai_thinking || gs.ai_animating) return;

    std::string uci = (uci_c && *uci_c) ? uci_c : "";
    int fc, fr, tc, tr;
    bool ok = false;
    if (!uci.empty() && parse_uci_move(uci, fc, fr, tc, tr)) {
        if (is_legal_ai_move(gs, fc, fr, tc, tr)) ok = true;
    }

    if (ok) {
        std::printf("AI plays: %s (legal)\n", uci.c_str());
        start_ai_animation(a, fc, fr, tc, tr);
    } else {
        if (!uci.empty())
            std::fprintf(stderr, "AI move %s rejected; falling back\n",
                         uci.c_str());
        ai_random_fallback(a);
    }
}

void app_eval_ready(AppState& a, int cp, int score_index) {
    GameState& gs = a.game;
    if (cp == INT_MIN) return;
    if (score_index < 0 ||
        score_index >= static_cast<int>(gs.score_history.size())) return;

    // Collapse mate scores to a bounded ±100 spike for display.
    int mate_threshold = 30000 - 100;
    float pawn_units;
    if (cp >= mate_threshold) {
        pawn_units = 100.0f - static_cast<float>(30000 - cp);
    } else if (cp <= -mate_threshold) {
        pawn_units = -(100.0f - static_cast<float>(30000 + cp));
    } else {
        pawn_units = cp / 100.0f;
    }
    gs.score_history[score_index] = pawn_units;
    queue_redraw(a);
}

// ===========================================================================
// Per-frame tick
// ===========================================================================
void app_tick(AppState& a) {
    GameState& gs = a.game;
    int64_t now = now_us(a);

    // Menu physics
    if (a.mode == MODE_MENU) {
        float dt = static_cast<float>(
            static_cast<double>(now - a.menu_last_update_us) / 1e6);
        a.menu_last_update_us = now;
        if (dt < 0.0f)     dt = 0.0f;
        if (dt > 0.05f)    dt = 0.05f;
        menu_update_physics(a.menu_pieces, dt);
        queue_redraw(a);
    }

    // AI move animation: when elapsed >= duration, commit the move.
    if (gs.ai_animating) {
        float elapsed =
            static_cast<float>(static_cast<double>(now - a.ai_anim_start_us) / 1e6);
        if (elapsed >= gs.ai_anim_duration) {
            gs.ai_animating = false;
            execute_move(gs, gs.ai_from_col, gs.ai_from_row,
                         gs.ai_to_col,   gs.ai_to_row);
            gs.ai_thinking = false;
            app_refresh_status(a);
            // Refresh the score graph for the position after the AI move.
            if (a.mode == MODE_PLAYING) {
                trigger_eval(a, static_cast<int>(gs.score_history.size()) - 1);
            }
        }
        queue_redraw(a);
    }

    // Selection ring pulses while a piece is selected — renderer reads
    // gs.anim_start_time directly, but we need redraws to animate it.
    if (gs.selected_col >= 0) queue_redraw(a);

    // Shatter transition has its own elapsed check in the render path;
    // we just need to keep issuing redraws while it's active.
    if (a.transition_active) queue_redraw(a);
}

// ===========================================================================
// Rendering dispatch
// ===========================================================================
void app_render(AppState& a, int width, int height) {
    GameState& gs = a.game;
    int64_t now = now_us(a);

    if (a.mode == MODE_MENU) {
        float t = static_cast<float>(
            static_cast<double>(now - a.menu_start_time_us) / 1e6);
        renderer_draw_menu(a.menu_pieces, width, height, t, a.menu_hover);
        return;
    }

    if (a.mode == MODE_CHALLENGE_SELECT) {
        renderer_draw_challenge_select(
            a.challenge_names, width, height, a.challenge_select_hover);
        return;
    }

    if (a.mode == MODE_CHALLENGE && a.challenge_show_summary) {
        std::vector<SummaryEntry> entries;
        for (size_t i = 0; i < a.challenge_solutions.size(); i++) {
            SummaryEntry e;
            char buf[32];
            std::snprintf(buf, sizeof(buf), "Puzzle %zu", i + 1);
            e.puzzle_name = buf;
            e.moves = a.challenge_solutions[i];
            entries.push_back(e);
        }
        renderer_draw_challenge_summary(
            a.current_challenge.name, entries, width, height);
        return;
    }

    // In-game / challenge board render path.

    // Suppress the desktop "wins by checkmate" overlay during challenge
    // mode — the challenge-specific "Next" button replaces it.
    bool save_game_over = false;
    std::string save_result;
    if (a.mode == MODE_CHALLENGE) {
        save_game_over = gs.game_over;
        save_result    = gs.game_result;
        gs.game_over = false;
        gs.game_result.clear();
    }

    renderer_draw(gs, width, height, a.rot_x, a.rot_y, a.zoom);

    if (a.mode != MODE_CHALLENGE) return;

    // Restore game_over before any further drawing.
    gs.game_over   = save_game_over;
    gs.game_result = save_result;

    renderer_draw_challenge_overlay(
        a.current_challenge.name,
        a.current_challenge.current_index,
        static_cast<int>(a.current_challenge.fens.size()),
        a.challenge_moves_made,
        a.current_challenge.max_moves,
        a.current_challenge.starts_white,
        width, height);

    if (a.challenge_solved && !a.transition_active &&
        a.transition_pending_next < 0) {
        renderer_draw_next_button(width, height, a.challenge_next_hover);
    }

    // Transition trigger: capture current frame, load next puzzle,
    // redraw the new state, then the shatter overlay animates the
    // captured texture fading away.
    if (a.transition_pending_next >= 0) {
        renderer_capture_frame(width, height);
        app_load_challenge_puzzle(a, a.transition_pending_next);
        a.transition_pending_next = -1;
        a.transition_active = true;
        a.transition_start_time_us = now;

        // The renderer_draw path itself clears its color/depth buffers.
        renderer_draw(gs, width, height, a.rot_x, a.rot_y, a.zoom);
        renderer_draw_challenge_overlay(
            a.current_challenge.name,
            a.current_challenge.current_index,
            static_cast<int>(a.current_challenge.fens.size()),
            a.challenge_moves_made,
            a.current_challenge.max_moves,
            a.current_challenge.starts_white,
            width, height);
    }

    if (a.transition_active) {
        float t = static_cast<float>(
            static_cast<double>(now - a.transition_start_time_us) / 1e6);
        static constexpr float TRANSITION_DURATION = 1.3f;
        if (t >= TRANSITION_DURATION) {
            a.transition_active = false;
        } else {
            renderer_draw_shatter(t, width, height);
        }
    }
}

// ===========================================================================
// Lifecycle
// ===========================================================================
void app_init(AppState& a, const AppPlatform* platform) {
    a.platform = platform;
    game_reset(a.game);
}
