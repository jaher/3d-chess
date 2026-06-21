// ===========================================================================
// android/app/src/main/cpp/main_android.cpp — Android driver (SDL2 + GLES 3.0)
// ===========================================================================
//
// The Android sibling of ios/main_ios.cpp (iOS), main_sdl.cpp (desktop) and
// web/main_web.cpp (browser). Like all of them, every bit of gameplay lives in
// the platform-agnostic app_state.cpp — this file only wires SDL events +
// windowing + the worker-thread plumbing to the shared app_* functions. It is
// kept SEPARATE from the other drivers (not #if-branched into them) so each
// platform's main loop stays byte-for-byte untouched, exactly as the iOS
// author kept main_ios.cpp separate from main_sdl.cpp.
//
// Architecture (closest sibling = iOS; both mirror the WEB build):
//   * GL            → OpenGL ES 3.0 via SDL's Android GL context (the NDK ships
//                     <GLES3/gl3.h>, the SAME header the web/emscripten branch
//                     uses). No compute shaders → per-quad Gaussian-splat path,
//                     like web/iOS/macOS. The shared headers pick <GLES3/gl3.h>
//                     under the new CHESS_GLES superset (defined for
//                     __ANDROID__ — see shader.h / render_internal.h / …).
//   * Text          → stb_truetype (web/font_atlas_stb.cpp); Android joins the
//                     CHESS_TEXT_STB path in text_atlas.cpp.
//   * Touch         → the SDL_FINGER* handling lifted verbatim from
//                     ios/main_ios.cpp (originally web/main_web.cpp): one finger
//                     = mouse (press/motion/release), two fingers = pinch zoom.
//   * AI            → android/ai_player_android.cpp PLACEHOLDER (no fork/exec →
//                     no subprocess Stockfish; an APK cannot spawn one). The
//                     real fix is an in-process Stockfish; see docs/ANDROID.md.
//   * Voice/Chessnut→ reported unsupported (android/platform_android.cpp), like
//                     the web feature-detect.
//   * Puzzle fetch  → stubbed by default (see plat_trigger_puzzle_fetch).
//   * Assets        → the shared engine loads "models/…", "fonts/…", "sounds/…",
//                     "puzzles/…", "openings/…", "challenges/…" via std::ifstream
//                     / fopen, which do NOT read from the APK asset manager. So
//                     on first run we EXTRACT the bundled assets out of the APK
//                     (via SDL_RWFromFile, which DOES route to the asset manager)
//                     into SDL_AndroidGetInternalStoragePath(), then chdir()
//                     there — after which every relative path resolves with no
//                     change to the shared asset-loading code. See docs/ANDROID.md.
//
// SDL provides the real entry point on Android: the Java SDLActivity loads
// "libmain.so" and calls SDL_main from the native thread. <SDL.h> #defines
// `main` → `SDL_main`, so the plain `int main()` below IS that entry point. We
// do NOT define SDL_MAIN_HANDLED (unlike main_sdl.cpp).
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

#include <sys/stat.h>   // mkdir
#include <unistd.h>     // chdir, access

#include <SDL.h>
#include <GLES3/gl3.h>

#include <android/log.h>

// Engine headers are found via the repo-root include dir added by
// app/src/main/cpp/CMakeLists.txt (target_include_directories → REPO_ROOT),
// the same way ios/CMakeLists.txt wires the iOS target.
#include "ai_player.h"
#include "app_state.h"
#include "audio.h"
#include "board_renderer.h"
#include "chess_types.h"
#include "stl_model.h"
#include "voice_tts.h"

#define ALOG_TAG "3dchess"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO,  ALOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, ALOG_TAG, __VA_ARGS__)

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static std::string   g_models_dir = "models";   // resolved against the CWD
static StlModel      g_loaded_models[PIECE_COUNT];
static SDL_Window*   g_window  = nullptr;
static SDL_GLContext g_gl_ctx  = nullptr;
static AppState      g_app;
static bool          g_running = true;

// Logical window size — what SDL mouse/finger events are expressed in. On
// Android the drawable matches the surface; we still feed the logical size to
// app_* hit-testing and the drawable size to glViewport.
static int g_win_w = 1280;
static int g_win_h = 720;

// ---------------------------------------------------------------------------
// Main-thread task queue (same idiom as main_ios.cpp / main_sdl.cpp). Worker
// threads hand results back to the render loop, the only place it's safe to
// mutate GameState / the renderer.
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

