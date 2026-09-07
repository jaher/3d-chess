// Desktop driver: GTK+3 window, GtkGLArea, libcurl-free subprocess
// Stockfish dispatch via std::thread + g_idle_add. Everything gameplay-
// related lives in app_state.cpp — this file just wires GTK events and
// widgets to the platform-agnostic app_* functions.

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <thread>

#include <epoxy/gl.h>
#include <gtk/gtk.h>
#include <curl/curl.h>

#include "ai_player.h"
#include "app_state.h"
#include "audio.h"
#include "board_renderer.h"
#include "voice_tts.h"
#include "chess_rules.h"
#include "chess_types.h"
#include "net_sync.h"
#include "stl_model.h"

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static std::string g_models_dir = "models";
static StlModel    g_loaded_models[PIECE_COUNT];
static GtkWidget*  g_window  = nullptr;
static GtkWidget*  g_gl_area = nullptr;
static AppState    g_app;

// ---------------------------------------------------------------------------
// Platform hooks (AppPlatform)
// ---------------------------------------------------------------------------
static void plat_set_status(const char* text) {
    if (g_window) gtk_window_set_title(GTK_WINDOW(g_window), text);
}

static void plat_queue_redraw(void) {
    if (g_gl_area) gtk_widget_queue_draw(g_gl_area);
}

static int64_t plat_now_us(void) {
    return static_cast<int64_t>(g_get_monotonic_time());
}

// --- AI dispatch ---
// std::thread + g_idle_add: thread runs the (potentially blocking)
// ask_ai_move() call, then hands the result back to GTK's main thread
// via g_idle_add so we can safely touch GameState.
struct AiMoveArrived {
    std::string uci;
    int         game_id;
};
struct EvalArrived {
    int cp;
    int score_index;
    std::string best_uci;    // empty when engine returned (none)
    int         game_id;
    // Second-best PV — drives the "Great move" / "Miss"
    // classifications. Empty UCI / 0 cp means we didn't get a
    // second PV (either MultiPV=1 or the position has only one
    // legal move).
    std::string second_uci;
    int         second_cp = 0;
    std::string pv;          // full UCI principal variation of the best line
};

static gboolean on_ai_move_ready(gpointer data) {
    auto* r = static_cast<AiMoveArrived*>(data);
    app_ai_move_ready(g_app, r->uci.c_str(), r->game_id);
    delete r;
    return G_SOURCE_REMOVE;
}

static gboolean on_eval_ready(gpointer data) {
    auto* r = static_cast<EvalArrived*>(data);
    app_eval_ready(g_app, r->cp, r->score_index, r->best_uci, r->game_id,
                   r->second_uci, r->second_cp, r->pv);
    delete r;
    return G_SOURCE_REMOVE;
}

static void plat_trigger_ai_move(const char* fen_c, int movetime,
                                 int game_id) {
    std::string fen = fen_c ? fen_c : "";
    int mt = movetime;
    std::thread([fen, mt, game_id]() {
        (void)mt; // ask_ai_move reads CHESS_AI_MOVETIME_MS itself
        std::printf("AI thinking... FEN: %s\n", fen.c_str());
        std::string uci = ask_ai_move(fen);
        auto* r = new AiMoveArrived{uci, game_id};
        g_idle_add(on_ai_move_ready, r);
    }).detach();
}

static void plat_trigger_eval(const char* fen_c, int movetime, int idx,
                              int game_id) {
    std::string fen = fen_c ? fen_c : "";
    int mt = movetime;
    std::thread([fen, mt, idx, game_id]() {
        std::string best, second, pv;
        int second_cp = 0;
        int cp = stockfish_eval(fen, mt, best, &second, &second_cp, &pv);
        auto* r = new EvalArrived{cp, idx, std::move(best), game_id,
                                  std::move(second), second_cp, std::move(pv)};
        g_idle_add(on_eval_ready, r);
    }).detach();
}

static void plat_set_ai_elo(int elo) {
    ai_player_set_elo(elo);
}

static void plat_request_quit() {
    gtk_main_quit();
}

// --- Puzzle fetch dispatch ---
// Same idiom as AI move / eval: spawn a worker thread that runs the
// blocking HTTP fetch via libcurl, then marshal the JSON body back
// to the GTK main thread via g_idle_add. On any failure (network,
// non-2xx status, libcurl init error) the worker delivers an empty
// body and the UI surfaces a "couldn't load puzzle" hint via
// app_puzzle_ready.
struct PuzzleArrived {
    std::string body;
    bool        daily;
};

