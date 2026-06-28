# Learning-mode design sketch

A design sketch for extending 3D Chess into a chess-*learning* tool. Nothing
here is built yet — this is the plan. The guiding principle: **most of the
hard parts already exist** (per-move Stockfish evals, a pile of position
classifiers, voice TTS, a 3D board). The learning features are mostly about
*surfacing* that data and adding a pedagogy layer on top.

## What we already have to build on

| Asset | Symbol / file | Used by the sketches below |
|---|---|---|
| Per-move centipawn eval | `score_history`, `app_eval_ready(a, cp, score_index, game_id)`, `trigger_eval(fen, movetime_ms, score_index, game_id)` | move classification, weakness detection, post-game review |
| Engine best move + PV | Stockfish (`ai_player`), already async via `trigger_ai_move` | "why?" lines, hint levels, predict-the-move |
| Move list (SAN) | `GameState::move_history` (`std::vector<std::string>`) | review, annotations, export |
| Per-move motif label | `pending_move_tactic` (Castles/En passant/Fork/Pin/Skewer/Discovered/Double check/Promotion), `pending_move_endgame`, mate-in-N from `app_eval_ready` | tactics flags, weakness buckets |
| Position classifiers | `opening_name_for_position(fen)`, `opening_plan_for(name)`, `classify_tactic(post,…)`, `classify_mate_pattern(post,…)`, `classify_pawn_structure(gs)`/`classify_pawn_family(gs)`, `classify_endgame(gs)`/`classify_critical_position(gs)` | every training mode, the "explain this position" panel |
| FEN load | `parse_fen`, `apply_fen_to_state(gs, parsed)` (`challenge.h`) | every drill / puzzle / setup-from-position |
| Puzzle/Challenge infra | `Puzzle`, `puzzle_load_from_md`, `puzzle_archive_save`; `Challenge`, `load_challenge`, `is_tactic_type` | tactics/endgame/opening drills, from-your-games puzzles |
| Hint state | `enum HintMode { Off, Auto, OnDemand }`, `hint_mode`, `hint_request_pending` | escalating hints, guardrails |
| Voice TTS | `voice_tts_speak(text)`, `san_to_speech`, `uci_to_speech`, `voice_tts_init` | the spoken coach |
| Voice input | `app_voice_press/release`, `parse_voice_command` | hands-free coach Q&A |
| Modes + screens | `GameMode mode` (`MODE_MENU/PLAYING/PUZZLE/OPTIONS/…`), `render_*` per mode, `options_ui` toggle pattern | new training screens + toggles |
| 3D render | `board_renderer.cpp` (move-ring draw, square highlights, ghost-piece capable) | board overlays |

## Shared infrastructure to add first (the spine)

Two small additions unlock most of the features:

### A. `MoveAnalysis` record per ply
Attach analysis to each move in `move_history`. New parallel vector in
`GameState` (or a struct-of-arrays):

```cpp
struct MoveAnalysis {
    int   cp_before = 0;        // eval before the move (side-to-move POV)
    int   cp_after  = 0;        // eval after the move
    std::string best_uci;       // engine's preferred move in that position
    std::string best_san;
    int   best_cp   = 0;        // eval if best had been played
    enum Class { Book, Best, Good, Inaccuracy, Mistake, Blunder, Brilliant } cls;
    std::string motif;          // reuse pending_move_tactic
    std::string reason;         // templated human string (see 1b)
};
std::vector<MoveAnalysis> analysis;   // parallel to move_history
```

The evals are *already* flowing through `app_eval_ready`; today they feed only
the score graph. Persist `cp_before/cp_after/best_*` there and the spine is
done. Cost: one extra `trigger_eval` per ply asking for the *best* line too
(or read it from the eval you already request) — the engine is already async,
so no UX cost.

### B. `LearnerProfile` store
A small persisted profile (extend the `settings.ini` writer, or a sibling
JSON in the config dir): puzzle rating, play rating, per-category mistake
counts, and a spaced-repetition queue. Read at startup, written on game/puzzle
end. ~50 lines next to `app_settings_load/save`.

---

# The features

Effort key: **S** ≈ a day, **M** ≈ a few days, **L** ≈ a week+. ★ = quick win
(≥70% of the data already exists).

## 1 — Coach from the eval graph

