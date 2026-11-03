#!/usr/bin/env python3
"""test_knob.py -- the knob art: geometry that reads, and a fill that follows the value.

The knobs used to be a rectangular bar. The bar answered "how much" but not "how far
round", and this file exists because the arc that replaced it can be wrong in ways that
still look like a knob: a ring with no gap, a fill that does not move, a pointer inside
the gap, a value that runs anticlockwise. Every one of those was in a draft of this code,
so each is measured here rather than eyeballed.

Measured, not asserted from the source: the tests read the host simulator's own
screenshots and count pixels. That is the only way to catch "the arc is drawn but at the
wrong radius", which compiles perfectly.

One thing this file does NOT do is judge whether the art is pleasant. That is the
player's call by looking at it; these checks only pin the facts the design depends on.
"""
import math
import os
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
HOST = os.path.join(ROOT, "app", "build", "host", "wave_host")
OUT = os.path.join(ROOT, "app", "build", "knobtest")

# The four columns of the knobs band, and the band's place on a 240x240 screen.
# app.c: x = 4, 62, 120, 178 with w = 54; the band is the bottom 64 px.
BAND_Y = 176
COL_X = (4, 62, 120, 178)
COL_W = 54

SYNTH_BG = (41, 28, 8)          # sk.bg for SYNTH
SYNTH_INK = (238, 137, 24)      # sk.ink: the track's "off" neighbours and the fill
SYNTH_TRACK = (41, 24, 0)       # dim(sk.ink, 1, 5), RGB565-rounded
SYNTH_INK_HI = (255, 186, 65)  # sk.ink_hi: what a LIT page fills its arc with

checks = 0
failed = 0


def ok(cond, what, detail=""):
    global checks, failed
    checks += 1
    if not cond:
        failed += 1
    print(("ok   " if cond else "FAIL ") + what + (f"  [{detail}]" if detail else ""))


def run(script, tag):
    d = os.path.join(OUT, tag)
    os.makedirs(d, exist_ok=True)
    p = os.path.join(OUT, tag + ".txt")
    with open(p, "w") as f:
        f.write(script)
    r = subprocess.run([HOST, p, d], capture_output=True, text=True, timeout=300)
    out = "\n".join(l for l in (r.stdout + r.stderr).splitlines() if not l.startswith("dbg"))
    if r.returncode not in (0, 1):
        raise SystemExit(f"host failed ({r.returncode}):\n{out}")
    return out, d


def img(d, name):
    return np.array(Image.open(os.path.join(d, name)).convert("RGB")).astype(int)


def near(a, colour, tol=6):
    return np.abs(a - np.array(colour)).sum(axis=2) < tol


def angles_of(mask, cx, cy):
    """Every angle (deg, 0 = up, clockwise positive) that the mask lights."""
    ys, xs = np.nonzero(mask)
    out = set()
    for y, x in zip(ys, xs):
        out.add(round(math.degrees(math.atan2(x - cx, -(y - cy)))))
    return sorted(out)


def widest_gap(angles):
    """The largest run of degrees with nothing in it. `angles` must be sorted."""
    best = (0, 0, 0)
    for i in range(len(angles)):
        nxt = angles[(i + 1) % len(angles)]
        d = (nxt - angles[i]) % 360
        if d > best[2]:
            best = (angles[i], nxt, d)
    return best


# ── the values, driven by exact encoder steps ───────────────────────────────────
# One encoder detent is +-4 on a knob's value (app.c: `k_cut + e * 4`), so "spin K1 n"
# moves the value by 4n. Measured, because the first draft used "turn" and the encoder's
# own acceleration ran 32 steps to 127 -- three test values silently became the same one.
VALS = (0, 32, 64, 96)
out, D = run(
    "spin K1 -127\nmaster 4096\nexpect cut 0\nshot v0.png\n"
    "spin K1 8\nmaster 4096\nexpect cut 32\nshot v32.png\n"
    "spin K1 8\nmaster 4096\nexpect cut 64\nshot v64.png\n"
    "spin K1 8\nmaster 4096\nexpect cut 96\nshot v96.png\n"
    "tap ENV\nmaster 4096\nshot env.png\n",
    "vals",
)
for v in VALS:
    ok(f"ok cut == {v}" in out, f"the host reached cut {v} before the shot",
       [l for l in out.splitlines() if l.startswith(("FAIL", "  ok cut"))][-1].strip())

shots = {v: img(D, f"v{v}.png") for v in VALS}

