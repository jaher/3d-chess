#pragma once

// Lightweight SFX layer on top of SDL2 audio. One short WAV per
// effect, mixed via SDL_QueueAudio. No-op if audio_init() failed.

enum class SoundEffect {
    Move,
    Capture,
    Check,
    _Count,
};

bool audio_init();
void audio_shutdown();
void audio_play(SoundEffect effect);
