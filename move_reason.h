#pragma once

#include <string>

#include "chess_types.h"   // GameState, MoveClass, PieceType

// Generate a one-line, board-grounded explanation of why a flagged move
// was a concession — the bottom line of the "why?" panel.
//
// `gs` is the position AFTER the move (white_turn has already flipped,
// so the side that just moved is `!gs.white_turn`). The concrete case we
// detect cheaply is a hung piece — a friendly piece the opponent now
// attacks that nothing defends; that reads well and is the most common
// reason. Otherwise we fall back to a class-based line. Pure board
// reasoning (uses is_square_attacked only) — no engine call, no GL, so
// it's unit-testable in isolation.
std::string generate_why_reason(const GameState& gs, MoveClass cls);
