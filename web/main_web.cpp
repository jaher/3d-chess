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
#include "../audio.h"
#include "../board_renderer.h"
#include "../chess_types.h"
#include "../stl_model.h"

// ---------------------------------------------------------------------------
// Bridge into web/ai_player_web.cpp
// ---------------------------------------------------------------------------
extern void web_request_ai_move(const std::string& fen, int movetime_ms);
extern void web_request_eval(const std::string& fen, int movetime_ms,
                             int score_index);
extern void web_set_ai_elo(int elo);

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

static void plat_set_ai_elo(int elo) {
    web_set_ai_elo(elo);
}

static const AppPlatform g_platform = {
    plat_set_status,
    plat_queue_redraw,
    plat_now_us,
    plat_trigger_ai_move,
    plat_trigger_eval,
    plat_set_ai_elo,
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
        case SDLK_s:      return KEY_S;
        default:          return KEY_UNKNOWN;
    }
}

// ---------------------------------------------------------------------------
// Touch state for pinch-zoom on mobile.
// ---------------------------------------------------------------------------
// SDL_HINT_TOUCH_MOUSE_EVENTS is disabled in chess_start() so touches
// never synthesise mouse events — we handle SDL_FINGER* directly. One
// or two fingers are tracked (anything past the second is ignored):
//
//   - One finger: behaves as a mouse — app_press/motion/release with
//     the finger's pixel position.
//   - Two fingers: enters pinch mode. The change in distance between
//     the two fingers is fed into app_scroll(delta) (negative delta =
//     fingers moving apart = zoom in closer, matching the wheel
//     convention used above). Entering pinch mode also emits a
//     synthetic app_release so the initial one-finger press doesn't
//     linger as a board click when the user drops the second finger.
static constexpr SDL_FingerID NO_FID = -1;
static SDL_FingerID g_fid_a = NO_FID;
static SDL_FingerID g_fid_b = NO_FID;
static float g_fa_x = 0.0f, g_fa_y = 0.0f;  // normalised 0..1
static float g_fb_x = 0.0f, g_fb_y = 0.0f;
static bool  g_pinch_active = false;
static float g_pinch_last_dist = 0.0f;

static inline int fpx(float norm_x) {
    return static_cast<int>(norm_x * static_cast<float>(g_width));
}
static inline int fpy(float norm_y) {
    return static_cast<int>(norm_y * static_cast<float>(g_height));
}

// Pixel distance between the two tracked fingers. Uses pixels, not
// normalised coords, so pinch sensitivity scales naturally with
// viewport size.
static inline float pinch_distance_px() {
    float dx = (g_fa_x - g_fb_x) * static_cast<float>(g_width);
    float dy = (g_fa_y - g_fb_y) * static_cast<float>(g_height);
    return std::sqrt(dx * dx + dy * dy);
}

// Converts a pinch pixel-delta into the scroll-delta units that
// app_scroll understands. app_scroll halves the delta internally and
// clamps zoom to [3, 40]. 0.035 makes a ~300 px pinch span the
// noticeable part of the zoom range without feeling twitchy.
static constexpr double PINCH_SENSITIVITY = 0.035;

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
            case SDL_FINGERDOWN: {
                SDL_FingerID fid = ev.tfinger.fingerId;
                if (g_fid_a == NO_FID) {
                    g_fid_a = fid;
                    g_fa_x  = ev.tfinger.x;
                    g_fa_y  = ev.tfinger.y;
                    app_press(g_app, fpx(g_fa_x), fpy(g_fa_y));
                } else if (g_fid_b == NO_FID && fid != g_fid_a) {
                    g_fid_b = fid;
                    g_fb_x  = ev.tfinger.x;
                    g_fb_y  = ev.tfinger.y;
                    // Cancel the one-finger press so dropping the
                    // second finger doesn't also register as a
                    // board click.
                    app_release(g_app, fpx(g_fa_x), fpy(g_fa_y),
                                g_width, g_height);
                    g_pinch_active = true;
                    g_pinch_last_dist = pinch_distance_px();
                }
                // 3+ fingers: silently ignored.
                break;
            }
            case SDL_FINGERMOTION: {
                SDL_FingerID fid = ev.tfinger.fingerId;
                if (fid == g_fid_a) {
                    g_fa_x = ev.tfinger.x; g_fa_y = ev.tfinger.y;
                } else if (fid == g_fid_b) {
                    g_fb_x = ev.tfinger.x; g_fb_y = ev.tfinger.y;
                } else {
                    break;
                }
                if (g_pinch_active) {
                    float cur = pinch_distance_px();
                    float delta = cur - g_pinch_last_dist;
                    g_pinch_last_dist = cur;
                    // 0.5 px dead zone avoids feeding jitter into
                    // the clamped zoom accumulator.
                    if (std::fabs(delta) > 0.5f) {
                        app_scroll(g_app,
                                   -static_cast<double>(delta) *
                                   PINCH_SENSITIVITY);
                    }
                } else if (fid == g_fid_a) {
                    app_motion(g_app, fpx(g_fa_x), fpy(g_fa_y),
                               g_width, g_height);
                }
                break;
            }
            case SDL_FINGERUP: {
                SDL_FingerID fid = ev.tfinger.fingerId;
                if (fid == g_fid_b) {
                    // Second finger lifted. Exit pinch mode but
                    // leave finger_a tracked — the user can still
                    // rotate the camera by dragging finger_a,
                    // though without a fresh app_press we won't
                    // reselect a square.
                    g_fid_b = NO_FID;
                    g_pinch_active = false;
                } else if (fid == g_fid_a) {
                    if (g_pinch_active) {
                        // Pinch ended by lifting finger_a. Promote
                        // finger_b to be the primary finger.
                        g_fa_x = g_fb_x; g_fa_y = g_fb_y;
                        g_fid_a = g_fid_b;
                        g_fid_b = NO_FID;
                        g_pinch_active = false;
                    } else {
                        // Normal single-finger release.
                        app_release(g_app,
                                    fpx(ev.tfinger.x), fpy(ev.tfinger.y),
                                    g_width, g_height);
                        g_fid_a = NO_FID;
                    }
                }
                break;
            }
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

    // Disable synthetic mouse events from touch so we can handle
    // SDL_FINGER* directly (for two-finger pinch-zoom on mobile).
    // Must be set BEFORE SDL_Init. With this off, a tap on a mobile
    // browser comes through as SDL_FINGERDOWN/UP only, which we
    // translate back into app_press/release in pump_events.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        js_log("SDL_Init failed");
        return 1;
    }
    js_log("SDL_Init ok");

    audio_init();

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
    g_app.loaded_models = g_loaded_models;
    app_enter_menu(g_app);
    js_log("entering main loop");

    emscripten_set_main_loop(main_loop_iter, 0, 1);
    return 0;
}
