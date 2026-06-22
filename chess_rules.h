#pragma once

#include "chess_types.h"
#include <utility>
#include <vector>

// Position evaluation
float evaluate_position(const GameState& gs);

// Move generation (pseudo-legal)
std::vector<std::pair<int,int>> generate_moves(const GameState& gs, int col, int row);

// Check / checkmate detection
bool is_square_attacked(const GameState& gs, int c, int r, bool by_white);
bool is_in_check(const GameState& gs, bool white_king);
bool move_leaves_in_check(GameState& gs, int from_c, int from_r, int to_c, int to_r, bool is_white);
std::vector<std::pair<int,int>> generate_legal_moves(GameState& gs, int col, int row);
bool has_any_legal_move(GameState& gs, bool is_white);
void check_game_over(GameState& gs);

// Move execution
void execute_move(GameState& gs, int from_col, int from_row, int to_col, int to_row);

// Starting position
std::vector<BoardPiece> build_starting_position();

// Convert a UCI move + snapshot context to algebraic notation (e.g. "Nf3", "exd5", "O-O")
std::string uci_to_algebraic(const BoardSnapshot& before, const std::string& uci);

// Convert a space-joined UCI principal variation ("e2e4 e7e5 g1f3") into a
// SAN line ("e4 e5 Nf3"), played out from `start`, showing at most
// max_plies moves. Stops early on a malformed/illegal move.
std::string pv_to_san(const BoardSnapshot& start, const std::string& pv_uci,
                      int max_plies);
