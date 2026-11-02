#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 johnskiller
"""Measure a recorded note: what pitch period it actually has, and what shape.

    python3 tools/measure_wave.py REC.wav [--expect-hz 261.6] [--compare OTHER.wav]

Why this exists. A scope picture and a rendered wav had both been used to argue about
a waveform, and both were read through an analyzer written for the moment -- which is
how "every pitch is an octave low" turned out to be the reader mistaking stereo int16
for mono (reading L and R as adjacent samples halves every period). An argument about
the instrument that rests on ad-hoc reading code is not an argument. So: one reader,
used on the host's render AND on a recording from the device, printing the same numbers.

What it prints, and what each number can settle:

  period   the samples between autocorrelation peaks, at the strongest lag. A wrong
           pitch shows here as a ratio off by a small integer or a fraction -- and the
           repeats table below shows whether the signal is periodic at all.
  rep      how strongly the signal repeats at lag/2, lag, 2*lag: a clean single
           oscillator repeats at the period and at twice it. A picture spliced from
           two instants repeats at neither, which is the case this was written for.
  h[1..8]  the first eight harmonics, normalised to h1. THIS is the shape, and it does
           not care about pitch, phase or window: a saw is 1/k, a square is odd only
           (1/k on odd k, ~0 on even), a triangle is 1/k^2 on odd k. Two oscillators
           beating show as harmonics that do not fit any of those three.
  duty     the fraction of a period spent above the midline. A square is 0.5; a saw
           ramps so the mean sits at the middle of its range, also ~0.5 -- but a square
           is bimodal (most samples at the rails) and a saw is not, so `rail` is the
           one that separates them: the fraction of samples within 10% of the extremes.

Everything is measured on the mono sum of a stereo file and on the middle of the note,
away from the attack.
"""
import sys
import wave

import numpy as np


def read_wav(path):
    """stereo or mono, any of int16/24/f32 -- returned as mono float in [-1, 1].

    The stereo/mono question is not a detail: this exact mistake made every pitch in a
    previous measurement come out an octave low, because L and R interleaved were read
    as consecutive samples of one channel.
    """
    with wave.open(path, "rb") as w:
        ch, sw, sr, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if sw == 2:
        x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    elif sw == 4:
        x = np.frombuffer(raw, dtype="<i4").astype(np.float64) / 2147483648.0
    elif sw == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v & 0x800000, v - 0x1000000, v)
        x = v.astype(np.float64) / 8388608.0
    else:
        raise SystemExit(f"{path}: {sw * 8}-bit samples are not handled")
    if ch == 2:
        x = x.reshape(-1, 2).mean(axis=1)
    elif ch != 1:
        raise SystemExit(f"{path}: {ch} channels")
    return x, sr


def pick_segment(x, sr, ms=300):
    """the middle ms of the loudest stretch, so an attack or a release tail is not measured"""
    n = int(sr * ms / 1000.0)
    if len(x) <= n:
        return x
    win = int(sr * 0.02)
    env = np.convolve(np.abs(x), np.ones(win) / win, mode="valid")
    # loudest start, then step in a little so a 20 ms envelope does not straddle the attack
    i = int(np.argmax(env)) + win
    i = min(i, len(x) - n - 1)
    return x[max(i, 0):max(i, 0) + n]


def period_by_autocorr(x, sr, lo_hz=40.0, hi_hz=3000.0):
    x = x - x.mean()
    x = x * np.hanning(len(x))
    ac = np.correlate(x, x, mode="full")[len(x) - 1:]
    ac /= ac[0] if ac[0] else 1.0
    lo = max(2, int(sr / hi_hz))
    hi = min(len(ac) - 2, int(sr / lo_hz))
    seg = ac[lo:hi]
    # the first peak that is nearly as strong as the strongest: the period, not a
    # multiple of it (a saw's 2*period peak is almost as tall and comes later)
    best = int(np.argmax(seg)) + lo
    thresh = 0.85 * ac[best]
    for k in range(lo, hi - 1):
        if ac[k] > thresh and ac[k] >= ac[k - 1] and ac[k] >= ac[k + 1]:
            best = k
            break
    # parabolic refinement: at 44.1 k an integer lag is a whole percent of a period
    a, b, c = ac[best - 1], ac[best], ac[best + 1]
    den = a - 2 * b + c
    frac = 0.5 * (a - c) / den if den else 0.0
    return best + frac, ac


