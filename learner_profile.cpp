#include "learner_profile.h"

namespace {
// Elo-flavoured steps. A solve is worth a little more than a miss costs,
// so a player who solves more than they miss drifts upward. The floor
// keeps the number from going embarrassingly low after a rough streak.
constexpr int RATING_SOLVE_STEP = 20;
constexpr int RATING_MISS_STEP  = 12;
constexpr int RATING_FLOOR      = 100;

int cat_index(TacticCategory cat) { return static_cast<int>(cat); }
}  // namespace

TacticCategory category_for_type(const std::string& type) {
    if (type == "find_forks") return TacticCategory::Fork;
    if (type == "find_pins")  return TacticCategory::Pin;
    // mate_in_1 / mate_in_2 / mate_in_3 (and any future mate_in_N).
    if (type.rfind("mate_in_", 0) == 0) return TacticCategory::Mate;
    return TacticCategory::Count;
}

void learner_record(LearnerProfile& p, TacticCategory cat, bool solved) {
    if (cat == TacticCategory::Count) return;
    CategoryStat& s = p.stats[cat_index(cat)];
    if (solved) {
        s.solved++;
        p.rating += RATING_SOLVE_STEP;
    } else {
        s.missed++;
        p.rating -= RATING_MISS_STEP;
        if (p.rating < RATING_FLOOR) p.rating = RATING_FLOOR;
    }
}

int learner_total_attempts(const LearnerProfile& p) {
    int n = 0;
    for (int i = 0; i < static_cast<int>(TacticCategory::Count); ++i)
        n += p.stats[i].solved + p.stats[i].missed;
    return n;
}

double learner_accuracy(const LearnerProfile& p, TacticCategory cat) {
    if (cat == TacticCategory::Count) return -1.0;
    const CategoryStat& s = p.stats[cat_index(cat)];
    int attempts = s.solved + s.missed;
    if (attempts == 0) return -1.0;
    return static_cast<double>(s.solved) / static_cast<double>(attempts);
}

TacticCategory learner_weakest(const LearnerProfile& p, int min_attempts) {
    TacticCategory weakest = TacticCategory::Count;
    double worst = 2.0;  // any real accuracy (<= 1.0) beats this
    for (int i = 0; i < static_cast<int>(TacticCategory::Count); ++i) {
        const CategoryStat& s = p.stats[i];
        int attempts = s.solved + s.missed;
        if (attempts < min_attempts) continue;
        double acc = static_cast<double>(s.solved) /
                     static_cast<double>(attempts);
        if (acc < worst) {            // strict: earlier enum wins ties
            worst = acc;
            weakest = static_cast<TacticCategory>(i);
        }
    }
    return weakest;
}

const char* category_label(TacticCategory cat) {
    switch (cat) {
        case TacticCategory::Mate: return "Mates";
        case TacticCategory::Fork: return "Forks";
        case TacticCategory::Pin:  return "Pins";
        default:                   return "";
    }
}
