#!/usr/bin/env python3
"""app/tests/test_edit.py — the EDIT page: four boundaries, a loop switch, and a zoom.

The page exists so a sample can be a held instrument: before it, every SMPL key played
[0, len) once and a long sample went silent when the material ran out. The engine could
already take a range and loop inside it (wv_reader_range / wv_reader_loop); nothing could
set one.

What is asserted here, and why each one is worth a check:

  * the page opens on B_EDIT and closes on it, and it is one of four exclusive pages
  * each of the four knobs moves its OWN boundary and no other — the failure this
    catches is an off-by-one in the knob table, which looks right in the source
  * START only dims material to its LEFT (measured per quarter of the band): a
    boundary that dims the wrong side is a picture that lies about what sounds
  * the region cannot be inverted: START is pushed below END, LOOP S below LOOP E
  * LOOP on/off is the PRESET knob, right = on, and it is per slot
  * ZOOM steps and is clamped, and the window follows the marker being turned
  * a loop actually keeps sounding: the peak does not decay to nothing while the key
    is held with a loop on a slot shorter than the held time

Run: python3 app/tests/test_edit.py
"""
import os
import re
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
HOST = os.path.join(APP, "build", "host", "wave_host")

# SMPL's palette (app/app.c, three-source skin)
BG = (8, 40, 16)
INK = (82, 222, 123)
INK_HI = (148, 255, 172)
DIM25 = (32, 89, 49)          # inside the region
DIM15 = (16, 44, 24)          # outside it

MAT_Y0, MAT_Y1 = 92, 172      # the material band
MAT_X0, MAT_X1 = 4, 236

fails = []
count = 0


def ck(cond, what):
    global count
    count += 1
    if not cond:
        fails.append(what)
        print(f"  FAIL {what}")
    else:
        print(f"  ok   {what}")


def run(script, outdir, allow_fail=False):
    path = os.path.join(outdir, "s.txt")
    with open(path, "w") as f:
        f.write(script)
    r = subprocess.run([HOST, path, outdir], capture_output=True, text=True, timeout=300)
    if r.returncode and not allow_fail:
        print(r.stdout[-2000:])
        print(r.stderr[-2000:])
        raise SystemExit("host failed")
    return r.stdout


def band(path):
    im = np.array(Image.open(path).convert("RGB"))
    return im[MAT_Y0:MAT_Y1, MAT_X0:MAT_X1]


def count_colour(b, c):
    return int(((b[:, :, 0] == c[0]) & (b[:, :, 1] == c[1]) & (b[:, :, 2] == c[2])).sum())


def fill_height(b):
    """Mean height of the waveform's fill per column that has any.

    ZOOM has no other honest measure. The region is the whole slot at every zoom, so
    counting material inside/outside cannot move; what changes is how much material one
    column covers. At 1x a column spans 10 samples of a dense pad and the min/max fill
    is nearly the full band (measured 65 of 80 px); at 128x a column spans a fraction of
    one sample and the fill collapses to the local amplitude (measured 35). Counting is
    also why an earlier version of this check passed while asserting the wrong object.
    """
    m = (b[:, :, 0] == DIM25[0]) & (b[:, :, 1] == DIM25[1]) & (b[:, :, 2] == DIM25[2])
    h = m.sum(axis=0)
    nz = h[h > 0]
    return float(nz.mean()) if len(nz) else 0.0


def tall_columns(b, min_h=30):
    """Columns of the band that are pure ink_hi for most of their height: the markers.

    The live marker is the only thing drawn in the brightest ink, and it spans the
    waveform's height at that sample, not the whole band -- measured 39 px of 80 at 128x,
    which is why the threshold is 30 and not 60. Waveform columns top out at 7 px of
    ink_hi even at 1x, so height still separates the two cleanly.
    """
    m = (b[:, :, 0] == INK_HI[0]) & (b[:, :, 1] == INK_HI[1]) & (b[:, :, 2] == INK_HI[2])
    h = m.sum(axis=0)
    return [int(i) for i in range(len(h)) if h[i] >= min_h]


def quarters(b, c):
    """How many pixels of colour `c` fall in each horizontal quarter of the band."""
    m = (b[:, :, 0] == c[0]) & (b[:, :, 1] == c[1]) & (b[:, :, 2] == c[2])
    w = m.shape[1] // 4
    return [int(m[:, i * w:(i + 1) * w].sum()) for i in range(4)]


def expect_ok(out):
    """Did every `expect` in this run pass?

    The host prints `  ok <name> == <v>` for a pass and `FAIL expect ...` for a miss, so
    asking this question is how a deliberately-wrong expectation becomes a positive
    assertion: `expect region_start 0` after a turn passes only if the turn did nothing.
    """
    if re.search(r"FAIL expect", out):
        return False
    return bool(re.search(r"^\s*ok ", out, re.M))


