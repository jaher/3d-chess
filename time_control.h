#pragma once

// ===========================================================================
// Chess time controls
// ===========================================================================
//
// A single lookup table that defines the four standard formats plus an
// "Unlimited" sentinel for untimed play. Used by the pregame dropdown,
// the in-game clock widget, and the AI move-time heuristic.
//
// Order matters: the enum order also drives the dropdown order shown
// to the user, which is why Classical sits at index 0 (the top row,
// the natural default for a casual player) and Unlimited is last.

#include <cstdint>

enum TimeControl {
    TC_CLASSICAL = 0, // 30 min + 30 s
    TC_RAPID,         // 15 min + 10 s
    TC_BLITZ,         //  5 min +  3 s
    TC_BULLET,        //  1 min +  1 s
    TC_UNLIMITED,     // no clock
    TC_COUNT
};

struct TimeControlSpec {
    const char* short_name;   // "Classical", "Unlimited", etc.
    const char* display;      // "30+30" (or "--" for unlimited)
    int64_t     base_ms;      // Starting budget per side. 0 == unlimited.
    int64_t     increment_ms; // Added per completed move. 0 for unlimited.
};

extern const TimeControlSpec TIME_CONTROLS[TC_COUNT];
