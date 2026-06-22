// Native desktop driver: SDL2 window + OpenGL 3.3 Core context, no GTK.
//
// This is the portable sibling of main.cpp (the GTK driver). It targets
// native macOS (and Linux / Windows) where GTK either isn't available or
// isn't wanted: it opens a real SDL2 window, creates a 3.3-core GL context
// (the highest common denominator that works on macOS, which caps at GL 4.1
// core and has NO compute shaders), and runs an explicit
// `while (running) { poll; tick; render; swap; }` loop.
//
// Everything gameplay-related lives in the platform-agnostic app_state.cpp —
// this file only wires SDL events + windowing + the worker-thread plumbing
// to the shared app_* functions, exactly like main.cpp does for GTK and
// web/main_web.cpp does for emscripten. The platform-hook implementations
// (curl puzzle fetch, Stockfish subprocess dispatch via std::thread, the
// voice push-to-talk and Chessnut bridge marshalling) are ported verbatim
// from main.cpp; the only GTK-specific bits replaced here are:
//
//   * windowing / GL context  → SDL_CreateWindow + SDL_GL_CreateContext
//   * input events            → SDL_PollEvent translation
//   * main loop               → explicit while-loop (no gtk_main)
//   * g_idle_add marshalling  → post_to_main() task queue (see below)
//
// GL loading uses libepoxy (already a project dependency), which resolves
// entry points lazily against the current context — no glewInit() needed,
// just `#include <epoxy/gl.h>` and make a context current first.

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// epoxy must come before SDL pulls in any system GL header.
#include <epoxy/gl.h>

// We drive our own main loop, so we don't want SDL's platform main()
// shim (libSDL2main / the Cocoa main wrapper). SDL_MAIN_HANDLED tells
// SDL we're handling main ourselves; SDL_SetMainReady() (called at the
// top of main) completes the contract. This keeps the build identical
// on Linux and macOS and avoids needing -lSDL2main.
#define SDL_MAIN_HANDLED
#include <SDL2/SDL.h>

#include <curl/curl.h>

#include "ai_player.h"
#include "app_state.h"
#include "audio.h"
#include "board_renderer.h"
#include "voice_tts.h"
#include "chess_rules.h"
#include "chess_types.h"
#include "stl_model.h"

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static std::string   g_models_dir = "models";
static StlModel      g_loaded_models[PIECE_COUNT];
static SDL_Window*   g_window  = nullptr;
static SDL_GLContext g_gl_ctx  = nullptr;
static AppState      g_app;
static bool          g_running = true;

// Logical (point) window size — what SDL mouse/finger events are
// expressed in. On a non-HiDPI display this equals the drawable
// (pixel) size; on macOS Retina the drawable is 2× larger. We feed
// the *logical* size to the app_* input hit-testing (so it matches the
// event coordinates) and the *drawable* size to the renderer's
// glViewport (so the framebuffer is crisp). Kept in sync on resize.
static int g_win_w = 1024;
static int g_win_h = 768;

// ---------------------------------------------------------------------------
// Main-thread task queue (replaces GTK's g_idle_add)
// ---------------------------------------------------------------------------
// Worker threads (Stockfish dispatch, puzzle HTTP fetch, whisper voice
// transcription, the Chessnut BLE bridge) must hand their results back
// to the main thread before touching GameState / the renderer, which
// are not thread-safe. GTK does this with g_idle_add; here we use a
// mutex-guarded vector of closures. Workers call post_to_main(closure);
// the render loop drains and runs them once per frame via
// drain_main_tasks(). Draining swaps the vector out under the lock and
// runs the closures with the lock released, so a closure that itself
// posts a follow-up task just defers it to the next frame instead of
// deadlocking.
static std::mutex                         g_task_mutex;
static std::vector<std::function<void()>> g_tasks;

static void post_to_main(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(g_task_mutex);
    g_tasks.push_back(std::move(fn));
}

static void drain_main_tasks() {
    std::vector<std::function<void()>> local;
    {
        std::lock_guard<std::mutex> lock(g_task_mutex);
        local.swap(g_tasks);
    }
    for (auto& fn : local) fn();
}

// ---------------------------------------------------------------------------
// Platform hooks (AppPlatform)
// ---------------------------------------------------------------------------
static void plat_set_status(const char* text) {
    if (g_window) SDL_SetWindowTitle(g_window, text);
}

static void plat_queue_redraw(void) {
    // No-op: the render loop draws every frame unconditionally, the
    // same as web/main_web.cpp. GTK needs an explicit invalidate; we
    // don't.
}

