// Small helpers shared by the test files. Intentionally header-only —
// each test translation unit includes this and gets its own copies of
// the trivial functions (the linker folds them). No separate .cpp file
// to avoid growing the test Makefile.

#pragma once

#include "doctest.h"

#include "../challenge.h"
#include "../chess_types.h"

// Build a GameState from a FEN string by way of challenge.cpp's parser.
// The parse_fen / apply_fen_to_state round-trip is exercised by
// challenge_test.cpp independently, so tests that use this helper are
// not circular with their own fixture.
inline GameState state_from_fen(const char* fen) {
    ParsedFEN p = parse_fen(fen);
    REQUIRE_MESSAGE(p.valid, "parse_fen failed for FEN: " << fen);
    GameState gs;
    apply_fen_to_state(gs, p);
    return gs;
}

// Convenience: starting position.
inline GameState starting_state() {
    return state_from_fen(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

// Find the piece at an (internal) (col, row) square. Returns nullptr
// if the square is empty or out of bounds.
inline const BoardPiece* piece_at(const GameState& gs, int col, int row) {
    if (col < 0 || col > 7 || row < 0 || row > 7) return nullptr;
    for (const auto& p : gs.pieces) {
        if (p.alive && p.col == col && p.row == row) return &p;
    }
    return nullptr;
}

// Internal column index for a standard UCI file letter ('a'..'h').
// The internal column convention reverses the file order so that
// col 7 = a-file and col 0 = h-file (see ai_player.cpp).
inline int col_of(char file) {
    return 7 - (file - 'a');
}

// Internal row index for a standard UCI rank digit ('1'..'8').
inline int row_of(char rank) {
    return rank - '1';
}
