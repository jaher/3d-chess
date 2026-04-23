#pragma once

// Lightweight SFX layer on top of SDL2 audio. One short WAV per
// effect, mixed via SDL_QueueAudio. No-op if audio_init() failed.

enum class SoundEffect {
    Move,
    Capture,
    Check,
    GlassBreak,
    Mistake,
    _Count,
};

bool audio_init();
void audio_shutdown();
void audio_play(SoundEffect effect);

// Playback duration of a loaded SFX, in seconds. Returns 0 if the
// clip isn't loaded (audio_init failed, or the file was missing).
float audio_clip_duration_seconds(SoundEffect effect);

// Background-music slot (looped). Opens a second SDL audio device
// so the music and SFX streams don't fight — SFX can still clear
// their own queue without truncating the music.
//
//   audio_music_play("intro_music.wav") — loads once, begins loop.
//   audio_music_stop()                  — silence + free.
//   audio_music_tick()                  — call once per frame to
//                                          re-queue the clip when
//                                          the tail drains.
bool audio_music_play(const char* filename);
void audio_music_stop();
void audio_music_tick();
