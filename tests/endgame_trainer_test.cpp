// Content test for the endgame-trainer starting positions
// (endgame_positions.h). Each non-standard entry must be a legal,
// correctly-shaped, White-to-move king-and-X-vs-king position so the
// trainer always hands the human a winning side to convert. (Whether
// the position is theoretically won is argued in the header; here we
// pin the mechanical invariants so a bad edit fails the build.)

#include "doctest.h"

#include "../challenge.h"          // parse_fen / apply_fen_to_state
#include "../chess_rules.h"        // is_in_check
#include "../chess_types.h"
#include "../endgame_positions.h"

#include <string>

namespace {
int count_type(const GameState& gs, bool white, PieceType t) {
    int n = 0;
    for (const auto& p : gs.pieces)
        if (p.alive && p.is_white == white && p.type == t) ++n;
    return n;
}
const BoardPiece* king_of(const GameState& gs, bool white) {
    for (const auto& p : gs.pieces)
        if (p.alive && p.is_white == white && p.type == KING) return &p;
    return nullptr;
}
}  // namespace

TEST_CASE("endgame trainer: index 0 is the standard game (no FEN)") {
    CHECK(std::string(ENDGAME_STARTS[0].fen).empty());
}

TEST_CASE("endgame trainer: every endgame is legal, White-to-move, king-vs-king+X") {
    for (int i = 1; i < ENDGAME_START_COUNT; ++i) {
        CAPTURE(ENDGAME_STARTS[i].name);
        ParsedFEN p = parse_fen(ENDGAME_STARTS[i].fen);
        REQUIRE(p.valid);
        CHECK(p.white_turn);                 // human (winning side) moves first

        GameState gs;
        apply_fen_to_state(gs, p);

        // Exactly one king each, and Black is the lone king.
        CHECK(count_type(gs, true,  KING) == 1);
        CHECK(count_type(gs, false, KING) == 1);
        CHECK(count_type(gs, false, QUEEN)  == 0);
        CHECK(count_type(gs, false, ROOK)   == 0);
        CHECK(count_type(gs, false, BISHOP) == 0);
        CHECK(count_type(gs, false, KNIGHT) == 0);
        CHECK(count_type(gs, false, PAWN)   == 0);

        // White has the king plus exactly one winning unit.
        int white_extra = count_type(gs, true, QUEEN) + count_type(gs, true, ROOK)
                        + count_type(gs, true, BISHOP) + count_type(gs, true, KNIGHT)
                        + count_type(gs, true, PAWN);
        CHECK(white_extra == 1);

        // Kings are not adjacent (would be an illegal position).
        const BoardPiece* wk = king_of(gs, true);
        const BoardPiece* bk = king_of(gs, false);
        REQUIRE(wk);
        REQUIRE(bk);
        int dc = wk->col - bk->col, dr = wk->row - bk->row;
        if (dc < 0) dc = -dc;
        if (dr < 0) dr = -dr;
        CHECK((dc > 1 || dr > 1));

        // White to move, so the Black king must not already be in check.
        CHECK_FALSE(is_in_check(gs, /*white_king=*/false));
    }
}
