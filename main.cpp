#include <cstdio>
#include <string>
#include <thread>

#include <epoxy/gl.h>
#include <gtk/gtk.h>

#include "board_renderer.h"
#include "challenge.h"
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

// Menu state
static GameMode g_mode = MODE_MENU;
static std::vector<PhysicsPiece> g_menu_pieces;
static gint64 g_menu_start_time = 0;
static guint g_menu_tick_id = 0;
static gint64 g_menu_last_update = 0;
static int g_menu_hover = 0;

// Challenge state
static std::vector<std::string> g_challenge_files;
static std::vector<std::string> g_challenge_names;
static int g_challenge_select_hover = -1;
static Challenge g_current_challenge;
static int g_challenge_moves_made = 0;
static bool g_challenge_solved = false; // shows Next button
static bool g_challenge_next_hover = false;
// Solutions submitted by the user (one entry per puzzle: list of UCI moves)
static std::vector<std::vector<std::string>> g_challenge_solutions;
static bool g_challenge_show_summary = false;
static bool g_challenge_summary_hover = false;

// Glass shatter transition
static bool g_transition_active = false;
static int g_transition_pending_next = -1; // puzzle index to load when transitioning
static gint64 g_transition_start_time = 0;
static const float g_transition_duration = 1.3f;
static guint g_transition_tick_id = 0;

static void start_menu();
static void start_game();
static void start_challenge_select();
static void start_challenge(int index);
static void load_challenge_puzzle(int puzzle_index);
static void reset_challenge_puzzle();

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

    bool is_challenge = (g_mode == MODE_CHALLENGE);
    bool is_normal_game = (g_mode == MODE_PLAYING);

    if (is_normal_game) {
        if (gs.ai_thinking || gs.ai_animating || gs.analysis_mode || !gs.white_turn || gs.game_over)
            return;
    } else if (is_challenge) {
        if (gs.game_over || g_challenge_solved) return;
        int max_moves = g_current_challenge.max_moves;
        if (max_moves > 0) {
            bool starter_to_move = (gs.white_turn == g_current_challenge.starts_white);
            if (starter_to_move && g_challenge_moves_made >= max_moves) return;
        }
    }

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
                bool was_starter = (gs.white_turn == g_current_challenge.starts_white);
                execute_move(gs, gs.selected_col, gs.selected_row, col, row);
                gs.selected_col = gs.selected_row = -1;
                gs.valid_moves.clear();
                stop_animation();
                gtk_widget_queue_draw(widget);

                if (is_challenge) {
                    if (was_starter) g_challenge_moves_made++;
                    // Record the move in algebraic notation
                    if (!gs.move_history.empty() && gs.snapshots.size() >= 2) {
                        int pi = g_current_challenge.current_index;
                        if (pi >= 0 && pi < static_cast<int>(g_challenge_solutions.size())) {
                            const auto& before = gs.snapshots[gs.snapshots.size() - 2];
                            std::string alg = uci_to_algebraic(before, gs.move_history.back());
                            g_challenge_solutions[pi].push_back(alg);
                        }
                    }
                    // Check for solve: game_over with starting side as winner
                    if (gs.game_over) {
                        bool solved = false;
                        if (g_current_challenge.starts_white &&
                            gs.game_result.find("White wins") != std::string::npos)
                            solved = true;
                        if (!g_current_challenge.starts_white &&
                            gs.game_result.find("Black wins") != std::string::npos)
                            solved = true;

                        if (solved) {
                            g_challenge_solved = true;
                            std::printf("Puzzle solved!\n");
                        }
                    }
                    gtk_widget_queue_draw(widget);
                } else {
                    game_update_title(g_window);
                    if (!gs.white_turn && !gs.game_over)
                        game_trigger_ai(g_window, g_gl_area);
                }
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

    // Challenge mode: ESC = reset puzzle, M = back to menu
    if (g_mode == MODE_CHALLENGE) {
        if (event->keyval == GDK_KEY_Escape) {
            reset_challenge_puzzle();
            return TRUE;
        }
        if (event->keyval == GDK_KEY_m || event->keyval == GDK_KEY_M) {
            start_menu();
            return TRUE;
        }
        return TRUE;
    }

    // Challenge select: ESC = back to menu
    if (g_mode == MODE_CHALLENGE_SELECT) {
        if (event->keyval == GDK_KEY_Escape) {
            start_menu();
            return TRUE;
        }
        return TRUE;
    }

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
        int w = gtk_widget_get_allocated_width(GTK_WIDGET(gl_area));
        int h = gtk_widget_get_allocated_height(GTK_WIDGET(gl_area));

        if (g_mode == MODE_MENU) {
            int btn = menu_hit_test(event->x, event->y, w, h);
            if (btn == 1) start_game();
            else if (btn == 2) gtk_main_quit();
            else if (btn == 3) start_challenge_select();
        } else if (g_mode == MODE_CHALLENGE_SELECT) {
            int idx = challenge_select_hit_test(event->x, event->y, w, h,
                                                 static_cast<int>(g_challenge_names.size()));
            if (idx == -2) start_menu();
            else if (idx >= 0) start_challenge(idx);
        } else if (g_mode == MODE_CHALLENGE && g_challenge_show_summary) {
            // Click anywhere on summary returns to menu
            start_menu();
        } else if (g_mode == MODE_CHALLENGE && g_challenge_solved && !g_transition_active) {
            if (next_button_hit_test(event->x, event->y, w, h)) {
                int next = g_current_challenge.current_index + 1;
                if (next < static_cast<int>(g_current_challenge.fens.size())) {
                    // Defer load until next on_render so we can capture the frame first
                    g_transition_pending_next = next;
                } else {
                    g_challenge_show_summary = true;
                }
                gtk_widget_queue_draw(GTK_WIDGET(gl_area));
            }
        } else {
            double dx = event->x - g_press_x, dy = event->y - g_press_y;
            if (dx*dx + dy*dy < 25.0)
                handle_board_click(event->x, event->y, GTK_WIDGET(gl_area));
        }
    }
    return TRUE;
}

