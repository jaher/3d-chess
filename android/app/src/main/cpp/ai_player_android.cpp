// ===========================================================================
// android/app/src/main/cpp/ai_player_android.cpp — PLACEHOLDER AI (Android).
// ===========================================================================
//
//   ⚠️  THIS IS A PLACEHOLDER. It does NOT play strong chess. ⚠️
//
// Identical in spirit to ios/ai_player_ios.cpp. The desktop build drives
// Stockfish as a child process over stdin/stdout pipes (ai_player.cpp's
// StockfishEngine, via fork()/exec()). **An Android APK cannot fork()/exec() a
// bundled native executable** under the app sandbox (and the Play Store forbids
// it), so that path cannot run on a device. The web build sidesteps this by
// running Stockfish.js in a Web Worker (web/ai_player_web.cpp); Android has no
// equivalent drop-in.
//
// For the SCAFFOLD we provide a legal-move generator so the app is runnable on
// an emulator / device without an engine. It parses the FEN the shared layer
// hands us, enumerates the legal moves for the side to move (reusing the
// engine's own move generator in chess_rules.cpp) and returns one — lightly
// preferring captures so the "AI" is not purely random. If anything goes wrong
// it returns "" and the shared layer's own ai_random_fallback()
// (app_state.cpp) plays a legal move.
//
// ---------------------------------------------------------------------------
// THE REAL FIX (primary remaining TODO — see docs/ANDROID.md):
//
//   Link Stockfish as an IN-PROCESS static library (built for the target ABIs
//   with the NDK) and drive UCI over an in-process channel (a worker
//   std::thread + a couple of queues, or in-process pipes), exactly the way
//   embedded-engine chess apps on Android do. Stockfish builds cleanly with the
//   NDK clang for arm64-v8a; the only work is:
//     1. Add Stockfish's sources (or a prebuilt libstockfish.a per ABI) to the
//        Android CMake target instead of the subprocess.
//     2. Replace ask_ai_move()/stockfish_eval() below with calls that push UCI
//        commands onto the in-process engine thread and read back `bestmove` /
//        `info ... score cp` lines — the SAME protocol ai_player.cpp already
//        speaks, just without fork()/exec().
//   The FEN/UCI helpers (parse_fen, move_to_uci, …) are already shared:
//   ai_player.cpp is compiled with -DAI_PLAYER_HELPERS_ONLY on Android (see the
//   CMakeLists), exactly as the web build does, so those parsers are available
//   and need no duplication.
// ===========================================================================

#include "ai_player.h"

#include "challenge.h"     // parse_fen / apply_fen_to_state / ParsedFEN
#include "chess_rules.h"   // generate_legal_moves
#include "game_state.h"
#include "chess_types.h"

#include <android/log.h>

#include <chrono>
#include <climits>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Placeholder opponent: pick a legal move for the side to move in `fen`.
// Returns a 4-char UCI move ("e7e5"), or "" so the shared layer's
// ai_random_fallback() takes over.
// ---------------------------------------------------------------------------
std::string ask_ai_move(const std::string& fen) {
    ParsedFEN parsed = parse_fen(fen);
    if (!parsed.valid) {
        __android_log_print(ANDROID_LOG_WARN, "3dchess",
            "[ai] could not parse FEN; deferring to shared fallback");
        return "";  // shared ai_random_fallback() plays a legal move
    }

    GameState gs;
    apply_fen_to_state(gs, parsed);

    std::vector<std::pair<std::pair<int, int>, std::pair<int, int>>> all_legal;
    std::vector<std::pair<std::pair<int, int>, std::pair<int, int>>> captures;
    for (const auto& p : gs.pieces) {
        if (!p.alive || p.is_white != gs.white_turn) continue;
        auto moves = generate_legal_moves(gs, p.col, p.row);
        for (const auto& [mc, mr] : moves) {
            all_legal.push_back({{p.col, p.row}, {mc, mr}});
            if (mr >= 0 && mr < 8 && mc >= 0 && mc < 8 && gs.grid[mr][mc] >= 0)
                captures.push_back({{p.col, p.row}, {mc, mr}});
        }
    }
    if (all_legal.empty()) return "";  // mate / stalemate — nothing to play

    auto& pool = captures.empty() ? all_legal : captures;
    uint64_t t = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto& [from, to] = pool[t % pool.size()];
    std::string uci = move_to_uci(from.first, from.second, to.first, to.second);
    __android_log_print(ANDROID_LOG_INFO, "3dchess",
        "[ai] placeholder plays %s (legal)", uci.c_str());
    return uci;
}

// ---------------------------------------------------------------------------
// Evaluation is unavailable without a real engine. Returning INT_MIN matches
// the web/iOS stubs and tells the shared layer "no eval"; the hint /
// move-classification / eval-bar features degrade gracefully. Wiring an
// in-process Stockfish (above) restores all of them for free.
// ---------------------------------------------------------------------------
int stockfish_eval(const std::string& /*fen*/, int /*movetime_ms*/) {
    return INT_MIN;
}

int stockfish_eval(const std::string& /*fen*/, int /*movetime_ms*/,
                   std::string& out_best_uci,
                   std::string* out_second_uci,
                   int* out_second_cp) {
    out_best_uci.clear();
    if (out_second_uci) out_second_uci->clear();
    if (out_second_cp)  *out_second_cp = 0;
    return INT_MIN;
}

// No-op: no engine strength to set on the placeholder. With an in-process
// Stockfish this would latch UCI_Elo / Skill Level the same way
// ai_player.cpp's ai_player_set_elo() does.
void ai_player_set_elo(int /*elo*/) {}
