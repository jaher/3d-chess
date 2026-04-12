#include "game_state.h"
#include "chess_rules.h"
#include "ai_player.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include <gtk/gtk.h>

static GameState g_state;

GameState& game_get_state() { return g_state; }

void game_reset() {
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

void game_init(const std::string& /*models_dir*/) {
    game_reset();
}

// ---------------------------------------------------------------------------
// Title
// ---------------------------------------------------------------------------
void game_update_title(GtkWidget* window) {
    if (g_state.ai_thinking) return;

    if (g_state.game_over) {
        std::string title = "Chess — " + g_state.game_result;
        gtk_window_set_title(GTK_WINDOW(window), title.c_str());
        return;
    }

    float score = evaluate_position(g_state);
    char buf[128];
    const char* turn = g_state.white_turn ? "White" : "Black";
    bool check = is_in_check(g_state, g_state.white_turn);

    if (std::abs(score) < 0.1f)
        std::snprintf(buf, sizeof(buf), "Chess — %s to move%s — Even",
                      turn, check ? " (CHECK)" : "");
    else if (score > 0)
        std::snprintf(buf, sizeof(buf), "Chess — %s to move%s — White +%.1f",
                      turn, check ? " (CHECK)" : "", score);
    else
        std::snprintf(buf, sizeof(buf), "Chess — %s to move%s — Black +%.1f",
                      turn, check ? " (CHECK)" : "", -score);

    gtk_window_set_title(GTK_WINDOW(window), buf);
}

void game_update_analysis_title(GtkWidget* window) {
    int total = static_cast<int>(g_state.snapshots.size()) - 1;
    char buf[128];
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
    gtk_window_set_title(GTK_WINDOW(window), buf);
}

// ---------------------------------------------------------------------------
// Analysis mode
// ---------------------------------------------------------------------------
void game_enter_analysis(GtkWidget* gl_area) {
    if (g_state.ai_thinking || g_state.ai_animating) return;
    g_state.analysis_mode = true;
    g_state.analysis_index = static_cast<int>(g_state.snapshots.size()) - 1;
    g_state.live_pieces = g_state.pieces;
    g_state.live_white_turn = g_state.white_turn;
    g_state.live_castling = g_state.castling;
    g_state.selected_col = g_state.selected_row = -1;
    g_state.valid_moves.clear();
    if (g_state.tick_id != 0) {
        gtk_widget_remove_tick_callback(gl_area, g_state.tick_id);
        g_state.tick_id = 0;
    }
}

void game_exit_analysis() {
    g_state.analysis_mode = false;
    g_state.pieces = g_state.live_pieces;
    g_state.white_turn = g_state.live_white_turn;
    g_state.castling = g_state.live_castling;
    g_state.rebuild_grid();
}

// ---------------------------------------------------------------------------
// AI integration
// ---------------------------------------------------------------------------
struct AiMoveResult {
    int from_col, from_row, to_col, to_row;
    bool valid;
    GtkWidget* window;
    GtkWidget* gl_area;
};

static gboolean on_ai_anim_tick(GtkWidget* widget, GdkFrameClock*, gpointer data);

static bool is_legal_ai_move(GameState& gs, int fc, int fr, int tc, int tr) {
    // Check source has a black piece
    int idx = gs.grid[fr][fc];
    if (idx < 0 || gs.pieces[idx].is_white) return false;

    // Check destination is in the legal moves list
    auto legal = generate_legal_moves(gs, fc, fr);
    for (const auto& [mc, mr] : legal) {
        if (mc == tc && mr == tr) return true;
    }
    return false;
}

static gboolean on_ai_move_ready(gpointer data) {
    auto* result = static_cast<AiMoveResult*>(data);
    auto& gs = g_state;

    bool accepted = false;
    if (result->valid) {
        if (is_legal_ai_move(gs, result->from_col, result->from_row,
                             result->to_col, result->to_row)) {
            std::printf("AI plays: %s (legal)\n",
                        move_to_uci(result->from_col, result->from_row,
                                    result->to_col, result->to_row).c_str());
            gs.ai_from_col = result->from_col;
            gs.ai_from_row = result->from_row;
            gs.ai_to_col = result->to_col;
            gs.ai_to_row = result->to_row;
            gs.ai_animating = true;
            gs.ai_anim_start = g_get_monotonic_time();
            gs.ai_anim_tick = gtk_widget_add_tick_callback(
                result->gl_area, on_ai_anim_tick, result->window, nullptr);
            accepted = true;
        } else {
            std::fprintf(stderr, "AI move %s is illegal, retrying...\n",
                         move_to_uci(result->from_col, result->from_row,
                                     result->to_col, result->to_row).c_str());
        }
    }

    if (!accepted) {
        // Retry: pick a random legal move for black
        std::vector<std::pair<std::pair<int,int>, std::pair<int,int>>> all_legal;
        for (const auto& p : gs.pieces) {
            if (!p.alive || p.is_white) continue;
            auto moves = generate_legal_moves(gs, p.col, p.row);
            for (const auto& [tc, tr] : moves)
                all_legal.push_back({{p.col, p.row}, {tc, tr}});
        }

        if (!all_legal.empty()) {
            // Pick a random legal move
            int idx = static_cast<int>(g_get_monotonic_time() % all_legal.size());
            auto& [from, to] = all_legal[idx];
            std::printf("AI fallback: %s\n",
                        move_to_uci(from.first, from.second,
                                    to.first, to.second).c_str());
            gs.ai_from_col = from.first;
            gs.ai_from_row = from.second;
            gs.ai_to_col = to.first;
            gs.ai_to_row = to.second;
            gs.ai_animating = true;
            gs.ai_anim_start = g_get_monotonic_time();
            gs.ai_anim_tick = gtk_widget_add_tick_callback(
                result->gl_area, on_ai_anim_tick, result->window, nullptr);
        } else {
            // No legal moves at all
            gs.ai_thinking = false;
            game_update_title(result->window);
        }
    }

    gtk_widget_queue_draw(result->gl_area);
    delete result;
    return G_SOURCE_REMOVE;
}

static gboolean on_ai_anim_tick(GtkWidget* widget, GdkFrameClock*, gpointer data) {
    auto& gs = g_state;
    GtkWidget* window = static_cast<GtkWidget*>(data);

    gint64 now = g_get_monotonic_time();
    float elapsed = static_cast<float>(now - gs.ai_anim_start) / 1000000.0f;

    if (elapsed >= gs.ai_anim_duration) {
        gs.ai_animating = false;
        gtk_widget_remove_tick_callback(widget, gs.ai_anim_tick);
        gs.ai_anim_tick = 0;

        execute_move(gs, gs.ai_from_col, gs.ai_from_row, gs.ai_to_col, gs.ai_to_row);
        gs.ai_thinking = false;
        game_update_title(window);
        gtk_widget_queue_draw(widget);
        return G_SOURCE_REMOVE;
    }

    gtk_widget_queue_draw(widget);
    return G_SOURCE_CONTINUE;
}

void game_trigger_ai(GtkWidget* window, GtkWidget* gl_area) {
    auto& gs = g_state;
    gs.ai_thinking = true;
    gtk_window_set_title(GTK_WINDOW(window), "Chess Board — AI thinking...");
    gtk_widget_queue_draw(gl_area);

    BoardSquare board[8][8];
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            board[r][c] = {-1, false};
    for (const auto& p : gs.pieces)
        if (p.alive)
            board[p.row][p.col] = {p.type, p.is_white};

    std::string fen = board_to_fen(board, false,
        gs.castling.white_king_moved, gs.castling.black_king_moved,
        gs.castling.white_rook_a_moved, gs.castling.white_rook_h_moved,
        gs.castling.black_rook_a_moved, gs.castling.black_rook_h_moved);
    std::vector<std::string> history = gs.move_history;

    // Capture widget pointers for the callback
    GtkWidget* win_ptr = window;
    GtkWidget* gl_ptr = gl_area;

    std::thread([fen, history, board, win_ptr, gl_ptr]() {
        std::printf("AI thinking... FEN: %s\n", fen.c_str());
        std::string uci = ask_ai_move(fen, history, board);

        auto* result = new AiMoveResult{};
        result->valid = false;
        result->window = win_ptr;
        result->gl_area = gl_ptr;

        if (!uci.empty()) {
            int fc, fr, tc, tr;
            if (parse_uci_move(uci, fc, fr, tc, tr)) {
                result->from_col = fc;
                result->from_row = fr;
                result->to_col = tc;
                result->to_row = tr;
                result->valid = true;
            }
        }

        g_idle_add(on_ai_move_ready, result);
    }).detach();
}
