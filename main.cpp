// Desktop driver: GTK+3 window, GtkGLArea, libcurl-free subprocess
// Stockfish dispatch via std::thread + g_idle_add. Everything gameplay-
// related lives in app_state.cpp — this file just wires GTK events and
// widgets to the platform-agnostic app_* functions.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include <epoxy/gl.h>
#include <gtk/gtk.h>

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
};
struct EvalArrived {
    int cp;
    int score_index;
    std::string best_uci;  // empty when engine returned (none)
};

static gboolean on_ai_move_ready(gpointer data) {
    auto* r = static_cast<AiMoveArrived*>(data);
    app_ai_move_ready(g_app, r->uci.c_str());
    delete r;
    return G_SOURCE_REMOVE;
}

static gboolean on_eval_ready(gpointer data) {
    auto* r = static_cast<EvalArrived*>(data);
    app_eval_ready(g_app, r->cp, r->score_index, r->best_uci);
    delete r;
    return G_SOURCE_REMOVE;
}

static void plat_trigger_ai_move(const char* fen_c, int movetime) {
    std::string fen = fen_c ? fen_c : "";
    int mt = movetime;
    std::thread([fen, mt]() {
        (void)mt; // ask_ai_move reads CHESS_AI_MOVETIME_MS itself
        std::printf("AI thinking... FEN: %s\n", fen.c_str());
        std::string uci = ask_ai_move(fen);
        auto* r = new AiMoveArrived{uci};
        g_idle_add(on_ai_move_ready, r);
    }).detach();
}

static void plat_trigger_eval(const char* fen_c, int movetime, int idx) {
    std::string fen = fen_c ? fen_c : "";
    int mt = movetime;
    std::thread([fen, mt, idx]() {
        std::string best;
        int cp = stockfish_eval(fen, mt, best);
        auto* r = new EvalArrived{cp, idx, std::move(best)};
        g_idle_add(on_eval_ready, r);
    }).detach();
}

static void plat_set_ai_elo(int elo) {
    ai_player_set_elo(elo);
}

static void plat_request_quit() {
    gtk_main_quit();
}

static const AppPlatform g_platform = {
    plat_set_status,
    plat_queue_redraw,
    plat_now_us,
    plat_trigger_ai_move,
    plat_trigger_eval,
    plat_set_ai_elo,
    plat_request_quit,
};

// ---------------------------------------------------------------------------
// Per-frame tick — runs every GTK frame, delegates to app_tick.
// ---------------------------------------------------------------------------
static gboolean on_tick(GtkWidget*, GdkFrameClock*, gpointer) {
    app_tick(g_app);
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
static void on_realize(GtkGLArea* area) {
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) != nullptr) return;
    renderer_init(g_loaded_models);

    // One continuous tick callback drives all animation.
    gtk_widget_add_tick_callback(GTK_WIDGET(area), on_tick, nullptr, nullptr);
}

static gboolean on_render(GtkGLArea* area, GdkGLContext*) {
    int w = gtk_widget_get_allocated_width(GTK_WIDGET(area));
    int h = gtk_widget_get_allocated_height(GTK_WIDGET(area));
    app_render(g_app, w, h);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    if (argc > 1) g_models_dir = argv[1];

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
    gtk_window_set_default_size(GTK_WINDOW(g_window), 1024, 768);
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
    gtk_main();
    app_voice_shutdown(g_app);
    voice_tts_shutdown();
    app_chessnut_shutdown(g_app);
    return 0;
}
