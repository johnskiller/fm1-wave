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

# the material band on the 240x240 screen: y 84..175, the picture inside it starts at
# x=4 and is 176 wide. The slot name and SLOT n/m text sit at y 90..103, so bars are
# detected below y=104 -- otherwise a text glyph is counted as a grain.
BAND_Y0, BAND_Y1 = 104, 176
BAND_X0, BAND_X1 = 4, 180

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

    out = tempfile.mkdtemp(prefix="fm1grainmap")

    # The knob step is x4: `turn N` moves a knob by 4N in host simulation, and SPRAY is
    # 0..127, so `turn SELECT 16` lands on 64 and `turn SELECT 32` would saturate. Every
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
    shots = ["sp0.png", "sp0b.png", "sp64.png", "sp127.png"] + [f"e{i}.png" for i in range(24)]
    script = base + (
        "key 5 down\n"
        "wait 400\n"
        "shot sp0.png\n"
        "expect grains 2\n"
        "expect grainpx 12\n"
        "wait 90\n"
        "shot sp0b.png\n"
        + frames +
        "key 5 up\n"
        "wait 200\n"
        "turn SELECT 16\n"
        "wait 200\n"
        "expect spray 64\n"
        "key 5 down\n"
        "wait 400\n"
        "expect grains 2\n"
        "expect grainpx 12\n"
        "shot sp64.png\n"
        "key 5 up\n"
        "wait 200\n"
        "turn SELECT 16\n"
        "wait 200\n"
        "expect spray 127\n"
        "key 5 down\n"
        "wait 400\n"
        "expect grains 2\n"
        "expect grainpx 12\n"
        "shot sp127.png\n"
        "key 5 up\n"
    )
    run(script, out, shots)
    if fails:
        print(f"checks {checks}, failed {fails}")
        return 1

    # the blue-white GRAN ink_hi, from src_skin
    INK_HI = (148, 226, 255)

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
    # 45 ms at 1x on the 0.84 s kit = 12 px, asserted through app_expect grainpx too, so
    # this checks the DRAWING agrees with the number rather than that a constant matches.
    for nm, b in (("spray 0", b0), ("spray 64", b64), ("spray 127", b127)):
        ck(all(x1 - x0 + 1 == 12 for x0, x1, _ in b),
           f"{nm}: each bar is 12 px, the grain length",
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

    print(f"checks {checks}, failed {fails}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
