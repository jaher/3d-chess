#pragma once

#include <string>

#include "chess_types.h"

// Per-position pawn-structure summary. Detects features of the
// pawn skeleton that matter for play and returns a single TTS-
// ready phrase, or "" if there's nothing structural to call out.
//
// Detection focuses on transitions worth announcing:
//   * Isolated queen pawn (IQP) — d-file pawn with no friendly
//     pawn on c or e.
//   * Passed pawn — pawn with no enemy pawn ahead on its file or
//     either adjacent file.
//   * Doubled pawns — two of one side's pawns on the same file.
//   * Backward pawn — pawn that can't safely advance and has no
//     friendly pawn on adjacent files at or behind it.
//   * Hanging pawns — two side-by-side pawns on adjacent files
//     with no friendly pawns on the files outside that pair.
//
// One-shot semantics — the caller dedups via
// pending_move_pawn_structure / last_announced_pawn_structure so
// the same label doesn't repeat while the structure persists.
std::string classify_pawn_structure(const GameState& gs);
