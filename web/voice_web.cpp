// Web continuous-voice driver. Bridges between the shared AppState
// continuous-mode toggle and the browser's SpeechRecognition API
// (webkitSpeechRecognition / SpeechRecognition). The desktop build
// uses whisper.cpp + SDL2 (voice_whisper.cpp); the web build uses
// the browser's built-in speech engine, which is free, fast, and
// streams partial results natively. No model download needed.
//
// Browser support is uneven: Chrome, Edge and Safari ship it;
// Firefox does not (as of 2026). voice_web_supported() lets the
// Options UI hide the toggle on unsupported browsers.

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <emscripten/em_macros.h>

#include <cstring>
#include <string>

#include "../app_state.h"
#include "../voice_tts.h"
#include "../voice_input.h"  // parse_voice_move (not used here directly,
                             // but keeps the include graph consistent)

extern AppState& web_app();   // defined below; trampoline so the
                              // exported C functions can reach the
                              // singleton without a .h leak.

// ---------------------------------------------------------------------------
// JS-side glue. We instantiate (webkit)SpeechRecognition once on
// start, hold it on window.__chessVoiceRec, and route every result
// back through Module.ccall.
//
// `onresult` events come in batches; each result has a `transcript`
// and `isFinal`. We collect:
//   - the latest interim transcript across non-final results → partial
//   - the concatenation of all `isFinal` transcripts → final utterance
//
// SpeechRecognition with continuous=true keeps streaming until the
// user toggles off or the browser gives up (some browsers cap at
// ~60 s of silence). We arm an `onend` handler that automatically
// restarts the recognition if continuous is still active, so the
// user doesn't have to re-toggle after a long pause.
// ---------------------------------------------------------------------------
EM_JS(int, voice_web_supported_js, (), {
    return (typeof window !== 'undefined' &&
            (typeof window.SpeechRecognition === 'function' ||
             typeof window.webkitSpeechRecognition === 'function'))
        ? 1 : 0;
});

EM_JS(int, voice_web_start_js, (), {
    if (window.__chessVoiceRec) return 1;
    var Ctor = window.SpeechRecognition || window.webkitSpeechRecognition;
    if (!Ctor) return 0;
    try {
        var r = new Ctor();
        r.continuous     = true;
        r.interimResults = true;
        r.lang           = 'en-US';
        r.maxAlternatives = 1;

        r.onresult = function(ev) {
            var partial = "";
            var finalText = "";
            for (var i = ev.resultIndex; i < ev.results.length; ++i) {
                var t = ev.results[i][0].transcript || "";
                if (ev.results[i].isFinal) finalText += t;
                else                       partial   += t;
            }
            if (partial.trim().length > 0) {
                Module.ccall(
                    'on_voice_partial_from_js',
                    null, ['string'], [partial.trim()]);
            }
            if (finalText.trim().length > 0) {
                Module.ccall(
                    'on_voice_utterance_from_js',
                    null, ['string'], [finalText.trim()]);
            }
        };

        r.onerror = function(ev) {
            // 'no-speech', 'aborted', 'audio-capture', 'network',
            // 'not-allowed', 'service-not-allowed' are the common
            // ones. Forward to C so set_status can surface them.
            var msg = (ev && ev.error) ? String(ev.error) : 'unknown';
            Module.ccall(
                'on_voice_error_from_js',
                null, ['string'], [msg]);
        };

        r.onend = function() {
            // Browsers stop recognition after extended silence; if
            // the user still has the toggle on, transparently
            // restart. window.__chessVoiceWanted is set true by
            // start, false by stop.
            if (window.__chessVoiceWanted) {
                try { r.start(); } catch (e) {
                    // restart can fail if a stop is racing — let
                    // the C side toggle off and the user restart.
                    Module.ccall(
                        'on_voice_error_from_js',
                        null, ['string'], ['restart-failed']);
                }
            }
        };

        window.__chessVoiceWanted = true;
        window.__chessVoiceRec = r;
        r.start();
        return 1;
    } catch (e) {
        console.error('voice_web start failed', e);
        Module.ccall(
            'on_voice_error_from_js',
            null, ['string'], [String(e && e.message || e)]);
        return 0;
    }
});

EM_JS(void, voice_web_stop_js, (), {
    window.__chessVoiceWanted = false;
    var r = window.__chessVoiceRec;
    if (r) {
        try { r.onend = null; r.onresult = null; r.onerror = null; } catch (e) {}
        try { r.stop(); } catch (e) {}
        window.__chessVoiceRec = null;
    }
});