### 1a ★ Move classification *(S)*
**Value:** the single biggest learning multiplier — players need to know *which*
of their moves was the mistake, not just that the curve dropped.
**Builds on:** `score_history`, `MoveAnalysis` (A). **New state:** the `cls`
field. **UI:** a colored badge in the move list (♦ brilliant, ! good, ?!
inaccuracy, ? mistake, ?? blunder) and a dot on the eval graph at each swing.
**Sketch:** classify from the cp delta vs the engine best (standard Lichess
thresholds): `loss = best_cp - cp_after`; `<10`→Best, `<50`→Good, `<100`→
Inaccuracy, `<250`→Mistake, else Blunder; Brilliant = a sound sacrifice (best
move *and* material was given). Compute in `app_eval_ready` when both evals are
in.

### 1b ★ "Why?" on demand *(M)*
**Value:** turns a label into understanding. **Builds on:** the engine PV +
`classify_tactic`/`classify_mate_pattern` + material diff. **UI:** tap a
flagged move → the board shows the engine's line as **ghost pieces**
(`board_renderer` already draws translucent/animated pieces) and a one-line
templated reason. **Sketch:** `reason` is generated, not LLM'd — from what
changed: material delta ("hangs the knight on f6"), a new `classify_tactic`
motif on the refutation ("allows a fork on c7"), or a king-safety/structure
delta from `classify_pawn_structure`. A dozen templates covers most cases.

### 1c "Post-game review walkthrough" *(M)*
**Value:** the lesson loop. **Builds on:** `analysis`, the existing
**analysis mode** (step through with ←/→). **UI:** new `MODE_REVIEW` (clone
`render` of analysis mode) that auto-stops at the 2–3 biggest `cp` swings,
shows "your turn — find a better move," accepts a board move, then reveals the
engine line via 1b. **Sketch:** sort plies by `|cp_after - best_cp|`, queue the
top swings, reuse the move-input path gated to "guess mode."

## 2 — Lean into 3D + voice (the differentiators)

### 2a Board overlays *(M)*
**Value:** spatial understanding reads far better in 3D than on a flat board.
**Builds on:** `board_renderer` (square highlight + ring draw already exist),
`chess_rules` move generation. **UI:** an overlay toggle group (mirror the
splat/environment toggles in `options_ui`):
- **Square-control heatmap** — per square, (white attackers − black
  attackers); tint the 3D square blue↔red. One pass over all legal-attack
  squares.
- **Threat arrows** — when you lift a piece, draw arrows to what it attacks and
  red rings on your hanging pieces (re-uses the AI-move arrow renderer).
- **Piece vision** — hovering a piece lights its legal/attacked squares (extend
  the existing move-ring path from "selected" to "hovered").

**Sketch:** all three are pure functions of the current board → a set of
(square, color) and (from, to) arrows the renderer already knows how to draw.
No engine call.