static gboolean on_puzzle_ready(gpointer data) {
    auto* r = static_cast<PuzzleArrived*>(data);
    app_puzzle_ready(g_app, r->body.c_str(), r->daily);
    delete r;
    return G_SOURCE_REMOVE;
}

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
    // libcurl's defaults are fine for everything we need: no
    // redirect follow → enable it (chess.com sometimes 301s); fail
    // on HTTP ≥ 400 so we don't pass an HTML error page through to
    // the JSON parser; 10 s hard timeout so a hung connection can't
    // wedge the worker thread for the session; small connect timeout
    // so DNS / network errors surface fast.
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
        auto* r = new PuzzleArrived{std::move(body), daily};
        g_idle_add(on_puzzle_ready, r);
    }).detach();
}

// --- Glasses <-> desktop move sync (net_sync.cpp over the relay) ------------
// AppPlatform::trigger_send_move: a local move was made in network_mode — push
// its UCI to the linked glasses (no-op if not connected).
static void plat_send_move(const char* uci) { net_sync_send_move(uci); }

// These run on the GLib main thread (net_sync marshals via g_idle_add), so they
// may touch g_app directly.
static void on_sync_move(const char* uci) { app_remote_move_ready(g_app, uci); }
static void on_sync_reset(bool from_white) {
    // The initiator chose `from_white`; we take the opposite colour.
    app_start_network_game(g_app, !from_white);
}
static bool g_sync_initiator = false;
static void on_sync_role(bool initiator) {
    // First into the room is the colour authority: start as white and tell the
    // follower. A follower waits for the initiator's reset (on_sync_reset).
    g_sync_initiator = initiator;
    if (initiator) { app_start_network_game(g_app, true); net_sync_send_reset(true); }
}
static void on_sync_peer(bool joined) {
    plat_set_status(joined ? "3D Chess — glasses linked" : "3D Chess — glasses disconnected");
    // As the authority, re-send our reset so a peer that joined AFTER us syncs
    // (the first reset went to an empty room).
    if (joined && g_sync_initiator) { app_start_network_game(g_app, true); net_sync_send_reset(true); }
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
    plat_send_move,   // trigger_send_move: also used for the glasses link
};

// ---------------------------------------------------------------------------
// Per-frame tick — runs every GTK frame, delegates to app_tick.
// ---------------------------------------------------------------------------
static gboolean on_tick(GtkWidget*, GdkFrameClock*, gpointer) {
    app_tick(g_app);
    // Debug/headless: jump straight into a game after a few settle ticks
    // so the in-game board can be inspected without driving the menu.
    static int autostart_ticks = -1;
    if (std::getenv("CHESS_AUTOSTART")) {
        if (autostart_ticks < 0) autostart_ticks = 0;
        if (autostart_ticks >= 0 && autostart_ticks < 10) {
            if (++autostart_ticks == 10) app_enter_game(g_app);
        }
    }
    return G_SOURCE_CONTINUE;
}

// ---------------------------------------------------------------------------
// Key translation
// ---------------------------------------------------------------------------
static AppKey translate_key(guint keyval) {
    switch (keyval) {
        case GDK_KEY_Left:   return KEY_LEFT;
        case GDK_KEY_Right:  return KEY_RIGHT;
        case GDK_KEY_Escape: return KEY_ESCAPE;
        case GDK_KEY_a:
        case GDK_KEY_A:      return KEY_A;
        case GDK_KEY_m:
        case GDK_KEY_M:      return KEY_M;
        case GDK_KEY_s:
        case GDK_KEY_S:      return KEY_S;
        case GDK_KEY_d:
        case GDK_KEY_D:      return KEY_D;
        case GDK_KEY_l:
        case GDK_KEY_L:      return KEY_L;
        case GDK_KEY_c:
        case GDK_KEY_C:      return KEY_C;
        case GDK_KEY_r:
        case GDK_KEY_R:      return KEY_R;
        default:             return KEY_UNKNOWN;
    }
}