static int64_t plat_now_us(void) {
    // Monotonic microseconds since an arbitrary epoch — steady_clock
    // is the portable equivalent of GTK's g_get_monotonic_time().
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// --- AI dispatch ---
// std::thread + post_to_main: the worker runs the (blocking)
// ask_ai_move() / stockfish_eval() subprocess call, then marshals the
// result back to the main thread where it's safe to mutate GameState.
// Ported from main.cpp's plat_trigger_ai_move / plat_trigger_eval, with
// g_idle_add swapped for post_to_main.
static void plat_trigger_ai_move(const char* fen_c, int movetime,
                                 int game_id) {
    std::string fen = fen_c ? fen_c : "";
    (void)movetime;  // ask_ai_move reads CHESS_AI_MOVETIME_MS itself
    std::thread([fen, game_id]() {
        std::printf("AI thinking... FEN: %s\n", fen.c_str());
        std::string uci = ask_ai_move(fen);
        post_to_main([uci, game_id]() {
            app_ai_move_ready(g_app, uci.c_str(), game_id);
        });
    }).detach();
}

static void plat_trigger_eval(const char* fen_c, int movetime, int idx,
                              int game_id) {
    std::string fen = fen_c ? fen_c : "";
    int mt = movetime;
    std::thread([fen, mt, idx, game_id]() {
        std::string best, second;
        int second_cp = 0;
        int cp = stockfish_eval(fen, mt, best, &second, &second_cp);
        post_to_main([cp, idx, best, game_id, second, second_cp]() {
            app_eval_ready(g_app, cp, idx, best, game_id, second, second_cp);
        });
    }).detach();
}

static void plat_set_ai_elo(int elo) {
    ai_player_set_elo(elo);
}

static void plat_request_quit() {
    g_running = false;
}

// --- Puzzle fetch dispatch ---
// Same idiom as the AI move / eval path: a worker thread runs the
// blocking libcurl fetch, then marshals the JSON body back to the main
// thread via post_to_main. On any failure (network, non-2xx, libcurl
// init) it delivers an empty body and the UI surfaces a "couldn't load
// puzzle" hint via app_puzzle_ready. Ported from main.cpp.
static size_t curl_write_to_string(char* ptr, size_t size, size_t nmemb,
                                   void* userdata) {
    auto* dst = static_cast<std::string*>(userdata);
    size_t n = size * nmemb;
    dst->append(ptr, n);
    return n;
}

static std::string fetch_url(const char* url) {
    std::string body;
    CURL* c = curl_easy_init();
    if (!c) return body;
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write_to_string);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "3d-chess/1.0 (libcurl)");
    CURLcode rc = curl_easy_perform(c);
    if (rc != CURLE_OK) {
        std::fprintf(stderr, "[puzzle] curl_easy_perform: %s\n",
                     curl_easy_strerror(rc));
        body.clear();
    }
    curl_easy_cleanup(c);
    return body;
}

static void plat_trigger_puzzle_fetch(bool daily) {
    std::thread([daily]() {
        const char* url = daily
            ? "https://api.chess.com/pub/puzzle"
            : "https://api.chess.com/pub/puzzle/random";
        std::string body = fetch_url(url);
        post_to_main([body, daily]() {
            app_puzzle_ready(g_app, body.c_str(), daily);
        });
    }).detach();
}

static const AppPlatform g_platform = {
    plat_set_status,
    plat_queue_redraw,
    plat_now_us,
    plat_trigger_ai_move,
    plat_trigger_eval,
    plat_set_ai_elo,
    plat_request_quit,
    plat_trigger_puzzle_fetch,
};

// ---------------------------------------------------------------------------
// Voice + Chessnut driver bridges (ported from main.cpp, g_idle_add →
// post_to_main). These are the platform-specific definitions the shared
// app_state.cpp calls out to.
// ---------------------------------------------------------------------------

// Desktop always supports continuous voice (whisper.cpp is built in).
bool app_voice_continuous_supported() {
    return true;
}

// Always offered on desktop; the bridge surfaces a runtime failure if
// python3 / bleak / Bluetooth aren't available.
bool app_chessnut_supported() {
    return true;
}

void app_chessnut_toggle_request(AppState& a) {
    bool target = !a.chessnut_enabled;
    app_chessnut_set_enabled(a, target,
        [](const std::string& status) {
            post_to_main([status]() {
                app_chessnut_apply_status(g_app, status);
            });
        });
}