// ===========================================================================
// First-run asset extraction (APK assets → internal storage).
//
// The APK packs the runtime assets under its assets/ root (see the Gradle
// `stageAssets` task in app/build.gradle). Those are NOT real files — they live
// compressed inside the APK and can only be read through the Android asset
// manager. The shared engine, however, loads with std::ifstream / fopen, which
// bypass the asset manager. So we copy every asset listed in the bundled
// `asset_manifest.txt` (generated at build time) out to a writable directory
// (SDL_AndroidGetInternalStoragePath()) on first launch, then chdir() there.
// SDL_RWFromFile() with a RELATIVE path DOES go through the asset manager on
// Android, so it is our bridge out of the APK.
// ===========================================================================
namespace {

// mkdir -p for the parent directories of `full_path` (an absolute path).
void make_parent_dirs(const std::string& full_path) {
    std::string::size_type pos = 0;
    // Skip the leading '/'.
    while ((pos = full_path.find('/', pos + 1)) != std::string::npos) {
        std::string dir = full_path.substr(0, pos);
        if (!dir.empty()) mkdir(dir.c_str(), 0755);
    }
}

// Copy one asset (relative path, e.g. "models/King.stl") from the APK to
// dest_root + "/" + rel. Returns false on failure.
bool extract_one(const std::string& rel, const std::string& dest_root) {
    SDL_RWops* in = SDL_RWFromFile(rel.c_str(), "rb");  // APK asset manager
    if (!in) {
        ALOGE("asset missing in APK: %s (%s)", rel.c_str(), SDL_GetError());
        return false;
    }
    Sint64 size = SDL_RWsize(in);
    std::vector<unsigned char> buf;
    if (size > 0) {
        buf.resize(static_cast<size_t>(size));
        size_t got = SDL_RWread(in, buf.data(), 1, buf.size());
        buf.resize(got);
    } else {
        // Unknown size — read in chunks.
        unsigned char chunk[1 << 16];
        size_t n;
        while ((n = SDL_RWread(in, chunk, 1, sizeof chunk)) > 0)
            buf.insert(buf.end(), chunk, chunk + n);
    }
    SDL_RWclose(in);

    std::string out_path = dest_root + "/" + rel;
    make_parent_dirs(out_path);
    FILE* out = std::fopen(out_path.c_str(), "wb");
    if (!out) {
        ALOGE("cannot write extracted asset: %s", out_path.c_str());
        return false;
    }
    if (!buf.empty()) std::fwrite(buf.data(), 1, buf.size(), out);
    std::fclose(out);
    return true;
}

// Extract every path listed in assets/asset_manifest.txt. A sentinel file keeps
// re-launches fast; bump ASSET_VERSION (and the Gradle copy) to force a refresh.
constexpr const char* ASSET_SENTINEL = ".assets_extracted_v1";

bool extract_assets(const std::string& dest_root) {
    std::string sentinel = dest_root + "/" + ASSET_SENTINEL;
    if (access(sentinel.c_str(), F_OK) == 0) {
        ALOGI("assets already extracted (%s)", sentinel.c_str());
        return true;
    }

    SDL_RWops* mf = SDL_RWFromFile("asset_manifest.txt", "rb");
    if (!mf) {
        ALOGE("asset_manifest.txt not found in APK — did Gradle run stageAssets? (%s)",
              SDL_GetError());
        return false;
    }
    Sint64 msz = SDL_RWsize(mf);
    std::string manifest;
    if (msz > 0) {
        manifest.resize(static_cast<size_t>(msz));
        SDL_RWread(mf, manifest.data(), 1, manifest.size());
    }
    SDL_RWclose(mf);

    int ok = 0, fail = 0;
    std::string::size_type start = 0;
    while (start < manifest.size()) {
        std::string::size_type nl = manifest.find('\n', start);
        std::string line = manifest.substr(
            start, nl == std::string::npos ? std::string::npos : nl - start);
        start = (nl == std::string::npos) ? manifest.size() : nl + 1;
        // Trim CR / whitespace.
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        if (line.empty()) continue;
        if (extract_one(line, dest_root)) ok++; else fail++;
    }
    ALOGI("asset extraction: %d ok, %d failed", ok, fail);

    // Drop the sentinel even on partial failure so we don't loop forever; the
    // missing files will surface as the shared layer's graceful fallbacks.
    if (FILE* s = std::fopen(sentinel.c_str(), "wb")) std::fclose(s);
    return fail == 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Platform hooks (AppPlatform) — identical shape to main_ios.cpp.
// ---------------------------------------------------------------------------
static void plat_set_status(const char* text) {
    if (text) ALOGI("[status] %s", text);  // shows in logcat
}

static void plat_queue_redraw(void) {
    // No-op: the render loop draws every frame unconditionally (like web/iOS).
}

static int64_t plat_now_us(void) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Worker std::thread runs the (placeholder) ask_ai_move(), then marshals the
// result back to the main thread. Swap android/ai_player_android.cpp for an
// in-process Stockfish and this path needs no changes.
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
    // Android apps don't self-terminate (the OS manages the lifecycle). No-op,
    // and the mobile menu hides its Quit button via CHESS_PLATFORM_MOBILE.
}

// chess.com puzzle fetch. libcurl can be cross-compiled for Android, but it's
// an optional extra dependency, so the scaffold stubs it: we deliver an empty
// body so app_puzzle_ready surfaces the shared "couldn't load puzzle" hint
// rather than wedging. Wire a native HttpURLConnection (JNI) or cURL behind
// CHESS_ANDROID_ENABLE_CURL to restore it — see docs/ANDROID.md.
static void plat_trigger_puzzle_fetch(bool daily) {
    post_to_main([daily]() {
        app_puzzle_ready(g_app, "", daily);
    });
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
        case SDLK_ESCAPE: return KEY_ESCAPE;   // Android back button maps here
        case SDLK_AC_BACK:return KEY_ESCAPE;
        case SDLK_a:      return KEY_A;
        case SDLK_m:      return KEY_M;
        case SDLK_s:      return KEY_S;
        case SDLK_d:      return KEY_D;
        case SDLK_l:      return KEY_L;
        default:          return KEY_UNKNOWN;
    }
}

