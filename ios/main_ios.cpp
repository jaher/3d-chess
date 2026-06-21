// ===========================================================================
// ios/main_ios.cpp — iOS / iPadOS driver (SDL2 + OpenGL ES 3.0).
// ===========================================================================
//
// The iOS sibling of main_sdl.cpp (desktop) and web/main_web.cpp (browser).
// Like all three, every bit of gameplay lives in the platform-agnostic
// app_state.cpp — this file only wires SDL events + windowing + the
// worker-thread plumbing to the shared app_* functions. It is kept SEPARATE
// from main_sdl.cpp (rather than #if-branching it) so the desktop SDL build
// stays byte-for-byte untouched.
//
// Architecture (closer to the WEB build than the desktop SDL build):
//   * GL            → OpenGL ES 3.0 via SDL's iOS GL context (no desktop GL,
//                     no compute shaders → per-quad Gaussian-splat path, like
//                     web). board_renderer.cpp selects <OpenGLES/ES3/gl.h>
//                     and the stb_truetype text path under
//                     `defined(__APPLE__) && TARGET_OS_IPHONE`.
//   * Touch         → the SDL_FINGER* handling lifted verbatim from
//                     web/main_web.cpp: one finger = mouse (press/motion/
//                     release), two fingers = pinch → app_scroll (zoom).
//   * AI            → ios/ai_player_ios.cpp PLACEHOLDER (no fork/exec → no
//                     subprocess Stockfish). The real fix is an in-process
//                     Stockfish; see docs/IOS.md.
//   * Voice/Chessnut→ reported unsupported (ios/platform_ios.cpp), like the
//                     web feature-detect.
//   * Puzzle fetch  → stubbed by default (libcurl works on iOS but is kept
//                     optional behind CHESS_IOS_ENABLE_CURL — see below).
//
// SDL provides the real entry point on iOS: <SDL.h> #defines `main` to
// `SDL_main`, and SDL's UIKit shim (SDL_uikit_main) calls it from
// UIApplicationMain. So we do NOT define SDL_MAIN_HANDLED here (unlike
// main_sdl.cpp), and we must link SDL2main.
// ===========================================================================

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>  // chdir

#include <SDL.h>
#include <OpenGLES/ES3/gl.h>

#include "../ai_player.h"
#include "../app_state.h"
#include "../audio.h"
#include "../board_renderer.h"
#include "../chess_types.h"
#include "../stl_model.h"
#include "../voice_tts.h"

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static std::string   g_models_dir = "models";   // resolved against the bundle
static StlModel      g_loaded_models[PIECE_COUNT];
static SDL_Window*   g_window  = nullptr;
static SDL_GLContext g_gl_ctx  = nullptr;
static AppState      g_app;
static bool          g_running = true;

// Logical (point) window size — what SDL mouse/finger events are expressed
// in. On a Retina display the GL drawable is 2×/3× larger; we feed the
// *logical* size to app_* hit-testing and the *drawable* size to glViewport.
static int g_win_w = 768;
static int g_win_h = 1024;

// ---------------------------------------------------------------------------
// Main-thread task queue (same idiom as main_sdl.cpp's, replacing GTK's
// g_idle_add). Worker threads hand results back to the render loop, which
// is the only place it's safe to mutate GameState / the renderer.
// ---------------------------------------------------------------------------
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
    // No title bar on iOS; the on-screen status string is drawn by the
    // renderer. Log so it shows in the Xcode console.
    if (text) std::printf("[status] %s\n", text);
}

static void plat_queue_redraw(void) {
    // No-op: the render loop draws every frame unconditionally (like web).
}