void app_voice_toggle_continuous_request(AppState& a) {
    bool target = !a.voice_continuous_enabled;
    app_voice_set_continuous(a, target,
        [](const std::string& utterance, const std::string& error) {
            post_to_main([utterance, error]() {
                app_voice_continuous_apply(g_app, utterance, error);
            });
        },
        [](const std::string& partial) {
            post_to_main([partial]() {
                app_voice_continuous_apply_partial(g_app, partial);
            });
        });
}

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
        case SDLK_d:      return KEY_D;
        case SDLK_l:      return KEY_L;
        case SDLK_c:      return KEY_C;
        default:          return KEY_UNKNOWN;
    }
}

static void pump_events() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_QUIT:
                g_running = false;
                break;

            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT)
                    app_press(g_app, ev.button.x, ev.button.y);
                break;

            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT)
                    app_release(g_app, ev.button.x, ev.button.y,
                                g_win_w, g_win_h);
                break;

            case SDL_MOUSEMOTION:
                app_motion(g_app, ev.motion.x, ev.motion.y,
                           g_win_w, g_win_h);
                break;

            case SDL_MOUSEWHEEL: {
                // Match GTK's convention: scroll up = -1 (zoom in),
                // down = +1. SDL reports wheel.y = +1 up / -1 down, so
                // negate to line up with app_scroll's "positive = zoom
                // out" expectation (same as web/main_web.cpp).
                double y = static_cast<double>(ev.wheel.y);
                if (ev.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) y = -y;
                app_scroll(g_app, -y);
                break;
            }

            case SDL_KEYDOWN:
                if (ev.key.keysym.sym == SDLK_SPACE) {
                    // Push-to-talk: SPACE starts voice capture (and
                    // doubles as the "capture light direction" trigger
                    // inside app_state when light-positioning mode is
                    // on). Ignore auto-repeat so holding SPACE doesn't
                    // restart capture every key-repeat.
                    if (!ev.key.repeat) app_voice_press(g_app);
                } else {
                    app_key(g_app, translate_key(ev.key.keysym.sym));
                }
                break;

            case SDL_KEYUP:
                if (ev.key.keysym.sym == SDLK_SPACE) {
                    // Release → stop capture, transcribe on a worker
                    // thread, marshal the result back to the main
                    // thread. Same pattern as main.cpp's on_key_release.
                    app_voice_release(g_app,
                        [](const std::string& utterance,
                           const std::string& error) {
                            post_to_main([utterance, error]() {
                                app_voice_apply_result(g_app, utterance,
                                                       error);
                            });
                        });
                }
                break;

            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                    ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    // data1/data2 are the new *logical* window size.
                    g_win_w = ev.window.data1;
                    g_win_h = ev.window.data2;
                }
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// One frame
// ---------------------------------------------------------------------------
static void render_frame() {
    drain_main_tasks();
    app_tick(g_app);

    // Render into the default framebuffer at the *drawable* (pixel)
    // resolution so HiDPI / Retina displays stay crisp. On a 1× display
    // this equals the logical size.
    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(g_window, &dw, &dh);

    glViewport(0, 0, dw, dh);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    app_render(g_app, dw, dh);

    SDL_GL_SwapWindow(g_window);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    if (argc > 1) g_models_dir = argv[1];

#ifdef __APPLE__
    // macOS GL tops out at 4.1 core and has no compute shaders, so the
    // GL-compute tile rasterizer for the Gaussian-splat environment
    // (gl_raster, which needs GL 4.3) cannot run. Force the per-quad
    // WebGL2-style splat path OFF *before* renderer_init reads the env
    // var. The rest of the scene (board, pieces, PBR, shadows, MSAA) is
    // identical to the GTK build — only the splat backdrop differs.
    setenv("CHESS_GL_COMPUTE_SPLATS", "0", /*overwrite=*/0);
#endif
    // On Linux/Windows we request a 4.3 context below, so gl_raster is
    // available and the splat backdrop matches the GTK build exactly —
    // the window toolkit (SDL2 vs GTK) has no effect on render quality.

    // libcurl global init has documented thread-safety caveats — it
    // must run before any concurrent CURL usage, from a single thread.
    // Doing it here, before workers spawn, satisfies that. Cleanup is
    // deferred to atexit so it runs after detached puzzle workers
    // finish (we can't join them).
    if (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {
        std::atexit([] { curl_global_cleanup(); });
    } else {
        std::fprintf(stderr,
            "[puzzle] curl_global_init failed; puzzle fetch disabled\n");
    }

    // Startup fan-out, mirroring main.cpp: parse the piece STL models on
    // PIECE_COUNT worker threads while the main thread sets up SDL. The
    // models are pure CPU work (no SDL/GL), so they can run before
    // SDL_Init. Joined before first use (renderer_init).
    std::printf("Loading models...\n");
    std::thread model_threads[PIECE_COUNT];
    for (int i = 0; i < PIECE_COUNT; i++) {
        model_threads[i] = std::thread([i] {
            g_loaded_models[i].load(g_models_dir + "/" + piece_filenames[i]);
            std::printf("  %s: %zu triangles\n",
                        piece_filenames[i],
                        g_loaded_models[i].triangle_count());
        });
    }

    // We handle our own main loop (see SDL_MAIN_HANDLED above), so tell
    // SDL the platform main is ready. Must precede SDL_Init.
    SDL_SetMainReady();

    // Video + events on the main thread (SDL requires it). audio.cpp
    // brings up its own SDL_INIT_AUDIO subsystem on a worker thread
    // below; SDL subsystems are independent and refcounted, so that's
    // safe.
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    // Audio device setup on a worker thread (matches main.cpp). Joined
    // before entering the menu so the first move/sound isn't dropped.
    std::thread audio_thread([] { audio_init(); });

    // Request a Core profile context. The renderer's GLSL is 3.3-core
    // (and 4.3 is backward-compatible), so the version only gates the
    // GL-compute splat rasterizer:
    //   * Linux/Windows → 4.3, exposing compute shaders so gl_raster runs
    //     and the splat backdrop is identical to the GTK build. (GTK gets
    //     the same because its GtkGLArea 3.3 "minimum" yields a 4.6 driver
    //     context — SDL needs us to ask for 4.3 explicitly to match.)
    //   * macOS → 4.1, the Apple ceiling; no compute, per-quad backdrop.
    // A 24-bit depth buffer + double buffering + 4× MSAA match GTK.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                        SDL_GL_CONTEXT_PROFILE_CORE);
#ifdef __APPLE__
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif
    // Forward-compatible flag is required for core profiles on macOS.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,
                        SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

    g_window = SDL_CreateWindow(
        "3D Chess",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        g_win_w, g_win_h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!g_window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    g_gl_ctx = SDL_GL_CreateContext(g_window);
    if (!g_gl_ctx) {
        std::fprintf(stderr, "SDL_GL_CreateContext failed: %s\n",
                     SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(g_window, g_gl_ctx);
    // Vsync: cap the loop to the display refresh so we don't busy-spin.
    // -1 = adaptive vsync where supported, else fall back to 1.
    if (SDL_GL_SetSwapInterval(-1) != 0) SDL_GL_SetSwapInterval(1);

    std::printf("GL vendor:   %s\n", glGetString(GL_VENDOR));
    std::printf("GL renderer: %s\n", glGetString(GL_RENDERER));
    std::printf("GL version:  %s\n", glGetString(GL_VERSION));

    // Track the actual logical size SDL gave us (a tiling WM may have
    // resized the window on map).
    SDL_GetWindowSize(g_window, &g_win_w, &g_win_h);

    for (auto& t : model_threads) t.join();
    std::printf("All models loaded.\n");

    // app_init records persisted settings (including the saved splat
    // environment) but never touches GL — safe to call before/after
    // renderer_init. We follow main.cpp's order: init shared state,
    // hand it the loaded models, then upload them via renderer_init
    // under the live GL context.
    app_init(g_app, &g_platform);
    g_app.loaded_models = g_loaded_models;

    renderer_init(g_loaded_models);

    // Apply the saved splat environment now that a GL context is current
    // (app_settings_load only recorded the choice — it can't touch GL).
    // Mirrors main.cpp's on_realize.
    if (g_app.environment != AppState::Environment::MedievalRoom) {
        renderer_set_environment(static_cast<int>(g_app.environment));
    }

    audio_thread.join();

    app_enter_menu(g_app);

    // Main loop: poll input, run any marshalled worker results +
    // per-frame tick, render, present. Vsync paces it.
    while (g_running) {
        pump_events();
        render_frame();
    }

    // Shutdown — release voice/TTS/Chessnut worker resources, the GL
    // context, and SDL. atexit handles curl_global_cleanup.
    app_voice_shutdown(g_app);
    voice_tts_shutdown();
    app_chessnut_shutdown(g_app);

    SDL_GL_DeleteContext(g_gl_ctx);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