def harmonics(seg, sr, f0, count=8):
    """|X(k f0)| normalised to k=1, from a whole number of periods"""
    if f0 <= 0:
        return np.zeros(count)
    per = sr / f0
    nper = max(1, int(len(seg) / per))
    n = int(round(per * nper))
    s = seg[:n] - seg[:n].mean()
    s = s * np.hanning(n)
    spec = np.abs(np.fft.rfft(s)) / (n / 2.0)
    out = []
    for k in range(1, count + 1):
        b = int(round(k * f0 * n / sr))
        if b < 1 or b >= len(spec):
            out.append(0.0)
            continue
        out.append(float(max(spec[b - 1], spec[b], spec[b + 1] if b + 1 < len(spec) else spec[b])))
    h1 = out[0] or 1.0
    return np.array(out) / h1


def shape_of(h):
    """which of saw / square / triangle the harmonic ratios look like, and how far off"""
    k = np.arange(1, len(h) + 1, dtype=float)
    models = {"saw": 1.0 / k, "square": np.where(k % 2 == 1, 1.0 / k, 0.0),
              "triangle": np.where(k % 2 == 1, 1.0 / k ** 2, 0.0)}
    best, errs = None, {}
    for name, m in models.items():
        e = float(np.sqrt(np.mean((h - m) ** 2)))
        errs[name] = e
        if best is None or e < errs[best]:
            best = name
    return best, errs


def report(path, expect_hz=None):
    x, sr = read_wav(path)
    seg = pick_segment(x, sr)
    lag, ac = period_by_autocorr(seg, sr)
    f0 = sr / lag if lag > 0 else 0.0
    h = harmonics(seg, sr, f0)
    mid = 0.5 * (seg.max() + seg.min())
    rng = (seg.max() - seg.min()) or 1.0
    duty = float(np.mean(seg > mid))
    rail = float(np.mean(np.abs(seg - mid) > 0.4 * rng))
    best, errs = shape_of(h)

    print(f"{path}")
    print(f"  sr {sr}  n {len(x)}  peak {np.abs(x).max():.4f}")
    print(f"  period {lag:.2f} samples  -> {f0:.2f} Hz"
          + (f"   (expected {expect_hz:.2f}, ratio {f0 / expect_hz:.4f})" if expect_hz else ""))
    print("  rep " + "  ".join(f"{m}x:{ac[int(round(lag * m))]:.3f}" for m in (0.5, 1, 2, 3, 4)
                               if int(round(lag * m)) < len(ac)))
    print("  h  " + "  ".join(f"{v:.3f}" for v in h[:8]))
    print("  " + "   ".join(f"{n} err {e:.3f}" for n, e in errs.items())
          + f"   -> {best}")
    print(f"  duty {duty:.3f}  rail {rail:.3f}"
          + ("   (square is bimodal: rail high)" if rail > 0.5 else "   (not bimodal)"))
    return f0, h


def main():
    args = sys.argv[1:]
    if not args:
        sys.exit("usage: measure_wave.py REC.wav [--expect-hz HZ] [--compare OTHER.wav]")
    expect = None
    if "--expect-hz" in args:
        i = args.index("--expect-hz")
        expect = float(args[i + 1])
        del args[i:i + 2]
    cmp_path = None
    if "--compare" in args:
        i = args.index("--compare")
        cmp_path = args[i + 1]
        del args[i:i + 2]

    f0, h = report(args[0], expect)
    if cmp_path:
        print()
        f0b, hb = report(cmp_path)
        print()
        d = float(np.max(np.abs(h - hb)))
        print(f"  compare: worst harmonic difference {d:.3f}"
              + ("   IDENTICAL SHAPE" if d < 0.05 else "   DIFFERENT SHAPE"))


if __name__ == "__main__":
    main()
