// JS bridge to a Stockfish.js Worker for the WebAssembly build.
//
// The desktop build (ai_player.cpp) talks to a native Stockfish subprocess
// over stdin/stdout pipes. In the browser, Stockfish runs inside a Web
// Worker (web/stockfish/stockfish.js) and we exchange UCI commands by
// posting messages. The C++ side here is a thin shim:
//
//   - js_request_ai_move() / js_request_eval(): EM_JS-defined functions
//     that hand off to the JS-side StockfishBridge object (defined in
//     web/stockfish-bridge.js, which is loaded in index.html before the
//     Emscripten module).
//
//   - on_ai_move_from_js() / on_eval_from_js(): EMSCRIPTEN_KEEPALIVE C
//     entry points that the JS side calls (via Module.ccall) when the
//     Stockfish worker reports a result. They write the result into
//     small global slots that main_web.cpp polls each frame.
//
// Helper functions like board_to_fen() / parse_uci_move() / move_to_uci()
// come from compiling ai_player.cpp itself with -DAI_PLAYER_HELPERS_ONLY.
// That macro #ifdefs out the StockfishEngine subprocess code while leaving
// the FEN/UCI parsers in place, so we don't duplicate them here.

#include "../ai_player.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <emscripten.h>
#include <emscripten/em_macros.h>

// ---------------------------------------------------------------------------
// Result slots written by JS, read by main_web.cpp.
// ---------------------------------------------------------------------------
namespace web_ai {
    bool        move_ready = false;
    std::string move_uci;

    bool eval_ready = false;
    int  eval_cp = 0;
    int  eval_index = -1;
}

// ---------------------------------------------------------------------------
// JS-side request stubs (defined in web/stockfish-bridge.js).
// ---------------------------------------------------------------------------
EM_JS(void, js_request_ai_move, (const char* fen, int movetime), {
    if (typeof StockfishBridge !== 'undefined') {
        StockfishBridge.requestMove(UTF8ToString(fen), movetime);
    } else {
        console.warn('StockfishBridge not loaded; AI move dropped');
    }
});

EM_JS(void, js_request_eval, (const char* fen, int movetime, int idx), {
    if (typeof StockfishBridge !== 'undefined') {
        StockfishBridge.requestEval(UTF8ToString(fen), movetime, idx);
    } else {
        console.warn('StockfishBridge not loaded; eval dropped');
    }
});

EM_JS(void, js_set_ai_elo, (int elo), {
    if (typeof StockfishBridge !== 'undefined' &&
        typeof StockfishBridge.setElo === 'function') {
        StockfishBridge.setElo(elo);
    }
});

// ---------------------------------------------------------------------------
// Public C++ entry points used by main_web.cpp.
// ---------------------------------------------------------------------------
void web_request_ai_move(const std::string& fen, int movetime_ms) {
    web_ai::move_ready = false;
    web_ai::move_uci.clear();
    js_request_ai_move(fen.c_str(), movetime_ms);
}

void web_request_eval(const std::string& fen, int movetime_ms, int score_index) {
    web_ai::eval_ready = false;
    web_ai::eval_cp = 0;
    web_ai::eval_index = score_index;
    js_request_eval(fen.c_str(), movetime_ms, score_index);
}

void web_set_ai_elo(int elo) {
    js_set_ai_elo(elo);
}

// ---------------------------------------------------------------------------
// EMSCRIPTEN_KEEPALIVE callbacks invoked from JS via Module.ccall.
// ---------------------------------------------------------------------------
extern "C" {

EMSCRIPTEN_KEEPALIVE
void on_ai_move_from_js(const char* uci) {
    if (uci && uci[0]) {
        web_ai::move_uci = uci;
    } else {
        web_ai::move_uci.clear();
    }
    web_ai::move_ready = true;
}

EMSCRIPTEN_KEEPALIVE
void on_eval_from_js(int cp, int idx) {
    web_ai::eval_cp = cp;
    web_ai::eval_index = idx;
    web_ai::eval_ready = true;
}

}  // extern "C"

// ---------------------------------------------------------------------------
// Stub implementations of the desktop ask_ai_move / stockfish_eval entry
// points so any stray caller (and the linker, if it imports the symbols)
// gets a sensible empty response. main_web.cpp uses web_request_* directly
// instead of these.
// ---------------------------------------------------------------------------
std::string ask_ai_move(const std::string& /*fen*/) {
    return "";  // synchronous interface unused in the web build
}

int stockfish_eval(const std::string& /*fen*/, int /*movetime_ms*/) {
    return INT_MIN;
}
