# Claude instructions for this repo

## README sync

Whenever a change lands that could make `README.md` stale, update
`README.md` as part of the same change. In particular, re-check the
README before pushing if any of these happen:

- A file is **added, renamed, or removed** (the Project Structure
  tree lists every top-level source file).
- A **build step** changes (Makefile inputs, new `-s` emscripten
  flags, new tool invocation).
- A **user-visible feature** is added, removed, or reworked
  (anything that would change the Features / Controls / How-to-play
  sections).
- An **asset pipeline** step changes (tools under `tools/`,
  `models*/`, `sounds/`, preload list in `web/Makefile`).
- A **platform limitation** is lifted or newly introduced (the
  "Limitations vs the desktop build" list).

If the change is purely internal and no user-facing or layout
details move, leave the README alone and say so in the response.

## Voice-command sync

Voice recognition (continuous mode + push-to-talk) doubles as a
mouse-free way to drive the UI: spoken button labels are routed
through `parse_voice_command()` in `voice_input.cpp` and dispatched
in `app_state.cpp`'s `try_voice_command()`. Whenever a clickable UI
control is **added, renamed, or removed**, update the voice
parser/dispatcher in the same change so the spoken interface stays
in sync. Specifically:

- Add a new `VoiceCommand` enum value in `voice_input.h` for the new
  control, plus a phrase list in the appropriate mode arm of
  `parse_voice_command()` (include common synonyms — "back" /
  "menu" / "exit" pattern).
- Wire the dispatch in `try_voice_command()` (`app_state.cpp`) to
  the same code path the click handler runs — typically an existing
  `app_enter_*` or a small state mutation. Reuse, don't fork.
- Update the "Voice UI commands" subsection of `README.md` so users
  know the new phrase.

Skip when the control has no sensible spoken form (drag handles,
sliders, dropdown rows tied to mouse position, hover-only chrome) —
note that explicitly in the response so the omission is visible.
