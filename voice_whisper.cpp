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
#include <chrono>
#include <cmath>
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

// VAD tuning for continuous mode. Energy thresholds are linear RMS on
// the float32 samples (range [-1, 1]); 0.01 ≈ -40 dBFS, comfortably
// above typical room noise but below normal speech. `kSilenceMs` is
// how long the trailing silence has to persist before we dispatch.
constexpr float kSpeechThreshold      = 0.012f;
constexpr float kSilenceThreshold     = 0.006f;
constexpr int   kVadTickMs            = 200;
constexpr int   kSilenceMs            = 600;
constexpr int   kVadWindowSamples     = kSampleRate / 10;   // 100 ms RMS window
constexpr int   kMinUtteranceSamples  = kSampleRate / 4;    // 250 ms

struct VoiceState {
    std::mutex          mu;
    whisper_context*    ctx = nullptr;
    SDL_AudioDeviceID   capture_dev = 0;
    std::vector<float>  buffer;
    std::atomic<bool>   capturing{false};
    std::atomic<bool>   transcribing{false};

    // Continuous-mode state. The monitor thread is owned here; the
    // stop flag is the only inter-thread signal. on_utterance is set
    // by voice_start_continuous and read only by detached workers.
    std::atomic<bool>   continuous_running{false};
    std::thread         monitor_thread;
    std::function<void(const std::string&, const std::string&)> on_utterance;
};

VoiceState g_voice;

// Run whisper_full() on the supplied PCM and return the trimmed text
// or an error. Pure compute — caller handles the transcribing flag
// and threading. Defined out here so both push-to-talk and
// continuous-mode workers share one implementation.
void run_whisper(std::vector<float> pcm,
                 std::function<void(const std::string&,
                                    const std::string&)> on_done) {
    whisper_context* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_voice.mu);
        ctx = g_voice.ctx;
    }
    if (!ctx) {
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
        if (on_done) on_done("", "Whisper inference failed");
        return;
    }

    std::string text;
    int n = whisper_full_n_segments(ctx);
    for (int i = 0; i < n; ++i) {
        const char* seg = whisper_full_get_segment_text(ctx, i);
        if (seg) text += seg;
    }
    size_t a = 0, b = text.size();
    while (a < b && (text[a] == ' ' || text[a] == '\t' ||
                     text[a] == '\n' || text[a] == '\r')) ++a;
    while (b > a && (text[b-1] == ' ' || text[b-1] == '\t' ||
                     text[b-1] == '\n' || text[b-1] == '\r')) --b;
    text = text.substr(a, b - a);

    if (on_done) {
        if (text.empty()) on_done("", "No speech recognised");
        else              on_done(text, "");
    }
}

// Compute RMS over the last `kVadWindowSamples` samples in `buf`.
// Returns 0 if there isn't enough data yet.
float tail_rms(const std::vector<float>& buf) {
    if (static_cast<int>(buf.size()) < kVadWindowSamples) return 0.0f;
    size_t start = buf.size() - kVadWindowSamples;
    double sumsq = 0.0;
    for (size_t i = start; i < buf.size(); ++i) {
        double s = buf[i];
        sumsq += s * s;
    }
    return static_cast<float>(std::sqrt(sumsq / kVadWindowSamples));
}

