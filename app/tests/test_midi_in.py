#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 johnskiller
"""MIDI in plays the instrument.

The defect this exists for: fm1-wave called plat_midi_send for the panel's keys and
never plat_midi_in, so the FM-1 was a MIDI *out* device -- a computer saw the port and
nothing ever happened. The check is not "a message was counted", it is that the same
note from MIDI and from the panel make the same sound, because that is what "an
instrument, not a master keyboard" means.

Checked against the defect: with midi_in() not called from app_render, the note-on
case renders silence (peak 0.0000) and every assertion here fails.
"""
import os
import subprocess
import sys
import tempfile
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
HOST = os.path.join(APP, "build", "host", "wave_host")

checks = failures = 0


def check(what, ok, detail=""):
    global checks, failures
    checks += 1
    if not ok:
        failures += 1
    print(f"  {'ok  ' if ok else 'FAIL'} {what}{('  ' + detail) if detail else ''}")


def run(script):
    """(peak, rms, last-half rms) of the rendered wav, and the host's exit code."""
    d = tempfile.mkdtemp()
    sp = os.path.join(d, "s.txt")
    with open(sp, "w") as f:
        f.write(script)
    r = subprocess.run([HOST, sp, d], capture_output=True, text=True, timeout=300)
    w = os.path.join(d, "t.wav")
    if not os.path.exists(w) or os.path.getsize(w) <= 44:
        return 0.0, 0.0, 0.0, r.returncode
    with wave.open(w) as f:
        a = np.frombuffer(f.readframes(f.getnframes()), dtype=np.int16).astype(np.float64) / 32768.0
    half = a[len(a) // 2:]
    return (float(np.abs(a).max()), float(np.sqrt((a ** 2).mean())),
            float(np.sqrt((half ** 2).mean())), r.returncode)


def main():
    if not os.path.exists(HOST):
        raise SystemExit(f"build the host first: {HOST}")

    print("midi in")
    # note 60 is panel key 7 (NKEYS 27, F3..G5 from 53); MIDI 0x3C = 60
    panel = run("master 4096\nwav t.wav\nkey 7 down\nwait 300\nwavstop\n")
    midi = run("master 4096\nwav t.wav\nmidi USB 90 3C 64\nwait 300\nwavstop\n")

    check("a MIDI note on sounds", midi[0] > 0.05, f"peak {midi[0]:.4f}")
    # the panel touch strip gives a velocity this cannot know; what has to match is the
    # engine, which is why peak is compared and not the whole waveform
    check("the same note from MIDI and from the panel are the same sound",
          abs(midi[0] - panel[0]) < 0.01, f"panel {panel[0]:.4f} midi {midi[0]:.4f}")

    off = run("master 4096\nwav t.wav\nmidi USB 90 3C 64\nwait 50\n"
              "midi USB 80 3C 00\nwait 400\nwavstop\n")
    held = run("master 4096\nwav t.wav\nmidi USB 90 3C 64\nwait 450\nwavstop\n")
    check("a MIDI note off releases it", off[2] < 0.4 * held[2],
          f"held {held[2]:.4f} released {off[2]:.4f}")

    cc = run("master 4096\nwav t.wav\nmidi USB 90 3C 64\nwait 50\n"
             "midi USB B0 7B 00\nwait 400\nwavstop\n")
    check("CC123 all notes off releases it", cc[2] < 0.4 * held[2],
          f"held {held[2]:.4f} cc123 {cc[2]:.4f}")

    # 0x90 with velocity 0 is a note off, and it is what most controllers send
    z = run("master 4096\nwav t.wav\nmidi USB 90 3C 64\nwait 50\n"
            "midi USB 90 3C 00\nwait 400\nwavstop\n")
    check("a note on with velocity 0 is a note off", z[2] < 0.4 * held[2],
          f"held {held[2]:.4f} vel0 {z[2]:.4f}")

    # clock must not be counted as a message (it would flood midi_seen and, worse, keep
    # the ring busy in a real device)
    d = tempfile.mkdtemp()
    sp = os.path.join(d, "s.txt")
    with open(sp, "w") as f:
        f.write("master 4096\nmidi USB F8\nmidi USB F8\nmidi USB 90 3C 64\nwait 60\n")
    r = subprocess.run([HOST, sp, d], capture_output=True, text=True, timeout=300)
    # midi_seen counts only what was acted on: 2 clocks + 1 note on -> 1
    seen = None
    for line in (r.stdout + r.stderr).splitlines():
        if "midi_seen" in line:
            seen = line
    d2 = tempfile.mkdtemp()
    sp2 = os.path.join(d2, "s.txt")
    with open(sp2, "w") as f:
        f.write("master 4096\nmidi USB F8\nmidi USB F8\nmidi USB 90 3C 64\nwait 60\n"
                "expect midi_seen 1\n")
    r2 = subprocess.run([HOST, sp2, d2], capture_output=True, text=True, timeout=300)
    check("clock is not counted (2 clocks + 1 note on -> midi_seen 1)", r2.returncode == 0,
          (r2.stdout + r2.stderr).strip().splitlines()[-1][:70] if (r2.stdout + r2.stderr).strip() else "")

    print(f"\nchecks {checks}, failed {failures}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
