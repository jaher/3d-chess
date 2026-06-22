#pragma once

#include <string>

// Lightweight learner model for the Practice screen. Every tactic the
// player solves or misses (mate / fork / pin) is folded into per-category
// solved/missed tallies plus an Elo-flavoured "tactics rating", so the
// trainer can surface a personal rating and point out the weakest area.
// Pure + unit-tested (learner_profile_test.cpp); the app owns one of
// these on AppState and persists it to the settings INI.

enum class TacticCategory { Mate, Fork, Pin, Count };

struct CategoryStat {
    int solved = 0;
    int missed = 0;
};

// Starting tactics rating for a fresh profile.
constexpr int LEARNER_START_RATING = 1000;

struct LearnerProfile {
    CategoryStat stats[static_cast<int>(TacticCategory::Count)];
    int rating = LEARNER_START_RATING;
};

// Map a challenge `type:` string ("mate_in_2", "find_forks", "find_pins",
// …) to a category. Returns Count for unknown / non-tactic types.
TacticCategory category_for_type(const std::string& type);

// Record one attempt in `cat` and nudge the rating (fixed Elo-style
// steps: a solve raises it, a miss lowers it, clamped to a floor).
// No-op when cat == Count.
void learner_record(LearnerProfile& p, TacticCategory cat, bool solved);

// Total attempts (solved + missed) across every category.
int learner_total_attempts(const LearnerProfile& p);

// Accuracy in [0,1] for a category, or -1.0 if it has no attempts yet.
double learner_accuracy(const LearnerProfile& p, TacticCategory cat);

// The category the learner is weakest at — lowest accuracy among
// categories with at least `min_attempts` attempts (ties broken by the
// earlier enum order). Returns Count when none qualifies.
TacticCategory learner_weakest(const LearnerProfile& p, int min_attempts = 3);

// Human-readable plural label ("Mates" / "Forks" / "Pins"); "" for Count.
const char* category_label(TacticCategory cat);
