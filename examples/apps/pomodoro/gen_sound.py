#!/usr/bin/env python3
"""Generate sounds/ding.wav — the pomodoro phase-change chime.

Pure synthesis (no assets, no deps): 0.3s 880Hz sine with an exponential
fade-out, 22050 Hz 16-bit mono. Re-run after changing parameters; the wav is
checked in so builds don't need Python.
"""
import math
import struct
import wave
from pathlib import Path

RATE = 22050
DUR = 0.3          # seconds
FREQ = 880.0       # A5
AMP = 0.55         # peak amplitude (0..1)

out = Path(__file__).parent / "sounds" / "ding.wav"
out.parent.mkdir(parents=True, exist_ok=True)

n = int(RATE * DUR)
frames = bytearray()
for i in range(n):
    t = i / RATE
    env = math.exp(-6.0 * t / DUR)           # exponential fade-out
    attack = min(1.0, i / (0.005 * RATE))    # 5ms attack, no click
    v = AMP * env * attack * math.sin(2 * math.pi * FREQ * t)
    frames += struct.pack("<h", int(max(-1.0, min(1.0, v)) * 32767))

with wave.open(str(out), "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(RATE)
    w.writeframes(bytes(frames))
print(f"wrote {out} ({n} samples, {DUR}s @ {RATE}Hz)")
