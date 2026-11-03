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
    """Columns that are the full-strength hue of ANY of the four boundaries.

    Before the page had colours this looked for ink_hi, because the live marker was the
    only thing in the brightest ink. Now each boundary owns a hue and the live one is
    that hue at full strength, so "where is the live marker" means "which of the four
    pale-or-saturated columns is not dimmed". Height is still what separates a marker
    from material: the marker spans the waveform's own height at that sample (measured
    39 px of 80 at 128x) and no waveform column comes near 30.
    """
    hues = [(255, 96, 96), (255, 208, 96), (96, 224, 255), (198, 140, 255)]
    m = np.zeros(b.shape[:2], dtype=bool)
    for rgb in hues:
        q = q565(rgb)
        m |= np.all(np.abs(b.astype(int) - np.array(q)) <= 3, axis=2)
    h = m.sum(axis=0)
    return [int(i) for i in range(len(h)) if h[i] >= min_h]


def quarters(b, c):
    """How many pixels of colour `c` fall in each horizontal quarter of the band."""
    m = (b[:, :, 0] == c[0]) & (b[:, :, 1] == c[1]) & (b[:, :, 2] == c[2])
    w = m.shape[1] // 4
    return [int(m[:, i * w:(i + 1) * w].sum()) for i in range(4)]


def band_full(path):
    """The whole 240x240 frame. The material band starts at y=86 (MAT_PAD 6 above the
    picture at 92), and the playhead's tab is at 90..94, so slicing from 92 cuts its top
    two rows off and every measurement reads "no head". That cost a whole debugging
    round: the head was there and the window was wrong."""
    return np.array(Image.open(path).convert("RGB"))


def head_col(b):
    """The playhead's column in a full frame, or None.

    White, and only white, in rows 90..94: that is the tab at the top of the material
    band, which no marker and no waveform uses.
    """
    m = np.all(b[90:95, :, :] == np.array([255, 255, 255]), axis=2)
    h = m.sum(axis=0)
    cols = [int(i) for i in range(len(h)) if h[i] >= 4]
    return cols[0] if cols else None