### 2b Spoken coach *(S for the pipe, M for content)*
**Value:** nothing else on the market is a hands-free 3D voice coach — and you
already have **both** TTS and speech input.
**Builds on:** `voice_tts_speak`, `san_to_speech`; voice input + `parse_voice_command`.
**UI:** an options toggle "Coach voice." **Sketch:** after each of the human's
moves, if `cls ≥ Inaccuracy` speak a short note ("careful — your bishop's
undefended"); on a good move, occasional praise. Add voice intents to
`parse_voice_command`: "what should I play?" → speak the engine hint; "what's
the plan?" → speak `opening_plan_for(opening_name_for_position(fen))` in the
opening, else a structure note from `classify_pawn_structure`. Reuse the
existing push-to-talk → worker → main-thread marshalling.

## 3 — Structured training modes (your data modules are half-built)

### 3a Opening trainer *(M)*
**Builds on:** `openings/` data, `opening_name_for_position(fen)`,
`opening_plan_for(name)`. **UI:** new `MODE_OPENING_TRAIN`. **Sketch:** pick an
opening, play it out move-by-move; when the user leaves "book," flag it and
show the main line as ghost pieces; display the ECO name + the *idea* via
`opening_plan_for`; "play on from here vs Stockfish" hands off to the normal
game path with the current FEN.

### 3b Endgame drills *(M)*
**Builds on:** `endgame.cpp`, `classify_endgame`, `classify_critical_position`,
`apply_fen_to_state`. **UI:** a drill list (K+Q vs K, K+R vs K, Lucena,
Philidor, opposition / the square rule). **Sketch:** each drill is a FEN +
a goal (mate / promote / hold); set it up, you convert, Stockfish defends at
full strength; success = goal reached, with a hint button that surfaces the
technique name. The classifiers already recognize these families for the
move announcements — reuse them to detect "you've reached a winning/drawn
key position."

### 3c Tactics by motif + spaced repetition *(M)*
**Builds on:** the existing fork/pin/mate puzzles (`puzzle.*`, `challenge.*`,
`is_tactic_type`, `classify_tactic`/`classify_mate_pattern`), `LearnerProfile` (B).
**UI:** a motif picker (fork / pin / skewer / discovered / deflection /
back-rank / zwischenzug) + "due today" count. **Sketch:** tag each puzzle with
its motif (the classifier can auto-tag the archive), schedule misses with a
simple SM-2 scheduler in `LearnerProfile`, resurface due ones first. Seeds:
the chess.com archive you already accumulate + `pawn_structure` "good/bad
structure" mini-lessons.

## 4 — Personalization (what actually makes people improve)

### 4a Weakness detection *(M)*
**Builds on:** `analysis` (A) + `pending_move_tactic` + `LearnerProfile` (B).
**Sketch:** every blunder/mistake increments a per-category counter keyed by
*why* (hung piece / missed tactic motif / back-rank / king safety / endgame
technique). After N games, a "Your patterns" card: "you've hung 6 pieces to
back-rank ideas this week," with a button → generate targeted puzzles.

### 4b Adaptive difficulty + personal rating *(S)*
**Builds on:** the existing 1320–2850 Elo slider, `LearnerProfile`. **Sketch:**
maintain a play rating (Elo-style update on game result vs the Stockfish Elo
faced) and a puzzle rating; a "match my level" button auto-sets the slider.
Closes a loop that's 90% wired.

### 4c From-your-games puzzles *(M)*
**Builds on:** `analysis`, `puzzle_archive_save`, the FEN-load path. **Sketch:**
on game end, for each blunder where a clearly better move existed, emit a
`Puzzle` (FEN before the blunder + the engine line as solution) into the
archive tagged "from your games." Replays as "find the move you missed."

## 5 — Scaffolded play (lower the floor)

### 5a Escalating hints *(S)*
**Builds on:** `HintMode`, `hint_request_pending`, the engine hint. **Sketch:**
extend OnDemand into levels on repeat taps: **nudge** ("look at the kingside" —
derived from where the engine line starts) → **piece** ("your rook wants the
open file") → **move** (the actual move). Teaches *finding* ideas, not copying.

### 5b Beginner guardrails *(S)*
**Builds on:** the eval you compute for the human's candidate, `classify_tactic`
on the reply. **UI:** a "Coach guardrails" toggle. **Sketch:** before
committing a move, if it hangs material or walks into mate-in-1, show a gentle
"are you sure? this loses the queen" with a one-tap undo — opt-in, off by
default so stronger players aren't nagged.

### 5c Annotated master games + predict-the-move *(M)*
**Builds on:** the analysis-mode stepper, `move_history`, a PGN loader (new,
small). **Sketch:** ship a few annotated classics as `challenges/`-style files;
step through with notes; "predict the move" mode hides the next move, you
guess on the 3D board, scored against the master's choice (and the engine eval
of yours).

---

# Suggested phasing

1. **Spine** — `MoveAnalysis` (A) + persist best-move evals through
   `app_eval_ready`. *(S)*
2. **1a move classification** + **1b "why?"** — immediate, visible payoff. *(S/M)*
3. **2a board overlays** — visually striking, no engine cost, sells the 3D angle. *(M)*
4. **2b spoken coach** — the showpiece; unique to this app. *(S/M)*
5. **3b endgame + 3a opening drills** — your classifier modules are already
   there. *(M each)*
6. **4 personalization** (`LearnerProfile`, weakness card, ratings) once the
   spine has produced a few games of data. *(M)*
7. **5 scaffolding + 3c spaced repetition + 4c from-your-games** — the
   long-tail retention loop. *(M)*

Each is independently shippable behind an options toggle, so none of it blocks
the core game.
