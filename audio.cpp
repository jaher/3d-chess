#include "audio.h"

#include <SDL2/SDL.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

// The audio layer opens a single SDL audio device in callback mode
// and mixes everything in software. That sidesteps two problems the
// earlier queue-mode + second-device design had on web:
//   * Emscripten's SDL backend doesn't reliably open a second
//     SDL_AudioDevice (the browser exposes one AudioContext and
//     ``SDL_OpenAudioDevice`` in the web port is essentially a
//     singleton).
//   * ``SDL_ClearQueuedAudio`` used to wipe any music that was
//     queued alongside SFX, so dual-stream playback via a shared
//     queue wasn't possible.
//
// The device is opened with S16_LSB mono at 22050 Hz. Every clip is
// converted to that format at load time, so the mixer only has to
// sum int16 samples and clip the result.

namespace {

#ifdef __EMSCRIPTEN__
constexpr const char* SOUND_DIR = "/sounds/";
#else
constexpr const char* SOUND_DIR = "sounds/";
#endif

struct Clip {
    Uint8* buf = nullptr;
    Uint32 len = 0;
    bool   loaded = false;
    bool   owned_by_free = false;  // allocated via SDL_malloc vs SDL_LoadWAV
};

struct Voice {
    const Uint8* buf = nullptr;
    Uint32 len = 0;
    Uint32 pos = 0;
    bool   active = false;
    // Runtime-generated PCM (TTS): when non-empty, `buf` aliases
    // its `.data()` pointer. Lifetime is tied to the voice slot,
    // so the mixer never reads a freed buffer. Empty for static
    // SFX clips that point at g_clips[].
    std::vector<int16_t> owned_pcm;
};

struct MusicTrack {
    Uint8* buf = nullptr;
    Uint32 len = 0;
    Uint32 pos = 0;  // byte offset inside buf
    bool   owned_by_free = false;
    std::string name;
    bool   playing = false;
};

// Maximum number of overlapping SFX voices. Four is enough for a
// chess game — you rarely get more than one or two sounds on top of
// the music at a time.
constexpr size_t VOICE_COUNT = 8;

SDL_AudioDeviceID g_device = 0;
SDL_AudioSpec     g_have{};

std::array<Clip, static_cast<size_t>(SoundEffect::_Count)> g_clips{};
std::array<Voice, VOICE_COUNT>                              g_voices{};
MusicTrack                                                  g_music{};
std::mutex                                                  g_audio_mu;

const char* clip_filename(SoundEffect e) {
    switch (e) {
    case SoundEffect::Move:       return "move.wav";
    case SoundEffect::Capture:    return "capture.wav";
    case SoundEffect::Check:      return "check.wav";
    case SoundEffect::GlassBreak: return "glass-breaking.wav";
    case SoundEffect::Mistake:    return "mistake.wav";
    case SoundEffect::_Count:     return nullptr;
    }
    return nullptr;
}

bool load_wav_to_device_spec(const std::string& path,
                              Uint8** out_buf, Uint32* out_len,
                              bool* out_owned_by_free) {
    SDL_AudioSpec wav_spec{};
    Uint8* wav_buf = nullptr;
    Uint32 wav_len = 0;
    if (SDL_LoadWAV(path.c_str(), &wav_spec, &wav_buf, &wav_len) == nullptr) {
        std::fprintf(stderr, "audio: load %s failed: %s\n",
                     path.c_str(), SDL_GetError());
        return false;
    }

    if (wav_spec.format   == g_have.format &&
        wav_spec.channels == g_have.channels &&
        wav_spec.freq     == g_have.freq) {
        *out_buf = wav_buf;
        *out_len = wav_len;
        *out_owned_by_free = false;
        return true;
    }

    SDL_AudioCVT cvt{};
    int rc = SDL_BuildAudioCVT(
        &cvt,
        wav_spec.format, wav_spec.channels, wav_spec.freq,
        g_have.format,   g_have.channels,   g_have.freq);
    if (rc < 0) {
        std::fprintf(stderr, "audio: BuildAudioCVT %s failed: %s\n",
                     path.c_str(), SDL_GetError());
        SDL_FreeWAV(wav_buf);
        return false;
    }
    if (rc == 0) {
        *out_buf = wav_buf;
        *out_len = wav_len;
        *out_owned_by_free = false;
        return true;
    }

    cvt.len = static_cast<int>(wav_len);
    cvt.buf = static_cast<Uint8*>(SDL_malloc(cvt.len * cvt.len_mult));
    if (!cvt.buf) {
        std::fprintf(stderr, "audio: alloc for %s failed\n", path.c_str());
        SDL_FreeWAV(wav_buf);
        return false;
    }
    std::memcpy(cvt.buf, wav_buf, wav_len);
    SDL_FreeWAV(wav_buf);
    if (SDL_ConvertAudio(&cvt) < 0) {
        std::fprintf(stderr, "audio: ConvertAudio %s failed: %s\n",
                     path.c_str(), SDL_GetError());
        SDL_free(cvt.buf);
        return false;
    }
    *out_buf = cvt.buf;
    *out_len = static_cast<Uint32>(cvt.len_cvt);
    *out_owned_by_free = true;
    return true;
}

void load_clip(SoundEffect e) {
    Clip& c = g_clips[static_cast<size_t>(e)];
    if (c.loaded) return;
    const char* name = clip_filename(e);
    if (!name) return;
    std::string path = std::string(SOUND_DIR) + name;
    if (load_wav_to_device_spec(path, &c.buf, &c.len, &c.owned_by_free)) {
        c.loaded = true;
    }
}

// Mix a mono S16 source into the output, clipping the sum to [-1, 1].
void mix_into(int16_t* out, int out_samples, const Uint8* src, Uint32 src_bytes) {
    const int16_t* src16 = reinterpret_cast<const int16_t*>(src);
    int src_samples = static_cast<int>(src_bytes / 2);
    int n = (src_samples < out_samples) ? src_samples : out_samples;
    for (int i = 0; i < n; ++i) {
        int32_t sum = static_cast<int32_t>(out[i]) + src16[i];
        if (sum > INT16_MAX) sum = INT16_MAX;
        else if (sum < INT16_MIN) sum = INT16_MIN;
        out[i] = static_cast<int16_t>(sum);
    }
}

// SDL pulls this on its own audio thread whenever the device needs
// more samples. Everything inside runs under ``g_audio_mu`` so main-
// thread calls into ``audio_play`` / ``audio_music_*`` never race a
// buffer that's being read.
void SDLCALL audio_callback(void* /*user*/, Uint8* stream, int len) {
    std::memset(stream, 0, static_cast<size_t>(len));
    std::lock_guard<std::mutex> lk(g_audio_mu);

    int16_t* out = reinterpret_cast<int16_t*>(stream);
    int out_samples = len / 2;  // S16 mono

    // Music: loop by wrapping ``pos`` back to 0 once it hits the end.
    if (g_music.playing && g_music.buf && g_music.len > 1) {
        int written = 0;
        while (written < out_samples) {
            Uint32 remaining = g_music.len - g_music.pos;
            int chunk_samples =
                static_cast<int>(remaining / 2);
            if (chunk_samples > out_samples - written)
                chunk_samples = out_samples - written;
            mix_into(out + written, chunk_samples,
                     g_music.buf + g_music.pos,
                     static_cast<Uint32>(chunk_samples * 2));
            g_music.pos += static_cast<Uint32>(chunk_samples * 2);
            written += chunk_samples;
            if (g_music.pos >= g_music.len) g_music.pos = 0;
        }
    }

    // SFX voices: mix each active one, deactivate when it's drained.
    for (auto& v : g_voices) {
        if (!v.active || !v.buf) continue;
        Uint32 remaining = (v.pos < v.len) ? v.len - v.pos : 0;
        int chunk_samples = static_cast<int>(remaining / 2);
        if (chunk_samples > out_samples) chunk_samples = out_samples;
        mix_into(out, chunk_samples,
                 v.buf + v.pos,
                 static_cast<Uint32>(chunk_samples * 2));
        v.pos += static_cast<Uint32>(chunk_samples * 2);
        if (v.pos >= v.len) {
            v.active = false;
            v.buf = nullptr;
            // Free the dynamic buffer if this voice owned one.
            // Cheap deallocation under the audio lock — fine at
            // 22050 Hz × 1024-sample frames.
            v.owned_pcm.clear();
            v.owned_pcm.shrink_to_fit();
        }
    }
}

}  // namespace