static int64_t plat_now_us(void) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// --- AI dispatch ---
// Worker std::thread runs the (placeholder) ask_ai_move(), then marshals the
// result back to the main thread. Same shape as main_sdl.cpp, minus the
// subprocess. Swap ai_player_ios.cpp for an in-process Stockfish and this
// path needs no changes.
static void plat_trigger_ai_move(const char* fen_c, int movetime, int game_id) {
    std::string fen = fen_c ? fen_c : "";
    (void)movetime;
    std::thread([fen, game_id]() {
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
    // iOS apps don't self-terminate (the OS manages the lifecycle). No-op,
    // and the iOS menu hides its Quit button via CHESS_PLATFORM_IOS.
}

// --- Puzzle fetch ---
// chess.com puzzle fetch. libcurl DOES work on iOS, but it's an optional
// extra dependency, so the scaffold stubs it by default: we deliver an empty
// body so app_puzzle_ready surfaces the shared "couldn't load puzzle" hint
// rather than wedging. Define CHESS_IOS_ENABLE_CURL (and link libcurl in
// ios/CMakeLists.txt) to use the same NSURLSession-backed libcurl path the
// desktop build uses; or replace this with a native NSURLSession fetch.
static void plat_trigger_puzzle_fetch(bool daily) {
#ifdef CHESS_IOS_ENABLE_CURL
    // See main_sdl.cpp::fetch_url for the libcurl implementation to port.
    #error "CHESS_IOS_ENABLE_CURL set but the libcurl fetch is not wired yet — port main_sdl.cpp::fetch_url here."
#else
    post_to_main([daily]() {
        app_puzzle_ready(g_app, "", daily);
    });
#endif
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
        default:          return KEY_UNKNOWN;
    }
}

// ---------------------------------------------------------------------------
// Touch state for pinch-zoom — lifted verbatim from web/main_web.cpp.
//
// SDL_HINT_TOUCH_MOUSE_EVENTS is disabled in main() so touches never
// synthesise mouse events; we handle SDL_FINGER* directly. One finger acts
// as a mouse (press/motion/release); two fingers enter pinch mode and feed
// the change in finger distance into app_scroll(delta).
// ---------------------------------------------------------------------------
static constexpr SDL_FingerID NO_FID = -1;
static SDL_FingerID g_fid_a = NO_FID;
static SDL_FingerID g_fid_b = NO_FID;
static float g_fa_x = 0.0f, g_fa_y = 0.0f;  // normalised 0..1
static float g_fb_x = 0.0f, g_fb_y = 0.0f;
static bool  g_pinch_active = false;
static float g_pinch_last_dist = 0.0f;

static inline int fpx(float norm_x) {
    return static_cast<int>(norm_x * static_cast<float>(g_win_w));
}
static inline int fpy(float norm_y) {
    return static_cast<int>(norm_y * static_cast<float>(g_win_h));
}

static inline float pinch_distance_px() {
    float dx = (g_fa_x - g_fb_x) * static_cast<float>(g_win_w);
    float dy = (g_fa_y - g_fb_y) * static_cast<float>(g_win_h);
    return std::sqrt(dx * dx + dy * dy);
}

// Same tuning as web; adjust on-device (docs/IOS.md lists touch tuning as a
// follow-up). app_scroll halves the delta internally and clamps zoom [3, 40].
static constexpr double PINCH_SENSITIVITY = 0.035;

static void pump_events() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_QUIT:
                g_running = false;
                break;

            // Mouse events are kept for the Simulator (trackpad/mouse) and
            // for desktop-class iPad pointer input.
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
                app_motion(g_app, ev.motion.x, ev.motion.y, g_win_w, g_win_h);
                break;
            case SDL_MOUSEWHEEL:
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
                    // Cancel the one-finger press so dropping the second
                    // finger doesn't also register as a board click.
                    app_release(g_app, fpx(g_fa_x), fpy(g_fa_y),
                                g_win_w, g_win_h);
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
                    if (std::fabs(delta) > 0.5f) {
                        app_scroll(g_app,
                                   -static_cast<double>(delta) *
                                   PINCH_SENSITIVITY);
                    }
                } else if (fid == g_fid_a) {
                    app_motion(g_app, fpx(g_fa_x), fpy(g_fa_y),
                               g_win_w, g_win_h);
                }
                break;
            }
            case SDL_FINGERUP: {
                SDL_FingerID fid = ev.tfinger.fingerId;
                if (fid == g_fid_b) {
                    g_fid_b = NO_FID;
                    g_pinch_active = false;
                } else if (fid == g_fid_a) {
                    if (g_pinch_active) {
                        g_fa_x = g_fb_x; g_fa_y = g_fb_y;
                        g_fid_a = g_fid_b;
                        g_fid_b = NO_FID;
                        g_pinch_active = false;
                    } else {
                        app_release(g_app,
                                    fpx(ev.tfinger.x), fpy(ev.tfinger.y),
                                    g_win_w, g_win_h);
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
                    g_win_w = ev.window.data1;
                    g_win_h = ev.window.data2;
                }
                break;

            // The OS is about to background us. SDL stops the GL context;
            // we just keep the loop quiescent. (For a production app, route
            // SDL_APP_DIDENTERBACKGROUND / *FOREGROUND through an
            // SDL_iPhoneSetAnimationCallback-driven loop — see docs/IOS.md.)
            default:
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

    int dw = 0, dh = 0;
    SDL_GL_GetDrawableSize(g_window, &dw, &dh);

    glViewport(0, 0, dw, dh);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    app_render(g_app, dw, dh);

    SDL_GL_SwapWindow(g_window);
}

// ---------------------------------------------------------------------------
// Entry point. SDL remaps this to SDL_main; the UIKit shim calls it.
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    // Asset resolution: the shared layer loads "models/…", "fonts/…",
    // "sounds/…", "puzzles/…", "openings/…", "challenges/…" via RELATIVE
    // paths. On iOS the process CWD is not the bundle, so chdir() into the
    // bundle resource directory (SDL_GetBasePath() → the .app's resource
    // path) up front and every relative path resolves against the bundled
    // RESOURCES (see ios/CMakeLists.txt). No shared asset-loading code needs
    // to change.
    if (char* base = SDL_GetBasePath()) {
        if (chdir(base) != 0)
            std::fprintf(stderr, "[ios] chdir(%s) failed\n", base);
        SDL_free(base);
    }

    // iOS GLES has no compute shaders → force the per-quad Gaussian-splat
    // path OFF before renderer_init reads the env var (same as main_sdl.cpp
    // does for macOS). gl_raster isn't even compiled into the iOS target.
    setenv("CHESS_GL_COMPUTE_SPLATS", "0", /*overwrite=*/0);

    // Parse the piece STL models on PIECE_COUNT worker threads while the main
    // thread brings up SDL (pure CPU work, no SDL/GL). Joined before
    // renderer_init.
    std::thread model_threads[PIECE_COUNT];
    for (int i = 0; i < PIECE_COUNT; i++) {
        model_threads[i] = std::thread([i] {
            g_loaded_models[i].load(g_models_dir + "/" + piece_filenames[i]);
        });
    }

    // Handle touch ourselves (no synthetic mouse events) so two-finger
    // pinch-zoom works — must be set BEFORE SDL_Init, exactly as web does.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    std::thread audio_thread([] { audio_init(); });

    // OpenGL ES 3.0 context (the web build's profile). Depth 24, double
    // buffer, 4× MSAA to match the other platforms' anti-aliasing request.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

    // Fullscreen at the device's native resolution.
    SDL_DisplayMode dm;
    if (SDL_GetCurrentDisplayMode(0, &dm) == 0) {
        g_win_w = dm.w;
        g_win_h = dm.h;
    }

    g_window = SDL_CreateWindow(
        "3D Chess",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        g_win_w, g_win_h,
        SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_ALLOW_HIGHDPI);
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
    SDL_GL_SetSwapInterval(1);  // vsync to the display refresh

    std::printf("GL vendor:   %s\n", glGetString(GL_VENDOR));
    std::printf("GL renderer: %s\n", glGetString(GL_RENDERER));
    std::printf("GL version:  %s\n", glGetString(GL_VERSION));

    // Logical (point) size SDL actually gave us; finger coords scale by this.
    SDL_GetWindowSize(g_window, &g_win_w, &g_win_h);

    for (auto& t : model_threads) t.join();

    app_init(g_app, &g_platform);
    g_app.loaded_models = g_loaded_models;
    renderer_init(g_loaded_models);

    if (g_app.environment != AppState::Environment::MedievalRoom) {
        renderer_set_environment(static_cast<int>(g_app.environment));
    }

    audio_thread.join();
    app_enter_menu(g_app);

    // Main loop. A plain while-loop works under SDL's iOS event pump; for
    // proper background/foreground handling a production build should switch
    // to SDL_iPhoneSetAnimationCallback (see docs/IOS.md).
    while (g_running) {
        pump_events();
        render_frame();
    }

    // Shutdown. voice_tts_shutdown / app_chessnut_shutdown are the iOS stubs
    // (ios/platform_ios.cpp); there's no whisper voice engine to release.
    voice_tts_shutdown();
    app_chessnut_shutdown(g_app);

    SDL_GL_DeleteContext(g_gl_ctx);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
