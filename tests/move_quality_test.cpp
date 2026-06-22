// Unit tests for the pure move-quality classifier (move_quality.cpp).
//
// The classifier keys off win-probability swing (lichess logistic), so
// the eval pairs below are chosen to land in each band. evals are in
// pawn units from WHITE's perspective — exactly what
// GameState::score_history stores. Unless a case says otherwise, white
// is the side that just moved, so player-POV == white-POV.

#include "doctest.h"

#include "../move_quality.h"

#include <string>

namespace {

// Build an input with neutral defaults; tests override the fields that
// matter for the case. Defaults: white just moved, middlegame (not
// book), no MultiPV #2, didn't match the engine's best, nothing en
// prise.
MoveQualityInput base() {
    MoveQualityInput in;
    in.white_just_moved     = true;
    in.played_best          = false;
    in.moved_piece_en_prise = false;
    in.in_opening           = false;
    in.have_second_pv       = false;
    in.prev_best_cp         = 0;
    in.prev_second_cp       = 0;
    return in;
}

}  // namespace

TEST_CASE("book move: small loss inside the opening window") {
    MoveQualityInput in = base();
    in.in_opening  = true;
    in.eval_before = 0.20f;   // 20cp -> 15cp, ~no swing
    in.eval_after  = 0.15f;
    CHECK(classify_move_quality(in).cls == MoveClass::Book);
}

TEST_CASE("best move: matched the engine, essentially no swing") {
    MoveQualityInput in = base();
    in.played_best = true;
    in.eval_before = 0.50f;
    in.eval_after  = 0.50f;
    auto r = classify_move_quality(in);
    CHECK(r.cls == MoveClass::Best);
    CHECK(r.cp_loss == 0);
}

TEST_CASE("excellent: tiny win-% concession, not the top move") {
    MoveQualityInput in = base();
    in.eval_before = 0.50f;   // ~1.4% wp lost -> Excellent band (<=2%)
    in.eval_after  = 0.35f;
    CHECK(classify_move_quality(in).cls == MoveClass::Excellent);
}

TEST_CASE("good: a few percent lost") {
    MoveQualityInput in = base();
    in.eval_before = 0.80f;   // ~3.6% wp lost -> Good band (2-5%)
    in.eval_after  = 0.40f;
    CHECK(classify_move_quality(in).cls == MoveClass::Good);
}

TEST_CASE("inaccuracy: 5-10% band, still a visible badge") {
    MoveQualityInput in = base();
    in.eval_before = 0.90f;   // ~7% wp lost, both endpoints "equal"
    in.eval_after  = 0.10f;
    CHECK(classify_move_quality(in).cls == MoveClass::Inaccuracy);
}

TEST_CASE("mistake: 10-20% band") {
    MoveQualityInput in = base();
    in.eval_before =  1.00f;  // ~13.7% wp lost
    in.eval_after  = -0.50f;
    CHECK(classify_move_quality(in).cls == MoveClass::Mistake);
}

TEST_CASE("blunder: >20% band, not from a winning position") {
    MoveQualityInput in = base();
    in.eval_before =  1.00f;  // equal -> losing, ~34% wp lost
    in.eval_after  = -3.00f;
    CHECK(classify_move_quality(in).cls == MoveClass::Blunder);
}

TEST_CASE("missed win: threw away a winning advantage") {
    MoveQualityInput in = base();
    in.eval_before = 2.00f;   // winning (cat +1) ...
    in.eval_after  = 0.00f;   // ... down to equal, cp_loss 200 > 80
    CHECK(classify_move_quality(in).cls == MoveClass::MissedWin);
}

TEST_CASE("great via only-move: matched a forced best move") {
    MoveQualityInput in = base();
    in.played_best     = true;
    in.have_second_pv  = true;
    in.prev_best_cp    = 200;  // #1 vs #2 gap ~17.6% wp (>= 8)
    in.prev_second_cp  = 0;
    in.eval_before     = 2.00f;
    in.eval_after      = 2.00f;
    auto r = classify_move_quality(in);
    CHECK(r.only_move);
    CHECK(r.cls == MoveClass::Great);
}

TEST_CASE("great via category swing: best move that flips equal->winning") {
    MoveQualityInput in = base();
    in.played_best = true;
    in.eval_before = 0.50f;   // equal ...
    in.eval_after  = 2.00f;   // ... to winning (cat_delta +1)
    CHECK(classify_move_quality(in).cls == MoveClass::Great);
}

TEST_CASE("brilliant: top move that hangs a minor-or-better piece") {
    MoveQualityInput in = base();
    in.played_best          = true;
    in.moved_piece_en_prise = true;
    in.eval_before          = 0.50f;
    in.eval_after           = 0.50f;   // still at least equal afterwards
    CHECK(classify_move_quality(in).cls == MoveClass::Brilliant);

    // Same position but the piece is NOT en prise -> not brilliant;
    // matching the engine with no swing is just Best.
    in.moved_piece_en_prise = false;
    CHECK(classify_move_quality(in).cls == MoveClass::Best);
}

TEST_CASE("miss: had a clearly better forced move and skipped it") {
    MoveQualityInput in = base();
    in.played_best    = false;
    in.have_second_pv = true;
    in.prev_best_cp   = 300;   // big #1 vs #2 gap -> only-move existed
    in.prev_second_cp = 0;
    in.eval_before    = 1.00f; // and the played move lost ~13.7% wp
    in.eval_after     = -0.50f;
    CHECK(classify_move_quality(in).cls == MoveClass::Miss);
}

TEST_CASE("mate-search noise: deep-mate churn on one side is unbadged") {
    MoveQualityInput in = base();
    in.eval_before = 80.0f;   // both endpoints deep in mate territory,
    in.eval_after  = 60.0f;   // same side -> cp delta is distance churn
    CHECK(classify_move_quality(in).cls == MoveClass::None);
}

TEST_CASE("symmetry: a black blunder classifies from black's POV") {
    // White-POV eval swings from slightly-good-for-black to
    // winning-for-white after BLACK moves: that's black's blunder.
    MoveQualityInput in = base();
    in.white_just_moved = false;
    in.eval_before      = -1.00f;  // equal-ish for black (player cat 0)
    in.eval_after       =  3.00f;  // white now winning
    auto r = classify_move_quality(in);
    CHECK(r.cls == MoveClass::Blunder);
    CHECK(r.cp_loss == 400);

    // The mirror-image white move (same magnitudes, white to move)
    // is just as bad — the classifier is sign-symmetric.
    MoveQualityInput w = base();
    w.white_just_moved = true;
    w.eval_before      =  1.00f;
    w.eval_after       = -3.00f;
    CHECK(classify_move_quality(w).cls == MoveClass::Blunder);
}
