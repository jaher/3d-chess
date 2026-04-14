#include "time_control.h"

// Base budgets use the canonical FIDE / online-chess values for each
// format: 30+30 classical, 15+10 rapid, 5+3 blitz, 1+1 bullet.
// Unlimited intentionally leaves base_ms and increment_ms at zero so
// the clock tick loop in app_state.cpp treats it as "never tick".
const TimeControlSpec TIME_CONTROLS[TC_COUNT] = {
    { "Classical", "30+30", 30 * 60 * 1000, 30 * 1000 },
    { "Rapid",     "15+10", 15 * 60 * 1000, 10 * 1000 },
    { "Blitz",     "5+3",    5 * 60 * 1000,  3 * 1000 },
    { "Bullet",    "1+1",    1 * 60 * 1000,  1 * 1000 },
    { "Unlimited", "--",     0,              0        },
};
