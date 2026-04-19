#include "audio.h"

#include <SDL2/SDL.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

#ifdef __EMSCRIPTEN__
constexpr const char* SOUND_DIR = "/sounds/";
#else
constexpr const char* SOUND_DIR = "sounds/";
#endif

struct Clip {
    // Owned buffer in the device's spec (format / channels / rate),
    // so SDL_QueueAudio can copy straight through without having to
    // resample on every play.
    Uint8* buf = nullptr;
    Uint32 len = 0;
    bool   loaded = false;
    bool   owned_by_free = false;  // allocated via SDL_malloc vs SDL_LoadWAV
};

SDL_AudioDeviceID g_device = 0;        // SFX device
SDL_AudioDeviceID g_music_device = 0;  // background-music device
SDL_AudioSpec     g_have{};
std::array<Clip, static_cast<size_t>(SoundEffect::_Count)> g_clips{};

struct MusicTrack {
    Uint8* buf = nullptr;
    Uint32 len = 0;
    bool   owned_by_free = false;
    std::string name;       // filename we last loaded
    bool   playing = false;
};
MusicTrack g_music{};

const char* clip_filename(SoundEffect e) {
    switch (e) {
    case SoundEffect::Move:       return "move.wav";
    case SoundEffect::Capture:    return "capture.wav";
    case SoundEffect::Check:      return "check.wav";
    case SoundEffect::GlassBreak: return "glass-breaking.wav";
    case SoundEffect::_Count:     return nullptr;
    }
    return nullptr;
}

// Load a WAV at ``path``, resampling / downmixing to the output
// device spec (g_have) if needed. Returns a buffer caller must
// eventually free — ``out_owned_by_free`` says whether to call
// ``SDL_free`` (true) or ``SDL_FreeWAV`` (false). Returns false on
// failure, in which case no output is written.
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

}  // namespace

bool audio_init() {
    // SDL_Init is OK to call multiple times on independent subsystems
    // — the web build already initialises SDL_VIDEO / SDL_EVENTS in
    // main_web.cpp, the desktop build is GTK-native so audio is the
    // first (and only) SDL subsystem it touches.
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        std::fprintf(stderr, "audio: init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec want{};
    want.freq     = 22050;
    want.format   = AUDIO_S16LSB;
    want.channels = 1;
    want.samples  = 512;
    g_device = SDL_OpenAudioDevice(nullptr, 0, &want, &g_have, 0);
    if (g_device == 0) {
        std::fprintf(stderr, "audio: open device failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(g_device, 0);

    // Second device for music, matching the SFX spec. Keeps SFX
    // clears from wiping the music queue.
    SDL_AudioSpec music_want = want;
    SDL_AudioSpec music_have{};
    g_music_device = SDL_OpenAudioDevice(
        nullptr, 0, &music_want, &music_have, 0);
    if (g_music_device == 0) {
        std::fprintf(stderr, "audio: open music device failed: %s\n",
                     SDL_GetError());
        // Not fatal — SFX still works.
    } else {
        // Keep paused until audio_music_play() starts a track.
        SDL_PauseAudioDevice(g_music_device, 1);
    }

    for (size_t i = 0; i < g_clips.size(); ++i) {
        load_clip(static_cast<SoundEffect>(i));
    }
    return true;
}

void audio_shutdown() {
    audio_music_stop();
    for (auto& c : g_clips) {
        if (c.buf) {
            if (c.owned_by_free) SDL_free(c.buf);
            else                 SDL_FreeWAV(c.buf);
            c.buf = nullptr;
        }
        c.loaded = false;
        c.owned_by_free = false;
    }
    if (g_music_device) {
        SDL_CloseAudioDevice(g_music_device);
        g_music_device = 0;
    }
    if (g_device) {
        SDL_CloseAudioDevice(g_device);
        g_device = 0;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool audio_music_play(const char* filename) {
    if (!filename || !*filename || g_music_device == 0) return false;
    // Idempotent: same track already playing → do nothing.
    if (g_music.playing && g_music.name == filename) return true;

    audio_music_stop();

    std::string path = std::string(SOUND_DIR) + filename;
    if (!load_wav_to_device_spec(path, &g_music.buf, &g_music.len,
                                  &g_music.owned_by_free)) {
        return false;
    }
    g_music.name = filename;
    g_music.playing = true;
    SDL_ClearQueuedAudio(g_music_device);
    SDL_QueueAudio(g_music_device, g_music.buf, g_music.len);
    SDL_PauseAudioDevice(g_music_device, 0);
    return true;
}

void audio_music_stop() {
    if (g_music_device) {
        SDL_PauseAudioDevice(g_music_device, 1);
        SDL_ClearQueuedAudio(g_music_device);
    }
    if (g_music.buf) {
        if (g_music.owned_by_free) SDL_free(g_music.buf);
        else                        SDL_FreeWAV(g_music.buf);
        g_music.buf = nullptr;
    }
    g_music.len = 0;
    g_music.owned_by_free = false;
    g_music.name.clear();
    g_music.playing = false;
}

void audio_music_tick() {
    if (!g_music.playing || g_music_device == 0 || !g_music.buf) return;
    // Keep at least one full clip length queued so there's no gap
    // at the loop point.
    Uint32 queued = SDL_GetQueuedAudioSize(g_music_device);
    if (queued < g_music.len) {
        SDL_QueueAudio(g_music_device, g_music.buf, g_music.len);
    }
}

void audio_play(SoundEffect effect) {
    if (g_device == 0) return;
    size_t i = static_cast<size_t>(effect);
    if (i >= g_clips.size()) return;
    const Clip& c = g_clips[i];
    if (!c.loaded) return;
    // Clear the queue so a new move's sound interrupts the lingering
    // tail of the previous one — keeps rapid moves from stacking.
    SDL_ClearQueuedAudio(g_device);
    SDL_QueueAudio(g_device, c.buf, c.len);
}
