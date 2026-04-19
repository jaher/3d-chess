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

SDL_AudioDeviceID g_device = 0;
SDL_AudioSpec     g_have{};
std::array<Clip, static_cast<size_t>(SoundEffect::_Count)> g_clips{};

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

// Convert the raw WAV buffer into the device's output spec so any
// source format (stereo 44.1 kHz, mono 22 kHz, etc.) can be played
// through the single pre-opened audio device.
void load_clip(SoundEffect e) {
    Clip& c = g_clips[static_cast<size_t>(e)];
    if (c.loaded) return;
    const char* name = clip_filename(e);
    if (!name) return;
    std::string path = std::string(SOUND_DIR) + name;

    SDL_AudioSpec wav_spec{};
    Uint8* wav_buf = nullptr;
    Uint32 wav_len = 0;
    if (SDL_LoadWAV(path.c_str(), &wav_spec, &wav_buf, &wav_len) == nullptr) {
        std::fprintf(stderr, "audio: load %s failed: %s\n",
                     path.c_str(), SDL_GetError());
        return;
    }

    // If the WAV already matches the device spec, stash it as-is.
    if (wav_spec.format   == g_have.format &&
        wav_spec.channels == g_have.channels &&
        wav_spec.freq     == g_have.freq) {
        c.buf = wav_buf;
        c.len = wav_len;
        c.owned_by_free = false;
        c.loaded = true;
        return;
    }

    // Otherwise run SDL_AudioCVT once at load time to resample /
    // downmix into the device's spec.
    SDL_AudioCVT cvt{};
    int rc = SDL_BuildAudioCVT(
        &cvt,
        wav_spec.format,   wav_spec.channels,   wav_spec.freq,
        g_have.format,     g_have.channels,     g_have.freq);
    if (rc < 0) {
        std::fprintf(stderr, "audio: BuildAudioCVT %s failed: %s\n",
                     path.c_str(), SDL_GetError());
        SDL_FreeWAV(wav_buf);
        return;
    }
    if (rc == 0) {
        // Spec differs only by a rate that happens to equal the
        // device's — SDL reports "no conversion needed".
        c.buf = wav_buf;
        c.len = wav_len;
        c.owned_by_free = false;
        c.loaded = true;
        return;
    }

    cvt.len = static_cast<int>(wav_len);
    cvt.buf = static_cast<Uint8*>(SDL_malloc(cvt.len * cvt.len_mult));
    if (!cvt.buf) {
        std::fprintf(stderr, "audio: alloc for %s failed\n", path.c_str());
        SDL_FreeWAV(wav_buf);
        return;
    }
    std::memcpy(cvt.buf, wav_buf, wav_len);
    SDL_FreeWAV(wav_buf);
    if (SDL_ConvertAudio(&cvt) < 0) {
        std::fprintf(stderr, "audio: ConvertAudio %s failed: %s\n",
                     path.c_str(), SDL_GetError());
        SDL_free(cvt.buf);
        return;
    }
    c.buf = cvt.buf;
    c.len = static_cast<Uint32>(cvt.len_cvt);
    c.owned_by_free = true;
    c.loaded = true;
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

    for (size_t i = 0; i < g_clips.size(); ++i) {
        load_clip(static_cast<SoundEffect>(i));
    }
    return true;
}

void audio_shutdown() {
    for (auto& c : g_clips) {
        if (c.buf) {
            if (c.owned_by_free) SDL_free(c.buf);
            else                 SDL_FreeWAV(c.buf);
            c.buf = nullptr;
        }
        c.loaded = false;
        c.owned_by_free = false;
    }
    if (g_device) {
        SDL_CloseAudioDevice(g_device);
        g_device = 0;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
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