// ---------------------------------------------------------------------------
// GTK event handlers
// ---------------------------------------------------------------------------
static gboolean on_button_press(GtkWidget*, GdkEventButton* e, gpointer) {
    if (e->button == 1) app_press(g_app, e->x, e->y);
    return TRUE;
}

static gboolean on_button_release(GtkWidget*, GdkEventButton* e, gpointer gl_area) {
    if (e->button != 1) return TRUE;
    int w = gtk_widget_get_allocated_width(GTK_WIDGET(gl_area));
    int h = gtk_widget_get_allocated_height(GTK_WIDGET(gl_area));
    app_release(g_app, e->x, e->y, w, h);
    return TRUE;
}

static gboolean on_motion(GtkWidget*, GdkEventMotion* e, gpointer gl_area) {
    int w = gtk_widget_get_allocated_width(GTK_WIDGET(gl_area));
    int h = gtk_widget_get_allocated_height(GTK_WIDGET(gl_area));
    app_motion(g_app, e->x, e->y, w, h);
    return TRUE;
}

static gboolean on_scroll(GtkWidget*, GdkEventScroll* e, gpointer) {
    double d = 0.0;
    if (e->direction == GDK_SCROLL_UP)        d = -1.0;
    else if (e->direction == GDK_SCROLL_DOWN) d = +1.0;
    else if (e->direction == GDK_SCROLL_SMOOTH) {
        double dx, dy;
        gdk_event_get_scroll_deltas(reinterpret_cast<GdkEvent*>(e), &dx, &dy);
        d = dy;
    }
    app_scroll(g_app, d);
    return TRUE;
}

// Voice push-to-talk: spacebar press starts capture, release dispatches
// transcription on a worker thread which posts the result back via
// g_idle_add. Same marshalling pattern as plat_trigger_ai_move above.
struct VoiceArrived {
    std::string utterance;
    std::string error;
};

static gboolean on_voice_result_main(gpointer data) {
    auto* r = static_cast<VoiceArrived*>(data);
    app_voice_apply_result(g_app, r->utterance, r->error);
    delete r;
    return G_SOURCE_REMOVE;
}

static gboolean on_voice_continuous_result_main(gpointer data) {
    auto* r = static_cast<VoiceArrived*>(data);
    app_voice_continuous_apply(g_app, r->utterance, r->error);
    delete r;
    return G_SOURCE_REMOVE;
}

// Live partial transcripts: the streaming worker fires one per
// successful pass, surfacing in the title bar so the user can see
// what whisper is hearing in real time.
struct VoicePartial { std::string text; };

static gboolean on_voice_continuous_partial_main(gpointer data) {
    auto* r = static_cast<VoicePartial*>(data);
    app_voice_continuous_apply_partial(g_app, r->text);
    delete r;
    return G_SOURCE_REMOVE;
}

// Desktop always supports continuous voice (whisper.cpp is built
// in). The web build uses a runtime feature-detect — see
// web/voice_web.cpp.
bool app_voice_continuous_supported() {
    return true;
}

// ---------------------------------------------------------------------------
// Chessnut Move bridge — status marshalling on the GTK main loop
// ---------------------------------------------------------------------------
struct ChessnutStatus { std::string line; };

static gboolean on_chessnut_status_main(gpointer data) {
    auto* s = static_cast<ChessnutStatus*>(data);
    app_chessnut_apply_status(g_app, s->line);
    delete s;
    return G_SOURCE_REMOVE;
}

bool app_chessnut_supported() {
    // Always offered on desktop; the bridge will surface a runtime
    // failure if python3 / bleak / Bluetooth aren't available.
    return true;
}

void app_chessnut_toggle_request(AppState& a) {
    bool target = !a.chessnut_enabled;
    app_chessnut_set_enabled(a, target,
        [](const std::string& status) {
            auto* s = new ChessnutStatus{status};
            g_idle_add(on_chessnut_status_main, s);
        });
}

// Bridge between the shared options-screen click handler (in
// app_state.cpp) and the GTK marshalling code that lives here. Keeps
// g_idle_add out of the cross-platform layer.
void app_voice_toggle_continuous_request(AppState& a) {
    bool target = !a.voice_continuous_enabled;
    app_voice_set_continuous(a, target,
        [](const std::string& utterance, const std::string& error) {
            auto* r = new VoiceArrived{utterance, error};
            g_idle_add(on_voice_continuous_result_main, r);
        },
        [](const std::string& partial) {
            auto* r = new VoicePartial{partial};
            g_idle_add(on_voice_continuous_partial_main, r);
        });
}

