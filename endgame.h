#pragma once

#include <string>

#include "chess_types.h"

// Cheap, deterministic classifier that names the endgame configuration
// the position has reduced to (or returns "" if we're still in the
// middlegame). The output is a short phrase suitable for TTS, e.g.
// "King and pawn endgame", "Rook endgame", "Opposite-coloured
// bishops endgame", "Queen versus lone king endgame".
//
// Detection is purely material-based — it counts pieces by type per
// side and matches the most-specific configuration, falling back to
// a generic "Endgame" label when material is low but the layout
// doesn't fit a recognised category.
std::string classify_endgame(const GameState& gs);
