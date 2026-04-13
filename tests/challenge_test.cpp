// Tests for challenge.cpp::parse_fen — the FEN parser used both by
// challenge mode loading and (via helpers::state_from_fen) as the
// shared fixture for every other test file. Hand-rolled assertions
// here (not via state_from_fen) so the parser isn't tested against
// itself.

#include "doctest.h"
#include "helpers.h"

#include "../challenge.h"
#include "../chess_types.h"

#include <string>

// Search `parsed` for a piece on (col, row). Returns nullptr on miss.
static const BoardPiece* parsed_piece_at(const ParsedFEN& p, int col, int row) {
    for (const auto& piece : p.pieces) {
        if (piece.col == col && piece.row == row) return &piece;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Starting position
// ---------------------------------------------------------------------------
TEST_CASE("parse_fen: standard starting position") {
    ParsedFEN p = parse_fen(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");

    REQUIRE(p.valid);
    CHECK(p.pieces.size() == 32);
    CHECK(p.white_turn);

    // Castling rights all present.
    CHECK_FALSE(p.castling.white_king_moved);
    CHECK_FALSE(p.castling.black_king_moved);
    CHECK_FALSE(p.castling.white_rook_a_moved);
    CHECK_FALSE(p.castling.white_rook_h_moved);
    CHECK_FALSE(p.castling.black_rook_a_moved);
    CHECK_FALSE(p.castling.black_rook_h_moved);

    // Spot-check a few pieces at known (col, row) coordinates.
    // Internal col 7 = a-file, col 0 = h-file.
    const BoardPiece* wk = parsed_piece_at(p, /*e*/ 7 - 4, /*rank 1*/ 0);
    REQUIRE(wk != nullptr);
    CHECK(wk->type == KING);
    CHECK(wk->is_white);

    const BoardPiece* bq = parsed_piece_at(p, /*d*/ 7 - 3, /*rank 8*/ 7);
    REQUIRE(bq != nullptr);
    CHECK(bq->type == QUEEN);
    CHECK_FALSE(bq->is_white);

    const BoardPiece* wp_e2 = parsed_piece_at(p, 7 - 4, 1);
    REQUIRE(wp_e2 != nullptr);
    CHECK(wp_e2->type == PAWN);
    CHECK(wp_e2->is_white);
}

TEST_CASE("parse_fen: black to move with empty side-to-move string defaults to white") {
    // "w" / "b" field missing — parse_fen treats empty side as white_turn=true.
    ParsedFEN p = parse_fen("8/8/8/8/8/8/8/4K3");
    // Only 1 piece (the king) — valid because pieces.size() > 0.
    REQUIRE(p.valid);
    CHECK(p.pieces.size() == 1);
    CHECK(p.white_turn);
}

TEST_CASE("parse_fen: explicit black to move") {
    ParsedFEN p = parse_fen("4k3/8/8/8/8/8/8/4K3 b - - 0 1");
    REQUIRE(p.valid);
    CHECK_FALSE(p.white_turn);
    CHECK(p.pieces.size() == 2);
}

TEST_CASE("parse_fen: partial castling rights") {
    // Only white kingside available.
    ParsedFEN p = parse_fen("4k3/8/8/8/8/8/8/4K2R w K - 0 1");
    REQUIRE(p.valid);
    CHECK_FALSE(p.castling.white_king_moved);
    CHECK(p.castling.white_rook_a_moved);     // Q not present
    CHECK_FALSE(p.castling.white_rook_h_moved); // K present
    CHECK(p.castling.black_king_moved);        // no k, no q
}

TEST_CASE("parse_fen: empty string is invalid") {
    ParsedFEN p = parse_fen("");
    CHECK_FALSE(p.valid);
}

TEST_CASE("parse_fen: position with no pieces is invalid") {
    // All-empty placement — no BoardPieces produced, so valid = false.
    ParsedFEN p = parse_fen("8/8/8/8/8/8/8/8 w - - 0 1");
    CHECK_FALSE(p.valid);
}

TEST_CASE("parse_fen: piece count for a known puzzle position") {
    // Philidor's mate pattern starting position — just a piece count
    // sanity check, not the exact mate puzzle.
    ParsedFEN p = parse_fen("6k1/5ppp/8/8/8/8/5PPP/R5K1 w - - 0 1");
    REQUIRE(p.valid);
    // 2 kings + 6 pawns + 1 rook = 9 pieces.
    CHECK(p.pieces.size() == 9);
}
