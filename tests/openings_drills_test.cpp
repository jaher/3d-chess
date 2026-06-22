// Parses the opening-drill data and checks every line is playable
// move-by-move from the start (each UCI move legal in turn), so a typo
// in openings/drills.md fails the build. run_tests executes from the
// tests/ directory, so the data file is one level up.

#include "doctest.h"
#include "helpers.h"

#include "../chess_rules.h"
#include "../chess_types.h"
#include "../openings_drills.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {
// Is (to) reachable from (from) for the side to move, per legal-move gen?
bool move_is_legal(GameState& gs, int fc, int fr, int tc, int tr) {
    auto moves = generate_legal_moves(gs, fc, fr);
    return std::find(moves.begin(), moves.end(),
                     std::make_pair(tc, tr)) != moves.end();
}
}  // namespace

TEST_CASE("parse_opening_drills reads name + line blocks") {
    std::string text =
        "# comment\n"
        "name: Test A\n"
        "line: e2e4 e7e5 g1f3\n"
        "\n"
        "name: Test B\n"
        "line: d2d4 d7d5\n";
    auto drills = parse_opening_drills(text);
    REQUIRE(drills.size() == 2);
    CHECK(drills[0].name == "Test A");
    CHECK(drills[0].moves == std::vector<std::string>{"e2e4", "e7e5", "g1f3"});
    CHECK(drills[1].name == "Test B");
    CHECK(drills[1].moves.size() == 2);
}

TEST_CASE("parse_opening_drills skips blocks with no name or no moves") {
    auto d = parse_opening_drills("line: e2e4\nname: Lonely\n");
    CHECK(d.empty());
}

TEST_CASE("every opening drill line is legal move-by-move, ends on your move") {
    auto drills = load_opening_drills("../openings/drills.md");
    REQUIRE_MESSAGE(!drills.empty(),
                    "could not load ../openings/drills.md");

    for (const auto& drill : drills) {
        CAPTURE(drill.name);
        // The human's side moves first, so an odd-length line ends on
        // the human's move — finishing it = drill solved.
        CHECK((drill.moves.size() % 2) == 1);

        GameState gs = starting_state();
        for (const std::string& uci : drill.moves) {
            CAPTURE(uci);
            REQUIRE(uci.size() >= 4);
            int fc = 7 - (uci[0] - 'a'), fr = uci[1] - '1';
            int tc = 7 - (uci[2] - 'a'), tr = uci[3] - '1';
            REQUIRE(move_is_legal(gs, fc, fr, tc, tr));
            execute_move(gs, fc, fr, tc, tr);
        }
    }
}
