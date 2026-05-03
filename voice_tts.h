#pragma once

// Move-announcement TTS — text-to-speech for the AI's reply (and
// optionally the user's own moves), gated behind the same Voice
// Options toggle as the input side.
//
// Two halves:
//
//   * Pure-logic helper `uci_to_speech` (this header + voice_tts.cpp,
//     no platform deps) — turns a UCI move + pre-move snapshot into
//     spoken English by post-processing `uci_to_algebraic` output.
//     Unit-tested in tests/voice_tts_test.cpp.
//
//   * Platform glue (`voice_tts_init`, `voice_tts_speak`,
//     `voice_tts_shutdown`) — desktop impl in voice_tts_native.cpp
//     uses espeak-ng with a synth callback that writes PCM into the
//     audio.cpp mixer; web impl in web/voice_tts_web.cpp uses the
//     browser's `window.speechSynthesis` API.
//
// Default trigger: speak the AI's reply only — reading back the
// user's own move would be redundant noise.

#include <string>

struct BoardSnapshot;  // chess_types.h

// ---------------------------------------------------------------------------
// Pure-logic — turn a UCI move into a spoken English string.
// ---------------------------------------------------------------------------
//
// Examples:
//   "e2e4"  → "Pawn to e four"
//   "Nf3"   → "Knight to f three"     (algebraic input also accepted)
//   "Bxd5"  → "Bishop takes d five"
//   "e1g1"  → "Castles kingside"
//   "e7e8q" → "Pawn to e eight, promotes to queen"
//   "Nf3+"  → "Knight to f three, check"
//   "Qh4#"  → "Queen to h four, checkmate"
//
// File numbers ('1'..'8') and files ('a'..'h') are spelled out so
// espeak-ng pronounces "f3" as "f three" not "ef three" or "forty-
// three". `before` is the position immediately before the move was
// played — used for piece lookup, capture detection, and check /
// mate suffix via uci_to_algebraic in chess_rules.cpp.
std::string uci_to_speech(const BoardSnapshot& before,
                          const std::string& uci);

// Standalone variant that accepts an already-rendered SAN string
// (the output of `uci_to_algebraic`) and converts it to spoken
// English. Useful for tests and for callers that already have SAN
// in hand. Identical phrasing rules as above.
std::string san_to_speech(const std::string& san);

// ---------------------------------------------------------------------------
// Platform glue — desktop espeak-ng or web speechSynthesis.
// ---------------------------------------------------------------------------
//
// `voice_tts_init` is lazy: the first call when the user toggles
// "Speak moves" on creates the espeak-ng / browser context; later
// calls are no-ops. Returns false (with `err_out` populated) if the
// engine couldn't be initialised — the caller should leave the
// toggle off in that case.
bool voice_tts_init(std::string& err_out);

// Fire-and-forget. Synthesises `text` and queues the resulting PCM
// for playback through the existing audio.cpp mixer (desktop) or
// hands it to `window.speechSynthesis.speak` (web). Safe to call
// before `voice_tts_init`; it's a no-op in that case.
void voice_tts_speak(const std::string& text);

// Releases the espeak-ng context (desktop) or no-ops (web). Called
// from the app's shutdown path.
void voice_tts_shutdown();
