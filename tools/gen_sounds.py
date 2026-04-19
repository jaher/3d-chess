#!/usr/bin/env python3
"""
Synthesize chess SFX that try to sound like wood-on-wood.

Approach per sound:
  1. Very sharp attack (< 1 ms) — the "impact".
  2. Short noise transient, low-pass filtered so it sounds like wood
     texture rather than white noise.
  3. A handful of **inharmonic** damped sine modes (real wood
     resonates at non-integer frequency ratios — 1 : 1.87 : 2.61 : …
     sounds like wood, 1 : 2 : 3 sounds like a bell).
  4. Fast exponential decay (20–80 ms).

Output lands in ``sounds/`` at the repo root as mono 16-bit PCM
22050 Hz WAVs. Re-run after tweaking:

    python3 tools/gen_sounds.py
"""

import math
import random
import struct
import wave
from pathlib import Path

SR = 22050


def write_wav(path: Path, samples: list[float]) -> None:
    """Normalize the peak to 0.9 full-scale (so clips don't clip and
    don't wander in loudness as synthesis tweaks move the peak
    around), then encode 16-bit little-endian PCM."""
    if samples:
        peak = max(abs(s) for s in samples)
        if peak > 0:
            gain = 0.9 / peak
            samples = [s * gain for s in samples]
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        data = b"".join(
            struct.pack("<h", max(-32768, min(32767, int(s * 32767))))
            for s in samples
        )
        w.writeframes(data)


def click(
    *,
    fundamental: float,
    partials: list[float],
    mode_decays: list[float],
    noise_amp: float,
    noise_decay: float,
    lp_alpha: float,
    duration: float,
    seed: int,
) -> list[float]:
    """Compose a single percussive wood click.

    ``partials`` lists the non-harmonic frequency ratios of the
    resonant modes; each gets its own ``mode_decays`` time constant.
    ``noise_amp`` sets the impact transient level and ``lp_alpha`` is
    a 1-pole low-pass coefficient (0 = full lowpass, 1 = pass-through)
    applied to the excitation noise so it sounds like struck wood
    rather than white-noise hiss."""
    n = int(duration * SR)
    out = [0.0] * n
    random.seed(seed)
    white = [random.random() * 2 - 1 for _ in range(n)]

    # Low-pass filter on the noise (1-pole: y[i] = alpha * x[i] +
    # (1 - alpha) * y[i-1]). Tuned so high frequencies are attenuated
    # and the residual noise is wood-like rather than sharp hiss.
    lp_prev = 0.0
    for i in range(n):
        lp_prev = lp_alpha * white[i] + (1.0 - lp_alpha) * lp_prev
        t = i / SR
        # Near-instantaneous attack (10 samples ≈ 0.45 ms).
        attack = 1.0 if i >= 10 else i / 10.0
        # Noise transient decays very fast — this IS the impact.
        noise = noise_amp * lp_prev * attack * math.exp(-t / noise_decay)
        # Inharmonic damped resonant modes give the "wood body".
        body = 0.0
        for ratio, decay in zip(partials, mode_decays):
            body += (
                math.sin(2.0 * math.pi * fundamental * ratio * t)
                * attack * math.exp(-t / decay)
            )
        body /= max(1, len(partials))
        out[i] = noise + 0.55 * body
    return out


def make_move() -> list[float]:
    """Small piece on wood: a crisp high click, very short."""
    return click(
        fundamental=900,
        partials=[1.0, 1.87, 2.61, 4.12],
        mode_decays=[0.022, 0.016, 0.011, 0.007],
        noise_amp=0.75,
        noise_decay=0.004,
        lp_alpha=0.35,
        duration=0.09,
        seed=1,
    )


def make_capture() -> list[float]:
    """Heavier piece striking — lower body, longer tail, more impact."""
    return click(
        fundamental=340,
        partials=[1.0, 1.71, 2.49, 3.88, 5.65],
        mode_decays=[0.070, 0.050, 0.032, 0.020, 0.014],
        noise_amp=0.95,
        noise_decay=0.012,
        lp_alpha=0.25,
        duration=0.18,
        seed=2,
    )


def make_check() -> list[float]:
    """Sharper, slightly brighter click so check is audibly distinct
    from a plain move without drifting into 'chime' territory."""
    return click(
        fundamental=1350,
        partials=[1.0, 1.94, 2.83, 4.30],
        mode_decays=[0.028, 0.020, 0.013, 0.009],
        noise_amp=0.70,
        noise_decay=0.005,
        lp_alpha=0.40,
        duration=0.11,
        seed=3,
    )


def make_castle() -> list[float]:
    """Two move-clicks about 55 ms apart (king + rook)."""
    s1 = make_move()
    gap = [0.0] * int(0.055 * SR)
    s2 = click(
        fundamental=950,
        partials=[1.0, 1.85, 2.60, 4.10],
        mode_decays=[0.020, 0.014, 0.010, 0.006],
        noise_amp=0.72,
        noise_decay=0.004,
        lp_alpha=0.35,
        duration=0.08,
        seed=5,
    )
    return s1 + gap + s2


def main() -> None:
    repo_root = Path(__file__).resolve().parent.parent
    out_dir = repo_root / "sounds"
    out_dir.mkdir(exist_ok=True)
    for name, fn in (
        ("move.wav", make_move),
        ("capture.wav", make_capture),
        ("check.wav", make_check),
        ("castle.wav", make_castle),
    ):
        write_wav(out_dir / name, fn())
        size = (out_dir / name).stat().st_size
        print(f"  {name}: {size:,} bytes")


if __name__ == "__main__":
    main()
