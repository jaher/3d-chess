#include <cstdio>
#include <string>
#include <thread>

#include <epoxy/gl.h>
#include <gtk/gtk.h>

#include "board_renderer.h"
#include "chess_rules.h"
#include "chess_types.h"
#include "game_state.h"
#include "mat4.h"
#include "stl_model.h"

// ---------------------------------------------------------------------------
// Camera state
// ---------------------------------------------------------------------------
static float g_rot_x = 30.0f;
static float g_rot_y = 180.0f;
static float g_zoom = 12.0f;
static double g_last_mouse_x = 0, g_last_mouse_y = 0;
static double g_press_x = 0, g_press_y = 0;
static gboolean g_dragging = FALSE;

static std::string g_models_dir = "models";
static StlModel g_loaded_models[PIECE_COUNT];
static GtkWidget* g_window = nullptr;
static GtkWidget* g_gl_area = nullptr;

// ---------------------------------------------------------------------------
// Screen-to-board picking
// ---------------------------------------------------------------------------
static bool screen_to_board(double mx, double my, int width, int height,
                            int& out_col, int& out_row) {
    float deg2rad = static_cast<float>(M_PI) / 180.0f;
    float aspect = static_cast<float>(width) / static_cast<float>(height);
    Mat4 view = mat4_multiply(
        mat4_translate(0, 0, -g_zoom),
        mat4_multiply(mat4_rotate_x(g_rot_x * deg2rad),
                      mat4_multiply(mat4_rotate_y(g_rot_y * deg2rad),
                                    mat4_translate(0, -BOARD_Y, 0))));
    Mat4 proj = mat4_perspective(45.0f * deg2rad, aspect, 0.1f, 100.0f);
    Mat4 inv_vp = mat4_inverse(mat4_multiply(proj, view));

    float ndc_x = 2.0f * static_cast<float>(mx) / width - 1.0f;
    float ndc_y = 1.0f - 2.0f * static_cast<float>(my) / height;

    Vec4 nw = mat4_mul_vec4(inv_vp, {ndc_x, ndc_y, -1, 1});
    Vec4 fw = mat4_mul_vec4(inv_vp, {ndc_x, ndc_y,  1, 1});
    if (std::abs(nw.w) < 1e-10f || std::abs(fw.w) < 1e-10f) return false;

    float np[3] = {nw.x/nw.w, nw.y/nw.w, nw.z/nw.w};
    float fp[3] = {fw.x/fw.w, fw.y/fw.w, fw.z/fw.w};
    float d[3] = {fp[0]-np[0], fp[1]-np[1], fp[2]-np[2]};
    if (std::abs(d[1]) < 1e-10f) return false;

    float t = (BOARD_Y - np[1]) / d[1];
    if (t < 0) return false;

    out_col = static_cast<int>(std::floor((np[0] + t*d[0]) / SQ + 4.0f));
    out_row = static_cast<int>(std::floor((np[2] + t*d[2]) / SQ + 4.0f));
    return in_bounds(out_col, out_row);
}

// ---------------------------------------------------------------------------
// Selection animation
// ---------------------------------------------------------------------------
static gboolean on_tick(GtkWidget* widget, GdkFrameClock*, gpointer) {
    auto& gs = game_get_state();
    if (gs.selected_col >= 0)
        gtk_widget_queue_draw(widget);
    return G_SOURCE_CONTINUE;
}

static void start_animation() {
    auto& gs = game_get_state();
    if (gs.tick_id == 0) {
        gs.anim_start_time = g_get_monotonic_time();
        gs.tick_id = gtk_widget_add_tick_callback(g_gl_area, on_tick, nullptr, nullptr);
    }
}

static void stop_animation() {
    auto& gs = game_get_state();
    if (gs.tick_id != 0) {
        gtk_widget_remove_tick_callback(g_gl_area, gs.tick_id);
        gs.tick_id = 0;
    }
}

