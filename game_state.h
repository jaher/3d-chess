#pragma once

#include "chess_types.h"

// Reset the chess position to the opening setup. Clears all per-game
// state (history, analysis mode, AI animation flags) and pushes the
// initial evaluation + snapshot.
void game_reset(GameState& gs);

// Enter analysis mode. Snapshots live state, positions the index at
// the latest move. No-op if an AI move is in flight.
void game_enter_analysis(GameState& gs);

// Exit analysis mode. Restores the live position.
void game_exit_analysis(GameState& gs);
