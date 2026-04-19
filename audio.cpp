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
    Uint8* buf = nullptr;
    Uint32 len = 0;
    SDL_AudioSpec spec{};
    bool loaded = false;
};

SDL_AudioDeviceID g_device = 0;
SDL_AudioSpec     g_have{};
std::array<Clip, static_cast<size_t>(SoundEffect::_Count)> g_clips{};

const char* clip_filename(SoundEffect e) {
    switch (e) {
    case SoundEffect::Move:    return "move.wav";
    case SoundEffect::Capture: return "capture.wav";
    case SoundEffect::Check:   return "check.wav";
    case SoundEffect::_Count:  return nullptr;
    }
    return nullptr;
}

void load_clip(SoundEffect e) {
    Clip& c = g_clips[static_cast<size_t>(e)];
    if (c.loaded) return;
    const char* name = clip_filename(e);
    if (!name) return;
    std::string path = std::string(SOUND_DIR) + name;
    if (SDL_LoadWAV(path.c_str(), &c.spec, &c.buf, &c.len) == nullptr) {
        std::fprintf(stderr, "audio: load %s failed: %s\n",
                     path.c_str(), SDL_GetError());
        return;
    }
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
            SDL_FreeWAV(c.buf);
            c.buf = nullptr;
        }
        c.loaded = false;
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
