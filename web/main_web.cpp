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
#include "../asset_loader.h"
#include "../audio.h"
#include "../board_renderer.h"
#include "../chess_types.h"
#include "../stl_model.h"
#include "../mat.h"

// Implemented in web/asset_loader_web.cpp.
void assets_web_pump_downloads(int environment);
bool assets_web_take_installed(AssetGroup* out);

// ---------------------------------------------------------------------------
// Bridge into web/ai_player_web.cpp + web/voice_web.cpp
// ---------------------------------------------------------------------------
extern void web_request_ai_move(const std::string& fen, int movetime_ms,
                                int game_id);
extern void web_request_eval(const std::string& fen, int movetime_ms,
                             int score_index, int game_id);
extern void web_set_ai_elo(int elo);

extern "C" void voice_web_bind_app(AppState* a);

namespace web_ai {
    extern bool        move_ready;
    extern std::string move_uci;
    extern int         move_game_id;
    extern bool        eval_ready;
    extern int         eval_cp;
    extern int         eval_index;
    extern std::string eval_best_uci;
    extern std::string eval_second_uci;
    extern int         eval_second_cp;
    extern std::string eval_pv;
    extern int         eval_game_id;
}

// ---------------------------------------------------------------------------
// JS-side hooks (set the HTML status div, emit console checkpoint logs)
// ---------------------------------------------------------------------------
EM_JS(void, set_status_js, (const char* s), {
    // Route through the shared helper defined in index.html so the
    // "Click to enable sound" hint is appended consistently while
    // the Web Audio context is still suspended. Falls back to direct
    // textContent write if the helper hasn't been defined (e.g.
    // bare-minimum test harness).
    var txt = UTF8ToString(s);
    if (typeof window.__applyChessStatus === 'function') {
        window.__applyChessStatus(txt);
    } else {
        var el = document.getElementById('chess-status');
        if (el) el.textContent = txt;
    }
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

static void plat_trigger_ai_move(const char* fen, int movetime_ms,
                                 int game_id) {
    web_request_ai_move(fen ? std::string(fen) : std::string(),
                        movetime_ms, game_id);
}

static void plat_trigger_eval(const char* fen, int movetime_ms, int idx,
                              int game_id) {
    web_request_eval(fen ? std::string(fen) : std::string(),
                     movetime_ms, idx, game_id);
}

static void plat_set_ai_elo(int elo) {
    web_set_ai_elo(elo);
}

static void plat_request_quit() {
    // No-op on web — closing a browser tab from inside a WASM app
    // isn't a natural action and the menu doesn't expose a Quit
    // button on this platform. The shared layer's hook stays
    // populated so AppPlatform's struct layout matches across
    // platforms.
}

// chess.com Daily / Random puzzle. Async fetch from the browser via
// fetch(); the response body is delivered back to C++ through
// on_puzzle_from_js below (Module.ccall). On any failure (network
// error, non-2xx status, CORS) we bomb back an empty body so the
// shared layer's app_puzzle_ready can show a "couldn't load" hint
// rather than wedge the screen.
EM_JS(void, js_request_puzzle, (int daily), {
    var url = daily
        ? 'https://api.chess.com/pub/puzzle'
        : 'https://api.chess.com/pub/puzzle/random';
    // Note: this body is double-tokenised — first by the C++
    // preprocessor (which interprets the EM_JS macro), then by the
    // browser's JS engine. JS would accept '' as an empty string
    // but the C++ preprocessor warns about it as an empty char
    // constant ([-Winvalid-pp-token]); use double-quoted "" since
    // both languages treat it as an empty string.
    fetch(url, { cache: "no-store" })
        .then(function (r) { return r.ok ? r.text() : ""; })
        .then(function (body) {
            try {
                Module.ccall("on_puzzle_from_js", null,
                             ["string", "number"],
                             [body || "", daily ? 1 : 0]);
            } catch (e) {
                console.error("[puzzle] ccall failed:", e);
            }
        })
        .catch(function (e) {
            console.warn("[puzzle] fetch failed:", e);
            try {
                Module.ccall("on_puzzle_from_js", null,
                             ["string", "number"], ["", daily ? 1 : 0]);
            } catch (_) {}
        });
});

static void plat_trigger_puzzle_fetch(bool daily) {
    js_request_puzzle(daily ? 1 : 0);
}

// --- Online multiplayer (WebRTC, driven by peer-bridge.js) ---
// Relay the local player's move to the opponent over the data channel.
EM_JS(void, js_send_move, (const char* uci), {
    if (typeof OnlineChess !== 'undefined' && OnlineChess.sendMove)
        OnlineChess.sendMove(UTF8ToString(uci));
});
static void plat_send_move(const char* uci) { js_send_move(uci); }

extern "C" {
EMSCRIPTEN_KEEPALIVE
void on_puzzle_from_js(const char* body, int daily) {
    app_puzzle_ready(g_app, body ? body : "", daily != 0);
}

// peer-bridge.js calls this once the WebRTC connection is up:
// local_is_white = 1 if this client is the host (White), 0 for the
// joiner (Black).
EMSCRIPTEN_KEEPALIVE
void chess_start_network_game(int local_is_white) {
    app_start_network_game(g_app, local_is_white != 0);
}

// peer-bridge.js calls this with the opponent's move (UCI) as it arrives.
EMSCRIPTEN_KEEPALIVE
void on_remote_move_from_js(const char* uci) {
    app_remote_move_ready(g_app, uci ? uci : "");
}
}  // extern "C"

static const AppPlatform g_platform = {
    plat_set_status,
    plat_queue_redraw,
    plat_now_us,
    plat_trigger_ai_move,
    plat_trigger_eval,
    plat_set_ai_elo,
    plat_request_quit,
    plat_trigger_puzzle_fetch,
    plat_send_move,   // trigger_send_move (online multiplayer)
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
        case SDLK_c:      return KEY_C;
        case SDLK_r:      return KEY_R;
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

// Set on the first click/keypress. Browsers suspend the Web Audio
// context until a real user gesture, so decoding the 5 MB intro WAV
// before one has happened is work that cannot possibly be heard — we
// hold it back rather than spend a frame on it during the menu.
static bool g_user_gesture = false;
static bool g_music_pending = false;

static void pump_events() {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_KEYDOWN ||
            ev.type == SDL_FINGERDOWN)
            g_user_gesture = true;
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
        app_ai_move_ready(g_app, web_ai::move_uci.c_str(),
                          web_ai::move_game_id);
    }
    if (web_ai::eval_ready) {
        web_ai::eval_ready = false;
        app_eval_ready(g_app, web_ai::eval_cp, web_ai::eval_index,
                       web_ai::eval_best_uci, web_ai::eval_game_id,
                       web_ai::eval_second_uci, web_ai::eval_second_cp,
                       web_ai::eval_pv);
    }
}

// ---------------------------------------------------------------------------
// Debug hooks (for headless inspection via CDP): jump straight into a
// cable-room game, and dump the current framebuffer to /dump.ppm.
// ---------------------------------------------------------------------------
// Jelly regression harness: uses the same shared app input/render paths.
extern "C" EMSCRIPTEN_KEEPALIVE void chess_dbg_jelly(int scene, int enabled) {
    g_app.jelly_pieces=enabled!=0;
    g_app.splats_enabled=false;
    g_app.environment=AppState::Environment::MedievalRoom;
    renderer_set_environment(0);
    if (scene==0) app_enter_menu(g_app);
    else if (scene==2) app_enter_options(g_app);
    else {
        g_app.two_player_mode=true; g_app.clock_enabled=false;
        app_enter_game(g_app);
        g_app.rot_x=45; g_app.rot_y=180; g_app.zoom=12;
    }
}
extern "C" EMSCRIPTEN_KEEPALIVE float chess_dbg_jelly_probe(int what) {
    if(what==10)return glGetError();
    if(what==13)return static_cast<int>(g_app.environment);
    if (what==0) return static_cast<int>(g_app.mode);
    if (what==7) return g_app.jelly_pieces;
    if (what==6) return g_app.rot_y;
    if (what==9) { float v=0; for(const auto& p:g_app.menu_pieces) for(float x:p.jelly.offset) v=std::max(v,std::abs(x)); return v; }
    if (g_app.games.empty()) return -1;
    const auto& gs=g_app.games[g_app.active_game].game;
    if(what==11){float j=1;for(const auto& p:gs.pieces)j=std::min(j,p.jelly.minimum_jacobian());return j;}
    if(what==12){float e=0;for(const auto& p:gs.pieces)e=std::max(e,std::abs(p.jelly.volume_ratio()-1));return e;}
    if (what==1) return gs.selected_col;
    if (what==2) return gs.selected_row;
    if (what==3) return gs.move_history.size();
    float v=0;
    for(const auto& p:gs.pieces) {
        if (what==5 && p.jelly.held) return 1;
        for(float x:p.jelly.offset) v=std::max(v,std::abs(x));
    }
    return what==5 ? 0 : v;
}
extern "C" EMSCRIPTEN_KEEPALIVE void chess_dbg_jelly_optics(float ior){renderer_dbg_jelly_ior(ior);}
extern "C" EMSCRIPTEN_KEEPALIVE float chess_dbg_square(int col,int row,float lift,int axis) {
    float x,z; square_center(col,row,x,z);
    float rad=3.14159265358979323846f/180.f;
    Mat4 view=mat4_multiply(mat4_translate(0,0,-g_app.zoom),mat4_multiply(mat4_rotate_x(g_app.rot_x*rad),mat4_multiply(mat4_rotate_y(g_app.rot_y*rad),mat4_translate(0,-BOARD_Y,0))));
    Mat4 proj=mat4_perspective(45*rad,static_cast<float>(g_width)/g_height,.1f,100.f);
    Vec4 p=mat4_mul_vec4(mat4_multiply(proj,view),{x,BOARD_Y+lift,z,1});
    return axis==0 ? (p.x/p.w+1)*.5f*g_width : (1-p.y/p.w)*.5f*g_height;
}

extern "C" EMSCRIPTEN_KEEPALIVE void chess_dbg_cable_game(void) {
    // Mirror the real user flow: enter a normal game first, then cycle
    // the environment mid-game (the btn==11 path in app_state.cpp) —
    // app_enter_game is NEVER called with retro pre-selected in the UI.
    app_enter_game(g_app);
    std::fprintf(stderr, "[web-dbg] after enter_game mode=%d games=%zu\n",
                (int)g_app.mode, g_app.games.size());
    g_app.environment = AppState::Environment::DataCenter;
    renderer_set_environment(1);
    int npieces = 0, nalive = 0;
    if (!g_app.games.empty()) {
        npieces = (int)g_app.games[0].game.pieces.size();
        for (const auto& p : g_app.games[0].game.pieces) if (p.alive) ++nalive;
    }
    std::fprintf(stderr, "[web-dbg] mode=%d games=%zu pieces=%d alive=%d\n",
                (int)g_app.mode, g_app.games.size(), npieces, nalive);
}
// GIF harness: force a retro piece type's animated part to a fixed phase
// (seconds), then chess_dbg_dump renders that frame. type<0 clears it.
extern "C" EMSCRIPTEN_KEEPALIVE void chess_dbg_animate(int type, float t_seconds) {
    renderer_dbg_animate(type, t_seconds);
}
// GIF harness: orbit the camera (degrees pitch, degrees yaw, zoom) so a face
// that points away from the default view (e.g. the king's screen) can be seen.
extern "C" EMSCRIPTEN_KEEPALIVE void chess_dbg_camera(float rot_x, float rot_y, float zoom) {
    g_app.rot_x = rot_x; g_app.rot_y = rot_y; if (zoom > 0.0f) g_app.zoom = zoom;
}
// Render one frame directly (headless rAF doesn't tick) and write the
// framebuffer to /dump.ppm — glReadPixels reads what was drawn even when
// the software compositor never presents to the visible canvas.
extern "C" EMSCRIPTEN_KEEPALIVE void chess_dbg_dump(void) {
    app_tick(g_app);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, g_width, g_height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    app_render(g_app, g_width, g_height);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // WebGL2 only guarantees RGBA/UNSIGNED_BYTE readback from the default
    // framebuffer; GL_RGB is invalid and silently yields zeros.
    std::vector<unsigned char> px(static_cast<size_t>(g_width) * g_height * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, g_width, g_height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    if (FILE* f = std::fopen("/dump.ppm", "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", g_width, g_height);
        for (int y = g_height - 1; y >= 0; --y) {
            const unsigned char* row = px.data() + static_cast<size_t>(y) * g_width * 4;
            for (int x = 0; x < g_width; ++x)
                std::fwrite(row + x * 4, 1, 3, f);   // RGB, drop alpha
        }
        std::fclose(f);
        std::printf("[web-dump] wrote /dump.ppm %dx%d\n", g_width, g_height);
    }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
// Downloading is async in JS, but the *install* — JPEG/PNG decode, mesh
// VBO upload, splat unpacking — runs on the main thread and is what makes
// the menu stutter. A whole group at once is far too much for one frame
// (the game tier alone decodes ~15 textures across board+clock+table, the
// retro set ~60), so each group is broken into steps and at most one step
// runs per frame. The group is only marked ready once its last step lands.
namespace {

enum InstallStep {
    STEP_BOARD = 0, STEP_CLOCK, STEP_TABLE, STEP_AUDIO,   // ASSET_GAME
    STEP_RETRO,                                           // ASSET_RETRO
    STEP_SPLAT,                                           // ASSET_SPLAT_*
};

struct PendingInstall {
    AssetGroup group;
    InstallStep step;
    bool last;          // marks the group ready once this step completes
};

PendingInstall g_steps[8];
int g_step_head = 0, g_step_count = 0;

void push_step(AssetGroup g, InstallStep s, bool last) {
    if (g_step_count >= (int)(sizeof(g_steps) / sizeof(g_steps[0]))) return;
    int tail = (g_step_head + g_step_count) % (int)(sizeof(g_steps) / sizeof(g_steps[0]));
    g_steps[tail] = PendingInstall{g, s, last};
    g_step_count++;
}

void queue_steps_for(AssetGroup g) {
    switch (g) {
        case ASSET_GAME:
            push_step(g, STEP_BOARD, false);
            push_step(g, STEP_CLOCK, false);
            push_step(g, STEP_TABLE, false);
            push_step(g, STEP_AUDIO, true);
            break;
        case ASSET_RETRO:
            push_step(g, STEP_RETRO, true);
            break;
        case ASSET_SPLAT_MEDIEVAL:
        case ASSET_SPLAT_DATACENTER:
            push_step(g, STEP_SPLAT, true);
            break;
        default:
            // Puzzles are read straight off the filesystem — no GL work.
            assets_set_state(g, ASSET_READY);
            break;
    }
}

void run_step(const PendingInstall& p) {
    switch (p.step) {
        case STEP_BOARD: renderer_load_game_asset_part(0); break;
        case STEP_CLOCK: renderer_load_game_asset_part(1); break;
        case STEP_TABLE: renderer_load_game_asset_part(2); break;
        case STEP_AUDIO:
            // sounds/ rides in the game package, so the WAVs only exist
            // now — audio_init() ran at startup against an empty
            // directory. Retry the clips and (re)start the menu track;
            // browser autoplay policy gates real sound on a user gesture
            // anyway, so starting it late costs nothing.
            audio_reload_clips();
            g_music_pending = true;
            break;
        case STEP_RETRO: renderer_load_retro_assets(); break;
        case STEP_SPLAT: {
            // Upload the cloud only if it belongs to the scene we're
            // actually in; the other environment's SPZ just sits in the
            // filesystem until Options switches to it. This is also where
            // the saved environment first reaches the renderer on the web
            // (renderer_init no longer loads a splat there).
            int want = (p.group == ASSET_SPLAT_DATACENTER) ? 1 : 0;
            if (static_cast<int>(g_app.environment) == want)
                renderer_set_environment(want);
            break;
        }
    }
}

}  // namespace

static void pump_asset_installs() {
    assets_web_pump_downloads(static_cast<int>(g_app.environment));

    // Move any newly-arrived package into the step queue.
    AssetGroup g;
    while (assets_web_take_installed(&g)) queue_steps_for(g);

    if (g_step_count <= 0) return;

    // The main menu draws exactly three things: the piece meshes, the
    // wood buttons and text — all of which are in the core preload. None
    // of the renderer installs (board, clock, table, retro, splat) put a
    // single pixel on the menu, and every one of them costs multiple
    // texture decodes or a multi-megabyte unpack. So while MODE_MENU is
    // up we run *no* renderer install at all: the menu animation stays
    // smooth and the work happens once the player moves on to the
    // pregame screen, where the loading panel already covers the wait.
    //
    // STEP_AUDIO is the exception — the menu music is a menu feature, so
    // it's worth its one-off cost there.
    const int CAP = (int)(sizeof(g_steps) / sizeof(g_steps[0]));
    const bool on_menu = (g_app.mode == MODE_MENU);
    int slot = -1;
    for (int i = 0; i < g_step_count; i++) {
        const PendingInstall& c = g_steps[(g_step_head + i) % CAP];
        if (on_menu && c.step != STEP_AUDIO) continue;   // leave it queued
        slot = i;
        break;
    }
    if (slot < 0) return;                    // nothing runnable this frame

    PendingInstall p = g_steps[(g_step_head + slot) % CAP];
    // Close the gap left by taking an out-of-order step.
    for (int i = slot; i > 0; i--)
        g_steps[(g_step_head + i) % CAP] = g_steps[(g_step_head + i - 1) % CAP];
    g_step_head = (g_step_head + 1) % CAP;
    g_step_count--;

    run_step(p);
    if (p.last) {
        assets_set_state(p.group, ASSET_READY);
        std::fprintf(stderr, "[assets] %s ready\n", assets_group_label(p.group));
    }
}

// Start the menu track the first time a gesture has happened and the
// sounds have arrived — see g_user_gesture.
static void pump_menu_music() {
    if (!g_music_pending || !g_user_gesture) return;
    if (g_app.mode != MODE_MENU) { g_music_pending = false; return; }
    g_music_pending = false;
    audio_music_play("intro_music.wav");
}

static void main_loop_iter() {
    pump_events();
    pump_menu_music();
    poll_ai_results();
    pump_asset_installs();
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
    // Ask the browser to multisample the WebGL canvas. SDL2 on
    // Emscripten translates these to `antialias = true` on the
    // WebGL context attributes; the browser then picks the
    // actual sample count (typically 4×). Without this the
    // canvas comes back with antialias=false and SAMPLES=0,
    // which is what was making the web build look noticeably
    // jaggier than the native build (which gets MSAA via
    // g_scene_ms_fbo).
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLEBUFFERS, 1);
    SDL_GL_SetAttribute(SDL_GL_MULTISAMPLESAMPLES, 4);

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
    voice_web_bind_app(&g_app);
    app_enter_menu(g_app);

    // Everything past the menu (board, clock, table, sounds, retro set,
    // splat backdrops, puzzles) now downloads in the background — see
    // web/asset_loader_web.cpp. assets_reset() leaves ASSET_CORE ready
    // (it rode in with chess.data) and every other group pending; the
    // frame loop pumps them in priority order for the saved environment.
    assets_reset();
    js_log("entering main loop");

    emscripten_set_main_loop(main_loop_iter, 0, 1);
    return 0;
}
