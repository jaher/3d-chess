#include "doctest.h"
#include "../puzzle.h"

#include <string>

TEST_CASE("puzzle_parse_string_field: simple flat fields") {
    std::string body =
        R"({"title":"Daily Puzzle","fen":"r1bq1rk1/pp/8 w - - 0 1"})";
    std::string out;
    CHECK(puzzle_parse_string_field(body, "fen", out));
    CHECK(out == "r1bq1rk1/pp/8 w - - 0 1");
    CHECK(puzzle_parse_string_field(body, "title", out));
    CHECK(out == "Daily Puzzle");
}

TEST_CASE("puzzle_parse_string_field: missing key returns false") {
    std::string body = R"({"fen":"abc"})";
    std::string out = "untouched";
    CHECK_FALSE(puzzle_parse_string_field(body, "url", out));
    CHECK(out.empty());
}

TEST_CASE("puzzle_parse_string_field: handles whitespace + escape") {
    std::string body =
        "{ \"title\" : \"Pretty \\\"Trap\\\" Mate\" , \"fen\" : \"abc\" }";
    std::string out;
    CHECK(puzzle_parse_string_field(body, "title", out));
    CHECK(out == "Pretty \"Trap\" Mate");
}

TEST_CASE("puzzle_parse_string_field: unescapes \\/ and \\n") {
    std::string body = R"({"url":"https:\/\/chess.com\/x","note":"a\nb"})";
    std::string out;
    CHECK(puzzle_parse_string_field(body, "url", out));
    CHECK(out == "https://chess.com/x");
    CHECK(puzzle_parse_string_field(body, "note", out));
    CHECK(out == "a\nb");
}

TEST_CASE("puzzle_parse_solution_uci: empty inputs return empty list") {
    CHECK(puzzle_parse_solution_uci("", "").empty());
    CHECK(puzzle_parse_solution_uci("not a fen", "1. e4 e5").empty());
    CHECK(puzzle_parse_solution_uci(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", "")
        .empty());
}

TEST_CASE("puzzle_parse_solution_uci: simple opening line") {
    auto out = puzzle_parse_solution_uci(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "1. e4 e5 2. Nf3 Nc6 *");
    REQUIRE(out.size() == 4);
    CHECK(out[0] == "e2e4");
    CHECK(out[1] == "e7e5");
    CHECK(out[2] == "g1f3");
    CHECK(out[3] == "b8c6");
}

TEST_CASE("puzzle_parse_solution_uci: chess.com Kubbel 1929 daily puzzle") {
    // FEN + PGN exactly as the chess.com /pub/puzzle endpoint
    // returns them. Verifies the full pipeline (header strip,
    // SAN→UCI, position update through the entire 9-ply line).
    std::string fen = "8/pb6/k1p4q/2P2Q1p/6P1/2P5/8/6K1 w - - 0 1";
    std::string pgn =
        "[Date \"1929.??.??\"]\n"
        "[White \"Kubbel\"]\n"
        "[Black \"Study - Win\"]\n"
        "[Result \"1-0\"]\n"
        "[FEN \"8/pb6/k1p4q/2P2Q1p/6P1/2P5/8/6K1 w - - 0 1\"]\n\n"
        "1. g5 Qh8 2. Qf1+ Ka5 3. Qa1+ Kb5 4. c4+ Kxc4 5. Qxh8 1-0";
    auto out = puzzle_parse_solution_uci(fen, pgn);
    REQUIRE(out.size() == 9);
    CHECK(out[0] == "g4g5");
    CHECK(out[1] == "h6h8");
    CHECK(out[2] == "f5f1");
    CHECK(out[3] == "a6a5");
    CHECK(out[4] == "f1a1");
    CHECK(out[5] == "a5b5");
    CHECK(out[6] == "c3c4");
    CHECK(out[7] == "b5c4");
    CHECK(out[8] == "a1h8");
}

TEST_CASE("puzzle_parse_solution_uci: kingside castling notation parses") {
    auto out = puzzle_parse_solution_uci(
        "r3k2r/pppqbppp/2nppn2/8/8/2NPPN2/PPPQBPPP/R3K2R w KQkq - 0 1",
        "1. O-O O-O *");
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "e1g1");
    CHECK(out[1] == "e8g8");
}

TEST_CASE("puzzle_parse_solution_uci: queenside castling notation parses") {
    auto out = puzzle_parse_solution_uci(
        "r3k2r/pppqbppp/2nppn2/8/8/2NPPN2/PPPQBPPP/R3K2R w KQkq - 0 1",
        "1. O-O-O O-O-O *");
    REQUIRE(out.size() == 2);
    CHECK(out[0] == "e1c1");
    CHECK(out[1] == "e8c8");
}

TEST_CASE("puzzle_parse_solution_uci: strips glued move-number prefixes") {
    // chess.com sometimes serialises the move list without a space
    // between the move number and the SAN ("1...Nh3+" instead of
    // "1... Nh3+"). The tokenizer must still turn each entry into
    // a clean SAN before brute-force matching, otherwise no legal
    // move starts with "1...".
    auto out = puzzle_parse_solution_uci(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "1.e4 e5 2.Nf3 Nc6");
    REQUIRE(out.size() == 4);
    CHECK(out[0] == "e2e4");
    CHECK(out[1] == "e7e5");
    CHECK(out[2] == "g1f3");
    CHECK(out[3] == "b8c6");

    // And the same with a black-side prefix.
    auto out2 = puzzle_parse_solution_uci(
        "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1",
        "1...e5 2.Nf3 Nc6");
    REQUIRE(out2.size() == 3);
    CHECK(out2[0] == "e7e5");
    CHECK(out2[1] == "g1f3");
    CHECK(out2[2] == "b8c6");
}

TEST_CASE("puzzle_parse_solution_uci: bails out on illegal SAN") {
    // After 1.e4 e5 2.Nf3 it's black to move; "Nf6" is legal but
    // "Nz9" isn't. The parser must bail rather than half-fill.
    auto out = puzzle_parse_solution_uci(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "1. e4 e5 2. Nf3 Nf6 3. Bzz5");
    CHECK(out.empty());
}

TEST_CASE("puzzle_parse_json: full envelope, FEN required") {
    std::string body =
        R"({"title":"Daily","url":"https://chess.com/p","fen":"k7/8 w - - 0 1",)"
        R"("pgn":"[Event \"Daily\"]\n\n1.e4 e5"})";
    Puzzle p;
    REQUIRE(puzzle_parse_json(body, p));
    CHECK(p.fen == "k7/8 w - - 0 1");
    CHECK(p.title == "Daily");
    CHECK(p.url   == "https://chess.com/p");
    CHECK(p.pgn   == "[Event \"Daily\"]\n\n1.e4 e5");

    // Missing FEN = parse failure.
    Puzzle q;
    CHECK_FALSE(puzzle_parse_json(R"({"title":"x"})", q));
}
