#pragma once

#include "chess_types.h"
#include <string>

// GTK forward declarations
typedef struct _GtkWidget GtkWidget;

// Game state — singleton, owns the global mutable state
void game_init(const std::string& models_dir);

// Reset the game state back to the initial position (clears all history, etc.)
void game_reset();

GameState& game_get_state();

// Title bar updates
void game_update_title(GtkWidget* window);
void game_update_analysis_title(GtkWidget* window);

// Analysis mode
void game_enter_analysis(GtkWidget* gl_area);
void game_exit_analysis();

// AI integration
void game_trigger_ai(GtkWidget* window, GtkWidget* gl_area);

// Kick off an async Stockfish eval of the current position. When the result
// arrives it overwrites gs.score_history[move_index] and redraws gl_area.
// No-op if move_index is no longer the last score_history entry by the time
// the result comes back (e.g. after a game reset).
void game_trigger_eval(int move_index, GtkWidget* gl_area);
