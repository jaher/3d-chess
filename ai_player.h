#pragma once

#include <string>

// Simplified board square for FEN generation.
// piece_type matches the main PieceType enum (-1 = empty).
struct BoardSquare {
    int piece_type;
    bool is_white;
};

// ---------------------------------------------------------------------------
// FEN / UCI helpers
// ---------------------------------------------------------------------------
char piece_to_fen(int type, bool is_white);

std::string board_to_fen(const BoardSquare board[8][8], bool white_turn,
                         bool wk_moved = false, bool bk_moved = false,
                         bool wra_moved = false, bool wrh_moved = false,
                         bool bra_moved = false, bool brh_moved = false);

// Internal column ↔ standard file index (reversed for camera orientation).
int internal_col_to_file(int col);
int file_to_internal_col(int file);

std::string square_to_uci(int col, int row);
std::string move_to_uci(int from_col, int from_row, int to_col, int to_row);

bool parse_uci_move(const std::string& move, int& from_col, int& from_row,
                    int& to_col, int& to_row);

// ---------------------------------------------------------------------------
// Stockfish engine integration
// ---------------------------------------------------------------------------
// Ask Stockfish for a move given a FEN position. Returns a 4-char UCI move
// ("e7e5") or "" if the engine is unavailable or produced no move. Caller is
// responsible for legality checking (the game_state layer does this anyway).
std::string ask_ai_move(const std::string& fen);

// Ask Stockfish to evaluate a position. Returns centipawns from WHITE's
// perspective (positive = white winning). Mate scores are encoded as
// ±(30000 - distance_to_mate). Returns INT_MIN if the engine is unavailable.
int stockfish_eval(const std::string& fen, int movetime_ms = 150);