// ───────────────────────────────────────────────────────────────────
// Public API
// ───────────────────────────────────────────────────────────────────

bool audio_init() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        std::fprintf(stderr, "audio: init failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_AudioSpec want{};
    want.freq     = 22050;
    want.format   = AUDIO_S16LSB;
    want.channels = 1;
    want.samples  = 1024;
    want.callback = audio_callback;
    want.userdata = nullptr;

    g_device = SDL_OpenAudioDevice(nullptr, 0, &want, &g_have, 0);
    if (g_device == 0) {
        std::fprintf(stderr, "audio: open device failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(g_device, 0);

    for (size_t i = 0; i < g_clips.size(); ++i) {
        load_clip(static_cast<SoundEffect>(i));
    }
    return true;
}

void audio_shutdown() {
    audio_music_stop();
    if (g_device) {
        SDL_CloseAudioDevice(g_device);
        g_device = 0;
    }
    // Clips are read by the audio callback; the device must be
    // closed before we free their buffers.
    for (auto& c : g_clips) {
        if (c.buf) {
            if (c.owned_by_free) SDL_free(c.buf);
            else                 SDL_FreeWAV(c.buf);
            c.buf = nullptr;
        }
        c.loaded = false;
        c.owned_by_free = false;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

float audio_clip_duration_seconds(SoundEffect effect) {
    size_t i = static_cast<size_t>(effect);
    if (i >= g_clips.size()) return 0.0f;
    const Clip& c = g_clips[i];
    if (!c.loaded || c.len == 0) return 0.0f;
    // Clips are stored in the device spec (mono S16 at g_have.freq).
    int bytes_per_sec = g_have.freq * g_have.channels *
                        SDL_AUDIO_BITSIZE(g_have.format) / 8;
    if (bytes_per_sec <= 0) return 0.0f;
    return static_cast<float>(c.len) / static_cast<float>(bytes_per_sec);
}

void audio_play(SoundEffect effect) {
    if (g_device == 0) return;
    size_t i = static_cast<size_t>(effect);
    if (i >= g_clips.size()) return;
    const Clip& c = g_clips[i];
    if (!c.loaded) return;

    std::lock_guard<std::mutex> lk(g_audio_mu);
    // Grab a free voice slot, or the one that's closest to finishing
    // if none are free (the older sound gets cut, which is the same
    // behaviour the old ClearQueuedAudio path had for rapid moves).
    Voice* chosen = nullptr;
    for (auto& v : g_voices) {
        if (!v.active) { chosen = &v; break; }
    }
    if (!chosen) {
        Uint32 best_remaining = UINT32_MAX;
        for (auto& v : g_voices) {
            Uint32 r = (v.pos < v.len) ? v.len - v.pos : 0;
            if (r < best_remaining) { best_remaining = r; chosen = &v; }
        }
    }
    if (!chosen) return;

    chosen->buf    = c.buf;
    chosen->len    = c.len;
    chosen->pos    = 0;
    chosen->active = true;
}

void audio_play_pcm(std::vector<int16_t> samples) {
    if (g_device == 0 || samples.empty()) return;

    std::lock_guard<std::mutex> lk(g_audio_mu);
    // Same slot allocation strategy as audio_play(): prefer a free
    // slot, fall back to the one closest to finishing if all 8 are
    // busy.
    Voice* chosen = nullptr;
    for (auto& v : g_voices) {
        if (!v.active) { chosen = &v; break; }
    }
    if (!chosen) {
        Uint32 best_remaining = UINT32_MAX;
        for (auto& v : g_voices) {
            Uint32 r = (v.pos < v.len) ? v.len - v.pos : 0;
            if (r < best_remaining) { best_remaining = r; chosen = &v; }
        }
    }
    if (!chosen) return;

    chosen->owned_pcm = std::move(samples);
    chosen->buf = reinterpret_cast<const Uint8*>(chosen->owned_pcm.data());
    chosen->len = static_cast<Uint32>(chosen->owned_pcm.size() *
                                       sizeof(int16_t));
    chosen->pos = 0;
    chosen->active = true;
}

bool audio_music_play(const char* filename) {
    if (!filename || !*filename || g_device == 0) return false;

    {
        std::lock_guard<std::mutex> lk(g_audio_mu);
        // Idempotent: same track already looping → nothing to do.
        if (g_music.playing && g_music.name == filename) return true;
    }

    // Load outside the lock — SDL_LoadWAV can do real I/O.
    Uint8* buf = nullptr;
    Uint32 len = 0;
    bool   owned_by_free = false;
    std::string path = std::string(SOUND_DIR) + filename;
    if (!load_wav_to_device_spec(path, &buf, &len, &owned_by_free)) {
        return false;
    }

    // Swap the new track in under the lock. Free the previous buffer
    // AFTER the swap so the callback can never read a freed pointer.
    Uint8* old_buf = nullptr;
    bool   old_free = false;
    {
        std::lock_guard<std::mutex> lk(g_audio_mu);
        old_buf  = g_music.buf;
        old_free = g_music.owned_by_free;

        g_music.buf = buf;
        g_music.len = len;
        g_music.pos = 0;
        g_music.owned_by_free = owned_by_free;
        g_music.name = filename;
        g_music.playing = true;
    }
    if (old_buf) {
        if (old_free) SDL_free(old_buf);
        else          SDL_FreeWAV(old_buf);
    }
    return true;
}

void audio_music_stop() {
    Uint8* old_buf = nullptr;
    bool   old_free = false;
    {
        std::lock_guard<std::mutex> lk(g_audio_mu);
        old_buf  = g_music.buf;
        old_free = g_music.owned_by_free;
        g_music.buf = nullptr;
        g_music.len = 0;
        g_music.pos = 0;
        g_music.owned_by_free = false;
        g_music.name.clear();
        g_music.playing = false;
    }
    if (old_buf) {
        if (old_free) SDL_free(old_buf);
        else          SDL_FreeWAV(old_buf);
    }
}

void audio_music_tick() {
    // Retained for API compatibility — the audio callback now loops
    // the track by wrapping ``g_music.pos`` back to 0 at end-of-clip,
    // so there's nothing to do on the main thread.
}
