// Content test: every position in the Practice files we author
// (challenges/training.md) really does contain the tactic its `type:`
// claims, checked with the engine's own move generation. Pinned here so
// a bad edit fails the build.
//
// mate_in_1  -> some legal move by the side-to-move checkmates.
// mate_in_2  -> a move that, against every reply, leaves a mate in one.
// find_forks -> find_tactic_moves(...) is non-empty.
// find_pins  -> find_tactic_moves(...) is non-empty.
//
// run_tests executes from the tests/ directory, so the data files are one
// level up. mate_in_3+ (none in our files) is reported but not asserted.
// The homework*.md files are intentionally NOT covered here — they are
// curated third-party data, and a couple of their find_pins pages are
// degenerate (no legal pin; the app auto-skips them).

#include "doctest.h"

#include "../challenge.h"
#include "../chess_rules.h"
#include "../chess_types.h"

#include <array>
#include <string>
#include <vector>

namespace {

std::vector<const BoardPiece*> side_pieces(const GameState& gs, bool white) {
    std::vector<const BoardPiece*> out;
    for (const auto& p : gs.pieces)
        if (p.alive && p.is_white == white) out.push_back(&p);
    return out;
}

bool is_checkmate(GameState gs, bool white_to_move) {
    if (!is_in_check(gs, white_to_move)) return false;
    for (const auto* p : side_pieces(gs, white_to_move)) {
        GameState tmp = gs;
        if (!generate_legal_moves(tmp, p->col, p->row).empty()) return false;
    }
    return true;
}

std::vector<std::array<int, 4>> all_moves(const GameState& base, bool mover) {
    std::vector<std::array<int, 4>> out;
    for (const auto* p : side_pieces(base, mover)) {
        GameState gen = base;
        for (auto [tc, tr] : generate_legal_moves(gen, p->col, p->row))
            out.push_back({p->col, p->row, tc, tr});
    }
    return out;
}

bool has_mate_in_1(const GameState& base, bool mover) {
    for (auto m : all_moves(base, mover)) {
        GameState after = base;
        execute_move(after, m[0], m[1], m[2], m[3]);
        if (is_checkmate(after, !mover)) return true;
    }
    return false;
}

// A move (not itself mate) after which every defender reply leaves the
// mover a mate in one.
bool has_mate_in_2(const GameState& base, bool mover) {
    for (auto m : all_moves(base, mover)) {
        GameState pos1 = base;
        execute_move(pos1, m[0], m[1], m[2], m[3]);
        if (is_checkmate(pos1, !mover)) continue;     // mate in 1, not 2
        auto replies = all_moves(pos1, !mover);
        if (replies.empty()) continue;                // stalemate, not mate
        bool forced = true;
        for (auto r : replies) {
            GameState pos2 = pos1;
            execute_move(pos2, r[0], r[1], r[2], r[3]);
            if (!has_mate_in_1(pos2, mover)) { forced = false; break; }
        }
        if (forced) return true;
    }
    return false;
}

void verify_practice_file(const char* path) {
    Challenge ch = load_challenge(path);
    REQUIRE_MESSAGE(!ch.fens.empty(), "could not load " << path);

    for (size_t i = 0; i < ch.fens.size(); ++i) {
        challenge_apply_current(ch, static_cast<int>(i));
        const std::string& type = ch.fen_types[i];
        bool white = ch.fen_starts_white[i];

        ParsedFEN p = parse_fen(ch.fens[i]);
        REQUIRE_MESSAGE(p.valid, "bad FEN in " << path << ": " << ch.fens[i]);
        GameState gs;
        apply_fen_to_state(gs, p);

        if (type == "mate_in_1") {
            CHECK_MESSAGE(has_mate_in_1(gs, white),
                          "no mate in 1 for: " << ch.fens[i]);
        } else if (type == "mate_in_2") {
            CHECK_MESSAGE(has_mate_in_2(gs, white),
                          "no mate in 2 for: " << ch.fens[i]);
        } else if (type == "find_forks" || type == "find_pins") {
            CHECK_MESSAGE(!find_tactic_moves(gs, white, type).empty(),
                          "no " << type << " move for: " << ch.fens[i]);
        }
    }
}

}  // namespace

TEST_CASE("training.md: every position contains its declared tactic") {
    verify_practice_file("../challenges/training.md");
}