static gboolean on_key_press(GtkWidget*, GdkEventKey* event, gpointer) {
    if (event->keyval == GDK_KEY_space) {
        app_voice_press(g_app);
        return TRUE;  // suppress GTK's default space-activates-button
    }
    app_key(g_app, translate_key(event->keyval));
    return TRUE;
}

static gboolean on_key_release(GtkWidget*, GdkEventKey* event, gpointer) {
    if (event->keyval == GDK_KEY_space) {
        app_voice_release(g_app,
            [](const std::string& utterance, const std::string& error) {
                // Worker thread → marshal onto the GTK main loop.
                auto* r = new VoiceArrived{utterance, error};
                g_idle_add(on_voice_result_main, r);
            });
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// GL callbacks
// ---------------------------------------------------------------------------
// Headless driver: on Xvfb the GdkFrameClock can stop ticking once
// animation settles, which freezes on_tick/on_render. When a dump or
// autostart is requested we install a wall-clock timer that forces a
// redraw every frame and jumps into a game, so the board can be
// inspected without a live frame clock or menu input.
static gboolean force_tick_cb(gpointer) {
    static int n = 0; static bool started = false;
    ++n;
    if (!started && std::getenv("CHESS_AUTOSTART") && n >= 12) {
        started = true;
        app_enter_game(g_app);
        if (const char* p = std::getenv("CHESS_CAM_PITCH")) g_app.rot_x = std::atof(p);
        if (const char* y = std::getenv("CHESS_CAM_YAW"))   g_app.rot_y = std::atof(y);
        if (const char* z = std::getenv("CHESS_CAM_ZOOM"))  g_app.zoom  = std::atof(z);
    }
    // Headless move driver: $CHESS_AUTOMOVE="c2c4,b1c3,..." plays the listed
    // moves through the AI sliding animation (app_dbg_animate_move), one at a
    // time as each previous animation finishes. $CHESS_AUTOMOVE_DUR (seconds)
    // slows the slide for capture. Debug/demo only — no legality checks.
    static std::vector<std::string> automoves = [] {
        std::vector<std::string> v;
        if (const char* am = std::getenv("CHESS_AUTOMOVE")) {
            std::string s(am), cur;
            for (char c : s) {
                if (c == ',') { if (!cur.empty()) v.push_back(cur); cur.clear(); }
                else cur += c;
            }
            if (!cur.empty()) v.push_back(cur);
        }
        return v;
    }();
    static size_t automove_next = 0;
    static int automove_cooldown = 0;
    if (started && automove_next < automoves.size()) {
        if (automove_cooldown > 0) {
            --automove_cooldown;
        } else {
            float dur = 0.0f;
            if (const char* d = std::getenv("CHESS_AUTOMOVE_DUR")) dur = std::atof(d);
            if (app_dbg_animate_move(g_app, automoves[automove_next].c_str(), dur)) {
                ++automove_next;
                automove_cooldown = 30;   // settle between moves
            }
        }
    }
    if (!g_gl_area) return G_SOURCE_CONTINUE;
    // The frame clock can stall on Xvfb, so render directly into the
    // GLArea's framebuffer here instead of waiting for the paint cycle.
    GtkGLArea* area = GTK_GL_AREA(g_gl_area);
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area)) return G_SOURCE_CONTINUE;
    app_tick(g_app);
    // Live camera override for headless orbit capture: re-read $CHESS_CAM_FILE
    // ("rot_x rot_y zoom") every frame so a capture script can sweep the view
    // and dump a rotating sequence without relaunching the process.
    if (const char* cf = std::getenv("CHESS_CAM_FILE")) {
        if (FILE* f = std::fopen(cf, "r")) {
            float rx, ry, z;
            if (std::fscanf(f, "%f %f %f", &rx, &ry, &z) == 3) {
                g_app.rot_x = rx; g_app.rot_y = ry; g_app.zoom = z;
            }
            std::fclose(f);
        }
    }
    int w = gtk_widget_get_allocated_width(g_gl_area);
    int h = gtk_widget_get_allocated_height(g_gl_area);
    gtk_gl_area_attach_buffers(area);   // bind the GLArea FBO
    app_render(g_app, w, h);
    static const char* dump_req = std::getenv("CHESS_DUMP_REQ");
    static const char* dump_out = std::getenv("CHESS_DUMP_OUT");
    if (dump_req && dump_out) {
        if (FILE* rf = std::fopen(dump_req, "rb")) {
            std::fclose(rf); std::remove(dump_req);
            std::vector<unsigned char> px(static_cast<size_t>(w) * h * 3);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
            if (FILE* of = std::fopen(dump_out, "wb")) {
                std::fprintf(of, "P6\n%d %d\n255\n", w, h);
                for (int y = h - 1; y >= 0; --y)
                    std::fwrite(px.data() + static_cast<size_t>(y) * w * 3, 1,
                                static_cast<size_t>(w) * 3, of);
                std::fclose(of);
            }
        }
    }
    return G_SOURCE_CONTINUE;
}