// ---------------------------------------------------------------------------
// Touch state for pinch-zoom — lifted verbatim from ios/main_ios.cpp /
// web/main_web.cpp. SDL_HINT_TOUCH_MOUSE_EVENTS is disabled in main() so
// touches never synthesise mouse events; we handle SDL_FINGER* directly. One
// finger acts as a mouse; two fingers enter pinch mode and feed the change in
// finger distance into app_scroll(delta).
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

// Same tuning as web/iOS; tune on-device (docs/ANDROID.md lists touch tuning
// as a follow-up). app_scroll halves the delta internally and clamps zoom.
static constexpr double PINCH_SENSITIVITY = 0.035;

static void pump_events() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
            case SDL_QUIT:
                g_running = false;
                break;

            // Mouse events are kept for emulator (mouse) input.
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

            // The OS is about to background us. SDL pauses the GL context on
            // Android; we keep the loop quiescent. (A production build should
            // route SDL_APP_WILLENTERBACKGROUND / *DIDENTERFOREGROUND through a
            // proper lifecycle handler — see docs/ANDROID.md.)
            default:
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// One frame — identical to main_ios.cpp.
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
// Entry point. SDL remaps this to SDL_main; SDLActivity calls it.
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    (void)argc; (void)argv;

    // Handle touch ourselves (no synthetic mouse events) so two-finger
    // pinch-zoom works — set BEFORE SDL_Init, exactly as web/iOS do.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        ALOGE("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // Asset bootstrap: extract the APK's bundled assets to internal storage and
    // chdir() there so the shared layer's RELATIVE std::ifstream/fopen loads
    // resolve. Must run BEFORE the STL model-load threads (which read files).
    if (const char* internal = SDL_AndroidGetInternalStoragePath()) {
        std::string dest = internal;
        extract_assets(dest);
        if (chdir(dest.c_str()) != 0)
            ALOGE("chdir(%s) failed", dest.c_str());
        else
            ALOGI("cwd → %s", dest.c_str());
    } else {
        ALOGE("SDL_AndroidGetInternalStoragePath() returned null");
    }

    // Android GLES has no compute shaders → force the per-quad Gaussian-splat
    // path OFF before renderer_init reads the env var (same as iOS/macOS).
    // gl_raster isn't compiled into the Android target at all.
    setenv("CHESS_GL_COMPUTE_SPLATS", "0", /*overwrite=*/0);

    // Parse the piece STL models on PIECE_COUNT worker threads (pure CPU work).
    // Joined before renderer_init. Runs after asset extraction so the files
    // exist under the CWD.
    std::thread model_threads[PIECE_COUNT];
    for (int i = 0; i < PIECE_COUNT; i++) {
        model_threads[i] = std::thread([i] {
            g_loaded_models[i].load(g_models_dir + "/" + piece_filenames[i]);
        });
    }

    std::thread audio_thread([] { audio_init(); });

    // OpenGL ES 3.0 context. Depth 24, double buffer, 4× MSAA to match the
    // other platforms' anti-aliasing request.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

    // Fullscreen at the device's native resolution. SDL on Android creates a
    // surface that fills the screen; SDL_WINDOW_FULLSCREEN makes that explicit.
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
        ALOGE("SDL_CreateWindow failed: %s", SDL_GetError());
        return 1;
    }

    g_gl_ctx = SDL_GL_CreateContext(g_window);
    if (!g_gl_ctx) {
        ALOGE("SDL_GL_CreateContext failed: %s", SDL_GetError());
        return 1;
    }
    SDL_GL_MakeCurrent(g_window, g_gl_ctx);
    SDL_GL_SetSwapInterval(1);  // vsync to the display refresh

    ALOGI("GL vendor:   %s", glGetString(GL_VENDOR));
    ALOGI("GL renderer: %s", glGetString(GL_RENDERER));
    ALOGI("GL version:  %s", glGetString(GL_VERSION));

    // Actual surface size SDL gave us; finger coords scale by this.
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

    // Main loop. A plain while-loop works under SDL's Android event pump; for
    // proper background/foreground handling a production build should handle
    // the SDL_APP_* lifecycle events (see docs/ANDROID.md).
    while (g_running) {
        pump_events();
        render_frame();
    }

    // Shutdown. voice_tts_shutdown / app_chessnut_shutdown are the Android
    // stubs (android/platform_android.cpp); there's no whisper engine to free.
    voice_tts_shutdown();
    app_chessnut_shutdown(g_app);

    SDL_GL_DeleteContext(g_gl_ctx);
    SDL_DestroyWindow(g_window);
    SDL_Quit();
    return 0;
}
