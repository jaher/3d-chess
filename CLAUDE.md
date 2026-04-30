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

## Chessnut Move bridge

Whenever something changes the on-board piece layout, the physical
Chessnut Move board needs to be re-synced. Most callers don't think
about this directly — `app_chessnut_sync_board(a, force)` is invoked
right after every `execute_move()` on the desktop side, and after
`app_enter_game()` / `app_load_challenge_puzzle()` for first-time
positioning. If you add a new code path that mutates `a.game`
(undo / redo, programmatic position load, mid-game FEN edits, …),
add a `app_chessnut_sync_board(a, /*force=*/false)` call in the
same gated `#ifndef __EMSCRIPTEN__` block — otherwise the physical
board will drift out of sync with the on-screen state.

If you change the wire format (e.g. switch to a different opcode or
re-encode the board), there's a single source of truth:
`chessnut_encode.h`. The header-only encoder is shared by:

- Desktop native impl (`chessnut_bridge_native.cpp`).
- Web Bluetooth impl (`web/chessnut_web.cpp`).

Update `chessnut_encode.h` and **also** mirror the change in the
Python prototype helper at `tools/chessnut_bridge.py` (same
function names, same byte layout — kept in sync as the
hand-driven debugging tool).

The connection-time handshake bytes (`0x0B 0x04 0x03 0xE8 0x00 0xC8`
and `0x27 0x01 0x00`) live separately in each driver
(`do_connect` in the native impl, the EM_JS `chessnut_web_start_js`
shim in the web impl, the Python `build_init_handshake` function).
If those need to change, update all three.

Cross-check against `~/chessnutapp/PROTOCOL.md` and the decompiled
Android app in `~/chessnutapp/decompiled/` — those are the source
of truth for what the firmware expects.