// ---------------------------------------------------------------------------
// AppState singleton. main_web.cpp owns the actual instance; we
// borrow it through this thin trampoline so we don't have to
// re-export it via a header.
// ---------------------------------------------------------------------------
namespace {
AppState* g_app_ref = nullptr;
}

AppState& web_app() {
    return *g_app_ref;
}

// Called once from main_web.cpp's chess_start() so this TU knows
// where the singleton lives.
extern "C" void voice_web_bind_app(AppState* a) {
    g_app_ref = a;
}

// ---------------------------------------------------------------------------
// JS → C bridge. Browser fires these via Module.ccall when results
// arrive. EMSCRIPTEN_KEEPALIVE plus the explicit names in the
// EXPORTED_FUNCTIONS list (web/Makefile) prevent the linker from
// dead-stripping them.
// ---------------------------------------------------------------------------
extern "C" EMSCRIPTEN_KEEPALIVE
void on_voice_utterance_from_js(const char* text) {
    if (!g_app_ref) return;
    app_voice_continuous_apply(*g_app_ref,
                               text ? std::string(text) : std::string(),
                               std::string());
}

extern "C" EMSCRIPTEN_KEEPALIVE
void on_voice_partial_from_js(const char* text) {
    if (!g_app_ref) return;
    app_voice_continuous_apply_partial(
        *g_app_ref, text ? std::string(text) : std::string());
}

extern "C" EMSCRIPTEN_KEEPALIVE
void on_voice_error_from_js(const char* msg) {
    if (!g_app_ref) return;
    // 'no-speech' fires constantly when the user simply isn't
    // talking — drop it so it doesn't spam the status bar. Other
    // errors get forwarded as-is so the user sees what happened.
    if (msg && std::strcmp(msg, "no-speech") == 0) return;
    app_voice_continuous_apply(
        *g_app_ref, std::string(),
        std::string("speech recognition: ") + (msg ? msg : "unknown"));
}

// ---------------------------------------------------------------------------
// Platform-specific implementations of app_state.h's continuous-
// voice API. The shared layer calls these via the toggle handler in
// release_options(); the actual mechanics live entirely in JS.
// ---------------------------------------------------------------------------
bool app_voice_continuous_supported() {
    return voice_web_supported_js() != 0;
}

void app_voice_set_continuous(
    AppState& a, bool on,
    std::function<void(const std::string&, const std::string&)> /*on_utterance*/,
    std::function<void(const std::string&)> /*on_partial*/) {
    // Web ignores the callbacks — results flow through the
    // EM_JS-installed handlers above and are dispatched directly to
    // app_voice_continuous_apply / _apply_partial. The signature
    // matches the desktop one only to keep the shared toggle
    // handler in app_state.cpp from caring about the platform.
    if (on == a.voice_continuous_enabled) return;
    if (!on) {
        voice_web_stop_js();
        a.voice_continuous_enabled = false;
        // Status only changes if no utterance is in flight; reuse
        // the desktop-style "off" message via a direct title set
        // through the platform hook.
        if (a.platform && a.platform->set_status)
            a.platform->set_status("Voice — continuous listening off");
        return;
    }

    if (!app_voice_continuous_supported()) {
        if (a.platform && a.platform->set_status)
            a.platform->set_status(
                "Voice unavailable — browser has no SpeechRecognition");
        return;
    }
    if (!voice_web_start_js()) {
        if (a.platform && a.platform->set_status)
            a.platform->set_status("Voice — failed to start");
        return;
    }
    a.voice_continuous_enabled = true;
    // Auto-enable TTS so a single click on "Continuous voice" gives
    // the full eyes-free voice experience (mic in + speech out).
    // Web's voice_tts_init is essentially a feature check around
    // window.speechSynthesis so this is cheap and rarely fails.
    if (!a.voice_tts_enabled) {
        std::string tts_err;
        if (voice_tts_init(tts_err)) a.voice_tts_enabled = true;
    }
    if (a.platform && a.platform->set_status)
        a.platform->set_status("Voice — continuous listening on");
}

void app_voice_toggle_continuous_request(AppState& a) {
    bool target = !a.voice_continuous_enabled;
    // The two callbacks are unused on web — see comment in
    // app_voice_set_continuous above.
    app_voice_set_continuous(a, target,
        [](const std::string&, const std::string&) {},
        [](const std::string&) {});
}

#endif  // __EMSCRIPTEN__
