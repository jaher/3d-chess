// Content test: every position in challenges/training.md really does
// contain the tactic its `type:` claims, checked with the engine's own
// move generation. This is the same logic the throwaway authoring
// verifier used, pinned here so the curated file can never silently go
// stale (a bad edit fails the build).
//
// mate_in_1  -> some legal move by the side-to-move delivers checkmate.
// find_forks -> find_tactic_moves(...) is non-empty.
// find_pins  -> find_tactic_moves(...) is non-empty.
//
// run_tests executes from the tests/ directory, so the data file is one
// level up. mate_in_2/3 positions (if ever added) are reported but not
// asserted — this simple verifier only proves mate-in-one.

#include "doctest.h"

#include "../challenge.h"
#include "../chess_rules.h"
#include "../chess_types.h"

#include <vector>

namespace {

std::vector<const BoardPiece*> side_pieces(const GameState& gs, bool white) {
    std::vector<const BoardPiece*> out;
    for (const auto& p : gs.pieces)
        if (p.alive && p.is_white == white) out.push_back(&p);
    return out;
}

// Side `white_to_move` is to move; checkmate iff in check with no legal move.
bool is_checkmate(GameState gs, bool white_to_move) {
    if (!is_in_check(gs, white_to_move)) return false;
    for (const auto* p : side_pieces(gs, white_to_move)) {
        GameState tmp = gs;
        if (!generate_legal_moves(tmp, p->col, p->row).empty()) return false;
    }
    return true;
}

// Does some legal move by `mover_white` deliver checkmate in one?
bool has_mate_in_1(const GameState& base, bool mover_white) {
    for (const auto* p : side_pieces(base, mover_white)) {
        GameState gen = base;
        auto dests = generate_legal_moves(gen, p->col, p->row);
        for (auto [tc, tr] : dests) {
            GameState after = base;
            execute_move(after, p->col, p->row, tc, tr);
            if (is_checkmate(after, !mover_white)) return true;
        }
    }
    return false;
}

}  // namespace

TEST_CASE("training.md: every position contains its declared tactic") {
    Challenge ch = load_challenge("../challenges/training.md");
    REQUIRE_MESSAGE(!ch.fens.empty(),
                    "could not load ../challenges/training.md");

    for (size_t i = 0; i < ch.fens.size(); ++i) {
        challenge_apply_current(ch, static_cast<int>(i));
        const std::string& type = ch.fen_types[i];
        bool white = ch.fen_starts_white[i];

        ParsedFEN p = parse_fen(ch.fens[i]);
        REQUIRE_MESSAGE(p.valid, "bad FEN in training.md: " << ch.fens[i]);
        GameState gs;
        apply_fen_to_state(gs, p);

        if (type == "mate_in_1") {
            CHECK_MESSAGE(has_mate_in_1(gs, white),
                          "no mate in 1 for: " << ch.fens[i]);
        } else if (type == "find_forks" || type == "find_pins") {
            CHECK_MESSAGE(!find_tactic_moves(gs, white, type).empty(),
                          "no " << type << " move for: " << ch.fens[i]);
        }
        // Other types (mate_in_2/3) are not asserted here.
    }
}
