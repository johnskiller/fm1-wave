#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 johnskiller
"""A pool that arrives while the FM-1 is running has to become playable.

The regression this exists for: app_init read the resource region once and kept
`smp_count` in RAM. Resources are delivered over USB AFTER a firmware update, by a
separate tool, into a running instrument -- so the sequence a player actually does
(fwsc, then fm1_resources.py) left the app believing it had no material until the
next power cycle. The transfer reported success and the region checked out: the
device had the samples and the instrument said NO SAMPLES.

The host can move the region under a running app (`res 0`, then `res 1`), which is
the only way to reproduce it without a device. `reboot` is needed because boot() runs
before the script does.

This test does not need the demo kit -- it only asks whether the pool is seen, not
what is in it.

    python3 app/tests/test_pool_arrival.py     # needs app/build/host/wave_host
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
HOST = os.path.join(ROOT, "app", "build", "host", "wave_host")

fails = 0
checks = 0


def ck(cond, what, detail=""):
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
        print(f"  FAIL {what}{(' -- ' + detail) if detail else ''}")


def main():
    if not os.path.exists(HOST):
        sys.exit(f"{HOST} is missing: build it with `python3 platform/tools/build.py app --host`")

    # EN_SELECT is an encoder: press/release is a detent, turn moves it.
    detent = "press SEL\nwait 80\nrelease SEL\nwait 200\n"
    script = (
        "res 0\n"                    # no region: what an FM-1 looks like after a firmware update
        "reboot\n"                   # boot() ran before the script; ask for another one
        "expect smp_count 0\n"
        + detent +                   # SYNTH -> SMPL
        "expect src 1\n"
        "wait 300\n"
        "expect smp_count 0\n"       # still none: this is the state the player saw
        "res 1\n"                    # the pool lands, as fm1_resources.py makes it land
        "wait 600\n"
        "expect smp_count 2\n"       # ...and the app has to notice without a power cycle
        "expect src 1\n"
        "key 12 down\n"
        "wait 300\n"
        "wav out.wav\n"
        "wait 500\n"
        "wavstop\n"
    )

    out = tempfile.mkdtemp(prefix="fm1pool")
    path = os.path.join(out, "script.txt")
    with open(path, "w") as f:
        f.write(script)
    try:
        r = subprocess.run([HOST, path, out], capture_output=True, text=True, timeout=300, cwd=out)
    finally:
        os.unlink(path)

    said = r.stdout + r.stderr
    ck("FAIL" not in said, "the pool that arrived mid-run was picked up",
       "\n".join(l.strip() for l in said.splitlines() if "FAIL" in l))
    ck("ok smp_count == 2" in said, "smp_count went 0 -> 2 without a power cycle")

    # ...and it is not merely counted: it makes a sound.
    wav = os.path.join(out, "out.wav")
    if os.path.exists(wav):
        import struct
        import wave
        w = wave.open(wav)
        data = w.readframes(w.getnframes())
        n, ch = w.getnframes(), w.getnchannels()
        w.close()
        s = struct.unpack("<%dh" % (len(data) // 2), data)
        peak = max(abs(v) for v in s) / 32768.0 if s else 0.0
        ck(peak > 0.005, "a key played on the arrived pool makes a sound",
           f"peak {peak:.4f}")
    else:
        ck(False, "out.wav was written")

    print(f"checks {checks}, failed {fails}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