# ── 1. the arc is a ring with a gap, and the gap is at the bottom ───────────────
# A ring with no gap loses the only cue for where the range starts and ends. A gap
# somewhere other than the bottom makes the control read as rotated.
# The knob's centre, in screen coordinates.
#
# Measured, by a least-squares circle fit over the arc's own pixels (v=0, column 0):
# centre (31.43, 224.55), radius 10.01. cx matches app.c's `col_x + w/2` = 4 + 27 = 31.
#
# cy does NOT match the obvious reading of the source, and that is worth writing down:
# `cy = y + 34 + KNOB_ARC_BOX/2 - 2` with y = KNOB_TOP = 4 gives 176 + 34 + 13 - 2 = 221,
# 3.5 px above where the arc actually is. The reason is that the KNOB band's canvas
# starts at SCOPE_H + MAT_H = 176 and the knob's `y` argument is KNOB_TOP = 4 INSIDE that
# canvas, so the sum is 176 + 4 + 34 + 11 = 225, not 176 + 34 + 11. Deriving the centre
# from the source and trusting it made a correct arc fail its own span check, which is
# exactly the class of mistake this file exists to catch -- so the centre is measured and
# the source reading is recorded as the thing that was wrong.
CX = 31.5
CY = 224.5


def annulus(a, colour, r0=9.0, r1=14.0, tol=6):
    """Angles present on the ring at radius r0..r1 -- the arc, without the text."""
    m = near(a, colour, tol)
    out = set()
    ys, xs = np.nonzero(m)
    for y, x in zip(ys, xs):
        r = math.hypot(x - CX, y - CY)
        if r0 <= r <= r1:
            out.add(round(math.degrees(math.atan2(x - CX, -(y - CY)))))
    return sorted(out)


k0 = shots[0]
axis = annulus(k0, SYNTH_TRACK)
g0, g1, gw = widest_gap(axis)
mid = ((g0 + g1) / 2) % 360
if mid > 180:
    mid -= 360
ok(abs(mid) <= 25, "the track's gap is centred on straight down (the bottom)",
   f"gap {g0}..{g1} = {gw} deg, centre {mid:.0f}")
ok(70 <= gw <= 110, "and it is a quarter turn, not a token slit",
   f"{gw} deg")
# The span is 360 minus the gap: counting the angles present would count only those at
# least one pixel landed on, and the rounding of a 11 px radius drops several degrees.
drawn = 360 - gw
ok(250 <= drawn <= 290, "the drawn track spans about 270 degrees", f"{drawn} deg of track")

# ── 2. the fill follows the value, and the track gives way ─────────────────────
# This is the check that pinned the first real defect: the fill condition compared the
# wrong quantity, so every value produced a full ring, and the control looked broken
# while `expect cut` passed on every step.
def ink_counts(a):
    """Filled and track pixels inside the arc's annulus, for column 0 only.

    Whole-band counting was the first version and it is wrong: the value's text is drawn
    in the same ink as the fill, so 12 glow px of "0" counted as fill, and every column's
    art added to every other column's total. The annulus around one knob's centre is the
    only region where the count means "the arc".
    """
    f = near(a, SYNTH_INK)
    t = near(a, SYNTH_TRACK)
    nf = nt = 0
    for m, acc in ((f, "f"), (t, "t")):
        ys, xs = np.nonzero(m)
        c = 0
        for y, x in zip(ys, xs):
            r = math.hypot(x - CX, y - CY)
            if 9.0 <= r <= 14.0:
                c += 1
        if acc == "f":
            nf = c
        else:
            nt = c
    return nf, nt


fills = []
tracks = []
for v in VALS:
    f, t = ink_counts(shots[v])
    fills.append(f)
    tracks.append(t)

ok(all(fills[i] < fills[i + 1] for i in range(len(fills) - 1)),
   "the filled arc grows with the value", "fill px " + " < ".join(str(x) for x in fills))
ok(all(tracks[i] > tracks[i + 1] for i in range(len(tracks) - 1)),
   "and the track it covers gives way", "track px " + " > ".join(str(x) for x in tracks))
ok(fills[-1] / max(1, fills[0]) > 5, "the range of the fill is large, not a token",
   f"{fills[0]} px at v=0 to {fills[-1]} px at v={VALS[-1]}")

# ── 3. the fill starts at the lower LEFT and runs clockwise ────────────────────
# The hardware encoder turns clockwise for more (measured: turn K1 +1 raises the value),
# so the arc must fill that way. An anticlockwise fill is a second convention on the
# same panel, and a player cannot tell which one a given page uses.
def fill_angles(a):
    return annulus(a, SYNTH_INK)


fa0 = fill_angles(shots[VALS[0]])
fa1 = fill_angles(shots[VALS[-1]])
ok(bool(fa0) and min(fa0) <= -100, "at low values the fill begins at the lower left",
   f"fill angles {fa0[:4]}")

