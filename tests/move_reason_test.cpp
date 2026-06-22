// Unit tests for the "why?" panel's reason generator (move_reason.cpp).
//
// generate_why_reason(gs, cls) takes the position AFTER a move (so the
// side that just moved is !gs.white_turn) and returns a one-line reason:
// a hung-piece callout when the mover left a piece en prise, else a
// class-based fallback. Positions are built from FEN via helpers.h.

#include "doctest.h"
#include "helpers.h"

#include "../move_reason.h"
#include "../chess_types.h"

#include <string>

// In all FENs below it is *Black* to move, so the side that just moved —
// the one generate_why_reason inspects — is White.

TEST_CASE("hung piece: names the piece and the correct square") {
    // White queen on d5 is attacked by the black pawn on c6 and nothing
    // defends it. (Also checks the file mapping: the square must read
    // "d5", not a mirrored file.)
    GameState gs = state_from_fen("4k3/8/2p5/3Q4/8/8/8/4K3 b - - 0 1");
    std::string r = generate_why_reason(gs, MoveClass::Blunder);
    CHECK(r == "Leaves the queen on d5 hanging.");
}

TEST_CASE("hung piece takes priority over the class fallback") {
    // Even an Inaccuracy reports the concrete hang when there is one.
    GameState gs = state_from_fen("4k3/8/2p5/3Q4/8/8/8/4K3 b - - 0 1");
    std::string r = generate_why_reason(gs, MoveClass::Inaccuracy);
    CHECK(r.find("hanging") != std::string::npos);
    CHECK(r.find("queen") != std::string::npos);
}

TEST_CASE("the king is never reported as 'hanging' (that's just check)") {
    // White king on e1 is attacked by the black rook on h1, but no other
    // white piece hangs — so we fall back to the class line, not a
    // king-hanging message.
    GameState gs = state_from_fen("4k3/8/8/8/8/8/8/4K2r b - - 0 1");
    std::string r = generate_why_reason(gs, MoveClass::Blunder);
    CHECK(r.find("hanging") == std::string::npos);
    CHECK(r == "A losing move - a much stronger option was available.");
}

TEST_CASE("no hang: class-based fallbacks") {
    // Quiet position, nothing of White's is en prise.
    const char* fen = "4k3/8/8/8/8/8/4P3/4K3 b - - 0 1";
    GameState gs = state_from_fen(fen);
    CHECK(generate_why_reason(gs, MoveClass::Blunder)    ==
          "A losing move - a much stronger option was available.");
    CHECK(generate_why_reason(gs, MoveClass::MissedWin)  ==
          "Lets a winning advantage slip away.");
    CHECK(generate_why_reason(gs, MoveClass::Mistake)    ==
          "Inaccurate - concedes a clear edge.");
    CHECK(generate_why_reason(gs, MoveClass::Miss)       ==
          "Misses a clearly stronger move.");
    CHECK(generate_why_reason(gs, MoveClass::Inaccuracy) ==
          "A small concession; a more precise move held more.");
}

TEST_CASE("no reason for non-negative classes when nothing hangs") {
    GameState gs = state_from_fen("4k3/8/8/8/8/8/4P3/4K3 b - - 0 1");
    CHECK(generate_why_reason(gs, MoveClass::Good).empty());
    CHECK(generate_why_reason(gs, MoveClass::Best).empty());
    CHECK(generate_why_reason(gs, MoveClass::None).empty());
}

TEST_CASE("a defended attacked piece is not 'hanging'") {
    // White queen d5 attacked by the c6 pawn, but now defended by a white
    // rook on d1 — so it is not reported as hanging.
    GameState gs = state_from_fen("4k3/8/2p5/3Q4/8/8/8/3RK3 b - - 0 1");
    std::string r = generate_why_reason(gs, MoveClass::Blunder);
    CHECK(r.find("hanging") == std::string::npos);
}
