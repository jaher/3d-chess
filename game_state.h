#pragma once

#include "chess_types.h"
#include <string>

// GTK forward declarations
typedef struct _GtkWidget GtkWidget;

// Game state — singleton, owns the global mutable state
void game_init(const std::string& models_dir);

GameState& game_get_state();

// Title bar updates
void game_update_title(GtkWidget* window);
void game_update_analysis_title(GtkWidget* window);

// Analysis mode
void game_enter_analysis(GtkWidget* gl_area);
void game_exit_analysis();

// AI integration
void game_trigger_ai(GtkWidget* window, GtkWidget* gl_area);
