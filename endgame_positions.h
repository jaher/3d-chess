#pragma once

// Starting positions for the endgame trainer — the "Starting position"
// selector on the pregame setup screen. Index 0 is the normal game from
// the standard array; indices 1.. are textbook-won king-and-X-vs-king
// endgames that the human plays as **White** (the winning side, to move)
// against the defending engine. Because the trainer reuses the ordinary
// MODE_PLAYING flow, the normal game-over screen reports the result:
// checkmate = you converted it, a draw = the technique slipped.
//
// KQ-vs-K and KR-vs-K are wins against any defence. The KP-vs-K position
// has White's king on the sixth rank ahead of a non-rook pawn, which is
// a theoretical win (the opposition lesson). All entries are verified
// legal + correct-material + White-to-move by tests/endgame_trainer_test.cpp.

struct EndgameStart {
    const char* short_label;  // compact glyph for the pregame button ("KQ")
    const char* name;         // status-bar / spoken name
    const char* fen;          // empty string for the standard game
};

inline const EndgameStart ENDGAME_STARTS[] = {
    {"Std", "Standard game",          ""},
    {"KQ",  "King + Queen vs King",   "4k3/8/8/8/3Q4/8/8/4K3 w - - 0 1"},
    {"KR",  "King + Rook vs King",    "4k3/8/8/8/8/8/8/R3K3 w - - 0 1"},
    {"KP",  "King + Pawn vs King",    "4k3/8/4K3/4P3/8/8/8/8 w - - 0 1"},
};
inline constexpr int ENDGAME_START_COUNT =
    static_cast<int>(sizeof(ENDGAME_STARTS) / sizeof(ENDGAME_STARTS[0]));
