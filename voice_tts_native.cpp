// Desktop TTS impl — espeak-ng via static-linked third_party submodule.
// Voice data path is baked in at compile time via -DESPEAK_DATA_PATH
// pointing at $(repo)/third_party/espeak-ng (the parent of the
// `espeak-ng-data/` directory the library expects).
//
// Threading: espeak-ng is built with USE_ASYNC=OFF, so espeak_Synth
// blocks on the calling thread until synthesis is complete. We run
// it on a dedicated worker thread that drains a request queue, so
// the GTK main thread never stalls on TTS work and so we never have
// two synth_callbacks racing each other (the library is not
// thread-safe). The callback collects PCM samples for one
// utterance, then on MSG_TERMINATED hands the buffer to
// audio_play_pcm where the existing 8-voice mixer plays it back.

#ifndef __EMSCRIPTEN__

#include "voice_tts.h"

#include "audio.h"

#include <espeak-ng/speak_lib.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

std::mutex                g_init_mu;
std::atomic<bool>         g_inited{false};
// Sticky-failed flag: once espeak_Initialize fails we stop retrying
// on every voice_tts_speak() call so a missing data path doesn't
// flood stderr with one error per move.
std::atomic<bool>         g_init_failed{false};

// Worker-thread state.
std::thread               g_worker;
std::mutex                g_q_mu;
std::condition_variable   g_q_cv;
std::deque<std::string>   g_q;
std::atomic<bool>         g_worker_running{false};

// Per-utterance PCM buffer. The synth callback runs on the worker
// thread (espeak_Synth is synchronous) so a plain global is fine
// as long as we serialise utterances one at a time, which the
// queue does by construction.
std::vector<int16_t>      g_synth_pcm;

int synth_callback(short* wav, int numsamples, espeak_EVENT* /*events*/) {
    // Just accumulate. MSG_TERMINATED isn't reliably emitted in
    // every espeak-ng configuration, so dispatch happens in the
    // worker after espeak_Synchronize() returns, not from the
    // events list.
    if (numsamples > 0 && wav) {
        g_synth_pcm.insert(g_synth_pcm.end(), wav, wav + numsamples);
    }
    return 0;  // continue
}

void worker_loop() {
    while (g_worker_running.load()) {
        std::string text;
        {
            std::unique_lock<std::mutex> lk(g_q_mu);
            g_q_cv.wait(lk, [] {
                return !g_q.empty() || !g_worker_running.load();
            });
            if (!g_worker_running.load()) return;
            text = std::move(g_q.front());
            g_q.pop_front();
        }
        if (text.empty()) continue;

        std::fprintf(stderr,
            "[voice_tts] synth start: \"%s\"\n", text.c_str());
        g_synth_pcm.clear();
        unsigned int unique_id = 0;
        espeak_ERROR rc = espeak_Synth(
            text.c_str(),
            text.size() + 1,   // size in bytes including null
            /*position=*/0,
            POS_CHARACTER,
            /*end_position=*/0,
            espeakCHARS_UTF8,
            &unique_id,
            /*user_data=*/nullptr);
        if (rc != EE_OK) {
            std::fprintf(stderr, "voice_tts: espeak_Synth rc=%d\n", rc);
            g_synth_pcm.clear();
            continue;
        }
        // Block until synthesis is fully complete — espeak_Synth
        // schedules the work; espeak_Synchronize waits for the
        // synth_callback to be drained. After this returns,
        // g_synth_pcm holds every sample for this utterance.
        espeak_Synchronize();
        std::fprintf(stderr,
            "[voice_tts] synth done — %zu samples queued for playback\n",
            g_synth_pcm.size());
        if (!g_synth_pcm.empty()) {
            audio_play_pcm(std::move(g_synth_pcm));
            g_synth_pcm = {};
        }
    }
}

}  // namespace

