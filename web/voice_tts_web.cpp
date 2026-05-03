// Web TTS impl — browser SpeechSynthesis API.
// Mirrors voice_tts_native.cpp's signatures so the call sites in
// app_state.cpp (and the pure-logic helpers in voice_tts.cpp) work
// identically across desktop and web.
//
// SpeechSynthesis is built into every modern browser (Chrome, Edge,
// Firefox, Safari) and ships zero extra weight to the page — there
// is no model to download, no audio plumbing to connect to. Voice
// quality varies by platform; on Linux Chromium it usually
// delegates to espeak-ng under the hood, so the desktop and web
// builds end up with nearly identical voicing on the same machine.

#ifdef __EMSCRIPTEN__

#include "voice_tts.h"

#include <emscripten.h>
#include <emscripten/em_macros.h>

#include <string>

// Speak `text` via the browser's SpeechSynthesisUtterance. Cancels
// any in-flight utterance first so back-to-back AI moves don't
// queue up — the user just wants the latest move read, not a
// historical recap.
EM_JS(void, voice_tts_speak_js, (const char* text), {
    if (typeof speechSynthesis === "undefined") return;
    if (!text) return;
    var s = UTF8ToString(text);
    if (!s) return;
    speechSynthesis.cancel();
    var u = new SpeechSynthesisUtterance(s);
    u.rate = 1.0;
    u.volume = 1.0;
    speechSynthesis.speak(u);
});

EM_JS(int, voice_tts_supported_js, (), {
    return (typeof speechSynthesis !== "undefined") ? 1 : 0;
});

bool voice_tts_init(std::string& err_out) {
    if (!voice_tts_supported_js()) {
        err_out = "browser has no speechSynthesis";
        return false;
    }
    return true;
}

void voice_tts_speak(const std::string& text) {
    if (text.empty()) return;
    voice_tts_speak_js(text.c_str());
}

void voice_tts_shutdown() {
    // No persistent state to release. Cancel any pending utterances
    // so the next page load starts clean.
    EM_ASM({
        if (typeof speechSynthesis !== "undefined") speechSynthesis.cancel();
    });
}

#endif  // __EMSCRIPTEN__
