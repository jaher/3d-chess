#pragma once

#include <string>

// Looks up a one-sentence "typical plan" for the given opening
// name. Lichess's opening DB names are long ("Sicilian Defense:
// Najdorf Variation, English Attack") and contain a comma — we
// match prefix-style on the parent opening's family so all the
// variations in a family share the same plan unless we have a
// more specific one. Returns "" when nothing matches; caller
// dedups so the plan only fires once per game.
//
// Pure function — no state, just a static table. Add new entries
// at the top of opening_plans.cpp.
std::string opening_plan_for(const std::string& opening_name);
