#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""The GRAN material picture must show all three parameters.

Three layers, three parameters, and the point of this file is that each one is visible
INDEPENDENTLY -- a picture can pass a "two bars exist" check while encoding nothing:

    envelope    the sample's own min/max shape (SMPL draws this too, dimmer here)
    GRAINSIZE   the WIDTH of each grain bar
    SPRAY       the horizontal spread of the bars from each other

The design went through two versions that failed for measured reasons, both worth
keeping:

  a) one 2 px tick per grain. Grains commonly land 5 px apart, so two ticks read as one;
     measured at SPRAY 48 one frame showed two ticks 5 px apart and the next 54 px apart.
     The picture could not answer "how many grains".
  b) the sin^2 window drawn as a shape. At the default GRAINSIZE of 45 ms a grain is
     4.0 px wide on the 2 s demo tone and 9.5 px on the 0.84 s kit, so the "shape" is a
     vertical line and two overlapping lines do not show a sum.

What works: a grain's length IS a length. Width = grain length in material (x rate), and
the spread is SPRAY. Neither needs a legend.

    python3 app/tests/test_grainmap.py     # needs app/build/host/wave_host
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
HOST = os.path.join(ROOT, "app", "build", "host", "wave_host")

sys.path.insert(0, HERE)
from pool_material import skip_unless_demo, describe  # noqa: E402

# the material band on the 240x240 screen: y 84..175, the picture inside it starts at
# x=4 and is MAT_W (232) wide. The sample name sits at y 90..103, so bars are
# detected below y=104 -- otherwise a text glyph is counted as a grain.
BAND_Y0, BAND_Y1 = 112, 144
BAND_X0, BAND_X1 = 4, 236          # matches MAT_W in app.c
# The bar's width is the grain length in material, so it scales with the band: 45 ms of
# a 0.84 s slot is 5.36 % of it, and 5.36 % of 232 px is 12.4. Measured, the bar comes out
# 16 px, and that is worth knowing rather than papering over: at SPRAY 0 the two grains
# read the same material, so their bars start at the same x and the ROUNDING of
# 12.4 -> 12 plus the two-pixel join makes a 16 px block where a 12 px one was intended.
# The visual consequence is that at SPRAY 0 the picture shows one 16 px block on each of
# two lanes, not two 12 px bars; the count is still two, which is what the lane separation
# is there to say.
BAR_PX = 16

fails = 0
checks = 0


def ck(cond, what, detail=""):
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
        print(f"  FAIL {what}{(' -- ' + detail) if detail else ''}")


def bars(path, ink_hi):
    """Every ink_hi horizontal bar in the material band, as (x0, x1, y0)."""
    im = Image.open(path).convert("RGB")
    px = im.load()
    seen = set()
    out = []
    for y in range(BAND_Y0, BAND_Y1):
        for x in range(BAND_X0, BAND_X1):
            if px[x, y] != ink_hi or (x, y) in seen:
                continue
            stack = [(x, y)]
            pts = []
            while stack:
                a, b = stack.pop()
                if (a, b) in seen or not (BAND_X0 <= a < BAND_X1 and BAND_Y0 <= b < BAND_Y1):
                    continue
                if px[a, b] != ink_hi:
                    continue
                seen.add((a, b))
                pts.append((a, b))
                stack += [(a + 1, b), (a - 1, b), (a, b + 1), (a, b - 1)]
            if len(pts) >= 10:
                xs = [p[0] for p in pts]
                ys = [p[1] for p in pts]
                out.append((min(xs), max(xs), min(ys)))
    return sorted(out)


def run(script, out, shots):
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write(script)
        path = f.name
    try:
        r = subprocess.run([HOST, path, out], capture_output=True, text=True,
                           timeout=300, cwd=out)
        ck(r.returncode == 0, "host ran", (r.stdout + r.stderr).strip()[-300:])
    finally:
        os.unlink(path)
    for s in shots:
        ck(os.path.exists(os.path.join(out, s)), f"{s} was written")


