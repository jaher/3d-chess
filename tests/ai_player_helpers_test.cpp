// Tests for the FEN/UCI helper functions in ai_player.cpp. These are
// the parts compiled when -DAI_PLAYER_HELPERS_ONLY is defined; the
// POSIX subprocess wrapper is excluded and doesn't affect the link.

#include "doctest.h"
#include "helpers.h"

#include "../ai_player.h"

#include <string>

// ---------------------------------------------------------------------------
// piece_to_fen
// ---------------------------------------------------------------------------
TEST_CASE("piece_to_fen: white uppercase, black lowercase") {
    CHECK(piece_to_fen(KING,   true)  == 'K');
    CHECK(piece_to_fen(QUEEN,  true)  == 'Q');
    CHECK(piece_to_fen(BISHOP, true)  == 'B');
    CHECK(piece_to_fen(KNIGHT, true)  == 'N');
    CHECK(piece_to_fen(ROOK,   true)  == 'R');
    CHECK(piece_to_fen(PAWN,   true)  == 'P');

    CHECK(piece_to_fen(KING,   false) == 'k');
    CHECK(piece_to_fen(QUEEN,  false) == 'q');
    CHECK(piece_to_fen(BISHOP, false) == 'b');
    CHECK(piece_to_fen(KNIGHT, false) == 'n');
    CHECK(piece_to_fen(ROOK,   false) == 'r');
    CHECK(piece_to_fen(PAWN,   false) == 'p');
}

// ---------------------------------------------------------------------------
// board_to_fen
// ---------------------------------------------------------------------------
TEST_CASE("board_to_fen: starting position matches standard FEN") {
    BoardSquare board[8][8];
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            board[r][c] = {-1, false};

    // Place the pieces. Internal col 7 = a-file, col 0 = h-file.
    // Rank 1 (row 0): white back rank.
    // Rank 2 (row 1): white pawns.
    // Rank 7 (row 6): black pawns.
    // Rank 8 (row 7): black back rank.
    PieceType back_rank_by_file[8] = {
        ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK
    };
    for (int file = 0; file < 8; file++) {
        int col = 7 - file;
        board[0][col] = {back_rank_by_file[file], true};
        board[1][col] = {PAWN, true};
        board[6][col] = {PAWN, false};
        board[7][col] = {back_rank_by_file[file], false};
    }

    std::string fen = board_to_fen(board, /*white_turn=*/true);
    CHECK(fen == "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

TEST_CASE("board_to_fen: empty board with black to move") {
    BoardSquare board[8][8];
    for (int r = 0; r < 8; r++)
        for (int c = 0; c < 8; c++)
            board[r][c] = {-1, false};

    // board_to_fen infers castling from the "moved" flags, not the
    // board, so an empty board with default (false) flags would emit
    // "KQkq". Pass moved=true for all four rook/king flags to get "-".
    std::string fen = board_to_fen(board, /*white_turn=*/false,
        /*wk*/ true, /*bk*/ true,
        /*wra*/ true, /*wrh*/ true,
        /*bra*/ true, /*brh*/ true);
    CHECK(fen == "8/8/8/8/8/8/8/8 b - - 0 1");
}

// ---------------------------------------------------------------------------
// parse_uci_move
// ---------------------------------------------------------------------------
TEST_CASE("parse_uci_move: valid moves") {
    int fc, fr, tc, tr;

    CHECK(parse_uci_move("e2e4", fc, fr, tc, tr));
    CHECK(fc == col_of('e'));
    CHECK(fr == row_of('2'));
    CHECK(tc == col_of('e'));
    CHECK(tr == row_of('4'));

    CHECK(parse_uci_move("g1f3", fc, fr, tc, tr));
    CHECK(fc == col_of('g'));
    CHECK(fr == row_of('1'));
    CHECK(tc == col_of('f'));
    CHECK(tr == row_of('3'));

    // Promotions — the 5th character is ignored but the move still parses.
    CHECK(parse_uci_move("a7a8q", fc, fr, tc, tr));
    CHECK(fc == col_of('a'));
    CHECK(fr == row_of('7'));
    CHECK(tc == col_of('a'));
    CHECK(tr == row_of('8'));
}

TEST_CASE("parse_uci_move: invalid inputs return false") {
    int fc, fr, tc, tr;

    // Too short.
    CHECK_FALSE(parse_uci_move("",     fc, fr, tc, tr));
    CHECK_FALSE(parse_uci_move("e2e",  fc, fr, tc, tr));

    // Out-of-range rank (9).
    CHECK_FALSE(parse_uci_move("e2e9", fc, fr, tc, tr));

    // Out-of-range file ('i' → file index 8, then 7 - 8 = -1).
    CHECK_FALSE(parse_uci_move("i2i4", fc, fr, tc, tr));
}

// ---------------------------------------------------------------------------
// square_to_uci / move_to_uci
// ---------------------------------------------------------------------------
TEST_CASE("square_to_uci: corners") {
    CHECK(square_to_uci(col_of('a'), row_of('1')) == "a1");
    CHECK(square_to_uci(col_of('h'), row_of('1')) == "h1");
    CHECK(square_to_uci(col_of('a'), row_of('8')) == "a8");
    CHECK(square_to_uci(col_of('h'), row_of('8')) == "h8");
}

TEST_CASE("move_to_uci: round-trips parse_uci_move") {
    const char* moves[] = {
        "e2e4", "g1f3", "b1c3", "d7d5", "a1a8", "h8h1", "c1h6"
    };
    for (const char* uci : moves) {
        int fc, fr, tc, tr;
        REQUIRE(parse_uci_move(uci, fc, fr, tc, tr));
        CHECK(move_to_uci(fc, fr, tc, tr) == std::string(uci));
    }
}

// ---------------------------------------------------------------------------
// internal_col_to_file / file_to_internal_col
// ---------------------------------------------------------------------------
TEST_CASE("internal_col_to_file and file_to_internal_col invert each other") {
    for (int file = 0; file < 8; file++) {
        int col = file_to_internal_col(file);
        CHECK(internal_col_to_file(col) == file);
    }
    for (int col = 0; col < 8; col++) {
        int file = internal_col_to_file(col);
        CHECK(file_to_internal_col(file) == col);
    }
}

TEST_CASE("internal col 7 corresponds to the a-file") {
    // This is the convention the rest of the codebase relies on:
    // col 7 = a, col 0 = h. Catch any future accidental swap.
    CHECK(internal_col_to_file(7) == 0);  // a
    CHECK(internal_col_to_file(0) == 7);  // h
    CHECK(file_to_internal_col(0) == 7);  // a-file → col 7
    CHECK(file_to_internal_col(7) == 0);  // h-file → col 0
}