// ---------------------------------------------------------------------------
// Click handling
// ---------------------------------------------------------------------------
static void handle_board_click(double mx, double my, GtkWidget* widget) {
    auto& gs = game_get_state();
    if (gs.ai_thinking || gs.ai_animating || gs.analysis_mode || !gs.white_turn || gs.game_over)
        return;

    int width = gtk_widget_get_allocated_width(widget);
    int height = gtk_widget_get_allocated_height(widget);

    int col, row;
    if (!screen_to_board(mx, my, width, height, col, row)) {
        gs.selected_col = gs.selected_row = -1;
        gs.valid_moves.clear();
        stop_animation();
        gtk_widget_queue_draw(widget);
        return;
    }

    if (gs.selected_col >= 0) {
        for (const auto& [mc, mr] : gs.valid_moves) {
            if (mc == col && mr == row) {
                execute_move(gs, gs.selected_col, gs.selected_row, col, row);
                gs.selected_col = gs.selected_row = -1;
                gs.valid_moves.clear();
                stop_animation();
                gtk_widget_queue_draw(widget);
                game_update_title(g_window);
                if (!gs.white_turn && !gs.game_over)
                    game_trigger_ai(g_window, g_gl_area);
                return;
            }
        }
    }

    int idx = gs.grid[row][col];
    if (idx >= 0 && gs.pieces[idx].is_white == gs.white_turn) {
        gs.selected_col = col;
        gs.selected_row = row;
        gs.valid_moves = generate_legal_moves(gs, col, row);
        gs.anim_start_time = g_get_monotonic_time();
        start_animation();
    } else {
        gs.selected_col = gs.selected_row = -1;
        gs.valid_moves.clear();
        stop_animation();
    }
    gtk_widget_queue_draw(widget);
}

// ---------------------------------------------------------------------------
// Key handling (analysis mode)
// ---------------------------------------------------------------------------
static gboolean on_key_press(GtkWidget*, GdkEventKey* event, gpointer) {
    auto& gs = game_get_state();

    if (gs.analysis_mode) {
        if (event->keyval == GDK_KEY_Left && gs.analysis_index > 0) {
            gs.analysis_index--;
            gs.restore_snapshot(gs.analysis_index);
            game_update_analysis_title(g_window);
            gtk_widget_queue_draw(g_gl_area);
        } else if (event->keyval == GDK_KEY_Right &&
                   gs.analysis_index < static_cast<int>(gs.snapshots.size()) - 1) {
            gs.analysis_index++;
            gs.restore_snapshot(gs.analysis_index);
            game_update_analysis_title(g_window);
            gtk_widget_queue_draw(g_gl_area);
        } else if (event->keyval == GDK_KEY_Escape) {
            game_exit_analysis();
            game_update_title(g_window);
            gtk_widget_queue_draw(g_gl_area);
        }
    } else {
        if (event->keyval == GDK_KEY_a || event->keyval == GDK_KEY_Left ||
            event->keyval == GDK_KEY_Right) {
            if (!gs.ai_thinking && !gs.ai_animating && gs.snapshots.size() > 1) {
                game_enter_analysis(g_gl_area);
                if (event->keyval == GDK_KEY_Left && gs.analysis_index > 0)
                    gs.analysis_index--;
                gs.restore_snapshot(gs.analysis_index);
                game_update_analysis_title(g_window);
                gtk_widget_queue_draw(g_gl_area);
            }
        }
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// Mouse callbacks
// ---------------------------------------------------------------------------
static gboolean on_button_press(GtkWidget*, GdkEventButton* event, gpointer) {
    if (event->button == 1) {
        g_dragging = TRUE;
        g_last_mouse_x = event->x; g_last_mouse_y = event->y;
        g_press_x = event->x; g_press_y = event->y;
    }
    return TRUE;
}

static gboolean on_button_release(GtkWidget*, GdkEventButton* event, gpointer gl_area) {
    if (event->button == 1) {
        g_dragging = FALSE;
        double dx = event->x - g_press_x, dy = event->y - g_press_y;
        if (dx*dx + dy*dy < 25.0)
            handle_board_click(event->x, event->y, GTK_WIDGET(gl_area));
    }
    return TRUE;
}

static gboolean on_motion(GtkWidget*, GdkEventMotion* event, gpointer gl_area) {
    if (g_dragging) {
        g_rot_y += static_cast<float>(event->x - g_last_mouse_x) * 0.3f;
        g_rot_x += static_cast<float>(event->y - g_last_mouse_y) * 0.3f;
        if (g_rot_x < 5.0f) g_rot_x = 5.0f;
        if (g_rot_x > 89.0f) g_rot_x = 89.0f;
        g_last_mouse_x = event->x; g_last_mouse_y = event->y;
        gtk_widget_queue_draw(GTK_WIDGET(gl_area));
    }
    return TRUE;
}

static gboolean on_scroll(GtkWidget*, GdkEventScroll* event, gpointer gl_area) {
    if (event->direction == GDK_SCROLL_UP) g_zoom -= 0.5f;
    else if (event->direction == GDK_SCROLL_DOWN) g_zoom += 0.5f;
    if (event->direction == GDK_SCROLL_SMOOTH) {
        double dx, dy; gdk_event_get_scroll_deltas((GdkEvent*)event, &dx, &dy);
        g_zoom += static_cast<float>(dy) * 0.3f;
    }
    if (g_zoom < 3.0f) g_zoom = 3.0f;
    if (g_zoom > 40.0f) g_zoom = 40.0f;
    gtk_widget_queue_draw(GTK_WIDGET(gl_area));
    return TRUE;
}

// ---------------------------------------------------------------------------
// GL callbacks
// ---------------------------------------------------------------------------
static void on_realize(GtkGLArea* area) {
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area) != nullptr) return;
    renderer_init(g_loaded_models);
}

