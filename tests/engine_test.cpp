// Tests for the Stockfish subprocess wrapper in ai_player.cpp —
// ask_ai_move(), stockfish_eval(), ai_player_set_elo(). These are
// excluded from the main test binary by the AI_PLAYER_HELPERS_ONLY
// define; this binary recompiles ai_player.cpp without that flag and
// drives a fake UCI engine (tests/fake_stockfish.py) so the tests
// neither need a real Stockfish nor talk to the network.
//
// The fake engine logs every received UCI command to a file the tests
// read back, and emits "go" responses from a multi-line file the test
// rewrites before each call. There's a single global engine instance
// behind a mutex (ai_player.cpp:g_engine), so test cases here run in
// declaration order and share that one spawned process for the
// success path.

#include "doctest.h"

#include "../ai_player.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {

const char* kLogFile      = "/tmp/3dchess_fake_stockfish.log";
const char* kResponseFile = "/tmp/3dchess_fake_stockfish.response";

void set_fake_env(const char* engine_path) {
    setenv("CHESS_STOCKFISH_PATH", engine_path, 1);
    setenv("FAKE_STOCKFISH_LOG", kLogFile, 1);
    setenv("FAKE_STOCKFISH_RESPONSE", kResponseFile, 1);
    // Keep movetime tiny so tests don't sleep.
    setenv("CHESS_AI_MOVETIME_MS", "10", 1);
}

void write_response(const std::string& contents) {
    std::ofstream out(kResponseFile);
    out << contents;
}

std::string read_log() {
    std::ifstream in(kLogFile);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void clear_log() {
    std::ofstream out(kLogFile, std::ios::trunc);
}

} // namespace

// ===========================================================================
// Failure path: missing engine binary. Runs FIRST so g_engine is still
// nullptr — get_engine_locked() will try to spawn, fail, and reset.
// ===========================================================================
TEST_CASE("ask_ai_move returns empty when the engine binary cannot be exec'd") {
    set_fake_env("/path/that/definitely/does/not/exist/stockfish");
    std::string mv = ask_ai_move(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    CHECK(mv.empty());
}

TEST_CASE("stockfish_eval returns INT_MIN when the engine cannot be exec'd") {
    set_fake_env("/path/that/definitely/does/not/exist/stockfish");
    int score = stockfish_eval(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 10);
    CHECK(score == INT_MIN);
}

// ===========================================================================
// Success path: the rest of these tests share one spawned fake engine.
// ===========================================================================
TEST_CASE("ai_player_set_elo + ask_ai_move spawns and handshakes the engine") {
    set_fake_env("./fake_stockfish.py");
    // 900 < 1320 → handshake takes the Skill Level branch.
    ai_player_set_elo(900);
    write_response(
        "info depth 5 score cp 25\n"
        "bestmove e2e4\n");

    std::string mv = ask_ai_move(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    CHECK(mv == "e2e4");

    std::string log = read_log();
    // UCI handshake.
    CHECK(log.find("uci\n") != std::string::npos);
    CHECK(log.find("isready") != std::string::npos);
    // Skill-level branch (low ELO).
    CHECK(log.find("UCI_LimitStrength value false") != std::string::npos);
    CHECK(log.find("Skill Level value") != std::string::npos);
    // Per-move plumbing.
    CHECK(log.find("ucinewgame") != std::string::npos);
    CHECK(log.find("position fen ") != std::string::npos);
    CHECK(log.find("go movetime ") != std::string::npos);
}

TEST_CASE("stockfish_eval decodes centipawn score from white's perspective") {
    write_response(
        "info depth 8 score cp 75\n"
        "bestmove e2e4\n");
    int score = stockfish_eval(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 10);
    CHECK(score == 75);
}

TEST_CASE("stockfish_eval flips sign when black is to move") {
    // Stockfish reports cp from the side-to-move's perspective; the
    // wrapper converts to white's perspective when black is on move.
    write_response(
        "info depth 8 score cp 75\n"
        "bestmove e7e5\n");
    int score = stockfish_eval(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR b KQkq - 0 1", 10);
    CHECK(score == -75);
}

TEST_CASE("stockfish_eval encodes mate-in-N as 30000 - distance") {
    write_response(
        "info depth 8 score mate 3\n"
        "bestmove a2a4\n");
    int score = stockfish_eval(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 10);
    CHECK(score == 30000 - 3);
}

TEST_CASE("stockfish_eval encodes negative mate as -(30000 - distance)") {
    write_response(
        "info depth 8 score mate -2\n"
        "bestmove a2a4\n");
    int score = stockfish_eval(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 10);
    CHECK(score == -(30000 - 2));
}

TEST_CASE("stockfish_eval picks the deepest score when multiple info lines arrive") {
    write_response(
        "info depth 1 score cp 10\n"
        "info depth 2 score cp 20\n"
        "info depth 5 score cp 50\n"
        "info depth 4 score cp 40\n"   // shallower than 5: should not win
        "bestmove a2a3\n");
    int score = stockfish_eval(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 10);
    CHECK(score == 50);
}

TEST_CASE("ask_ai_move treats 'bestmove (none)' as no move") {
    write_response("bestmove (none)\n");
    std::string mv = ask_ai_move(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    CHECK(mv.empty());
}

TEST_CASE("ask_ai_move strips ponder suffix and returns 4-char UCI") {
    write_response("bestmove g1f3 ponder e7e5\n");
    std::string mv = ask_ai_move(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    CHECK(mv == "g1f3");
}

// ===========================================================================
// ai_player_set_elo bounds + branch crossover at 1320.
// ===========================================================================
TEST_CASE("ai_player_set_elo clamps low input and stays in Skill Level mode") {
    clear_log();  // we'll inspect just the setoption traffic from this call
    ai_player_set_elo(500);  // → clamped to 800 → Skill Level path
    // Force the engine to actually receive the setoption by issuing any
    // call. set_elo on a running engine sends setoption directly,
    // BUT we want the log to contain the latest options. ai_player_set_elo
    // takes the engine mutex and calls apply_elo which writes to the
    // pipe. The fake script reads & logs each line. No need for another
    // ask_ai_move — the setoption traffic is already in the log.
    //
    // Give the script a beat to flush its append-write to disk.
    usleep(50 * 1000);

    std::string log = read_log();
    CHECK(log.find("UCI_LimitStrength value false") != std::string::npos);
    CHECK(log.find("Skill Level value 0") != std::string::npos);
}

TEST_CASE("ai_player_set_elo at 1320 crosses into UCI_Elo mode") {
    clear_log();
    ai_player_set_elo(1320);
    usleep(50 * 1000);

    std::string log = read_log();
    CHECK(log.find("UCI_LimitStrength value true") != std::string::npos);
    CHECK(log.find("UCI_Elo value 1320")          != std::string::npos);
}

TEST_CASE("ai_player_set_elo clamps high input to 3190") {
    clear_log();
    ai_player_set_elo(99999);  // → clamped to 3190
    usleep(50 * 1000);

    std::string log = read_log();
    CHECK(log.find("UCI_Elo value 3190") != std::string::npos);
}