static gboolean on_motion(GtkWidget*, GdkEventMotion* event, gpointer gl_area) {
    int w = gtk_widget_get_allocated_width(GTK_WIDGET(gl_area));
    int h = gtk_widget_get_allocated_height(GTK_WIDGET(gl_area));

    if (g_mode == MODE_MENU) {
        g_menu_hover = menu_hit_test(event->x, event->y, w, h);
        gtk_widget_queue_draw(GTK_WIDGET(gl_area));
        return TRUE;
    }
    if (g_mode == MODE_CHALLENGE_SELECT) {
        g_challenge_select_hover = challenge_select_hit_test(
            event->x, event->y, w, h, static_cast<int>(g_challenge_names.size()));
        gtk_widget_queue_draw(GTK_WIDGET(gl_area));
        return TRUE;
    }
    if (g_mode == MODE_CHALLENGE && g_challenge_solved) {
        bool h_now = next_button_hit_test(event->x, event->y, w, h);
        if (h_now != g_challenge_next_hover) {
            g_challenge_next_hover = h_now;
            gtk_widget_queue_draw(GTK_WIDGET(gl_area));
        }
    }
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
// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
static gboolean on_menu_tick(GtkWidget* widget, GdkFrameClock*, gpointer) {
    gint64 now = g_get_monotonic_time();
    float dt = static_cast<float>(now - g_menu_last_update) / 1000000.0f;
    g_menu_last_update = now;
    if (dt > 0.05f) dt = 0.05f; // clamp for stability
    menu_update_physics(g_menu_pieces, dt);
    gtk_widget_queue_draw(widget);
    return G_SOURCE_CONTINUE;
}

static void start_menu() {
    g_mode = MODE_MENU;
    menu_init_physics(g_menu_pieces);
    g_menu_start_time = g_get_monotonic_time();
    g_menu_last_update = g_menu_start_time;
    if (g_menu_tick_id == 0)
        g_menu_tick_id = gtk_widget_add_tick_callback(g_gl_area, on_menu_tick, nullptr, nullptr);
    gtk_window_set_title(GTK_WINDOW(g_window), "3D Chess");
}

static void start_game() {
    g_mode = MODE_PLAYING;
    if (g_menu_tick_id != 0) {
        gtk_widget_remove_tick_callback(g_gl_area, g_menu_tick_id);
        g_menu_tick_id = 0;
    }
    game_update_title(g_window);
    gtk_widget_queue_draw(g_gl_area);
}

static void start_challenge_select() {
    g_mode = MODE_CHALLENGE_SELECT;
    if (g_menu_tick_id != 0) {
        gtk_widget_remove_tick_callback(g_gl_area, g_menu_tick_id);
        g_menu_tick_id = 0;
    }
    // Re-scan challenges directory
    g_challenge_files = list_challenge_files("challenges");
    g_challenge_names.clear();
    for (const auto& f : g_challenge_files) {
        Challenge c = load_challenge(f);
        g_challenge_names.push_back(c.name);
    }
    g_challenge_select_hover = -1;
    gtk_window_set_title(GTK_WINDOW(g_window), "Select Challenge");
    gtk_widget_queue_draw(g_gl_area);
}

static void load_challenge_puzzle(int puzzle_index) {
    if (puzzle_index < 0 || puzzle_index >= static_cast<int>(g_current_challenge.fens.size()))
        return;
    g_current_challenge.current_index = puzzle_index;
    g_challenge_moves_made = 0;
    g_challenge_solved = false;
    g_challenge_next_hover = false;
    ParsedFEN parsed = parse_fen(g_current_challenge.fens[puzzle_index]);
    if (parsed.valid)
        apply_fen_to_state(game_get_state(), parsed);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Challenge: %s [%d/%d]",
                  g_current_challenge.name.c_str(),
                  puzzle_index + 1,
                  static_cast<int>(g_current_challenge.fens.size()));
    gtk_window_set_title(GTK_WINDOW(g_window), buf);
}

