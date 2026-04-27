// SDL2 microphone capture + whisper.cpp inference for the desktop
// build. The pure-logic parser lives in voice_input.cpp; this file
// supplies the lifecycle hooks declared in voice_input.h.
//
// Excluded from the web build (no microphone API parity) and from
// the unit-test binary (no SDL2 / whisper symbols on the link line).

#ifndef __EMSCRIPTEN__

#include "voice_input.h"

#include <whisper.h>

#include <SDL.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// Whisper expects 16 kHz mono float32. SDL2 will resample / convert
// for us if the device prefers different specs, via SDL_AudioCVT,
// but the modern callback-style API delivers whatever we ask for
// directly through SDL_OpenAudioDevice's `obtained` matching `desired`.
constexpr int kSampleRate = 16000;
constexpr int kBufferSamplesMax = kSampleRate * 30;  // 30 s safety cap

struct VoiceState {
    std::mutex          mu;
    whisper_context*    ctx = nullptr;
    SDL_AudioDeviceID   capture_dev = 0;
    std::vector<float>  buffer;
    std::atomic<bool>   capturing{false};
    std::atomic<bool>   transcribing{false};
};

VoiceState g_voice;

// SDL audio callback (capture). Runs on SDL's audio thread; locks
// the shared state briefly to append samples.
void SDLCALL audio_capture_callback(void* /*userdata*/, Uint8* stream, int len) {
    if (!g_voice.capturing.load()) return;
    const float* samples = reinterpret_cast<const float*>(stream);
    int n = len / static_cast<int>(sizeof(float));
    std::lock_guard<std::mutex> lk(g_voice.mu);
    if (static_cast<int>(g_voice.buffer.size()) + n > kBufferSamplesMax) {
        n = kBufferSamplesMax - static_cast<int>(g_voice.buffer.size());
        if (n <= 0) return;
    }
    g_voice.buffer.insert(g_voice.buffer.end(), samples, samples + n);
}

}  // namespace

bool voice_init(const std::string& model_path, std::string& err_out) {
    std::lock_guard<std::mutex> lk(g_voice.mu);
    if (g_voice.ctx) return true;  // idempotent

    if (SDL_WasInit(SDL_INIT_AUDIO) == 0) {
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            err_out = std::string("SDL_InitSubSystem failed: ") + SDL_GetError();
            return false;
        }
    }

    SDL_AudioSpec want{}, have{};
    want.freq     = kSampleRate;
    want.format   = AUDIO_F32SYS;
    want.channels = 1;
    want.samples  = 1024;
    want.callback = audio_capture_callback;
    want.userdata = nullptr;

    SDL_AudioDeviceID dev = SDL_OpenAudioDevice(
        nullptr, /*iscapture=*/SDL_TRUE, &want, &have,
        SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (dev == 0) {
        err_out = std::string("SDL_OpenAudioDevice (capture) failed: ") + SDL_GetError();
        return false;
    }
    if (have.freq != kSampleRate || have.format != AUDIO_F32SYS || have.channels != 1) {
        SDL_CloseAudioDevice(dev);
        err_out = "Microphone does not support 16 kHz mono float32 capture";
        return false;
    }

    whisper_context_params cparams = whisper_context_default_params();
    whisper_context* ctx = whisper_init_from_file_with_params(
        model_path.c_str(), cparams);
    if (!ctx) {
        SDL_CloseAudioDevice(dev);
        err_out = "Failed to load whisper model from " + model_path;
        return false;
    }

    g_voice.ctx = ctx;
    g_voice.capture_dev = dev;
    g_voice.buffer.clear();
    g_voice.buffer.reserve(kBufferSamplesMax);
    return true;
}

void voice_shutdown() {
    std::lock_guard<std::mutex> lk(g_voice.mu);
    g_voice.capturing.store(false);
    if (g_voice.capture_dev) {
        SDL_PauseAudioDevice(g_voice.capture_dev, 1);
        SDL_CloseAudioDevice(g_voice.capture_dev);
        g_voice.capture_dev = 0;
    }
    if (g_voice.ctx) {
        whisper_free(g_voice.ctx);
        g_voice.ctx = nullptr;
    }
    g_voice.buffer.clear();
}

void voice_start_capture() {
    {
        std::lock_guard<std::mutex> lk(g_voice.mu);
        if (!g_voice.ctx || g_voice.capture_dev == 0) return;
        g_voice.buffer.clear();
    }
    g_voice.capturing.store(true);
    SDL_PauseAudioDevice(g_voice.capture_dev, 0);  // unpause = start
}

void voice_stop_and_transcribe(
    std::function<void(const std::string& utterance,
                       const std::string& error)> on_done) {
    std::vector<float> pcm;
    {
        std::lock_guard<std::mutex> lk(g_voice.mu);
        if (!g_voice.ctx || g_voice.capture_dev == 0) {
            if (on_done) on_done("", "Voice engine not initialised");
            return;
        }
        SDL_PauseAudioDevice(g_voice.capture_dev, 1);
        g_voice.capturing.store(false);
        pcm.swap(g_voice.buffer);
    }

    if (pcm.size() < kSampleRate / 4) {  // <250 ms
        if (on_done) on_done("", "Audio too short");
        return;
    }

    bool expected = false;
    if (!g_voice.transcribing.compare_exchange_strong(expected, true)) {
        if (on_done) on_done("", "Already transcribing");
        return;
    }

    std::thread([pcm = std::move(pcm), on_done = std::move(on_done)]() {
        whisper_context* ctx = nullptr;
        {
            std::lock_guard<std::mutex> lk(g_voice.mu);
            ctx = g_voice.ctx;
        }
        if (!ctx) {
            g_voice.transcribing.store(false);
            if (on_done) on_done("", "Voice engine shut down");
            return;
        }

        whisper_full_params wparams =
            whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.print_realtime   = false;
        wparams.print_progress   = false;
        wparams.print_timestamps = false;
        wparams.print_special    = false;
        wparams.translate        = false;
        wparams.language         = "en";
        wparams.n_threads        = 4;
        wparams.no_context       = true;
        wparams.single_segment   = true;

        if (whisper_full(ctx, wparams, pcm.data(),
                         static_cast<int>(pcm.size())) != 0) {
            g_voice.transcribing.store(false);
            if (on_done) on_done("", "Whisper inference failed");
            return;
        }

        std::string text;
        int n = whisper_full_n_segments(ctx);
        for (int i = 0; i < n; ++i) {
            const char* seg = whisper_full_get_segment_text(ctx, i);
            if (seg) text += seg;
        }

        // Trim leading/trailing whitespace.
        size_t a = 0, b = text.size();
        while (a < b && (text[a] == ' ' || text[a] == '\t' ||
                         text[a] == '\n' || text[a] == '\r')) ++a;
        while (b > a && (text[b-1] == ' ' || text[b-1] == '\t' ||
                         text[b-1] == '\n' || text[b-1] == '\r')) --b;
        text = text.substr(a, b - a);

        g_voice.transcribing.store(false);
        if (on_done) {
            if (text.empty()) on_done("", "No speech recognised");
            else              on_done(text, "");
        }
    }).detach();
}

#endif  // !__EMSCRIPTEN__