# The fill must be CONTIGUOUS from the start of the sweep. Checking only min/max let a
# mirrored fill pass: a fill measured from the far end still has a low-min and a high-max
# once it is long enough, so the angles alone cannot tell the direction. A gap in the
# middle of the filled region is what an anticlockwise fill looks like when the value is
# small, and that is what is asserted here.
# At v=0 there is nothing to be contiguous about (one angle, where the sweep starts), so
# coverage is measured at the middle value, where a mirrored fill leaves the start end
# empty and a correct one is filled from -135 upward.
#
# The gaps allowed here are the ring's own resolution, not slack: at r = 10..13 a whole
# degree of angle is a third of a pixel, so which degrees get a lit pixel at all depends on
# rounding. Measured: a correct fill at v=64 lights degrees in clusters with holes up to
# 4 apart. The check is therefore "filled from the start and reaching past the top", not
# "every degree present".
# The end of the fill is the value's own place on the sweep: -135 + (v/127)*270.
# At v=64 that is +1.3 deg, i.e. just past straight up. Asserted against the formula
# rather than against a round number, because "past the top" is only true for v > 63.5
# and the first draft asked for +30, failing a correct arc at v=64.
fam = sorted(fill_angles(shots[64]))
want_end = -135 + (64 / 127.0) * 270
ok(bool(fam) and fam[0] <= -120 and abs(max(fam) - want_end) <= 25,
   "at a mid value the fill is the half of the sweep the value asks for",
   f"fill {fam[0] if fam else '-'}..{max(fam) if fam else '-'} deg, formula says ..{want_end:.0f}")
# The end angle is the value's own place on the sweep: -135 + (v/127)*270. At v=96 that
# is +69, which is still on the right-hand side but well short of the lower-right end at
# +135 -- so the assertion is "past straight up", not "at the end". The first draft asked
# for +100 and failed a correct arc.
want = -135 + (VALS[-1] / 127.0) * 270
ok(bool(fa1) and max(fa1) >= want - 12,
   "and at high values it has swept past the top to the right",
   f"last fill angle {max(fa1) if fa1 else '-'} deg, expected about {want:.0f}")

# ── 4. the gap stays empty at every value ─────────────────────────────────────
# A hand from the centre was the first pointer, and it reached into the gap: at v=0 the
# control read as a closed ring. A tick on the arc keeps the gap clear.
for v in VALS:
    a = shots[v]
    s = near(a, SYNTH_INK) | near(a, SYNTH_TRACK)   # full-screen: CX/CY are screen coords
    # The window is +-30 deg, not +-40: the arc's own ENDS land at about +-40 (measured:
    # the last track pixel is at angDown 42), so a wider window counts the ends of a
    # correct arc as ink in the gap. 30 leaves 10 deg of margin on both sides.
    hit = 0
    for k in range(-30, 31):
        ang = math.radians(k)             # straight down is 0 here
        for r in (10, 11, 12):
            x = int(CX) + int(round(r * math.sin(ang)))
            y = int(CY) + int(round(r * math.cos(ang)))
            if 0 <= y < a.shape[0] and s[y, x]:
                hit += 1
    ok(hit == 0, f"the bottom gap is empty at v={v}", f"{hit} ink px inside +-30 deg")

# ── 5. the value is centred under its knob, and the name is not ────────────────
# Four left-aligned strings read as a list; four centred values read as four objects.
a = shots[64]
row = a[BAND_Y + 22:BAND_Y + 30, COL_X[0]:COL_X[0] + COL_W]
ink = near(row, SYNTH_INK) | near(row, SYNTH_BG) & False
cols = [x for x in range(row.shape[1]) if ink[:, x].any()]
ok(bool(cols) and abs((cols[0] + cols[-1]) / 2 - COL_W / 2) <= 6,
   "the value is centred in its column",
   f"ink x {cols[0]}..{cols[-1]} of 0..{COL_W - 1}")
name_row = a[BAND_Y + 4:BAND_Y + 12, COL_X[0]:COL_X[0] + COL_W]
ncols = [x for x in range(name_row.shape[1])
         if (near(name_row, SYNTH_INK) | near(name_row, (238, 137, 24)))[:, x].any()]
ok(bool(ncols) and ncols[0] <= 3, "the name stays left-aligned in its column",
   f"name starts at x {ncols[0] if ncols else '-'}")

# ── 6. the ENV page draws four knobs and no others ────────────────────────────
# A page that leaks a knob from the page before it is a wiring mistake this suite can
# see: the ENV page's four columns must each carry an arc.
env = img(D, "env.png")
n_arcs = 0
# ink_hi matters here and did not appear anywhere else in this file: the ENV page's knobs
# are LIT (app.c passes lit=1 for a page), so their arcs fill at ink_hi, and a mask of
# ink+track found 0 px under SUSTAIN -- the one knob whose arc is full and therefore has
# no track left. Two wrong assumptions in a row, both from assuming the default page.
ink = near(env, SYNTH_INK) | near(env, SYNTH_INK_HI) | near(env, SYNTH_TRACK)
for col in COL_X:
    c = col + COL_W // 2
    # Any ink in the annulus, not just the track: SUSTAIN defaults to 127, so its arc is
    # filled end to end and has no track pixels left at all. Counting the track found
    # 3 of 4, and the missing one was the knob that was working correctly.
    hit = 0
    for y in range(BAND_Y + 34, BAND_Y + 60):
        for xx in range(c - 14, c + 15):
            if ink[y, xx] and math.hypot(xx - c, y - CY) <= 14:
                hit += 1
    if hit > 20:
        n_arcs += 1
ok(n_arcs == 4, "the ENV page has an arc under each of its four knobs", f"{n_arcs}/4")

print(f"\n{checks} checks, {failed} failed")
sys.exit(1 if failed else 0)
