// Tests for the pure-logic voice-utterance parser
// (voice_input.cpp::parse_voice_move). The parser doesn't depend on
// SDL2 or whisper.cpp, so it links into the regular run_tests binary
// and runs in microseconds.

#include "doctest.h"
#include "helpers.h"

#include "../voice_input.h"

#include <string>

namespace {

// Convenience: parse and return the UCI string, or "" on failure
// (with the error message available in `err`).
std::string parse_or_empty(const std::string& utterance,
                           const GameState& gs,
                           std::string& err) {
    std::string uci;
    err.clear();
    bool ok = parse_voice_move(utterance, gs, uci, err);
    return ok ? uci : std::string();
}

}  // namespace

// ===========================================================================
// Plain forms
// ===========================================================================
TEST_CASE("voice: 'e4' from starting position resolves to e2e4") {
    GameState gs = starting_state();
    std::string err;
    CHECK(parse_or_empty("e4", gs, err) == "e2e4");
}

TEST_CASE("voice: 'pawn e4' resolves the same as 'e4'") {
    GameState gs = starting_state();
    std::string err;
    CHECK(parse_or_empty("pawn e4", gs, err) == "e2e4");
}

TEST_CASE("voice: 'knight f3' resolves to g1f3") {
    GameState gs = starting_state();
    std::string err;
    CHECK(parse_or_empty("knight f3", gs, err) == "g1f3");
}

TEST_CASE("voice: 'knight to f3' (filler verb dropped)") {
    GameState gs = starting_state();
    std::string err;
    CHECK(parse_or_empty("knight to f3", gs, err) == "g1f3");
}

TEST_CASE("voice: 'knight takes' is treated as 'knight'") {
    // White knight on c3 captures pawn on e4 (Nxe4).
    GameState gs = state_from_fen(
        "rnbqkbnr/pppp1ppp/8/8/4p3/2N5/PPPP1PPP/R1BQKBNR w KQkq - 0 1");
    std::string err;
    CHECK(parse_or_empty("knight takes e4", gs, err) == "c3e4");
}

// ===========================================================================
// Homophones / ASR robustness
// ===========================================================================
TEST_CASE("voice: 'night f3' maps to knight f3") {
    GameState gs = starting_state();
    std::string err;
    CHECK(parse_or_empty("night f3", gs, err) == "g1f3");
}

TEST_CASE("voice: 'right a1' maps to rook a1 (when a rook can reach a1)") {
    // White rook on a3 is the only rook that can reach a1.
    GameState gs = state_from_fen("4k3/8/8/8/8/R7/8/4K3 w - - 0 1");
    std::string err;
    CHECK(parse_or_empty("right a1", gs, err) == "a3a1");
}

TEST_CASE("voice: spelled digits ('e four') map to algebraic squares") {
    GameState gs = starting_state();
    std::string err;
    CHECK(parse_or_empty("e four", gs, err) == "e2e4");
}

TEST_CASE("voice: punctuation and casing are tolerated") {
    GameState gs = starting_state();
    std::string err;
    CHECK(parse_or_empty("KNIGHT, F3.", gs, err) == "g1f3");
}

// ===========================================================================
// Disambiguation
// ===========================================================================
TEST_CASE("voice: ambiguous 'knight d3' returns an error listing the source files") {
    // Two white knights on b2 and f2 — both can reach d3.
    GameState gs = state_from_fen("4k3/8/8/8/8/8/1N3N2/4K3 w - - 0 1");
    std::string uci, err;
    bool ok = parse_voice_move("knight d3", gs, uci, err);
    CHECK_FALSE(ok);
    CHECK(err.find("Ambiguous") != std::string::npos);
    CHECK(err.find('b') != std::string::npos);
    CHECK(err.find('f') != std::string::npos);
}

TEST_CASE("voice: file disambiguator resolves 'b knight d3'") {
    GameState gs = state_from_fen("4k3/8/8/8/8/8/1N3N2/4K3 w - - 0 1");
    std::string err;
    CHECK(parse_or_empty("b knight d3", gs, err) == "b2d3");
}

TEST_CASE("voice: file disambiguator resolves 'knight f d3'") {
    GameState gs = state_from_fen("4k3/8/8/8/8/8/1N3N2/4K3 w - - 0 1");
    std::string err;
    CHECK(parse_or_empty("knight f d3", gs, err) == "f2d3");
}

// ===========================================================================
// Castling
// ===========================================================================
TEST_CASE("voice: 'castle kingside' maps to e1g1 for white") {
    GameState gs = state_from_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    std::string err;
    CHECK(parse_or_empty("castle kingside", gs, err) == "e1g1");
}

TEST_CASE("voice: 'castle queenside' maps to e1c1 for white") {
    GameState gs = state_from_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    std::string err;
    CHECK(parse_or_empty("castle queenside", gs, err) == "e1c1");
}

TEST_CASE("voice: 'long castle' is queenside") {
    GameState gs = state_from_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    std::string err;
    CHECK(parse_or_empty("long castle", gs, err) == "e1c1");
}

TEST_CASE("voice: 'short castle' is kingside") {
    GameState gs = state_from_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    std::string err;
    CHECK(parse_or_empty("short castle", gs, err) == "e1g1");
}

TEST_CASE("voice: 'oo' shorthand is kingside") {
    GameState gs = state_from_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    std::string err;
    CHECK(parse_or_empty("oo", gs, err) == "e1g1");
}

TEST_CASE("voice: 'ooo' shorthand is queenside") {
    GameState gs = state_from_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    std::string err;
    CHECK(parse_or_empty("ooo", gs, err) == "e1c1");
}

TEST_CASE("voice: castling on black's move maps to e8 squares") {
    GameState gs = state_from_fen("r3k2r/8/8/8/8/8/8/R3K2R b KQkq - 0 1");
    std::string err;
    CHECK(parse_or_empty("castle kingside", gs, err) == "e8g8");
}

TEST_CASE("voice: cannot castle when path is blocked → error") {
    // The king cannot castle from the standard starting position
    // (knight and bishop block both sides).
    GameState gs = starting_state();
    std::string uci, err;
    CHECK_FALSE(parse_voice_move("castle kingside", gs, uci, err));
    CHECK(err.find("Cannot castle") != std::string::npos);
}

// ===========================================================================
// Failure modes
// ===========================================================================
TEST_CASE("voice: empty utterance → error") {
    GameState gs = starting_state();
    std::string uci, err;
    CHECK_FALSE(parse_voice_move("", gs, uci, err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("voice: garbage with no destination square → error") {
    GameState gs = starting_state();
    std::string uci, err;
    CHECK_FALSE(parse_voice_move("hello world", gs, uci, err));
    CHECK(err.find("destination") != std::string::npos);
}

TEST_CASE("voice: 'knight a1' from starting position → illegal") {
    GameState gs = starting_state();
    std::string uci, err;
    CHECK_FALSE(parse_voice_move("knight a1", gs, uci, err));
    CHECK(err.find("Illegal") != std::string::npos);
}

TEST_CASE("voice: 'pawn e8 queen' (auto-queens — promotion suffix dropped)") {
    // White pawn on e7, e8 empty (black king parked on a8 so it's
    // not in the e-file). Pe7-e8 promotes; execute_move auto-queens.
    GameState gs = state_from_fen("k7/4P3/8/8/8/8/8/4K3 w - - 0 1");
    std::string err;
    CHECK(parse_or_empty("pawn e8 queen", gs, err) == "e7e8");
}
