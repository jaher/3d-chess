// Pure-logic tests for voice_tts.cpp's SAN-to-spoken-English
// conversion. Verifies every SAN flavour produced by
// `uci_to_algebraic` reads back as natural-sounding English,
// including pawn moves, captures, castling, promotion, check, and
// checkmate.

#include "doctest.h"
#include "helpers.h"

#include "../chess_rules.h"
#include "../chess_types.h"
#include "../voice_tts.h"

#include <string>

namespace {

// Build a fresh starting-position BoardSnapshot for the
// uci_to_speech integration tests. Reuses helpers.h's starting_state
// so we share the same FEN-parser path the other test files use.
BoardSnapshot starting_snapshot() {
    GameState gs = starting_state();
    BoardSnapshot snap;
    snap.pieces        = gs.pieces;
    snap.white_turn    = gs.white_turn;
    snap.castling      = gs.castling;
    snap.ep_target_col = gs.ep_target_col;
    snap.ep_target_row = gs.ep_target_row;
    return snap;
}

}  // namespace

TEST_CASE("san_to_speech: pawn moves") {
    CHECK(san_to_speech("e4")  == "Pawn to e four");
    CHECK(san_to_speech("d5")  == "Pawn to d five");
    CHECK(san_to_speech("a8")  == "Pawn to a eight");
    CHECK(san_to_speech("h1")  == "Pawn to h one");
}

TEST_CASE("san_to_speech: piece moves") {
    CHECK(san_to_speech("Nf3") == "Knight to f three");
    CHECK(san_to_speech("Bg5") == "Bishop to g five");
    CHECK(san_to_speech("Qd1") == "Queen to d one");
    CHECK(san_to_speech("Ke2") == "King to e two");
    CHECK(san_to_speech("Ra1") == "Rook to a one");
}

TEST_CASE("san_to_speech: captures") {
    CHECK(san_to_speech("Bxd5") == "Bishop takes d five");
    CHECK(san_to_speech("Nxe4") == "Knight takes e four");
    CHECK(san_to_speech("Qxh7") == "Queen takes h seven");
}

TEST_CASE("san_to_speech: pawn captures (source file disambig)") {
    // SAN spells the source file: exd5 = pawn on e captures d5.
    // Spoken: "Pawn from e takes d five" reads naturally.
    CHECK(san_to_speech("exd5") == "Pawn from e takes d five");
    CHECK(san_to_speech("axb6") == "Pawn from a takes b six");
}

TEST_CASE("san_to_speech: castling") {
    CHECK(san_to_speech("O-O")     == "Castles kingside");
    CHECK(san_to_speech("O-O-O")   == "Castles queenside");
    // 0-0 / 0-0-0 (digit form) accepted too.
    CHECK(san_to_speech("0-0")     == "Castles kingside");
    CHECK(san_to_speech("0-0-0")   == "Castles queenside");
}

TEST_CASE("san_to_speech: promotion") {
    CHECK(san_to_speech("e8=Q") == "Pawn to e eight, promotes to queen");
    CHECK(san_to_speech("a1=N") == "Pawn to a one, promotes to knight");
    CHECK(san_to_speech("h8=R") == "Pawn to h eight, promotes to rook");
    CHECK(san_to_speech("b1=B") == "Pawn to b one, promotes to bishop");
}

TEST_CASE("san_to_speech: capture + promotion") {
    CHECK(san_to_speech("dxe8=Q") ==
          "Pawn from d takes e eight, promotes to queen");
}

TEST_CASE("san_to_speech: check + mate suffix") {
    CHECK(san_to_speech("Nf3+") == "Knight to f three, check");
    CHECK(san_to_speech("Qh4#") == "Queen to h four, checkmate");
    CHECK(san_to_speech("e8=Q+") ==
          "Pawn to e eight, promotes to queen, check");
    CHECK(san_to_speech("Bxd7#") == "Bishop takes d seven, checkmate");
    CHECK(san_to_speech("O-O-O#") == "Castles queenside, checkmate");
}

TEST_CASE("san_to_speech: piece disambiguation") {
    // "Nbd2" — knight from the b-file. Reads as "Knight from b to
    // d two". File disambig flows naturally before "to/takes".
    CHECK(san_to_speech("Nbd2")  == "Knight from b to d two");
    CHECK(san_to_speech("Nbxd5") == "Knight from b takes d five");
    // Bare rank disambig (rare): "R1a3" prefixes "rank" so the
    // listener doesn't confuse "one a three" with a square name.
    CHECK(san_to_speech("R1a3")  == "Rook from rank one to a three");
    // File + rank: "Qa1b2".
    CHECK(san_to_speech("Qa1b2") == "Queen from a one to b two");
}

TEST_CASE("uci_to_speech: integrates with uci_to_algebraic") {
    BoardSnapshot snap = starting_snapshot();

    // From the starting position, white's first move e2-e4 should
    // surface as "Pawn to e four".
    CHECK(uci_to_speech(snap, "e2e4") == "Pawn to e four");

    // Knight g1 → f3.
    CHECK(uci_to_speech(snap, "g1f3") == "Knight to f three");

    // Knight b1 → c3.
    CHECK(uci_to_speech(snap, "b1c3") == "Knight to c three");
}

TEST_CASE("uci_to_speech: short input is treated as already-SAN") {
    BoardSnapshot snap = starting_snapshot();
    // uci_to_algebraic returns < 4 char inputs unchanged, and
    // san_to_speech parses "e2" as a pawn move to e2 — fine
    // because "e2" is already valid SAN.
    CHECK(uci_to_speech(snap, "e2") == "Pawn to e two");
}