static gboolean on_render(GtkGLArea* area, GdkGLContext*) {
    int w = gtk_widget_get_allocated_width(GTK_WIDGET(area));
    int h = gtk_widget_get_allocated_height(GTK_WIDGET(area));
    renderer_draw(game_get_state(), w, h, g_rot_x, g_rot_y, g_zoom);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    if (argc > 1) g_models_dir = argv[1];

    std::printf("Loading models...\n");
    {
        std::thread threads[PIECE_COUNT];
        for (int i = 0; i < PIECE_COUNT; i++) {
            threads[i] = std::thread([i] {
                g_loaded_models[i].load(g_models_dir + "/" + piece_filenames[i]);
                std::printf("  %s: %zu triangles\n", piece_filenames[i], g_loaded_models[i].triangle_count());
            });
        }
        for (auto& t : threads) t.join();
    }
    std::printf("All models loaded.\n");

    game_init(g_models_dir);

    gtk_init(&argc, &argv);

    g_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(g_window), "Chess Board");
    gtk_window_set_default_size(GTK_WINDOW(g_window), 1024, 768);
    g_signal_connect(g_window, "destroy", G_CALLBACK(gtk_main_quit), nullptr);

    g_gl_area = gtk_gl_area_new();
    gtk_gl_area_set_required_version(GTK_GL_AREA(g_gl_area), 3, 3);
    gtk_gl_area_set_has_depth_buffer(GTK_GL_AREA(g_gl_area), TRUE);
    gtk_container_add(GTK_CONTAINER(g_window), g_gl_area);

    g_signal_connect(g_gl_area, "realize", G_CALLBACK(on_realize), nullptr);
    g_signal_connect(g_gl_area, "render", G_CALLBACK(on_render), nullptr);

    gtk_widget_add_events(g_gl_area,
        GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK);

    g_signal_connect(g_gl_area, "button-press-event", G_CALLBACK(on_button_press), g_gl_area);
    g_signal_connect(g_gl_area, "button-release-event", G_CALLBACK(on_button_release), g_gl_area);
    g_signal_connect(g_gl_area, "motion-notify-event", G_CALLBACK(on_motion), g_gl_area);
    g_signal_connect(g_gl_area, "scroll-event", G_CALLBACK(on_scroll), g_gl_area);
    g_signal_connect(g_window, "key-press-event", G_CALLBACK(on_key_press), nullptr);

    game_update_title(g_window);

    gtk_widget_show_all(g_window);
    gtk_main();

    return 0;
}
