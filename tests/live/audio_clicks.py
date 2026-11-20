#!/usr/bin/env python3
"""Count audible clicks in a PCM dump written by the player (SIMPLEFFPLAY_TEST_AUDIO_DUMP).

The test source is a pure sine (default 440 Hz). For a sine of amplitude A the largest possible step between
two consecutive samples is 2*pi*f/fs*A; a step well above that (default 2.5x) is a waveform discontinuity,
which is what a click is. Steps closer than 10 ms apart are counted as one click.
Also reports silent gaps (runs of exact zeros >= 1 ms) after the first sound.

Usage: audio_clicks.py DUMP [--freq 440] [--factor 2.5] [--skip SECONDS] [--json]
"""
import argparse
import array
import json
import math
import struct
import sys


def analyze(path, tone=440.0, factor=2.5, skip=0.0):
    with open(path, "rb") as f:
        hdr = f.read(12)
        if len(hdr) < 12 or hdr[:4] != b"SFPA":
            raise ValueError("not a SimpleFFPlay audio dump")
        fs, ch = struct.unpack("=ii", hdr[4:])
        pcm = array.array("h")
        pcm.frombytes(f.read())
    if sys.byteorder != "little":
        pcm.byteswap()
    n = len(pcm) // ch
    mono = [pcm[i * ch] for i in range(n)]                 # first channel is enough for a mono sine
    start = next((i for i, v in enumerate(mono) if v), n)  # skip the leading silence before playback starts
    start = max(start, int(skip * fs))

    body = sorted(abs(v) for v in mono[start:]) or [0]
    amp = body[int(len(body) * 0.999)] if body else 0      # robust peak amplitude
    limit = factor * 2 * math.pi * tone / fs * amp

    clicks, last_click = [], -fs
    gaps, run = 0, 0
    for i in range(start + 1, n):
        if abs(mono[i] - mono[i - 1]) > limit and i - last_click > fs // 100:
            clicks.append(round(i / fs, 3))
            last_click = i
        if mono[i] == 0:
            run += 1
        else:
            if run >= fs // 1000:
                gaps += 1
            run = 0
    return {"seconds": round(n / fs, 2), "rate": fs, "channels": ch, "amplitude": amp,
            "clicks": len(clicks), "click_times": clicks[:20], "silent_gaps": gaps}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--freq", type=float, default=440.0)
    ap.add_argument("--factor", type=float, default=2.5)
    ap.add_argument("--skip", type=float, default=0.0)
    a = ap.parse_args()
    print(json.dumps(analyze(a.dump, a.freq, a.factor, a.skip)))


if __name__ == "__main__":
    main()
