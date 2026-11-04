#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 johnskiller
"""The arithmetic behind spec §5c1 (granulating a bank, NOT BUILT).

    python3 app/tools/grain_bank_ceiling.py

§5c1 claims a pitch-dependent ceiling on grain length, because one cycle of a bank is
256 points and a grain has to advance 256*hz/44100 samples per output sample to stay in
tune. Every number in that table comes from this file; run it to check the section rather
than trusting the table.

Not part of the build. It reads constants out of the DSP headers so the day WV_POINTS
changes, this prints the new consequence instead of a stale table.
"""
import math
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
DSP = os.path.join(os.path.dirname(HERE), "dsp")


def header_value(name, path, default=None):
    """Read `#define NAME  value` out of a header. Only the plain integer cases."""
    with open(path) as fp:
        for line in fp:
            m = re.match(r"#define\s+%s\s+(\d+)u?\b" % re.escape(name), line)
            if m:
                return int(m.group(1))
    if default is not None:
        return default
    raise SystemExit("%s not found in %s" % (name, path))


POINTS = header_value("WV_POINTS", os.path.join(DSP, "wv_dsp.h"))
FRAMES = header_value("WV_FRAMES", os.path.join(DSP, "wv_dsp.h"))
GRAIN_MIN_MS = header_value("WV_GRAIN_MIN_MS", os.path.join(DSP, "wv_grain.h"))
RATE = 44100.0

KEYS = [("F3", 53), ("A3", 57), ("C4", 60), ("C5", 72), ("G5", 79)]


def hz(note):
    return 440.0 * 2.0 ** ((note - 69) / 12.0)


def ceiling(samples, note):
    """Longest grain, in ms, that stays inside `samples` worth of material."""
    rate = POINTS * hz(note) / RATE          # bank samples consumed per output sample
    out = samples / rate                     # output samples the material lasts
    return 1000.0 * out / RATE


def main():
    one_bank = FRAMES * POINTS
    all_banks = one_bank * 8                 # WV_BANK_COUNT, see gen_wv_banks.py
    print("WV_POINTS %d, WV_FRAMES %d -> one bank %d samples, all eight %d"
          % (POINTS, FRAMES, one_bank, all_banks))
    print("GRAIN knob's shortest grain: %d ms (WV_GRAIN_MIN_MS); its longest: 200 ms"
          % GRAIN_MIN_MS)
    print()
    for label, samples in (("one bank", one_bank), ("all eight", all_banks)):
        print("%s (%d samples, %.1f ms at rate 1):"
              % (label, samples, 1000.0 * samples / RATE))
        print("  %-4s %7s %7s %14s" % ("key", "Hz", "rate", "longest grain"))
        for name, note in KEYS:
            print("  %-4s %7.1f %7.3f %11.1f ms"
                  % (name, hz(note), POINTS * hz(note) / RATE, ceiling(samples, note)))
        print()

    # Where does each configured grain length stop fitting? The knob is 10..200 ms, so
    # the answer is the lowest key at which even the LONGEST grain fits.
    print("the 200 ms end of the knob stops fitting above:")
    for note in range(53, 80):
        if ceiling(all_banks, note) < 200.0:
            name = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"][note % 12]
            print("  %s%d (note %d): %.1f ms" % (name, note // 12 - 1, note,
                                                 ceiling(all_banks, note)))
            break


if __name__ == "__main__":
    main()
