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
