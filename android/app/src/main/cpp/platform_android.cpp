// ===========================================================================
// android/app/src/main/cpp/platform_android.cpp — Android platform stubs.
// ===========================================================================
//
// The Android twin of ios/platform_ios.cpp. The desktop build backs voice
// (whisper.cpp), move-announcer TTS (Piper), and the Chessnut Move board
// (SimpleBLE) with native libraries; the web build replaces them with browser
// feature-detected stubs (web/voice_web.cpp, web/voice_tts_web.cpp,
// web/chessnut_web.cpp).
//
// The Android scaffold mirrors the WEB / iOS approach: these capabilities are
// reported **unsupported** so the Options UI hides/greys their toggles, and the
// shared app_state.cpp compiles its desktop whisper/SimpleBLE/subprocess blocks
// out via CHESS_PLATFORM_MOBILE (CHESS_PLATFORM_ANDROID is set in the
// CMakeLists; __ANDROID__ also implies it). That keeps whisper.cpp and SimpleBLE
// out of the build for now, exactly like the web feature-detect.
//
// This file therefore provides the SAME symbol surface the web/iOS stubs do, so
// the shared layer links. Each is an inert no-op / "unsupported" path.
//
// ---------------------------------------------------------------------------
// REMAINING TODO (see docs/ANDROID.md), in rough priority order:
//   * Move-announcer TTS → android.speech.tts.TextToSpeech via JNI (the Android
//     analogue of voice_tts_web.cpp's speechSynthesis). A thin JNI shim
//     implementing voice_tts_init/speak/shutdown.
//   * Voice move input → android.speech.SpeechRecognizer (or on-device
//     SpeechRecognizer) via JNI, feeding parse_voice_command() the same way
//     whisper does on desktop. Needs the RECORD_AUDIO permission.
//   * Chessnut Move board → Android BLE (android.bluetooth.le) via JNI. The
//     wire format is already shared in chessnut_encode.h; only the transport
//     differs. Needs BLUETOOTH_SCAN / BLUETOOTH_CONNECT runtime permissions.
// ===========================================================================

#include "app_state.h"
#include "voice_tts.h"

#include <functional>
#include <string>

namespace {
inline void android_status(AppState& a, const char* msg) {
    if (a.platform && a.platform->set_status) a.platform->set_status(msg);
}
}  // namespace

// ===========================================================================
// Continuous (hands-free) voice — unsupported in the scaffold.
// ===========================================================================
bool app_voice_continuous_supported() {
    return false;  // TODO: android.speech.SpeechRecognizer (JNI)
}

void app_voice_set_continuous(
    AppState& a, bool /*on*/,
    std::function<void(const std::string&, const std::string&)> /*on_utterance*/,
    std::function<void(const std::string&)> /*on_partial*/) {
    android_status(a, "Voice — not available on Android yet (see docs/ANDROID.md)");
}

void app_voice_toggle_continuous_request(AppState& a) {
    android_status(a, "Voice — not available on Android yet (see docs/ANDROID.md)");
}

// ===========================================================================
// Move-announcer TTS — unsupported in the scaffold (wire up TextToSpeech).
// ===========================================================================
bool voice_tts_init(std::string& err_out) {
    err_out = "Android TTS not wired (use android.speech.tts.TextToSpeech — see docs/ANDROID.md)";
    return false;
}

void voice_tts_speak(const std::string& /*text*/) {
    // No-op until TextToSpeech is wired up.
}

void voice_tts_shutdown() {
    // Nothing to release.
}

// ===========================================================================
// Chessnut Move physical board — unsupported in the scaffold (wire up Android
// BLE). Mirrors the no-op surface of web/chessnut_web.cpp / platform_ios.cpp so
// every shared call site links. app_chessnut_apply_status /
// _apply_sensor_frame are platform-independent and stay in app_state.cpp.
// ===========================================================================
bool app_chessnut_supported() {
    return false;  // TODO: Android BLE (JNI)
}

void app_chessnut_toggle_request(AppState& a) {
    android_status(a, "Chessnut Move — not available on Android yet (see docs/ANDROID.md)");
}

void app_chessnut_set_enabled(
    AppState& a, bool /*on*/,
    std::function<void(const std::string&)> /*on_status*/) {
    a.chessnut_enabled        = false;
    a.chessnut_bridge_running = false;
    a.chessnut_connected      = false;
    android_status(a, "Chessnut Move — not available on Android yet (see docs/ANDROID.md)");
}

void app_chessnut_sync_board(AppState& /*a*/, bool /*force*/) {}

bool app_chessnut_send_ai_move_preview(AppState& /*a*/,
                                       int /*fc*/, int /*fr*/,
                                       int /*tc*/, int /*tr*/) {
    return false;
}

void app_chessnut_shutdown(AppState& a) {
    a.chessnut_enabled        = false;
    a.chessnut_bridge_running = false;
    a.chessnut_connected      = false;
}

void app_chessnut_highlight_last_move(AppState& /*a*/) {}
void app_chessnut_open_picker(AppState& /*a*/) {}
void app_chessnut_close_picker(AppState& /*a*/) {}
void app_chessnut_forget_cached_device(AppState& /*a*/) {}
void app_chessnut_pick_device(AppState& /*a*/, const std::string& /*address*/) {}
