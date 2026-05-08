#pragma once

#include <string>

// Opening database lookup. Loads `openings/openings.tsv` (built from
// the lichess chess-openings repo via tools/build_openings.py) on the
// first call to opening_name_for_position(); subsequent calls hit a
// hashmap keyed on the four-field FEN.
//
// The DB stores positions as `<board> <turn> <castling> <ep>` so
// transpositions land on the same row regardless of move count.
// Pass either a four-field FEN or the longer six-field form — the
// lookup helper trims the halfmove / fullmove suffix internally.

// Returns the most-specific opening name matching this position, or
// the empty string if the position isn't in the database. Safe to
// call before openings_init(); first call populates the cache.
std::string opening_name_for_position(const std::string& fen);

// Optional: pre-load the database. Otherwise the first
// opening_name_for_position() call does it lazily. No-op if already
// loaded.
void openings_init();
