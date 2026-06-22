#include "move_quality.h"

#include <cmath>
#include <cstdlib>

namespace {

// Centipawn → win-percentage logistic (chess.com / lichess style).
// Coefficient 0.00368 matches lichess's published model. Clamped to
// [0, 100].
float cp_to_wp(int cp) {
    float k = -0.00368f * static_cast<float>(cp);
    float w = 50.0f + 50.0f * (2.0f / (1.0f + std::exp(k)) - 1.0f);
    if (w < 0.0f)   return 0.0f;
    if (w > 100.0f) return 100.0f;
    return w;
}

// Bucket a player-relative eval (pawn units) into losing / equal /
// winning, on the standard ±1 pawn convention.
int categorize(float pawn_units) {
    if (pawn_units >  1.0f) return  1;
    if (pawn_units < -1.0f) return -1;
    return 0;
}

}  // namespace

MoveQualityResult classify_move_quality(const MoveQualityInput& in) {
    MoveQualityResult r;
    const bool white_just_moved = in.white_just_moved;

    // Convert pawn units → centipawns; loss is from the moving side's
    // perspective (you can't help yourself by moving, so clamp >= 0).
    int cp_before = static_cast<int>(in.eval_before * 100.0f);
    int cp_after  = static_cast<int>(in.eval_after  * 100.0f);
    int cp_loss = white_just_moved ? (cp_before - cp_after)
                                   : (cp_after - cp_before);
    if (cp_loss < 0) cp_loss = 0;
    r.cp_loss = cp_loss;

    // Win-percentage swing for the side that moved (the real signal we
    // bucket on).
    int cp_pl_before = white_just_moved ? cp_before : -cp_before;
    int cp_pl_after  = white_just_moved ? cp_after  : -cp_after;
    float wp_loss = cp_to_wp(cp_pl_before) - cp_to_wp(cp_pl_after);
    if (wp_loss < 0.0f) wp_loss = 0.0f;
    r.wp_loss = wp_loss;

    // Player-relative category swing — what Brilliant / Great /
    // Missed-Win hinge on.
    float pe_before = white_just_moved ? in.eval_before : -in.eval_before;
    float pe_after  = white_just_moved ? in.eval_after  : -in.eval_after;
    int cat_before = categorize(pe_before);
    int cat_after  = categorize(pe_after);
    int cat_delta  = cat_after - cat_before;
    r.cat_before = cat_before;
    r.cat_after  = cat_after;

    // Missed Win: was winning, now equal or worse, with a real drop.
    bool missed_win = (cat_before == 1) && (cat_after < 1) && (cp_loss > 80);

    // Mate-search noise: both endpoints deep in mate territory on the
    // SAME side → the cp delta is mate-distance churn, not move quality.
    // (Sign flips in mate territory still classify — that's a real
    // blunder.)
    bool both_mate_same_side =
        (in.eval_before * in.eval_after > 0.0f) &&
        (std::abs(in.eval_before) >= 50.0f) &&
        (std::abs(in.eval_after)  >= 50.0f);

    // "Only-move" detection from the pre-move MultiPV=2 split: a large
    // win-% gap between the engine's #1 and #2 means the best move was
    // forced.
    float pv_gap_wp = 0.0f;
    if (in.have_second_pv) {
        int best_pl   = white_just_moved ? in.prev_best_cp   : -in.prev_best_cp;
        int second_pl = white_just_moved ? in.prev_second_cp : -in.prev_second_cp;
        pv_gap_wp = cp_to_wp(best_pl) - cp_to_wp(second_pl);
        if (pv_gap_wp < 0.0f) pv_gap_wp = 0.0f;
    }
    const bool only_move = in.have_second_pv && pv_gap_wp >= 8.0f;
    r.pv_gap_wp = pv_gap_wp;
    r.only_move = only_move;

    // Brilliant: the engine still rated the move #1 even though it put a
    // minor-or-better piece en prise, past the opening book, with the
    // position still at least equal afterwards.
    bool brilliant = in.played_best && cat_after >= 0 && !in.in_opening &&
                     !both_mate_same_side && in.moved_piece_en_prise;
    r.brilliant = brilliant;

    // Pick the most-distinctive applicable label. Order matters: Book
    // first so opening moves don't get branded "Best"; Brilliant /
    // Great / Missed-Win next because they describe *what changed*; ΔWP
    // buckets last as routine commentary. The 5-10% band is Inaccuracy
    // (visible badge, but the voice coach stays silent on it).
    MoveClass cls;
    if (in.in_opening && cp_loss <= 20)         cls = MoveClass::Book;
    else if (brilliant)                         cls = MoveClass::Brilliant;
    else if (in.played_best && only_move)       cls = MoveClass::Great;
    else if (in.played_best && cat_delta >= 1)  cls = MoveClass::Great;
    else if (missed_win)                        cls = MoveClass::MissedWin;
    else if (!in.played_best && only_move &&
             wp_loss >= 5.0f)                   cls = MoveClass::Miss;
    else if (in.played_best && wp_loss <= 1.0f) cls = MoveClass::Best;
    else if (wp_loss <= 2.0f)                   cls = MoveClass::Excellent;
    else if (wp_loss <= 5.0f)                   cls = MoveClass::Good;
    else if (wp_loss <= 10.0f)                  cls = MoveClass::Inaccuracy;
    else if (wp_loss <= 20.0f)                  cls = MoveClass::Mistake;
    else                                        cls = MoveClass::Blunder;
    if (both_mate_same_side) cls = MoveClass::None;
    r.cls = cls;
    return r;
}
