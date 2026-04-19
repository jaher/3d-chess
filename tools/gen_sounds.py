#!/usr/bin/env python3
"""
Synthesize the chess SFX used by the game (move / capture / check /
castle). Output lands in ``sounds/`` at the repo root — mono 16-bit
PCM 22050 Hz WAV files, small enough to commit.

Re-run after tweaking envelopes / frequencies:

    python3 tools/gen_sounds.py
"""

import math
import os
import random
import struct
import wave
from pathlib import Path

SR = 22050


def write_wav(path: Path, samples: list[float]) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        data = b"".join(
            struct.pack("<h", max(-32768, min(32767, int(s * 32767))))
            for s in samples
        )
        w.writeframes(data)


def env(t: float, attack: float, decay: float) -> float:
    """Quick-attack / exponential-decay envelope."""
    a = min(1.0, t / attack) if attack > 0 else 1.0
    return a * math.exp(-t / decay)


def make_move() -> list[float]:
    """Sharp wooden click with pitched body + short decay (~100 ms)."""
    n = int(0.10 * SR)
    out: list[float] = []
    random.seed(1)
    for i in range(n):
        t = i / SR
        e = env(t, 0.001, 0.025)
        s = 0.55 * math.sin(2 * math.pi * 520 * t)
        s += 0.30 * math.sin(2 * math.pi * 820 * t)
        s += 0.20 * math.sin(2 * math.pi * 1380 * t)
        s += 0.25 * (random.random() * 2 - 1)
        out.append(s * e * 0.55)
    return out


def make_capture() -> list[float]:
    """Heavier thud — lower pitch, more noise, slightly longer tail."""
    n = int(0.18 * SR)
    out: list[float] = []
    random.seed(2)
    for i in range(n):
        t = i / SR
        e = env(t, 0.002, 0.06)
        s = 0.55 * math.sin(2 * math.pi * 190 * t)
        s += 0.35 * math.sin(2 * math.pi * 330 * t)
        s += 0.20 * math.sin(2 * math.pi * 620 * t)
        s += 0.45 * (random.random() * 2 - 1)
        out.append(s * e * 0.65)
    return out


def make_check() -> list[float]:
    """Two-tone chime (A5 + E6)."""
    n = int(0.35 * SR)
    out: list[float] = []
    for i in range(n):
        t = i / SR
        e = env(t, 0.003, 0.09)
        s = 0.5 * math.sin(2 * math.pi * 880 * t)
        s += 0.5 * math.sin(2 * math.pi * 1318 * t)
        out.append(s * e * 0.45)
    return out


def make_castle() -> list[float]:
    """Two short move-clicks ~60 ms apart."""
    s1 = make_move()
    gap = [0.0] * int(0.06 * SR)
    s2 = make_move()[: int(0.08 * SR)]
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