static void on_realize(GtkGLArea* area) {
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) != nullptr) return;
    renderer_init(g_loaded_models);
    // Preview overrides never write the user's saved Options settings.
    if(const char* jelly=std::getenv("CHESS_JELLY"))g_app.jelly_pieces=std::atoi(jelly)!=0;
    if (std::getenv("CHESS_DUMP_REQ") || std::getenv("CHESS_AUTOSTART"))
        g_timeout_add(80, force_tick_cb, nullptr);

    // Headless capture override: $CHESS_ENV=0|1 forces the environment
    // (0=medieval, 1=datacenter) regardless of the saved setting, so a dump
    // can inspect either backdrop without touching the user's settings.ini.
    if (const char* e = std::getenv("CHESS_ENV")) {
        int ev = std::atoi(e);
        if (ev == 0 || ev == 1)
            g_app.environment = static_cast<AppState::Environment>(ev);
    }

    // Apply the splat environment saved by app_settings_load. This is
    // the right place for it: a GL context is current now (renderer_init
    // just loaded the medieval default under it), whereas app_settings_load
    // runs before the area is realized and cannot legally touch GL.
    if (g_app.environment != AppState::Environment::MedievalRoom) {
        renderer_set_environment(static_cast<int>(g_app.environment));
    }

    // One continuous tick callback drives all animation.
    gtk_widget_add_tick_callback(GTK_WIDGET(area), on_tick, nullptr, nullptr);
}

