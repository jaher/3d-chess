#include "doctest.h"

#include "../learner_profile.h"

TEST_CASE("category_for_type maps challenge types to categories") {
    CHECK(category_for_type("mate_in_1") == TacticCategory::Mate);
    CHECK(category_for_type("mate_in_2") == TacticCategory::Mate);
    CHECK(category_for_type("mate_in_3") == TacticCategory::Mate);
    CHECK(category_for_type("find_forks") == TacticCategory::Fork);
    CHECK(category_for_type("find_pins") == TacticCategory::Pin);
    CHECK(category_for_type("") == TacticCategory::Count);
    CHECK(category_for_type("nonsense") == TacticCategory::Count);
}

TEST_CASE("learner_record tallies and moves the rating") {
    LearnerProfile p;
    CHECK(p.rating == LEARNER_START_RATING);
    CHECK(learner_total_attempts(p) == 0);

    learner_record(p, TacticCategory::Mate, true);
    learner_record(p, TacticCategory::Mate, true);
    learner_record(p, TacticCategory::Mate, false);

    CHECK(p.stats[static_cast<int>(TacticCategory::Mate)].solved == 2);
    CHECK(p.stats[static_cast<int>(TacticCategory::Mate)].missed == 1);
    CHECK(learner_total_attempts(p) == 3);
    // +20 +20 -12 = +28
    CHECK(p.rating == LEARNER_START_RATING + 28);
}

TEST_CASE("learner_record on Count is a no-op") {
    LearnerProfile p;
    learner_record(p, TacticCategory::Count, true);
    learner_record(p, TacticCategory::Count, false);
    CHECK(p.rating == LEARNER_START_RATING);
    CHECK(learner_total_attempts(p) == 0);
}

TEST_CASE("rating has a floor") {
    LearnerProfile p;
    for (int i = 0; i < 500; ++i)
        learner_record(p, TacticCategory::Pin, false);
    CHECK(p.rating == 100);
}

TEST_CASE("learner_accuracy reflects solved/attempts, -1 when untried") {
    LearnerProfile p;
    CHECK(learner_accuracy(p, TacticCategory::Fork) == doctest::Approx(-1.0));
    learner_record(p, TacticCategory::Fork, true);
    learner_record(p, TacticCategory::Fork, true);
    learner_record(p, TacticCategory::Fork, false);
    learner_record(p, TacticCategory::Fork, false);
    CHECK(learner_accuracy(p, TacticCategory::Fork) == doctest::Approx(0.5));
}

TEST_CASE("learner_weakest needs min_attempts and picks the lowest accuracy") {
    LearnerProfile p;
    // Mates: 3/3 = 100%. Forks: 1/4 = 25%. Pins: only 2 attempts (below
    // the default threshold of 3) so it should be ignored even though
    // its accuracy is 0.
    for (int i = 0; i < 3; ++i) learner_record(p, TacticCategory::Mate, true);
    learner_record(p, TacticCategory::Fork, true);
    for (int i = 0; i < 3; ++i) learner_record(p, TacticCategory::Fork, false);
    learner_record(p, TacticCategory::Pin, false);
    learner_record(p, TacticCategory::Pin, false);

    CHECK(learner_weakest(p) == TacticCategory::Fork);
    // Lower the threshold and the 0%-but-thin Pins category wins.
    CHECK(learner_weakest(p, /*min_attempts=*/2) == TacticCategory::Pin);
}

TEST_CASE("learner_weakest returns Count when nothing qualifies") {
    LearnerProfile p;
    learner_record(p, TacticCategory::Mate, true);  // only 1 attempt
    CHECK(learner_weakest(p) == TacticCategory::Count);
}

TEST_CASE("category_label is stable") {
    CHECK(std::string(category_label(TacticCategory::Mate)) == "Mates");
    CHECK(std::string(category_label(TacticCategory::Fork)) == "Forks");
    CHECK(std::string(category_label(TacticCategory::Pin)) == "Pins");
    CHECK(std::string(category_label(TacticCategory::Count)).empty());
}