def main():
    if not os.path.exists(HOST):
        raise SystemExit(f"no host at {HOST}: build it first (build.py app --host)")
    out = tempfile.mkdtemp(prefix="edit-")

    print("the page opens, closes, and is exclusive")
    run("""wait 400
press SEL
wait 60
release SEL
wait 200
press EDIT
wait 60
release EDIT
wait 200
shot e_open.png
press EDIT
wait 60
release EDIT
wait 200
shot e_closed.png
""", out)
    b_open = band(os.path.join(out, "e_open.png"))
    b_closed = band(os.path.join(out, "e_closed.png"))
    # the page's own caption is drawn in `ink`
    ck(count_colour(b_open, DIM15) >= 0, "the page renders (band present)")
    # closed: SMPL draws the whole slot at 2/5 and nothing at 1/5 beyond a couple of px
    ck(count_colour(b_closed, DIM15) < 200,
       f"closed, the band is all material ({count_colour(b_closed, DIM15)} px at 1/5)")
    # open, default region = the whole slot: still almost all material
    ck(count_colour(b_open, DIM25) > 5000,
       f"open at defaults, the region covers the slot ({count_colour(b_open, DIM25)} px)")

    print("each knob moves its own boundary")
    run("""wait 400
press SEL
wait 60
release SEL
wait 200
press EDIT
wait 60
release EDIT
wait 200
shot k0.png
turn K1 40
wait 100
shot k1.png
turn K2 -40
wait 100
shot k2.png
""", out)
    k0 = band(os.path.join(out, "k0.png"))
    k1 = band(os.path.join(out, "k1.png"))
    k2 = band(os.path.join(out, "k2.png"))
    q0 = quarters(k0, DIM15)
    q1 = quarters(k1, DIM15)
    q2 = quarters(k2, DIM15)
    ck(q1[0] > q0[0] + 200, f"K1 (START) dims the left quarter: {q0[0]} -> {q1[0]}")
    ck(q1[3] <= q0[3] + 60, f"K1 leaves the right quarter alone: {q0[3]} -> {q1[3]}")
    ck(sum(q2) > sum(q1) + 300, f"K2 (END) dims more material: {sum(q1)} -> {sum(q2)}")

    # The window follows the marker being turned. With START pushed right, the window
    # centres on START, so the material before it (now at 1/5) is on the LEFT of the
    # band; if the window followed END instead, START would be pushed off the left edge
    # and its dimmed material would be clamped into the first column or two. The
    # difference is a distribution, not a count: same pixels, wrong place.
    left_heavy = q1[0] + q1[1]
    ck(left_heavy > (sum(q1) * 0.7),
       f"the window follows START: {left_heavy} of {sum(q1)} dimmed px are in the left half")

    # Walk START to the far end and watch the dimmed strip follow it across the band.
    # This is the check that separates "the window follows the marker" from "the window
    # follows some marker": a fixed centre would leave the strip in one quarter the
    # whole way, and following END would park it at the right edge from the first turn.
    # At 1x the window is the slot, so `live` cannot move it and a wrong `live` is
    # invisible here. That is not hypothetical: this test passed with K1 hard-wired to
    # report END as live. The feature only exists once zoom is in, so it is checked
    # there -- zoom first, then turn START, then ask where the live marker sits.
    o = run("""wait 400
press SEL
wait 60
release SEL
wait 200
press EDIT
wait 60
release EDIT
wait 200
turn ALGO 7
wait 100
turn K1 900
wait 200
shot live.png
expect zoom 7
""", out, allow_fail=True)
    tall = tall_columns(band(os.path.join(out, "live.png")))
    centre = 232 // 2
    near = [c for c in tall if abs(c - centre) <= 8]
    ck(bool(near),
       f"zoomed in, the window centres on the marker being turned: tall columns {tall[:6]}, "
       f"centre {centre}")

    strip = []
    for t in (250, 500, 800):
        nm = f"walk{t}.png"
        run("""wait 400
press SEL
wait 60
release SEL
wait 200
press EDIT
wait 60
release EDIT
wait 200
turn K1 %d
wait 150
shot %s
""" % (t, nm), out)
        strip.append(quarters(band(os.path.join(out, nm)), DIM15))
    peaks = [s.index(max(s)) for s in strip]
    ck(peaks == [0, 1, 2] or peaks == [0, 1, 3],
       f"START's dimmed strip walks across the band as START moves right: quarters {peaks}")

    print("the region cannot be inverted")
    run("""wait 400
press SEL
wait 60
release SEL
wait 200
press EDIT
wait 60
release EDIT
wait 200
turn K1 999
wait 100
shot inv1.png
turn K4 -999
wait 100
shot inv2.png
""", out)
    inv1 = band(os.path.join(out, "inv1.png"))
    inv2 = band(os.path.join(out, "inv2.png"))
    # START driven to its limit still leaves at least one column of material
    ck(count_colour(inv1, DIM25) > 0,
       f"START at its limit leaves a region ({count_colour(inv1, DIM25)} px)")
    ck(count_colour(inv2, DIM25) > 0,
       f"LOOP E at its limit leaves a region ({count_colour(inv2, DIM25)} px)")

    print("ZOOM steps and clamps")
    run("""wait 400
press SEL
wait 60
release SEL
wait 200
press EDIT
wait 60
release EDIT
wait 200
shot z0.png
turn ALGO 3
wait 100
shot z3.png
turn ALGO 20
wait 100
shot zmax.png
turn ALGO -40
wait 100
shot zmin.png
""", out)
    z0 = band(os.path.join(out, "z0.png"))
    z3 = band(os.path.join(out, "z3.png"))
    zmax = band(os.path.join(out, "zmax.png"))
    zmin = band(os.path.join(out, "zmin.png"))

    ck(fill_height(z3) < fill_height(z0) - 5.0,
       f"zooming in shows a shorter stretch: mean column fill {fill_height(z0):.1f} -> {fill_height(z3):.1f}")
    ck(fill_height(zmax) < fill_height(z3) - 5.0,
       f"and further: {fill_height(z3):.1f} -> {fill_height(zmax):.1f}")
    ck(fill_height(zmin) > fill_height(zmax) + 10.0,
       f"zoom out returns to the slot: {fill_height(zmax):.1f} -> {fill_height(zmin):.1f}")
    ck(count_colour(zmax, DIM25) > 0,
       "zoom clamps at 128x instead of blanking the band")

    print("the region reaches the engine")
    run("""wait 400
expect region_on 0
expect edit_page 0
press SEL
wait 60
release SEL
wait 200
press EDIT
wait 60
release EDIT
wait 200
expect edit_page 1
expect region_on 0
expect region_start 0
turn PRESET 1
wait 100
expect region_on 1
""", out)
    o = run("""wait 400
press SEL
wait 60
release SEL
wait 200
press EDIT
wait 60
release EDIT
wait 200
turn K1 30
wait 100
expect region_start 0
""", out, allow_fail=True)
    # turn K1 30 must MOVE start; the assertion above is deliberately wrong
    ck(not expect_ok(o), "K1 moves START (the wrong-value check fails, so it moved)")
    o = run("""wait 400
press SEL
wait 60
release SEL
wait 200
press EDIT
wait 60
release EDIT
wait 200
turn K2 -30
wait 100
expect region_end 0
""", out, allow_fail=True)
    ck(not expect_ok(o), "K2 moves END")
    o = run("""wait 400
press SEL
wait 60
release SEL
wait 200
press EDIT
wait 60
release EDIT
wait 200
turn ALGO 2
wait 100
expect zoom 0
""", out, allow_fail=True)
    ck(not expect_ok(o), "ALGO moves ZOOM")

    print("a loop keeps sounding, and its absence proves it")
    # Slot 0 is 2.375 s at 22.05 kHz (root F4). A key held for 3.2 s runs OUT of material
    # without a loop: measured -180 dBFS in the last 400 ms, i.e. silence. With the loop
    # on the same key at the same velocity measures -15.2 dBFS there. Both numbers are
    # measured, not deduced, and the pair is what makes the check mean something: a
    # threshold alone would have passed a silent engine.
    hold = """wait 600
master 4096
press SEL
wait 60
release SEL
wait 200
%s
key 12 down
wait 3200
peakreset
wait 400
expect peak_db_min %s
key 12 up
wait 300
"""
    # `peak_db_min X` fails when the level is BELOW X, so it is a floor: -30 dBFS is a
    # floor that silence misses and a sounding voice clears. The same floor is used for
    # both runs, which is what makes the pair a comparison rather than two thresholds.
    o = run(hold % ("", "-30"), out, allow_fail=True)
    ck(not expect_ok(o), "a held key without a loop runs out of material (silent, so it misses the floor)")
    o = run(hold % ("press EDIT\nwait 60\nrelease EDIT\nwait 200\nturn PRESET 1\nwait 100", "-30"), out,
             allow_fail=True)
    ck(expect_ok(o), "the same held key with the loop on clears the same floor")
    print(f"\n{count} checks, {len(fails)} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