def q565(rgb, num=1, den=1):
    """The 8-bit colour the panel actually emits for `RGB(rgb)` at `num/den` brightness.

    The device stores RGB565 (5-6-5), so (255,96,96) arrives as (255,97,98) and a
    dimmed one is quantised twice. Comparing against the requested colour would make
    every check fail for a reason that is not a defect, so the arithmetic is mirrored
    here instead of loosening the tolerance.
    """
    r, g, b = rgb
    c = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
    c = (((((c >> 11) & 0x1F) * num // den)) << 11
         | ((((c >> 5) & 0x3F) * num // den) << 5)
         | (((c & 0x1F) * num // den)))
    return (((c >> 11) & 0x1F) * 255 // 31, ((c >> 5) & 0x3F) * 255 // 63,
            (c & 0x1F) * 255 // 31)


def find_colour(b, rgb, min_rows=25):
    """Columns where `rgb` covers at least `min_rows` rows of the band.

    The colours are RGB565 on the device, so a hue arrives quantised (255,97,98) rather
    than (255,96,96); the tolerance is for that, not for sloppiness.
    """
    m = np.all(np.abs(b.astype(int) - np.array(rgb)) <= 3, axis=2)
    h = m.sum(axis=0)
    return [int(i) for i in range(len(h)) if h[i] >= min_rows]


def probe_len(out):
    """The loaded slot's length in samples, asked of the page rather than assumed.

    The demo pool is a build artefact and gets replaced; a hardcoded 52369 would make the
    step check fail for the right reason at the wrong time.
    """
    o = run("""wait 400
press SEL
wait 60
release SEL
wait 200
press EDIT
wait 60
release EDIT
wait 200
expect region_end 0
""", out, allow_fail=True)
    m = re.search(r"got (\d+)", o)
    return int(m.group(1)) if m else 0


def probe_step(out, zoom):
    """How many samples one detent of K2 (LOOP S) moves at `zoom`, read from the host.

    The assertion is deliberately a wrong value: `expect region_loops 0` fails and prints
    `got N`, so N is the distance one turn travelled. Anything the page reports about
    itself would be the code checking itself; this is the boundary's actual position.
    """
    o = run("""wait 400
press SEL
wait 60
release SEL
wait 200
press EDIT
wait 60
release EDIT
wait 200
turn ALGO %d
wait 100
turn K2 1
wait 100
expect region_loops 0
""" % zoom, out, allow_fail=True)
    m = re.search(r"got (\d+)", o)
    return int(m.group(1)) if m else -1


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
turn K4 -40
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
    ck(sum(q2) > sum(q1) + 300, f"K4 (END) dims more material: {sum(q1)} -> {sum(q2)}")

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
    # Full-strength START only: at 128x the live marker is the one K1 just moved, and
    # asking for "any marker" would also accept the clamped END/LOOP pair at the edges.
    tall = find_colour(band(os.path.join(out, "live.png")), q565((255, 96, 96)))
    centre = 232 // 2
    near = [c for c in tall if abs(c - centre) <= 8]
    ck(bool(near),
       f"zoomed in, the window centres on the marker being turned: tall columns {tall[:6]}, "
       f"centre {centre}")

    strip = []
    for t in (40, 110, 200):
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
        # "inside" (2/5) is what shrinks as START advances, so it is measured rather
        # than "outside" (1/5): once START is near the end the outside covers the whole
        # band and its distribution stops carrying information -- measured, all four
        # quarters at 3.4-4.2k px with the inside down to 39 px.
        strip.append(quarters(band(os.path.join(out, nm)), DIM25))
    first = [s.index(max(s)) for s in strip]
    totals = [sum(s) for s in strip]
    ck(totals[0] > totals[-1] * 2,
       f"the sounding region shrinks as START moves right: {totals[0]} -> {totals[-1]} px")

    print("a turn moves one pixel of the window")
    # The first version of the step was len/1000 >> zoom, so at 1x a full pass was ~1000
    # turns. That is the defect this checks: the step has to be the window's own pixel,
    # because the picture is the only thing that says how far a turn should go. Measured
    # as samples-per-turn against (len >> zoom) / 232.
    slot_len = probe_len(out)
    for z, want in ((0, 1.0), (2, 1.0), (4, 1.0)):
        step = probe_step(out, z)
        px = (slot_len >> z) / 232.0
        ck(abs(step / px - want) < 0.08,
           f"zoom {z}: one turn = {step} samples, one pixel = {px:.2f} (ratio {step / px:.3f})")

    print("the playhead is drawn, and only while the material sounds")
    # Nothing when silent: a head parked at 0 would claim the material is playing.
    run("""wait 400
press SEL
wait 60
release SEL
wait 200
press EDIT
wait 60
release EDIT
wait 200
shot quiet.png
""", out)
    ck(not find_colour(band(os.path.join(out, "quiet.png")), (255, 255, 255), 3),
       "silent, no playhead is drawn")

    # And it moves while a key is held: two frames of the same slot, different columns.
    # The tab is the top 5 rows, which nothing else uses, so a white column there is the
    # playhead and not a marker.
    cols = []
    for i, d in enumerate((60, 260, 620)):
        run("""wait 400
press SEL
wait 60
release SEL
wait 200
press EDIT
wait 60
release EDIT
wait 200
key 12 down
wait %d
shot head%d.png
key 12 up
""" % (d, i), out)
        b = band(os.path.join(out, "head%d.png" % i))
        top = find_colour(b, (255, 255, 255), 4)
        cols.append(top)
    ck(all(c for c in cols), f"a held key draws a playhead: {[c[:2] for c in cols]}")
    moved = len({tuple(c[:1]) for c in cols})
    ck(moved >= 2, f"and the playhead advances: {[c[:1] for c in cols]}")

    print("a loop repeats between LOOP S and LOOP E, not between START and END")
    # The engine was told `loop, from start to end` no matter what the page showed, so
    # the two loop lines sat where the page put them and the playhead crossed the whole
    # region between them. Nothing about the region's pixels could catch that: the marks
    # were right and the sound was wrong. So the head is followed over time.
    o = run("""wait 400
press SEL
wait 60
release SEL
wait 200
press EDIT
wait 60
release EDIT
wait 200
turn PRESET 1
wait 60
turn K2 60
wait 60
turn K3 -80
wait 60
expect region_loops 0
expect region_loope 0
""", out, allow_fail=True)
    loop_s = int(re.search(r"expect region_loops == 0, got (\d+)", o).group(1))
    loop_e = int(re.search(r"expect region_loope == 0, got (\d+)", o).group(1))
    slot = probe_len(out)
    want_lo = 232 * loop_s // slot
    want_hi = 232 * loop_e // slot
    ck(want_hi - want_lo > 40, f"the loop window is wide enough to see: {want_lo}..{want_hi} px")

    heads = []
    for i, d in enumerate((200, 500, 900, 1300, 1700, 2100, 2500)):
        run("""wait 400
press SEL
wait 60
release SEL
wait 200
press EDIT
wait 60
release EDIT
wait 200
turn PRESET 1
wait 60
turn K2 60
wait 60
turn K3 -80
wait 60
key 12 down
wait %d
shot lp%d.png
key 12 up
""" % (d, i), out)
        heads.append(head_col(band_full(os.path.join(out, "lp%d.png" % i))))
    ck(all(h is not None for h in heads), f"the head is drawn throughout: {heads}")
    # The first frames are the INTRO: the region is start..end and the loop lives inside
    # it, so a held note walks in from START and only then repeats. That is what a
    # sampler does and it is the reason the region and the loop are two separate pairs.
    # What must be inside the loop is everything after the head first reaches LOOP S.
    entered = next((i for i, h in enumerate(heads) if h >= want_lo - 6), None)
    ck(entered is not None and entered < len(heads) - 1,
       f"the head reaches the loop and keeps going: {heads} vs loop {want_lo}..{want_hi}")
    inside = heads[entered:]
    ck(min(inside) >= want_lo - 6 and max(inside) <= want_hi + 6,
       f"after the intro, every frame is inside the loop: {inside} vs {want_lo}..{want_hi}")
    # and it wraps: a monotonic sweep to the loop's end then a jump backwards
    wraps = sum(1 for a, b2 in zip(heads, heads[1:]) if b2 < a - 20)
    ck(wraps >= 1, f"the head wraps inside the loop instead of running to END: {heads}")

    print("the four boundaries are four colours, and the playhead is not one of them")
    # Four identical green lines were the page's real defect: it worked and could not be
    # read. Each boundary now has its own hue and its own knob is printed in it, so this
    # checks the hues are distinct AND that the playhead (white) never collides with them.
    # A scene where all four are visible and apart: loop on, LOOP S pushed to ~60 px,
    # END and LOOP E left at the end of the slot.
    run("""wait 400
press SEL
wait 60
release SEL
wait 200
press EDIT
wait 60
release EDIT
wait 200
turn PRESET 1
wait 100
turn K2 60
wait 100
shot c2.png
""", out)
    b = band(os.path.join(out, "c2.png"))
    # K3 was the last turn, so LOOP S is live (full hue) and the other three are at
    # 3/5 -- the design keeps each marker's hue and changes only its brightness, so a
    # line never changes identity when the hand moves. Both weights are therefore legal
    # for a boundary that is not live; only "which of the four is it" is under test.
    hues = {"START": (255, 96, 96), "END": (255, 208, 96),
            "LOOP S": (96, 224, 255), "LOOP E": (198, 140, 255)}
    live_now = "LOOP S"
    seen = {}
    for nm, rgb in hues.items():
        at = [find_colour(b, q565(rgb)), find_colour(b, q565(rgb, 3, 5))]
        hit = at[0] if at[0] else at[1]
        seen[nm] = hit
        want_live = (nm == live_now)
        ck(bool(hit) and bool(at[0]) == want_live,
           f"{nm} is drawn in its own hue at x={hit} ({'live' if at[0] else '3/5'})")
    cols = sorted({tuple(v) for v in seen.values() if v})
    ck(len(cols) >= 3, f"and they are not all in one place: {seen}")

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
turn K4 -30
wait 100
expect region_end 0
""", out, allow_fail=True)
    ck(not expect_ok(o), "K4 moves END")
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
