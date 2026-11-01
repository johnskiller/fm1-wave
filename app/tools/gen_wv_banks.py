#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 johnskiller
"""fm1-wave's synth banks: 8 frames of a 256-point single-cycle wave, int16 Q15,
one after another. 8 x 256 x 2 = 4096 bytes per bank, 8 banks = 32 KB of flash,
read in place (XIP) by app/dsp/wv_dsp.c.

    python3 tools/gen_wv_banks.py OUT.h        (build.py's gen step, or by hand)

Two rules, both of which matter more than they look:

1. **Frames in a bank share their partials.** Frame f and f+1 differ in the
   *amplitude* of the same harmonics, not in *which* harmonics exist, so MORPH
   sweeps instead of snapping. A saw -> bell morph clicks; saw -> pulse does not.

2. **A bank is normalised as a whole, not per frame.** Per-frame normalisation
   makes every frame peak at full scale, so MORPH becomes a volume knob. One peak
   across the bank, so MORPH is a timbre knob.

The five that came from the WT engine (SAW SQR, RESO, HOLLOW, VOX, DIGI) are
reproduced here so the two instruments can be compared; 6-8 are open (see
docs/spec.md).
"""
import math
import sys

N = 256          # points per cycle
F = 8            # frames per bank


def frame(parts):
    """A cycle from (harmonic, amplitude, phase) triples."""
    out = []
    for i in range(N):
        s = 0.0
        for k, a, ph in parts:
            s += a * math.sin(2.0 * math.pi * (k * i / N) + ph)
        out.append(s)
    return out


def norm_bank(frames):
    """Two steps, and the second is what actually makes MORPH a timbre knob.

    1. **Equalise each frame's RMS.** Without this, MORPH is a volume knob: OPEN3
       as first written swung 12.8 dB from frame 0 to frame 7 because a formant
       moving up the harmonic series gets spikier (crest 2.1 -> 4.1) even at
       identical partial count. Measuring that in the rendered demo is what caught
       it. Frames of a nearly silent spectrum are left alone rather than blown up
       into noise.
    2. **Then scale the bank by one factor** so its loudest point is full scale,
       so int16 never clips. Banks still differ from each other in loudness (their
       crest factors differ), which is correct: they are different instruments.

    The order matters. Peak-normalising the bank alone satisfies step 2 and leaves
    step 1 broken, which is exactly the bug this replaces.
    """
    rms = [math.sqrt(sum(v * v for v in fr) / len(fr)) for fr in frames]
    top = max(rms) if rms else 1.0
    out = []
    for fr, r in zip(frames, rms):
        k = (1.0 / r) if r > top * 0.01 else (1.0 / top)
        out.append([v * k for v in fr])
    peak = max(1e-9, max(abs(v) for fr in out for v in fr))
    k = 32767.0 / peak
    return [int(round(v * k)) for fr in out for v in fr]


def morph(start, stop):
    """8 frames from spectrum `start` to `stop`, each a mix of the two ends, so
    neighbouring frames share partials (rule 1)."""
    out = []
    for f in range(F):
        t = f / (F - 1.0)
        mix = [(k, (1 - t) * a0 + t * a1, ph)
               for (k, a0, ph), (_, a1, _ph2) in zip(start, stop)]
        out.append(frame(mix))
    return norm_bank(out)


def walk(fn):
    """8 frames from a per-frame spectrum function (a formant that moves, etc.)."""
    return norm_bank([frame(fn(f)) for f in range(F)])


def saw(n, phase=0.0):
    return [(k, 1.0 / k, phase) for k in range(1, n + 1)]


def square(n, phase=0.0):
    return [(k, 1.0 / k if k % 2 else 0.0, phase) for k in range(1, n + 1)]


BANKS = [
    # 0 SAW SQR: the classic rich sweep, evens fading out
    ("SAW SQR", morph(saw(16), square(16))),
    # 1 RESO: a bump walking up the harmonics
    ("RESO", walk(lambda f: saw(12) + [(5 + f, 1.2, 0.0)])),
    # 2 HOLLOW: odd harmonics fading in over a saw (clarinet -> square-ish)
    ("HOLLOW", morph(saw(12), [(k, 1.0 / k if k % 2 else 0.0, math.pi / 2) for k in range(1, 13)])),
    # 3 VOX: two formant bumps moving in opposite directions
    ("VOX", walk(lambda f: saw(16) + [(4 + f, 1.1, 0.0), (14 - f, 0.9, 0.0)])),
    # 4 DIGI: a bright edge softening (saw -> rounded pulse)
    ("DIGI", morph(saw(24), [(k, (1.0 / k) * (0.5 + 0.5 * math.cos(math.pi * k / 24.0)), 0.0)
                             for k in range(1, 25)])),
    # 5-7 open (docs/spec.md §5a): placeholders that are real banks, not dead slots,
    # so the instrument has eight playable timbres while the last three are decided
    ("OPEN1", morph(saw(10), [(k, 1.0 / (k * k), 0.0) for k in range(1, 11)])),
    ("OPEN2", walk(lambda f: [(k, 1.0 / k, (f / 4.0) if k % 2 else 0.0) for k in range(1, 17)])),
    # constant Q: the width scales with the centre, otherwise a formant climbing
    # the harmonic series narrows in relative terms and the frame gets spikier
    ("OPEN3", walk(lambda f: [(k, (1.0 / k) * math.exp(-((k - (3 + f)) ** 2) / (2.0 * 0.35 * (3 + f) ** 2)),
                               0.0) for k in range(1, 25)])),
]


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: gen_wv_banks.py OUT.h")
    out = sys.argv[1]
    names = ", ".join('"%s"' % b[0] for b in BANKS)
    with open(out, "w") as fp:
        fp.write("/* generated by tools/gen_wv_banks.py: do not edit */\n")
        fp.write("/* 8 banks x %d frames x %d points, int16 Q15, one bank after another */\n" % (F, N))
        fp.write("#pragma once\n#include <stdint.h>\n\n")
        fp.write("#define WV_BANK_COUNT %d\n" % len(BANKS))
        fp.write("static const char *const WV_BANK_NAME[WV_BANK_COUNT] = {%s};\n\n" % names)
        fp.write("static const int16_t WV_BANKS[WV_BANK_COUNT][%d] = {\n" % (F * N))
        for name, data in BANKS:
            fp.write("  { /* %s */\n" % name)
            for f in range(F):
                row = data[f * N:(f + 1) * N]
                fp.write("   ")
                fp.write(",".join("%d" % v for v in row))
                fp.write(",%s\n" % ("" if f == F - 1 else ""))
            fp.write("  },\n")
        fp.write("};\n")
    print("banks: %d x %d frames x %d points, %d B" % (len(BANKS), F, N, len(BANKS) * F * N * 2))


if __name__ == "__main__":
    main()
