#!/usr/bin/env python3
"""Generate small deterministic PCM cues for the engine's Android audio path."""
from __future__ import annotations

import math
import struct
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / "app/src/main/assets/audio"
RATE = 44_100


def write_clip(name: str, duration: float, synth) -> None:
    frame_count = round(duration * RATE)
    pcm = bytearray()
    for frame in range(frame_count):
        t = frame / RATE
        value = max(-0.98, min(0.98, synth(t, duration)))
        pcm.extend(struct.pack("<h", round(value * (32767 if value >= 0 else 32768))))
    ROOT.mkdir(parents=True, exist_ok=True)
    with wave.open(str(ROOT / name), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(RATE)
        output.writeframes(pcm)


def relic(t: float, duration: float) -> float:
    notes = ((0.000, 880.0, 0.19), (0.045, 1320.0, 0.15),
             (0.100, 1760.0, 0.12), (0.165, 2349.0, 0.08))
    total = 0.0
    for delay, frequency, amplitude in notes:
        age = t - delay
        if age >= 0.0:
            total += amplitude * math.exp(-7.5 * age) * math.sin(2.0 * math.pi * frequency * age)
    return total


def jump(t: float, duration: float) -> float:
    progress = t / duration
    envelope = math.sin(math.pi * progress) ** 1.25
    phase = 2.0 * math.pi * (190.0 * t + 220.0 * t * t / duration)
    return envelope * (0.62 * math.sin(phase) + 0.14 * math.sin(phase * 0.5))


def gate(t: float, duration: float) -> float:
    progress = t / duration
    attack = min(1.0, t / 0.22)
    release = min(1.0, (duration - t) / 0.30)
    envelope = max(0.0, min(attack, release))
    swell = 0.5 + 0.5 * math.sin(math.pi * progress)
    carrier = (0.48 * math.sin(2.0 * math.pi * 110.0 * t)
               + 0.30 * math.sin(2.0 * math.pi * 164.81 * t + 0.3)
               + 0.18 * math.sin(2.0 * math.pi * 220.0 * t + 0.8))
    return envelope * (0.38 + 0.62 * swell) * carrier


def gate_hum(t: float, duration: float) -> float:
    modulation = 0.78 + 0.22 * math.sin(2.0 * math.pi * 2.0 * t)
    chord = (0.48 * math.sin(2.0 * math.pi * 110.0 * t)
             + 0.31 * math.sin(2.0 * math.pi * 165.0 * t + 0.18)
             + 0.21 * math.sin(2.0 * math.pi * 220.0 * t + 0.42))
    return 0.105 * modulation * chord


if __name__ == "__main__":
    write_clip("relic.wav", 0.72, relic)
    write_clip("jump.wav", 0.34, jump)
    write_clip("gate.wav", 1.25, gate)
    write_clip("gate_hum.wav", 2.0, gate_hum)
