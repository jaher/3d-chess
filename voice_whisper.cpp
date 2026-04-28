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
//
// Silence threshold is short here (350 ms) because the streaming
// worker has been transcribing the audio in parallel during speech —
// we don't pay full inference latency at end-of-speech, so a short
// pause is enough to commit the move.
constexpr float kSpeechThreshold      = 0.012f;
constexpr float kSilenceThreshold     = 0.006f;
constexpr int   kVadTickMs            = 200;
constexpr int   kSilenceMs            = 350;
constexpr int   kVadWindowSamples     = kSampleRate / 10;   // 100 ms RMS window
constexpr int   kMinUtteranceSamples  = kSampleRate / 4;    // 250 ms

// Streaming worker tuning. While the user is speaking, a persistent
// background thread runs whisper repeatedly on the audio captured so
// far. By the time VAD says "end of speech", the latest streaming
// pass already has the transcript — we use it directly instead of
// running a fresh inference at end-of-speech.
//
// `kStreamMaxAudioSec` caps how much audio each streaming pass sees
// (chess utterances are ≤ 2 s; padding past that wastes encoder
// work). `kStreamFreshnessMs` is the staleness window for trusting
// the latest streaming result; older than that, we fall back to a
// fresh pass on the full buffer to avoid clipping the final word.
constexpr int   kStreamSleepMs         = 30;
constexpr int   kStreamMaxAudioSec     = 3;
constexpr int   kStreamFreshnessMs     = 600;
constexpr size_t kStreamCoverageSlackSamples = kSampleRate / 2;  // 500 ms

// Whisper params used by every inference call. `audio_ctx = 512`
// caps the encoder's mel context length — default is 1500 (30 s)
// which is wasteful for chess utterances that fit in 2 s.
// n_threads picks up the host's core count (capped at 8 — whisper
// scales poorly past that).
constexpr int kWhisperAudioCtx = 512;

int whisper_thread_count() {
    unsigned hc = std::thread::hardware_concurrency();
    if (hc < 2) hc = 2;
    if (hc > 8) hc = 8;
    return static_cast<int>(hc);
}

int64_t voice_now_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(
        steady_clock::now().time_since_epoch()).count();
}

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

    // Streaming worker. `should_stream` is set true by the monitor
    // when speech starts, cleared when it ends; the streaming thread
    // polls this every kStreamSleepMs.
    std::atomic<bool>   should_stream{false};
    std::thread         stream_thread;
    // Latest streaming result (protected by mu). audio_size is the
    // sample count fed into the pass that produced this text;
    // finish_us is the monotonic timestamp when it landed. Both are
    // zeroed when entering a new utterance so a stale result from
    // the previous turn can't be mistaken for the new one.
    std::string         last_stream_text;
    int64_t             last_stream_finish_us = 0;
    size_t              last_stream_audio_size = 0;
};

VoiceState g_voice;

// Run whisper_full() on the supplied PCM and return the trimmed text
// or an error string. Pure compute — caller handles the transcribing
// flag and threading. Defined out here so push-to-talk, continuous
// final-pass, and streaming workers all share one implementation.
bool run_whisper_text(const std::vector<float>& pcm,
                      std::string& text_out,
                      std::string& err_out) {
    whisper_context* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_voice.mu);
        ctx = g_voice.ctx;
    }
    if (!ctx) {
        err_out = "Voice engine shut down";
        return false;
    }

    whisper_full_params wparams =
        whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.print_realtime   = false;
    wparams.print_progress   = false;
    wparams.print_timestamps = false;
    wparams.print_special    = false;
    wparams.translate        = false;
    wparams.language         = "en";
    wparams.n_threads        = whisper_thread_count();
    wparams.no_context       = true;
    wparams.single_segment   = true;
    wparams.audio_ctx        = kWhisperAudioCtx;

    if (whisper_full(ctx, wparams, pcm.data(),
                     static_cast<int>(pcm.size())) != 0) {
        err_out = "Whisper inference failed";
        return false;
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
    text_out = text.substr(a, b - a);
    return true;
}

// Callback-style wrapper around run_whisper_text. Used by the
// push-to-talk and continuous final-pass paths, which post results
// back to the GUI thread via a marshalling callback.
void run_whisper(std::vector<float> pcm,
                 std::function<void(const std::string&,
                                    const std::string&)> on_done) {
    std::string text, err;
    if (!run_whisper_text(pcm, text, err)) {
        if (on_done) on_done("", err);
        return;
    }
    if (on_done) {
        if (text.empty()) on_done("", "No speech recognised");
        else              on_done(text, "");
    }
}

