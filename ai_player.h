#pragma once

#include <string>
#include <vector>

// Simplified board square for FEN generation / AI validation.
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
// Anthropic API
// ---------------------------------------------------------------------------
// Extract text from Anthropic API JSON response (minimal parser).
std::string extract_response_text(const std::string& json);

// Escape a string for JSON embedding.
std::string json_escape(const std::string& s);

// Send a prompt to the Anthropic API and return the text response.
std::string call_anthropic(const std::string& prompt);

// Extract a clean 4-char UCI move from raw AI text.
std::string extract_uci(const std::string& text);

// Call Anthropic API to get a move for black. Validates and retries up to 3 times.
std::string ask_ai_move(const std::string& fen,
                        const std::vector<std::string>& move_history,
                        const BoardSquare board[8][8]);