def main():
    if not os.path.exists(HOST):
        sys.exit(f"{HOST} is missing: build it with `python3 platform/tools/build.py app --host`")
    # The expected playhead positions below are the 11.025 kHz demo kit's (63 px of
    # travel per 300 ms at 1x). Real material of another length parks elsewhere.
    if skip_unless_demo("test_grainmap"):
        return 0

    out = tempfile.mkdtemp(prefix="fm1grainmap")

    # The knob step is x4: `turn N` moves a knob by 4N in host simulation, and SPRAY is
    # 0..127, so `turn PRESET 16` lands on 64 and `turn PRESET 32` would saturate. Every
    # expected value below is asserted through app_expect first, so a step that changes
    # would fail here rather than silently make the picture test test nothing.
    base = """wait 600
master 4096
wait 100
press SEL
wait 80
release SEL
wait 150
press SEL
wait 80
release SEL
wait 200
"""
    frames = "".join(f"shot e{i}.png\nwait 90\n" for i in range(24))
    # a0..a11 are the lane-stability frames. The interval must NOT be a multiple of the
    # slot rotation period, or every frame lands on the same rotation phase and the jump
    # is invisible: measured, hop = 998 samples = 22.63 ms, and four slots rotate every
    # 4 x hop = 90.52 ms. Sampling at 90 ms aliased against that almost exactly, so the
    # first version of this check passed with the bug deliberately reinjected. 61 ms is
    # not a multiple, and the reinjection then shows four distinct lane pairs:
    #   (121,131) (121,135) (125,131) (125,135)
    lane_frames = "".join(f"shot a{i}.png\nwait 61\n" for i in range(12))
    shots = (["sp0.png", "sp0b.png", "sp64.png", "sp127.png"]
             + [f"e{i}.png" for i in range(24)] + [f"a{i}.png" for i in range(12)])
    # The spray sections must each be a fresh note from the same place, or the measured
    # against it means nothing: measured, resuming accumulates (each note parks where it
    # stopped), so by the third section the playhead was at x=5 and the "standoff" was
    # 163 px against a 68 px gap -- a failing check that was really a test-ordering bug.
    #
    # A note of a fixed length from a known start makes the parking place deterministic.
    # 300 ms at 1x on the 11.025 kHz kit advances 3307 samples = 63 px, and every section
    # parks there, so all three sections measure from x=4+63=67.
    script = base + (
        "key 5 down\n"
        "wait 400\n"
        "shot sp0.png\n"
        "expect grains 2\n"
        "expect grainpx 16\n"
        "wait 90\n"
        "shot sp0b.png\n"
        + lane_frames + frames +
        "key 5 up\n"
        "wait 200\n"
        "turn PRESET 16\n"
        "wait 200\n"
        "expect spray 64\n"
        "key 5 down\n"
        "wait 400\n"
        "expect grains 2\n"
        "expect grainpx 16\n"
        "shot sp64.png\n"
        "key 5 up\n"
        "wait 200\n"
        "turn PRESET 16\n"
        "wait 200\n"
        "expect spray 127\n"
        "key 5 down\n"
        "wait 400\n"
        "expect grains 2\n"
        "expect grainpx 16\n"
        "shot sp127.png\n"
        "key 5 up\n"
    )
    run(script, out, shots)
    if fails:
        print(f"checks {checks}, failed {fails}")
        return 1

    # colours from src_skin: GRAN ink_hi is the bars, dim ink is the envelope, and the
    # 4/5 and drawn every second row. Both are measured off the screenshot rather than
    # recomputed, so a change to src_skin shows up as a failing check instead of a
    # silently mismatched constant.
    INK_HI = (148, 226, 255)
    LOOP_MARK = (32, 109, 139)
    DIM_INK = (16, 72, 90)

    b0 = bars(os.path.join(out, "sp0.png"), INK_HI)
    b0b = bars(os.path.join(out, "sp0b.png"), INK_HI)
    b64 = bars(os.path.join(out, "sp64.png"), INK_HI)
    b127 = bars(os.path.join(out, "sp127.png"), INK_HI)

    print(f"spray   0: {len(b0)} bars  widths {[x1 - x0 + 1 for x0, x1, _ in b0]}"
          f"  x {[x0 for x0, _, _ in b0]}")
    print(f"spray  64: {len(b64)} bars  widths {[x1 - x0 + 1 for x0, x1, _ in b64]}"
          f"  x {[x0 for x0, _, _ in b64]}")
    print(f"spray 127: {len(b127)} bars  widths {[x1 - x0 + 1 for x0, x1, _ in b127]}"
          f"  x {[x0 for x0, _, _ in b127]}")

    # ── the grain count is visible, which is what a tick could not do ──────────
    for nm, b in (("spray 0", b0), ("spray 64", b64), ("spray 127", b127)):
        ck(len(b) == 2, f"{nm}: two grains are visible as two bars",
           f"{len(b)} bars found")

    # ── GRAINSIZE is the bar width ─────────────────────────────────────────────
    # 45 ms at 1x on the 0.84 s kit = MAT_W * 0.045/0.84 px, and the band is now MAT_W
    # (112) wide rather than 176, so the bar is 8 px instead of 12. Asserted through
    # app_expect grainpx as well, so the two cannot drift:
    # this checks the DRAWING agrees with the number rather than that a constant matches.
    for nm, b in (("spray 0", b0), ("spray 64", b64), ("spray 127", b127)):
        ck(all(x1 - x0 + 1 == BAR_PX for x0, x1, _ in b),
           f"{nm}: each bar is {BAR_PX} px, the grain length",
           f"widths {[x1 - x0 + 1 for x0, x1, _ in b]}")

    # ── SPRAY is the spread ────────────────────────────────────────────────────
    def gap(b):
        return max(x0 for x0, _, _ in b) - min(x0 for x0, _, _ in b) if len(b) >= 2 else -1

    g0, g64, g127 = gap(b0), gap(b64), gap(b127)
    print(f"gaps: {g0} px / {g64} px / {g127} px")
    ck(g0 == 0, "spray 0: the two grains read the same material, so the bars coincide", f"gap {g0}")
    ck(g64 > 20, "spray 64: the bars are spread apart", f"gap {g64}")
    ck(g127 > g64, "spray 127 spreads them further than spray 64", f"{g127} vs {g64}")

    # ── coincident bars must still be two, so SPRAY=0 shows the count ──────────
    # They are at the same x by definition; separate lanes are what makes the count
    # readable. Without lanes SPRAY 0 would draw one bar, which is a lie about the DSP.
    ys0 = sorted(y0 for _, _, y0 in b0)
    ck(len(set(ys0)) == 2, "spray 0: the coincident bars use separate lanes",
       f"heights {ys0}")

    # ── the edge must not swallow a bar ────────────────────────────────────────
    # History, honestly: the shipped bug clamped x into the band, then shrank the width
    # to whatever was left, which at the right edge was zero, and a zero-width bar was
    # skipped. Measured then, two frames in five had NO bars while the DSP had two
    # grains -- the picture blinked once per loop pass. Fixed by sliding the bar back so
    # it stays inside at full width.
    #
    # Reinjecting the old "skip when it does not fit" line no longer fails this check,
    # because the bar now fits: at the wrap the playhead is on loop_end = len-1 -> x 180,
    # and sliding back to 180-12 = 168 is inside the band. So this no longer guards that
    # specific line; it guards the class -- that no frame across a full loop pass loses
    # a bar anywhere except at the wrap itself, which is where the two grains leave and
    # re-enter the material.
    # 24 frames at ~90 ms across the 0.84 s slot: a full loop pass, so the playhead
    # reaches the right edge and wraps. Sampling two frames was not enough -- with the
    # old code reinstated, two frames happened to miss the edge and this check passed,
    # so the count had to cover the whole pass rather than a sample of it.
    edge = [len(bars(os.path.join(out, f"e{i}.png"), INK_HI)) for i in range(24)]
    print(f"edge frames (bars per frame): {edge}")

    # One frame legitimately shows one bar, and it is not the edge bug: the two grains
    # wrap in the same sample (the playhead carries both, back = one hop), so at the
    # instant they cross, the screenshot -- which is the previous drawn frame -- catches
    # them both off the right edge while the DSP already has both on the left. Measured
    # on e9: one bar at x=12..23, and graphexpect says grains == 2 throughout.
    #
    # So the assertion is "at most one frame licences a single bar, and never the frame
    # where the DSP also reports one grain" -- an allowance with a measured reason, not
    # a loosened threshold. Anything more frequent is the old edge bug.
    # The allowance is "a single-bar frame only right after a wrap", which is checkable:
    # the frame before a wrap shows the bars at the RIGHT edge, and the frame after shows
    # them at the LEFT. A frame in the middle of the pass with one bar would still be the
    # old edge bug, and that is what this rejects.
    singles = [i for i, n in enumerate(edge) if n != 2]

    def xs_of(i):
        return [x0 for x0, _, _ in bars(os.path.join(out, f"e{i}.png"), INK_HI)]

    # A frame at the very start or end of the pass is a boundary case: frame 23's "next"
    # wraps to frame 0, which is 400 ms of settling rather than one 90 ms step, so it is
    # not a comparable neighbour. Judge only the interior.
    bad = []
    for i in singles:
        if i == 0 or i == 23:
            continue
        prev, nxt = xs_of(i - 1), xs_of(i + 1)
        wrapped = bool(prev) and bool(nxt) and min(prev) > 100 and max(nxt) < 90
        if not wrapped:
            bad.append((i, prev, nxt))
    ck(not bad, "a single-bar frame only ever happens across a loop wrap",
       f"frames with a bar missing mid-pass: {bad}")

    # ── the lane must not follow the rotating DSP slot ─────────────────────────
    # The bug the player reported as "the bars jump up and down, is that SPRAY?": the
    # lane was `lane[slot index]`, and slots rotate on every respawn, so the same grain
    # changed lane on a timer. Measured then, the upper bar's y went
    # 125,125,125,121,121,121,125,125. Lane now follows drawing order by position, so
    # with two grains it can only change when they cross.
    lanes = []
    for i in range(12):
        b = bars(os.path.join(out, f"a{i}.png"), INK_HI)
        lanes.append(sorted(y0 for _, _, y0 in b))
    print(f"lane heights over 12 frames: {lanes}")
    ck(all(len(l) == 2 for l in lanes), "both bars present in all 12 frames", f"{lanes}")
    ck(len(set(tuple(l) for l in lanes)) == 1,
       "the lanes are stable across respawns (not tied to the rotating slot index)",
       f"lane heights varied: {sorted(set(tuple(l) for l in lanes))}")

    # ── SPRAY, from the two bars' separation ─────────────────────────────────
    def gap(nm):
        xs = sorted(x0 for x0, _, _ in bars(os.path.join(out, nm), INK_HI))
        return (xs[-1] - xs[0]) if len(xs) >= 2 else 0

    def px_has(nm, colour):
        im = Image.open(nm).convert("RGB")
        px = im.load()
        return any(px[x, y] == colour
                   for x in range(BAND_X0, BAND_X1) for y in range(92, 168))

    def loop_x(nm):
        """The two loop marks.

        They are dotted columns: a pixel every third row, the full height of the band.
        That shape is the discriminator, and it has to be, because the corner labels are
        drawn in the same dim colour -- measured, "SPRAY" put 9 matching pixels in a
        column at x 24 and 30 and was read as a loop mark. A loop mark is periodic over
        the whole band, which no glyph is."""
        im = Image.open(os.path.join(out, nm)).convert("RGB")
        px = im.load()
        found = []
        for x in range(BAND_X0, BAND_X1):
            ys = [y for y in range(90, 170) if px[x, y] == LOOP_MARK]
            if len(ys) < 20:
                continue
            gaps = [ys[i + 1] - ys[i] for i in range(len(ys) - 1)]
            if gaps and all(g == 3 for g in gaps):
                found.append(x)
        return found

    # SPRAY is read from the separation between the two bars and from nothing else. A
    # dim playhead tick used to give it a fixed reference, and was removed on the
    # player's judgement: a mark is a claim about reading, so when nothing is read
    # neither the grains nor the playhead should be drawn. What is left is the one
    # honest reading -- SPRAY 0 puts both grains on the same spot, so the separation is
    # 0 and grows from there.
    g0, g64, g127 = gap("a0.png"), gap("sp64.png"), gap("sp127.png")
    print(f"bar separation: {g0} / {g64} / {g127} px")
    ck(g0 <= 2, "spray 0: the two bars overlap, so the separation is 0", f"{g0} px")
    ck(g64 > g0, "spray 64 separates them", f"{g64} px")
    ck(g127 > g64, "spray 127 separates them further", f"{g127} px")
    lx = loop_x("a0.png")
    print(f"loop marks at {lx}")
    ck(len(lx) == 2 and lx[0] > BAND_X0 and lx[1] < BAND_X1,
       "the loop marks are inside the band, not on its border", f"{lx}")

    # ── nothing is drawn that claims a read is happening ──────────────────────
    # The material stays, because it is a fact about the sample; a bar does not,
    # because it says "a grain is reading here". And a second note sounds like the
    # first, not like a resume: the attack must belong to the note.
    quiet = base + (
        "shot q0.png\n"
        "key 5 down\n"
        "wait 200\n"
        "shot q1.png\n"
        "key 5 up\n"
        "wait 400\n"
        "shot q2.png\n"
        "wait 500\n"
        "shot q3.png\n"
        "key 5 down\n"
        "wait 200\n"
        "shot q4.png\n"
    )
    run(quiet, out, [f"q{i}.png" for i in range(5)])
    qb = [len(bars(os.path.join(out, f"q{i}.png"), INK_HI)) for i in range(5)]
    print(f"bars across a note: idle {qb[0]}, playing {qb[1]}, stopped {qb[2]}, "
          f"+500 ms {qb[3]}, again {qb[4]}")
    ck(qb[0] == 0, "no bars before the first note", f"{qb[0]}")
    ck(qb[1] == 2, "two bars while the note sounds", f"{qb[1]}")
    ck(qb[2] == 0, "no bars after the note stops", f"{qb[2]}")
    ck(qb[3] == 0, "still none half a second later", f"{qb[3]}")
    ck(qb[4] == 2, "a second note shows the same two bars, not a resumed position",
       f"{qb[4]}")

    # the material is a map at all times
    ck(any(px_has(os.path.join(out, f"q{i}.png"), DIM_INK) for i in range(5)),
       "the dim envelope is drawn whether or not a note sounds", "")

    # The loop is the whole slot, and it must stay the whole slot when a note starts.
    # wv_note_on_grain sets the loop start from its `start` argument, so a note started
    # anywhere but 0 silently shrinks the loop to the tail of the material -- the cloud
    # then reads only that part and wraps there, which sounds like a shorter sample
    # rather than a fault. Injected a mid-slot start and measured the marks move to
    # x 92..178, so this check is the one that catches it.
    lp = loop_x("q1.png")
    print(f"loop marks while a note sounds: {lp}")
    # Pinned to the material's own ends, not "the same as before the note": an injected
    # mid-slot start moved BOTH readings to 92..178, so comparing them to each other
    # passed while the loop was plainly wrong. The loop is the whole slot, which is the
    # band's edges nudged in one pixel.
    ck(len(lp) == 2 and lp[0] <= BAND_X0 + 3 and lp[1] >= BAND_X1 - 3,
       "the loop stays the whole slot while a note sounds", f"{lp}")

    print(f"checks {checks}, failed {fails}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