bool voice_tts_init(std::string& err_out) {
    std::lock_guard<std::mutex> lk(g_init_mu);
    if (g_inited.load()) return true;

#ifndef ESPEAK_DATA_PATH
#  define ESPEAK_DATA_PATH nullptr
#endif
    int rate = espeak_Initialize(
        AUDIO_OUTPUT_RETRIEVAL,
        /*buflength_ms=*/200,
        ESPEAK_DATA_PATH,
        /*options=*/0);
    if (rate < 0) {
        err_out = "espeak_Initialize failed (data path: ";
#ifdef ESPEAK_DATA_PATH_STR
        err_out += ESPEAK_DATA_PATH_STR;
#else
        err_out += "<compile-time path>";
#endif
        err_out += ")";
        return false;
    }
    espeak_SetSynthCallback(synth_callback);
    // espeak-ng's data tree only has voice variants under voices/!v/
    // and language configs under lang/gmw/en — there is no
    // voices/en-us file, so SetVoiceByName("en-us") fails silently
    // and synth produces no PCM. SetVoiceByProperties with the
    // language code routes through espeak's language map and
    // resolves correctly to the en/en-us voice. Fall back to plain
    // "en" if the more specific code fails (e.g. on stripped data
    // builds).
    espeak_VOICE voice_props{};
    voice_props.languages = "en-us";
    espeak_ERROR vrc = espeak_SetVoiceByProperties(&voice_props);
    if (vrc != EE_OK) {
        std::fprintf(stderr,
            "voice_tts: SetVoiceByProperties(en-us) rc=%d, "
            "falling back to en\n", vrc);
        voice_props.languages = "en";
        vrc = espeak_SetVoiceByProperties(&voice_props);
        if (vrc != EE_OK) {
            std::fprintf(stderr,
                "voice_tts: SetVoiceByProperties(en) rc=%d "
                "(synth will use whatever default loaded)\n", vrc);
        }
    }
    std::fprintf(stderr,
        "voice_tts: initialised — espeak rate=%d Hz "
        "(device 22050 Hz; data path=%s)\n",
        rate,
#ifdef ESPEAK_DATA_PATH
        ESPEAK_DATA_PATH
#else
        "(default)"
#endif
    );
    // espeak's en-us default sample rate is 22050, matching what
    // audio.cpp opens. If a future voice change drops to 16000, the
    // mismatch is audible but not fatal — we'd need to resample in
    // audio_play_pcm at that point.
    if (rate != 22050) {
        std::fprintf(stderr,
            "voice_tts: warning — espeak rate %d != device rate 22050; "
            "playback may sound off-pitch\n", rate);
    }

    g_worker_running.store(true);
    g_worker = std::thread(worker_loop);
    g_inited.store(true);
    return true;
}

void voice_tts_speak(const std::string& text) {
    if (text.empty()) return;
    // Lazy init: voice_tts_enabled defaults to true, so the first
    // move call may arrive without any explicit toggle click. Init
    // here so it Just Works; sticky-failed avoids retry storms if
    // the data path is bad.
    if (!g_inited.load() && !g_init_failed.load()) {
        std::string err;
        if (!voice_tts_init(err)) {
            std::fprintf(stderr,
                "voice_tts: lazy init failed: %s\n", err.c_str());
            g_init_failed.store(true);
            return;
        }
    }
    if (!g_inited.load()) return;
    {
        std::lock_guard<std::mutex> lk(g_q_mu);
        // Drop in-flight backlog if it grows past a couple of
        // utterances — the user just wants the latest move read,
        // not a historical recap.
        if (g_q.size() >= 3) g_q.clear();
        g_q.push_back(text);
    }
    g_q_cv.notify_one();
}

void voice_tts_shutdown() {
    if (!g_inited.exchange(false)) return;
    g_worker_running.store(false);
    g_q_cv.notify_all();
    // Don't join — espeak_Synthesize is uninterruptible and may be
    // mid-utterance when the user closes the window. A blocking
    // join would stall process exit for up to ~1s per pending
    // utterance, which the user reported as a hang. Detaching
    // hands the worker to the OS reaper; the loop exits cleanly
    // on its next dequeue (the running flag flipped above), and
    // any in-flight synth runs to completion in the background
    // before being torn down at process exit.
    if (g_worker.joinable()) g_worker.detach();
    // Skip espeak_Terminate too — calling it while a detached
    // worker may still be inside espeak_Synth races on the
    // library's internal state. The process is exiting anyway, so
    // reclaiming the data tables is moot.
}

#endif  // !__EMSCRIPTEN__
