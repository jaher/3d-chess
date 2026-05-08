#pragma once

#include <string>

#include "chess_types.h"

// Cheap tactical-motif detector that runs *after* a move has been
// committed and looks at what just happened on the board:
//
//   * castling (kingside / queenside),
//   * en passant capture,
//   * pawn promotion (already handled by the move-text generator,
//     but called out here as well for the speech path),
//   * check / double check / discovered check,
//   * fork — the moved piece simultaneously attacks two or more
//     enemy pieces of higher (or equal-and-undefended) value,
//   * pin — the moved piece pins an enemy piece against a more
//     valuable piece behind it (absolute pin against the king is
//     the high-value case),
//   * skewer — the inverse: the moved piece attacks a high-value
//     enemy piece that, when it moves, exposes a less valuable
//     piece behind it.
//
// Returns the most-distinctive applicable label, or "" if no
// tactic was detected. Cheap heuristics — no engine call, just a
// few square-attack queries against the post-move position.
//
// `prev` is the snapshot taken *before* the move was executed
// (snapshots[size-2] in the live game). The current GameState is
// the post-move position.
std::string classify_tactic(const GameState& post,
                            const BoardSnapshot& prev,
                            const std::string& move_uci);
