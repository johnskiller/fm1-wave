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
# The knob's centre, in screen coordinates, and the radius band the art lives in.
#
# Measured, not derived: the ring was widened from R=11 to R=23 when the value moved
# inside it, and the centre moved with it. `cy = KNOB_ARC_TOP + KNOB_ARC_R` with
# KNOB_ARC_TOP = KNOB_TOP + 17 = 17 gives 40 on the band, 176 + 40 = 216 on screen. That
# one agrees with the source now -- the earlier disagreement (source said 221, pixels said
# 224.5) was an arithmetic slip in the reading, recorded in spec.md 9i, and re-deriving it
# here rather than carrying the old measured value is the point: the geometry changed, so
# the measurement had to be redone.
CX = 31.5
CY = 216.0
R0, R1 = 20.0, 26.5     # the stroke's own radii (23 and 22), plus the pointer's plinth


def annulus(a, colour, r0=R0, r1=R1, tol=6):
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
            if R0 <= r <= R1:
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
        for r in (21, 22, 23):
            x = int(CX) + int(round(r * math.sin(ang)))
            y = int(CY) + int(round(r * math.cos(ang)))
            if 0 <= y < a.shape[0] and s[y, x]:
                hit += 1
    ok(hit == 0, f"the bottom gap is empty at v={v}", f"{hit} ink px inside +-30 deg")

# ── 5. the value is centred INSIDE the ring, and the name is not ───────────────
# Four left-aligned strings read as a list; four centred values read as four objects.
# The value lives in the ring's hole now, so it is sampled at the ring's own middle row
# band and inside the inner radius -- a rectangle that excludes the stroke on both sides
# by construction. Reading it at the old y+22 row found the ring's own pixels instead.
a = shots[64]
ccx = COL_X[0] + COL_W // 2
# Sample the value by ROW, not through a circle. A radial window cannot work once the
# stroke is thick: at r = 18 the circle still reaches y = cy +- 18, and the 5 px stroke's
# rounded shoulder lives exactly there, so the window read the ring's own pixels and
# reported a centre of 25 against 31. The glyphs occupy 9 rows around the middle
# (measured), and those rows are well inside the hole at every radius that matters.
hole = []
for y in range(int(CY) - 4, int(CY) + 5):
    for x in range(ccx - 22, ccx + 23):
        if (near(a, SYNTH_INK) | near(a, SYNTH_INK_HI))[y, x] and abs(x - ccx) < 18:
            hole.append(x)
ok(bool(hole) and abs((min(hole) + max(hole)) / 2 - ccx) <= 2,
   "the value is centred in its column",
   f"ink x {min(hole) if hole else '-'}..{max(hole) if hole else '-'} vs centre {ccx}")
# and it must FIT the hole: the widest string any page prints has to clear the stroke.
# Measured against the chord at the text's own height (FONT_XS is 15 px tall, so text
# spans +-7.5 of the middle), not the inner diameter, which is the wider and wrong number.
# The half-height is MEASURED, not taken from the font: FONT_XS's cell is 15 px but its
# glyphs put ink on 9 rows (-4..+4 around the middle). Using 7.5 here made the chord 32.7
# for a 5 px stroke against a 33 px string -- a conflict that does not exist on screen,
# and the kind of "over" that would have sent the stroke back to 4 px for no reason.
GLYPH_HALF = 4.5
hole_r = 23.0 - 5                      # innermost lit radius is 23-4 = 19, clear hole 18
hole_chord = 2 * math.sqrt(hole_r ** 2 - GLYPH_HALF ** 2)
widest = (max(hole) - min(hole) + 1) if hole else 0
ok(widest <= hole_chord, "and the value fits inside the ring's hole",
   f"{widest} px against a {hole_chord:.0f} px chord")
name_row = a[BAND_Y + 0:BAND_Y + 12, COL_X[0]:COL_X[0] + COL_W]
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
    for y in range(BAND_Y + 17, BAND_Y + 64):
        for xx in range(c - 26, c + 27):
            if ink[y, xx] and R0 <= math.hypot(xx - c, y - CY) <= R1:
                hit += 1
    if hit > 20:
        n_arcs += 1
ok(n_arcs == 4, "the ENV page has an arc under each of its four knobs", f"{n_arcs}/4")

# ── 7. the stroke has no pinholes in it ───────────────────────────────────────
# This is the check that was missing when the art shipped with holes. A ring drawn as
# separate 1 px Bresenham chains per radius (the first 5 px stroke) does not tile the
# band: each chain rounds its own way and two neighbours can straddle a pixel, so the
# background shows through the stroke. Measured on the host at the time: 6 enclosed
# background pixels per knob at r = 19.8 .. 21.9, and they looked like exactly what they
# were -- specks of the background colour inside the ring. Two chains (the earlier 2 px
# stroke) almost always touch, which is why nobody saw it until the stroke got thick.
#
# So: flood the background from outside each knob's box, and anything still background
# AND inside it that is not reachable from the edge is a hole in the stroke. Counted over
# the stroke's own radial band, because the glyphs have counters of their own -- the hole
# inside a "6" or a "4" is the letter, not a defect, and counting those would make this
# check fail for the wrong reason.
from collections import deque


def stroke_holes(a, cx, cy, r_lo, r_hi):
    bg = near(a, SYNTH_BG)
    lit = ~bg
    x0, y0 = cx - 25, cy - 25
    sub = lit[y0:cy + 26, x0:cx + 26]
    h, w = sub.shape
    outside = np.zeros_like(sub)
    q = deque()
    for x in range(w):
        for y in (0, h - 1):
            if not sub[y, x] and not outside[y, x]:
                outside[y, x] = True
                q.append((y, x))
    for y in range(h):
        for x in (0, w - 1):
            if not sub[y, x] and not outside[y, x]:
                outside[y, x] = True
                q.append((y, x))
    # 8-connected: two background pixels touching at a corner are one gap to the eye, and
    # a 4-connected flood over-reports holes that a person cannot see.
    near8 = ((1, 0), (-1, 0), (0, 1), (0, -1), (1, 1), (1, -1), (-1, 1), (-1, -1))
    while q:
        y, x = q.popleft()
        for dy, dx in near8:
            ny, nx = y + dy, x + dx
            if 0 <= ny < h and 0 <= nx < w and not sub[ny, nx] and not outside[ny, nx]:
                outside[ny, nx] = True
                q.append((ny, nx))
    enclosed = (~sub) & (~outside)
    ys, xs = np.nonzero(enclosed)
    return sum(1 for yy, xx in zip(ys, xs)
               if r_lo <= math.hypot(x0 + xx - cx, cy - (y0 + yy)) <= r_hi)


for v in VALS:
    a = shots[v]
    n = stroke_holes(a, int(CX), int(CY), 18.5, 24.0)
    ok(n == 0, f"the stroke has no pinholes at cut {v}", f"{n} enclosed background px")


print(f"\n{checks} checks, {failed} failed")
sys.exit(1 if failed else 0)
