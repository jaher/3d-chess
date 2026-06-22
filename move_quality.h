#pragma once

#include "chess_types.h"   // MoveClass

// Pure, engine-eval-driven move-quality classifier.
//
// This is the numeric core of the move annotation that app_eval_ready()
// surfaces (the move-list NAG badges and the spoken coach). Every
// board-aware decision — does the move match the engine's first choice,
// did the moved piece land en prise — is resolved by the caller and
// passed in as a plain bool, so this unit has NO dependency on AppState,
// GameState, or OpenGL and can be unit-tested in isolation. The only
// type it touches is MoveClass (from chess_types.h).
//
// Band boundaries and gate ordering match chess.com / lichess
// conventions: classification keys off win-probability swing (via the
// lichess logistic) rather than raw centipawns, because the same 100cp
// loss matters far more at +0.5 than at +8.

struct MoveQualityInput {
    // Position eval before / after the move, in pawn units, from
    // WHITE's perspective (positive = good for white) — exactly the
    // values stored in GameState::score_history. Mate scores arrive
    // already collapsed to a bounded ~±100 spike by app_eval_ready.
    float eval_before = 0.0f;
    float eval_after  = 0.0f;

    // True when the side that just moved is white (so after the move it
    // is black to play). Drives the player-relative sign flips.
    bool white_just_moved = true;

    // The move played equals the engine's pre-move first choice.
    bool played_best = false;

    // The just-moved piece is a minor or better (value >= 3) and landed
    // on a square the opponent attacks — the cheap "sacrifice" proxy
    // that, combined with played_best, gates a Brilliant. The caller
    // computes this from the board.
    bool moved_piece_en_prise = false;

    // Within the opening-book heuristic window (first 16 plies).
    bool in_opening = false;

    // A MultiPV #2 line was available for the pre-move position (with
    // its eval), letting us measure how forced the best move was.
    bool have_second_pv = false;

    // Pre-move evals of the engine's #1 and #2 choices, from WHITE's
    // perspective, in centipawns. Only read when have_second_pv.
    int prev_best_cp   = 0;
    int prev_second_cp = 0;
};

// Result of classification. `cls` is the label; the remaining fields
// are the intermediate signals, exposed so callers can log them (and
// tests can assert on them) without recomputing.
struct MoveQualityResult {
    MoveClass cls = MoveClass::None;
    int   cp_loss   = 0;      // player-relative centipawn loss, clamped >= 0
    float wp_loss   = 0.0f;   // win-% lost by the side that moved (0..100)
    float pv_gap_wp = 0.0f;   // win-% gap between engine #1 and #2 pre-move
    int   cat_before = 0;     // -1 losing / 0 equal / +1 winning, player POV
    int   cat_after  = 0;
    bool  brilliant  = false; // the resolved Brilliant gate
    bool  only_move  = false; // pre-move best was clearly forced
};

MoveQualityResult classify_move_quality(const MoveQualityInput& in);