// Monitor loop: runs on g_voice.monitor_thread while
// continuous_running is true. Owns its own VAD state; communicates
// with the rest of the system only by reading the buffer and (on a
// dispatch) spawning a detached whisper worker.
void voice_continuous_loop() {
    bool in_speech = false;
    int  silence_ms = 0;

    while (g_voice.continuous_running.load()) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kVadTickMs));
        if (!g_voice.continuous_running.load()) break;

        // Snapshot just enough of the buffer to compute RMS, without
        // holding the mutex over the math.
        float rms = 0.0f;
        size_t buf_size = 0;
        {
            std::lock_guard<std::mutex> lk(g_voice.mu);
            buf_size = g_voice.buffer.size();
            rms = tail_rms(g_voice.buffer);
        }

        if (rms > kSpeechThreshold) {
            in_speech  = true;
            silence_ms = 0;
            continue;
        }
        if (!in_speech) {
            // Pre-speech silence: don't let the buffer grow forever
            // while the user just isn't talking. Trim so we never
            // hold more than ~2 seconds of pre-roll.
            if (buf_size > static_cast<size_t>(kSampleRate * 2)) {
                std::lock_guard<std::mutex> lk(g_voice.mu);
                size_t keep = kSampleRate * 1;  // keep last 1 s
                if (g_voice.buffer.size() > keep) {
                    g_voice.buffer.erase(
                        g_voice.buffer.begin(),
                        g_voice.buffer.end() - keep);
                }
            }
            continue;
        }
        if (rms > kSilenceThreshold) {
            // Voiced-but-quiet: still in speech, just don't accrue
            // silence yet.
            continue;
        }

        silence_ms += kVadTickMs;
        if (silence_ms < kSilenceMs) continue;

        // Trailing silence reached: dispatch the utterance. Snapshot
        // the callback under the same lock as the buffer swap so a
        // concurrent voice_shutdown() can't tear it out from under
        // the worker.
        std::vector<float> pcm;
        std::function<void(const std::string&, const std::string&)> cb;
        {
            std::lock_guard<std::mutex> lk(g_voice.mu);
            pcm.swap(g_voice.buffer);
            cb = g_voice.on_utterance;
        }
        in_speech  = false;
        silence_ms = 0;

        if (static_cast<int>(pcm.size()) < kMinUtteranceSamples) continue;

        bool expected = false;
        if (!g_voice.transcribing.compare_exchange_strong(expected, true)) {
            // Previous transcription still running — drop this one
            // and let the next utterance ride.
            continue;
        }

        std::thread([pcm = std::move(pcm), cb = std::move(cb)]() mutable {
            run_whisper(std::move(pcm), std::move(cb));
            g_voice.transcribing.store(false);
        }).detach();
    }
}

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
    voice_stop_continuous();  // joins monitor thread if running
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
    g_voice.on_utterance = nullptr;
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

    std::thread([pcm = std::move(pcm), on_done = std::move(on_done)]() mutable {
        run_whisper(std::move(pcm), std::move(on_done));
        g_voice.transcribing.store(false);
    }).detach();
}

bool voice_start_continuous(
    std::function<void(const std::string&, const std::string&)> on_utterance,
    std::string& err_out) {
    {
        std::lock_guard<std::mutex> lk(g_voice.mu);
        if (!g_voice.ctx || g_voice.capture_dev == 0) {
            err_out = "Voice engine not initialised";
            return false;
        }
        if (g_voice.continuous_running.load()) return true;  // idempotent
        g_voice.buffer.clear();
        g_voice.on_utterance = std::move(on_utterance);
    }
    g_voice.capturing.store(true);
    SDL_PauseAudioDevice(g_voice.capture_dev, 0);
    g_voice.continuous_running.store(true);
    g_voice.monitor_thread = std::thread(voice_continuous_loop);
    return true;
}

void voice_stop_continuous() {
    if (!g_voice.continuous_running.exchange(false)) {
        // Wasn't running — but if a stale thread handle exists (e.g.
        // the monitor exited on its own), still join it for hygiene.
        if (g_voice.monitor_thread.joinable())
            g_voice.monitor_thread.join();
        return;
    }
    if (g_voice.monitor_thread.joinable())
        g_voice.monitor_thread.join();

    std::lock_guard<std::mutex> lk(g_voice.mu);
    if (g_voice.capture_dev) {
        SDL_PauseAudioDevice(g_voice.capture_dev, 1);
    }
    g_voice.capturing.store(false);
    g_voice.buffer.clear();
}

#endif  // !__EMSCRIPTEN__