// Streaming worker: while continuous_running is true, polls
// should_stream and runs whisper passes on the current buffer. Each
// pass produces a transcript that the VAD monitor can consume on
// end-of-speech, hiding inference latency under the audio capture.
void voice_stream_loop() {
    while (g_voice.continuous_running.load()) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kStreamSleepMs));
        if (!g_voice.continuous_running.load()) break;
        if (!g_voice.should_stream.load()) continue;

        // Grab a tail of the buffer. Cap at kStreamMaxAudioSec so
        // the encoder stays cheap for long-winded utterances.
        std::vector<float> pcm;
        size_t covered_size = 0;
        {
            std::lock_guard<std::mutex> lk(g_voice.mu);
            const size_t total = g_voice.buffer.size();
            const size_t cap =
                static_cast<size_t>(kSampleRate * kStreamMaxAudioSec);
            const size_t offset = total > cap ? total - cap : 0;
            pcm.assign(g_voice.buffer.begin() + offset,
                       g_voice.buffer.end());
            covered_size = total;
        }
        if (static_cast<int>(pcm.size()) < kMinUtteranceSamples) continue;
        if (!g_voice.should_stream.load()) continue;  // re-check

        std::string text, err;
        if (!run_whisper_text(pcm, text, err)) continue;
        if (text.empty()) continue;

        // Drop results that arrive after the monitor already moved
        // on (should_stream cleared mid-inference); their text is
        // for an utterance that's already been dispatched.
        if (!g_voice.should_stream.load()) continue;

        std::lock_guard<std::mutex> lk(g_voice.mu);
        g_voice.last_stream_text       = std::move(text);
        g_voice.last_stream_finish_us  = voice_now_us();
        g_voice.last_stream_audio_size = covered_size;
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
// continuous_running is true. Owns the VAD state; gates the
// streaming worker via should_stream; consumes streaming results on
// end-of-speech and dispatches them to the user-supplied callback.
void voice_continuous_loop() {
    bool in_speech = false;
    int  silence_ms = 0;

    auto enter_speech = [&]() {
        // Reset streaming state so we never reuse the previous
        // utterance's transcript.
        std::lock_guard<std::mutex> lk(g_voice.mu);
        g_voice.last_stream_text.clear();
        g_voice.last_stream_finish_us = 0;
        g_voice.last_stream_audio_size = 0;
        g_voice.should_stream.store(true);
    };

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
            if (!in_speech) enter_speech();
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

        // End-of-speech reached. Stop streaming, then decide whether
        // to consume the latest streaming result or run a fresh
        // pass on the full buffer.
        g_voice.should_stream.store(false);

        std::vector<float> pcm;
        std::function<void(const std::string&, const std::string&)> cb;
        std::string streamed_text;
        int64_t     stream_finish_us = 0;
        size_t      stream_audio_size = 0;
        size_t      total_size = 0;
        {
            std::lock_guard<std::mutex> lk(g_voice.mu);
            pcm.swap(g_voice.buffer);
            total_size        = pcm.size();
            cb                = g_voice.on_utterance;
            streamed_text     = std::move(g_voice.last_stream_text);
            stream_finish_us  = g_voice.last_stream_finish_us;
            stream_audio_size = g_voice.last_stream_audio_size;
            g_voice.last_stream_text.clear();
            g_voice.last_stream_finish_us = 0;
            g_voice.last_stream_audio_size = 0;
        }
        in_speech  = false;
        silence_ms = 0;

        if (static_cast<int>(pcm.size()) < kMinUtteranceSamples) continue;

        // Use streaming result if it's recent and was on most of the
        // captured audio. The streaming pass missed at most one
        // sleep-interval's worth of samples plus the tail silence;
        // if the gap is bigger than that, the user must have spoken
        // a final word the streaming pass didn't cover — fall back.
        bool fresh = stream_finish_us != 0 &&
            (voice_now_us() - stream_finish_us) <
            kStreamFreshnessMs * 1000;
        bool covers = stream_audio_size + kStreamCoverageSlackSamples
            >= total_size;
        if (!streamed_text.empty() && fresh && covers) {
            if (cb) cb(streamed_text, "");
            continue;
        }

        // Fresh-pass fallback. Use the existing detached-worker
        // pattern with the transcribing flag to serialise.
        bool expected = false;
        if (!g_voice.transcribing.compare_exchange_strong(expected, true)) {
            continue;
        }

        std::thread([pcm = std::move(pcm), cb = std::move(cb)]() mutable {
            run_whisper(std::move(pcm), std::move(cb));
            g_voice.transcribing.store(false);
        }).detach();
    }

    // On exit, make sure the streaming worker is parked.
    g_voice.should_stream.store(false);
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
        g_voice.last_stream_text.clear();
        g_voice.last_stream_finish_us = 0;
        g_voice.last_stream_audio_size = 0;
    }
    g_voice.capturing.store(true);
    SDL_PauseAudioDevice(g_voice.capture_dev, 0);
    g_voice.should_stream.store(false);
    g_voice.continuous_running.store(true);
    g_voice.monitor_thread = std::thread(voice_continuous_loop);
    g_voice.stream_thread  = std::thread(voice_stream_loop);
    return true;
}

void voice_stop_continuous() {
    bool was_running = g_voice.continuous_running.exchange(false);
    g_voice.should_stream.store(false);

    if (g_voice.monitor_thread.joinable())
        g_voice.monitor_thread.join();
    if (g_voice.stream_thread.joinable())
        g_voice.stream_thread.join();

    if (!was_running) return;

    std::lock_guard<std::mutex> lk(g_voice.mu);
    if (g_voice.capture_dev) {
        SDL_PauseAudioDevice(g_voice.capture_dev, 1);
    }
    g_voice.capturing.store(false);
    g_voice.buffer.clear();
    g_voice.last_stream_text.clear();
    g_voice.last_stream_finish_us = 0;
    g_voice.last_stream_audio_size = 0;
}

#endif  // !__EMSCRIPTEN__
