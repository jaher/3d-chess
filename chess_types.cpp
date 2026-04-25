#include "chess_types.h"

const char* piece_filenames[PIECE_COUNT] = {
    "King.stl", "Queen.stl", "Bishop.stl", "Knight.stl", "Rook.stl", "Pawn.stl"
};

const float piece_scale[PIECE_COUNT] = {
    1.0f, 0.92f, 0.82f, 0.78f, 0.68f, 0.58f
};

void square_center(int col, int row, float& x, float& z) {
    x = (col - 3.5f) * SQ;
    z = (row - 3.5f) * SQ;
}

bool in_bounds(int c, int r) {
    return c >= 0 && c < 8 && r >= 0 && r < 8;
}

void GameState::rebuild_grid() {
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            grid[r][c] = -1;
    for (int i = 0; i < static_cast<int>(pieces.size()); i++) {
        auto& p = pieces[i];
        if (p.alive)
            grid[p.row][p.col] = i;
    }
}

void GameState::take_snapshot(const std::string& move_uci) {
    BoardSnapshot snap;
    snap.pieces = pieces;
    snap.white_turn = white_turn;
    snap.castling = castling;
    snap.last_move = move_uci;
    snap.ep_target_col = ep_target_col;
    snap.ep_target_row = ep_target_row;
    snapshots.push_back(snap);
}

void GameState::restore_snapshot(int index) {
    const auto& snap = snapshots[index];
    pieces = snap.pieces;
    white_turn = snap.white_turn;
    castling = snap.castling;
    ep_target_col = snap.ep_target_col;
    ep_target_row = snap.ep_target_row;
    rebuild_grid();
}