static gboolean on_render(GtkGLArea* area, GdkGLContext*) {
    int w = gtk_widget_get_allocated_width(GTK_WIDGET(area));
    int h = gtk_widget_get_allocated_height(GTK_WIDGET(area));
    app_render(g_app, w, h);
    // Headless inspection hook: when $CHESS_DUMP_REQ exists, write the
    // current frame to $CHESS_DUMP_OUT as a PPM and delete the request.
    static const char* dump_req = std::getenv("CHESS_DUMP_REQ");
    static const char* dump_out = std::getenv("CHESS_DUMP_OUT");
    if (dump_req && dump_out) {
        if (FILE* rf = std::fopen(dump_req, "rb")) {
            std::fclose(rf); std::remove(dump_req);
            std::vector<unsigned char> px(static_cast<size_t>(w) * h * 3);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
            if (FILE* of = std::fopen(dump_out, "wb")) {
                std::fprintf(of, "P6\n%d %d\n255\n", w, h);
                for (int y = h - 1; y >= 0; --y)   // GL origin is bottom-left
                    std::fwrite(px.data() + static_cast<size_t>(y) * w * 3, 1,
                                static_cast<size_t>(w) * 3, of);
                std::fclose(of);
            }
        }
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    if (argc > 1) g_models_dir = argv[1];

    // libcurl global init has documented thread-safety caveats — it
    // must run before any concurrent CURL usage and from a single
    // thread. Doing it here at the top of main(), before workers
    // spawn, satisfies both. curl_global_cleanup() is called via
    // atexit so it runs after the puzzle worker threads have
    // finished — they're detached, so we can't join them, and an
    // explicit cleanup before shutdown could race a still-running
    // request.
    if (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK) {
        std::atexit([]{ curl_global_cleanup(); });
    } else {
        std::fprintf(stderr,
            "[puzzle] curl_global_init failed; puzzle fetch disabled\n");
    }

    // Startup is fan-out: model parsing (6 threads), audio device
    // setup (1 thread), and gtk_init all run concurrently. gtk_init
    // must stay on the main thread (it owns the X11/Wayland display
    // connection), so the main thread takes it while the workers
    // handle the rest. Joins happen before first use.
    std::printf("Loading models...\n");
    std::thread model_threads[PIECE_COUNT];
    for (int i = 0; i < PIECE_COUNT; i++) {
        model_threads[i] = std::thread([i] {
            g_loaded_models[i].load(
                g_models_dir + "/" + piece_filenames[i]);
            std::printf("  %s: %zu triangles\n",
                        piece_filenames[i],
                        g_loaded_models[i].triangle_count());
        });
    }
    std::thread audio_thread([]{ audio_init(); });

    gtk_init(&argc, &argv);

    for (auto& t : model_threads) t.join();
    std::printf("All models loaded.\n");

    app_init(g_app, &g_platform);
    g_app.loaded_models = g_loaded_models;

    audio_thread.join();

    g_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_window), "3D Chess");
    int win_w = 1024, win_h = 768;
    if (const char* e = std::getenv("CHESS_WIN_W")) win_w = std::atoi(e);
    if (const char* e = std::getenv("CHESS_WIN_H")) win_h = std::atoi(e);
    gtk_window_set_default_size(GTK_WINDOW(g_window), win_w, win_h);
    g_signal_connect(g_window, "destroy",
                     G_CALLBACK(gtk_main_quit), nullptr);

    g_gl_area = gtk_gl_area_new();
    gtk_gl_area_set_required_version(GTK_GL_AREA(g_gl_area), 3, 3);
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(g_gl_area), TRUE);
    gtk_container_add(GTK_CONTAINER(g_window), g_gl_area);

    g_signal_connect(g_gl_area, "realize", G_CALLBACK(on_realize), nullptr);
    g_signal_connect(g_gl_area, "render",  G_CALLBACK(on_render),  nullptr);

    gtk_widget_add_events(g_gl_area,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);

    g_signal_connect(g_gl_area, "button-press-event",
                     G_CALLBACK(on_button_press),   g_gl_area);
    g_signal_connect(g_gl_area, "button-release-event",
                     G_CALLBACK(on_button_release), g_gl_area);
    g_signal_connect(g_gl_area, "motion-notify-event",
                     G_CALLBACK(on_motion),         g_gl_area);
    g_signal_connect(g_gl_area, "scroll-event",
                     G_CALLBACK(on_scroll),         nullptr);
    g_signal_connect(g_window,  "key-press-event",
                     G_CALLBACK(on_key_press),      nullptr);
    g_signal_connect(g_window,  "key-release-event",
                     G_CALLBACK(on_key_release),    nullptr);

    gtk_widget_show_all(g_window);

    app_enter_menu(g_app);

    // Optional glasses<->desktop move sync. Launch with CHESS_SYNC_ROOM set to
    // link this desktop to the glasses Web App through the relay
    // (glasses/sync-server.js). Host/port default to the local relay.
    if (const char* room = std::getenv("CHESS_SYNC_ROOM")) {
        const char* host = std::getenv("CHESS_SYNC_HOST"); if (!host) host = "127.0.0.1";
        int port = std::getenv("CHESS_SYNC_PORT") ? std::atoi(std::getenv("CHESS_SYNC_PORT")) : 8091;
        net_sync_init(on_sync_move, on_sync_reset, on_sync_peer, on_sync_role);
        net_sync_connect(host, port, room, "desktop");
        // Debug/headless: CHESS_SYNC_TESTMOVE=<uci> sends one move after a
        // short delay so the outgoing path can be verified without a click.
        if (const char* tm = std::getenv("CHESS_SYNC_TESTMOVE")) {
            static std::string s_tm = tm;
            g_timeout_add(2500, [](gpointer) -> gboolean { net_sync_send_move(s_tm.c_str()); return G_SOURCE_REMOVE; }, nullptr);
        }
    }

    gtk_main();
    net_sync_disconnect();     // close the glasses link + join its reader thread
    app_voice_shutdown(g_app);
    voice_tts_shutdown();      // stops TTS playback before the device closes
    app_chessnut_shutdown(g_app);
    audio_shutdown();          // close the audio device + free clip buffers
    return 0;
}
