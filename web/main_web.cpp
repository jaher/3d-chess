// WebAssembly driver: SDL2 window, WebGL 2 via Emscripten, Stockfish.js
// Worker dispatch via ai_player_web.cpp + stockfish-bridge.js. All
// gameplay logic lives in the shared app_state.cpp — this file just
// wires SDL events and the per-frame main loop to the app_* functions.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <emscripten.h>
#include <emscripten/em_macros.h>
#include <emscripten/html5.h>
#include <SDL2/SDL.h>
#include <GLES3/gl3.h>

#include "../app_state.h"
#include "../board_renderer.h"
#include "../chess_types.h"
#include "../stl_model.h"

// ---------------------------------------------------------------------------
// Bridge into web/ai_player_web.cpp
// ---------------------------------------------------------------------------
extern void web_request_ai_move(const std::string& fen, int movetime_ms);
extern void web_request_eval(const std::string& fen, int movetime_ms,
                             int score_index);

namespace web_ai {
    extern bool        move_ready;
    extern std::string move_uci;
    extern bool eval_ready;
    extern int  eval_cp;
    extern int  eval_index;
}

// ---------------------------------------------------------------------------
// JS-side hooks (set the HTML status div, emit console checkpoint logs)
// ---------------------------------------------------------------------------
EM_JS(void, set_status_js, (const char* s), {
    var el = document.getElementById('chess-status');
    if (el) el.textContent = UTF8ToString(s);
});

EM_JS(void, js_log, (const char* s), {
    console.log('[wasm-checkpoint]', UTF8ToString(s));
});

// ---------------------------------------------------------------------------
// Platform state
// ---------------------------------------------------------------------------
static SDL_Window*   g_window = nullptr;
static SDL_GLContext g_gl_ctx = nullptr;
static int g_width  = 1024;
static int g_height = 768;

static StlModel g_loaded_models[PIECE_COUNT];
static AppState g_app;

// Called from JS (web/index.html resizeCanvas) whenever the canvas drawing
// buffer changes — orientation change, viewport resize, or initial page
// load on a non-1024x768 viewport. Updates the SDL window so the renderer
// picks up the new viewport on the next frame.
extern "C" EMSCRIPTEN_KEEPALIVE
void chess_resize(int w, int h) {
    if (w <= 0 || h <= 0) return;
    g_width = w;
    g_height = h;
    if (g_window) SDL_SetWindowSize(g_window, w, h);
}

// ---------------------------------------------------------------------------
// Platform hooks (AppPlatform)
// ---------------------------------------------------------------------------
static void plat_set_status(const char* text) {
    set_status_js(text);
}

static void plat_queue_redraw(void) {
    // No-op: main_loop_iter renders every frame unconditionally.
}

static int64_t plat_now_us(void) {
    // emscripten_get_now() returns milliseconds since page load.
    return static_cast<int64_t>(emscripten_get_now() * 1000.0);
}

static void plat_trigger_ai_move(const char* fen, int movetime_ms) {
    web_request_ai_move(fen ? std::string(fen) : std::string(), movetime_ms);
}

static void plat_trigger_eval(const char* fen, int movetime_ms, int idx) {
    web_request_eval(fen ? std::string(fen) : std::string(), movetime_ms, idx);
}

static const AppPlatform g_platform = {
    plat_set_status,
    plat_queue_redraw,
    plat_now_us,
    plat_trigger_ai_move,
    plat_trigger_eval,
};

// ---------------------------------------------------------------------------
// SDL event translation
// ---------------------------------------------------------------------------
static AppKey translate_key(SDL_Keycode k) {
    switch (k) {
        case SDLK_LEFT:   return KEY_LEFT;
        case SDLK_RIGHT:  return KEY_RIGHT;
        case SDLK_ESCAPE: return KEY_ESCAPE;
        case SDLK_a:      return KEY_A;
        case SDLK_m:      return KEY_M;
        default:          return KEY_UNKNOWN;
    }
}

static void pump_events() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT)
                    app_press(g_app, ev.button.x, ev.button.y);
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT)
                    app_release(g_app, ev.button.x, ev.button.y,
                                g_width, g_height);
                break;
            case SDL_MOUSEMOTION:
                app_motion(g_app, ev.motion.x, ev.motion.y,
                           g_width, g_height);
                break;
            case SDL_MOUSEWHEEL:
                // SDL wheel y is +1 up, -1 down. app_scroll follows the
                // "positive delta = zoom out" convention.
                app_scroll(g_app, -static_cast<double>(ev.wheel.y));
                break;
            case SDL_KEYDOWN:
                app_key(g_app, translate_key(ev.key.keysym.sym));
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                    ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    g_width  = ev.window.data1;
                    g_height = ev.window.data2;
                }
                break;
        }
    }
}

static void poll_ai_results() {
    if (web_ai::move_ready) {
        web_ai::move_ready = false;
        app_ai_move_ready(g_app, web_ai::move_uci.c_str());
    }
    if (web_ai::eval_ready) {
        web_ai::eval_ready = false;
        app_eval_ready(g_app, web_ai::eval_cp, web_ai::eval_index);
    }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
static void main_loop_iter() {
    pump_events();
    poll_ai_results();
    app_tick(g_app);

    glViewport(0, 0, g_width, g_height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    app_render(g_app, g_width, g_height);

    SDL_GL_SwapWindow(g_window);
}

// ---------------------------------------------------------------------------
// Entry point (driven from JS, see web/index.html onRuntimeInitialized)
// ---------------------------------------------------------------------------
extern "C" EMSCRIPTEN_KEEPALIVE
int chess_start(void) {
    // Force stdio unbuffered so every printf/fprintf reaches Module.print
    // immediately — we never return from this function normally;
    // emscripten throws a JS exception out of emscripten_set_main_loop
    // with simulate_infinite_loop=1.
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);

    js_log("chess_start() entered");

    js_log("loading models");
    for (int i = 0; i < PIECE_COUNT; i++) {
        std::string path = std::string("/models/") + piece_filenames[i];
        try {
            g_loaded_models[i].load(path);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "STL load failed (%s): %s\n",
                         path.c_str(), e.what());
            js_log("STL load THREW");
            return 1;
        }
        std::fprintf(stderr, "  %s: %zu triangles\n",
                     piece_filenames[i],
                     g_loaded_models[i].triangle_count());
    }
    js_log("models loaded");

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        js_log("SDL_Init failed");
        return 1;
    }
    js_log("SDL_Init ok");

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    g_window = SDL_CreateWindow(
        "3D Chess",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        g_width, g_height,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!g_window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        js_log("SDL_CreateWindow failed");
        return 1;
    }
    js_log("SDL_CreateWindow ok");

    g_gl_ctx = SDL_GL_CreateContext(g_window);
    if (!g_gl_ctx) {
        std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        js_log("SDL_GL_CreateContext failed");
        return 1;
    }
    js_log("GL context created");

    renderer_init(g_loaded_models);
    js_log("renderer_init done");

    app_init(g_app, &g_platform);
    app_enter_menu(g_app);
    js_log("entering main loop");

    emscripten_set_main_loop(main_loop_iter, 0, 1);
    return 0;
}