static gboolean on_transition_tick(GtkWidget* widget, GdkFrameClock*, gpointer) {
    if (!g_transition_active) return G_SOURCE_REMOVE;
    gtk_widget_queue_draw(widget);
    return G_SOURCE_CONTINUE;
}

static void reset_challenge_puzzle() {
    int idx = g_current_challenge.current_index;
    if (idx >= 0 && idx < static_cast<int>(g_challenge_solutions.size()))
        g_challenge_solutions[idx].clear();
    load_challenge_puzzle(idx);
    gtk_widget_queue_draw(g_gl_area);
}

static void start_challenge(int index) {
    if (index < 0 || index >= static_cast<int>(g_challenge_files.size())) return;
    g_current_challenge = load_challenge(g_challenge_files[index]);
    if (g_current_challenge.fens.empty()) return;
    g_mode = MODE_CHALLENGE;
    g_challenge_solutions.assign(g_current_challenge.fens.size(), {});
    g_challenge_show_summary = false;
    load_challenge_puzzle(0);
    gtk_widget_queue_draw(g_gl_area);
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

    if (g_mode == MODE_MENU) {
        float t = static_cast<float>(g_get_monotonic_time() - g_menu_start_time) / 1000000.0f;
        renderer_draw_menu(g_menu_pieces, w, h, t, g_menu_hover);
    } else if (g_mode == MODE_CHALLENGE_SELECT) {
        renderer_draw_challenge_select(g_challenge_names, w, h, g_challenge_select_hover);
    } else if (g_mode == MODE_CHALLENGE && g_challenge_show_summary) {
        // Build summary entries
        std::vector<SummaryEntry> entries;
        for (size_t i = 0; i < g_challenge_solutions.size(); i++) {
            SummaryEntry e;
            char buf[16]; std::snprintf(buf, sizeof(buf), "Puzzle %zu", i + 1);
            e.puzzle_name = buf;
            e.moves = g_challenge_solutions[i];
            entries.push_back(e);
        }
        renderer_draw_challenge_summary(g_current_challenge.name, entries, w, h);
    } else {
        // Suppress the regular "wins by checkmate" overlay during challenge mode —
        // we show our own solved indicator + summary
        bool save_game_over = false;
        std::string save_result;
        auto& gs_render = game_get_state();
        if (g_mode == MODE_CHALLENGE) {
            save_game_over = gs_render.game_over;
            save_result = gs_render.game_result;
            gs_render.game_over = false;
            gs_render.game_result.clear();
        }

        renderer_draw(gs_render, w, h, g_rot_x, g_rot_y, g_zoom);

        if (g_mode == MODE_CHALLENGE) {
            // Restore
            gs_render.game_over = save_game_over;
            gs_render.game_result = save_result;

            renderer_draw_challenge_overlay(
                g_current_challenge.name,
                g_current_challenge.current_index,
                static_cast<int>(g_current_challenge.fens.size()),
                g_challenge_moves_made,
                g_current_challenge.max_moves,
                g_current_challenge.starts_white,
                w, h);
            if (g_challenge_solved && !g_transition_active && g_transition_pending_next < 0)
                renderer_draw_next_button(w, h, g_challenge_next_hover);

            // Start transition: capture this frame, load next puzzle, then redraw
            if (g_transition_pending_next >= 0) {
                renderer_capture_frame(w, h);
                load_challenge_puzzle(g_transition_pending_next);
                g_transition_pending_next = -1;
                g_transition_active = true;
                g_transition_start_time = g_get_monotonic_time();
                if (g_transition_tick_id == 0)
                    g_transition_tick_id = gtk_widget_add_tick_callback(
                        g_gl_area, on_transition_tick, nullptr, nullptr);

                // Redraw with the new puzzle state
                glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
                renderer_draw(gs_render, w, h, g_rot_x, g_rot_y, g_zoom);
                renderer_draw_challenge_overlay(
                    g_current_challenge.name,
                    g_current_challenge.current_index,
                    static_cast<int>(g_current_challenge.fens.size()),
                    g_challenge_moves_made,
                    g_current_challenge.max_moves,
                    g_current_challenge.starts_white,
                    w, h);
            }

            // Draw the shattering overlay
            if (g_transition_active) {
                float t = static_cast<float>(g_get_monotonic_time() - g_transition_start_time) / 1000000.0f;
                if (t >= g_transition_duration) {
                    g_transition_active = false;
                    if (g_transition_tick_id != 0) {
                        gtk_widget_remove_tick_callback(g_gl_area, g_transition_tick_id);
                        g_transition_tick_id = 0;
                    }
                } else {
                    renderer_draw_shatter(t, w, h);
                }
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

    gtk_widget_show_all(g_window);

    // Start in menu mode
    start_menu();

    gtk_main();

    return 0;
}
