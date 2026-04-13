#include "game_state.h"
#include "chess_rules.h"

// The chess position's lifecycle. Pure C++ — no platform dependencies.
// Everything that used to live in this file that touched GTK (title
// updates, AI dispatch, animation tick callbacks) now lives in the
// desktop platform layer (main.cpp) or the web platform layer
// (web/main_web.cpp), mediated through the shared app_state module.

void game_reset(GameState& gs) {
    gs.pieces = build_starting_position();
    gs.white_turn = true;
    gs.castling = CastlingRights();
    gs.selected_col = -1;
    gs.selected_row = -1;
    gs.valid_moves.clear();
    gs.game_over = false;
    gs.game_result.clear();
    gs.ai_thinking = false;
    gs.ai_animating = false;
    gs.analysis_mode = false;
    gs.analysis_index = 0;
    gs.move_history.clear();
    gs.score_history.clear();
    gs.snapshots.clear();
    gs.rebuild_grid();
    gs.score_history.push_back(evaluate_position(gs));
    gs.take_snapshot();
}

void game_enter_analysis(GameState& gs) {
    if (gs.ai_thinking || gs.ai_animating) return;
    gs.analysis_mode = true;
    gs.analysis_index = static_cast<int>(gs.snapshots.size()) - 1;
    gs.live_pieces = gs.pieces;
    gs.live_white_turn = gs.white_turn;
    gs.live_castling = gs.castling;
    gs.selected_col = gs.selected_row = -1;
    gs.valid_moves.clear();
}

void game_exit_analysis(GameState& gs) {
    gs.analysis_mode = false;
    gs.pieces = gs.live_pieces;
    gs.white_turn = gs.live_white_turn;
    gs.castling = gs.live_castling;
    gs.rebuild_grid();
}
