#!/usr/bin/env python3
"""Generate sounds/ring.wav — the alarm preview chime.

Pure synthesis (no assets, no deps): four short C6 pulses with exponential
fade-outs, 22050 Hz 16-bit mono, ~0.66s total. The wav is checked in so
builds don't need Python.
"""
import math
import struct
import wave
from pathlib import Path

RATE = 22050
FREQ = 1046.5      # C6
PULSE = 0.11       # seconds per beep
GAP = 0.055        # silence between beeps
PULSES = 4
AMP = 0.5

out = Path(__file__).parent / "sounds" / "ring.wav"
out.parent.mkdir(parents=True, exist_ok=True)

frames = bytearray()
for p in range(PULSES):
    n = int(RATE * PULSE)
    for i in range(n):
        t = i / RATE
        env = math.exp(-5.0 * t / PULSE)
        attack = min(1.0, i / (0.004 * RATE))
        v = AMP * env * attack * math.sin(2 * math.pi * FREQ * t)
        frames += struct.pack("<h", int(max(-1.0, min(1.0, v)) * 32767))
    frames += b"\x00\x00" * int(RATE * GAP)

with wave.open(str(out), "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(RATE)
    w.writeframes(bytes(frames))
print(f"wrote {out} ({len(frames)//2} samples @ {RATE}Hz)")
