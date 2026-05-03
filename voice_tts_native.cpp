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

int synth_callback(short* wav, int numsamples, espeak_EVENT* events) {
    if (numsamples > 0 && wav) {
        g_synth_pcm.insert(g_synth_pcm.end(), wav, wav + numsamples);
    }
    if (events) {
        for (espeak_EVENT* e = events;
             e->type != espeakEVENT_LIST_TERMINATED; ++e) {
            if (e->type == espeakEVENT_MSG_TERMINATED) {
                if (!g_synth_pcm.empty()) {
                    audio_play_pcm(std::move(g_synth_pcm));
                    g_synth_pcm = {};  // reset, move() left it empty
                }
                break;
            }
        }
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
            // Ensure the next utterance starts with an empty buffer
            // even if the callback never saw MSG_TERMINATED.
            g_synth_pcm.clear();
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
    espeak_SetVoiceByName("en-us");
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
    if (!g_inited.load() || text.empty()) return;
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
    std::lock_guard<std::mutex> lk(g_init_mu);
    if (!g_inited.load()) return;
    g_worker_running.store(false);
    g_q_cv.notify_all();
    if (g_worker.joinable()) g_worker.join();
    espeak_Terminate();
    g_inited.store(false);
}

#endif  // !__EMSCRIPTEN__
