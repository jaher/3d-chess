#pragma once

// Voice-controlled move input. The parser (parse_voice_move) is pure
// logic and is the only thing the unit test binary links against —
// the SDL2 capture and whisper.cpp inference live in voice_whisper.cpp
// and are excluded from the test build.

#include "chess_types.h"

#include <functional>
#include <string>

#ifndef __EMSCRIPTEN__

// ---------------------------------------------------------------------------
// Engine lifecycle (implemented in voice_whisper.cpp).
// ---------------------------------------------------------------------------
// Open the SDL2 capture device and load the whisper model. Idempotent
// — repeated calls after a successful init are no-ops. Returns true on
// success; on failure fills err_out with a user-facing message and
// leaves the engine uninitialised.
bool voice_init(const std::string& model_path, std::string& err_out);

// Close the capture device, release whisper resources, join any
// outstanding worker. Safe to call without a prior voice_init.
void voice_shutdown();

// Begin filling the audio ring buffer. Call on push-to-talk press.
// No-op if not initialised.
void voice_start_capture();

// Stop capturing and dispatch transcription on a worker thread. The
// callback fires from the worker; the caller is responsible for
// marshalling onto the GUI thread (e.g. via g_idle_add). Either
// utterance is non-empty (success) or error is non-empty (failure).
void voice_stop_and_transcribe(
    std::function<void(const std::string& utterance,
                       const std::string& error)> on_done);

// Continuous (hands-free) capture. Opens the SDL device, leaves it
// running, and launches a dedicated VAD monitor thread that watches
// the ring buffer and dispatches whisper transcription whenever it
// detects a pause after speech. on_utterance fires once per detected
// utterance from a worker thread (off the GUI thread); on_partial
// fires every time the streaming worker finishes a pass during
// speech and may be used to surface a live "what whisper is hearing"
// indicator. Both callbacks run off the GUI thread — the caller is
// responsible for marshalling back via g_idle_add. on_partial may be
// nullptr if the caller doesn't care about live updates. Returns
// false if the engine isn't initialised (run voice_init first).
// Idempotent: a second call while continuous is already running is
// a no-op.
bool voice_start_continuous(
    std::function<void(const std::string& utterance,
                       const std::string& error)> on_utterance,
    std::function<void(const std::string& partial)> on_partial,
    std::string& err_out);

// Stop the VAD monitor thread (joins it) and pause the SDL device.
// Detached transcription workers in flight at the time of the stop
// continue to run and will eventually fire on_utterance — callers
// must guard their callback against post-stop deliveries. Idempotent.
void voice_stop_continuous();

#endif // !__EMSCRIPTEN__

// ---------------------------------------------------------------------------
// Pure parser (implemented in voice_input.cpp). Always available so
// the unit tests can exercise it without SDL2 / whisper.
// ---------------------------------------------------------------------------
// Convert a transcribed utterance ("knight d3", "castle kingside",
// "e4", ...) into a UCI move legal in the supplied GameState. Returns
// true on success and writes the 4-character UCI to uci_out (no
// promotion suffix — the existing execute_move auto-queens). On
// failure writes a user-facing diagnostic to error_out and returns
// false.
//
// The parser does its own homophone normalisation, drops filler
// verbs ("to", "takes"), and resolves disambiguation by enumerating
// legal moves from gs. It does NOT mutate gs.
bool parse_voice_move(const std::string& utterance,
                      const GameState& gs,
                      std::string& uci_out,
                      std::string& error_out);
