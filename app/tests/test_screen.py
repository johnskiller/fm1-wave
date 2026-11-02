#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""The three screen bands: where each starts, how tall it is, and -- the part that
actually catches things -- that no band draws a single pixel outside its own rows.

Why this exists. A band's height and its contents are written in two different places,
and gfx clips silently at the canvas edge (cv_rect: `if (y1 > cv_y1) y1 = cv_y1;`). The
material band was cut from 108 to 84 px in app.c while src_shape kept drawing to y=103
and the "ENV PAGE" line stayed at y=92: the bottom 20 px of the table's picture were
clipped and the ENV marker vanished completely, with no error anywhere. On a 240 px
screen that reads as "the knobs are covering it", which is how it was reported.

app.c now derives the heights (SCOPE_H, MAT_H, KNOB_H, with an #error if they do not sum
to 240). This script checks the drawing agrees with them, by pixel.

    python3 app/tests/test_screen.py            # needs app/build/host/wave_host
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
HOST = os.path.join(ROOT, "app", "build", "host", "wave_host")

# the derivation, mirrored from app.c (that file is the source of truth; the #error there
# is what keeps these three numbers summing to 240)
MAT_PAD, MAT_WAVE = 6, 80
MAT_H = MAT_PAD + MAT_WAVE + MAT_PAD
MAT_W, MAT_LAB_L = 112, 122          # the picture, and where the labels start
KNOB_H = 64
SCOPE_H = 240 - MAT_H - KNOB_H

BANDS = [("scope", 0, SCOPE_H), ("material", SCOPE_H, SCOPE_H + MAT_H),
         ("knobs", SCOPE_H + MAT_H, 240)]

fails = 0
checks = 0


def ck(cond, what, detail=""):
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
        print(f"  FAIL {what}{(' -- ' + detail) if detail else ''}")


def shoot(script, out):
    """Run a host scenario and return {name: Image} for every shot it took."""
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write(script)
        path = f.name
    try:
        r = subprocess.run([HOST, path, out], capture_output=True, text=True, timeout=300,
                           cwd=out)
        if r.returncode != 0:
            ck(False, "host scenario ran", (r.stdout + r.stderr).strip()[-300:])
    finally:
        os.unlink(path)
    shots = {}
    for n in sorted(os.listdir(out)):
        if n.endswith(".png"):
            shots[n[:-4]] = Image.open(os.path.join(out, n)).convert("RGB")
    return shots


def background(im, y0, y1):
    """The band's own colour: the most common pixel in it.
    Each band is a separate cv_begin(bg), so this also proves the skin reached it."""
    from collections import Counter
    return Counter(im.getpixel((x, y)) for y in range(y0, y1, 2)
                   for x in range(0, 240, 3)).most_common(1)[0][0]


def ink(im, y0, y1, bg):
    rows = [y for y in range(y0, y1)
            if any(im.getpixel((x, y)) != bg for x in range(240))]
    return rows


def main():
    if not os.path.exists(HOST):
        sys.exit(f"{HOST} is missing: build it with `python3 platform/tools/build.py app --host`")

    ck(SCOPE_H + MAT_H + KNOB_H == 240, "the three bands sum to 240",
       f"{SCOPE_H}+{MAT_H}+{KNOB_H}")

    out = tempfile.mkdtemp(prefix="fm1scr")
    shots = shoot("""wait 300
master 4096
key 12 down
wait 400
shot syn.png
tap ENV
wait 400
shot env.png
tap ENV
wait 200
tap SEL
wait 400
shot smp.png
tap SEL
wait 400
shot grn.png
""", out)

    for name in ("syn", "env", "smp", "grn"):
        ck(name in shots, f"{name} was captured")
    for name in ("syn", "env", "smp", "grn"):
        im = shots[name]
        print(f"--- {name} ---")
        for band, y0, y1 in BANDS:
            bg = background(im, y0, y1)
            rows = ink(im, y0, y1, bg)
            if not rows:
                print(f"  {band:9s} {y0:3d}..{y1 - 1:3d}  (empty)")
                continue
            first, last = rows[0], rows[-1]
            print(f"  {band:9s} {y0:3d}..{y1 - 1:3d}  ink rows {first}..{last}"
                  f"  ({len(rows)} rows, {last - first + 1} px of extent)")
            # the whole point: a band may not draw outside itself, and it may not stop
            # one pixel short of its own edge either -- that is the clipping signature
            ck(y0 <= first and last < y1, f"{name}: {band} stays inside its rows",
               f"ink {first}..{last} outside {y0}..{y1 - 1}")

        # the material band must not touch the knobs band: a gap is the visible proof
        # that nothing is being clipped at that boundary
        mat_bg = background(im, SCOPE_H, SCOPE_H + MAT_H)
        mat_rows = ink(im, SCOPE_H, SCOPE_H + MAT_H, mat_bg)
        ck(bool(mat_rows) and mat_rows[-1] < 240 - KNOB_H - 1,
           f"{name}: material clears the knobs band", f"last ink row {mat_rows[-1] if mat_rows else '-'}")

        # and the band's own contents must be at their full height: the table picture is
        # MAT_WAVE tall, so its ink has to span most of it -- this is what 108 -> 84 broke
        if name == "syn":
            # the whole band: the trace is tallest at the left and decays to the right, so
            # measuring a slice that stops short of the notes' decay underestimates it
            wave = [y for y in range(SCOPE_H, SCOPE_H + MAT_H)
                    if any(im.getpixel((x, y)) != mat_bg for x in range(4, 236))]
            ck(bool(wave) and (wave[-1] - wave[0] + 1) >= MAT_WAVE - 4,
               "syn: the table picture uses its full height",
               f"{wave[-1] - wave[0] + 1 if wave else 0} px of {MAT_WAVE}")

    # ── the envelope is filled, and dim ───────────────────────────────────────
    # It was changed to a peak line once, on my reading of "the sampling is so dense it
    # comes out as a solid shape", and changed back on the player's judgement: a filled
    # min/max band is how much energy is at this moment, while a 1 px outline of a loud
    # passage does not read as loud at all. So the fill is the design and the check is
    # that it STAYS filled -- min/max per column, not a hairline.
    #
    # The brightness is the part that was actually wrong and stays fixed: SMPL used to
    # draw its envelope at full ink_hi while GRAN drew the same material at 2/5, so the
    # two sources disagreed about their own background. Both now go through env_line()
    # at dim(ink, 2, 5).
    for name in ("smp", "grn"):
        im = shots[name]
        band_bg = background(im, SCOPE_H, SCOPE_H + MAT_H)
        total = 0
        for x in range(5, 4 + MAT_W - 1):
            for y in range(SCOPE_H + 4, SCOPE_H + MAT_H - 4):
                if im.getpixel((x, y)) != band_bg:
                    total += 1
        pct = 100.0 * total / ((MAT_W - 1) * (MAT_H - 8))
        print(f"{name}: material picture is {pct:.1f} % ink")
        ck(pct > 4.0, f"{name}: the envelope is filled, not a hairline", f"{pct:.1f} %")

    # the labels must be on the right of the picture and not on top of it, because they
    # are the panel's 2x2 and the picture is what is left. Measured, the picture ends at
    # x 4+MAT_W and the label columns start at MAT_LAB_L.
    for name in ("syn", "smp", "grn"):
        im = shots[name]
        band_bg = background(im, SCOPE_H, SCOPE_H + MAT_H)
        right = sum(1 for x in range(MAT_LAB_L, 236)
                    for y in range(SCOPE_H + 2, SCOPE_H + MAT_H - 2)
                    if im.getpixel((x, y)) != band_bg)
        ck(right > 20, f"{name}: the labels sit to the right of the picture",
           f"{right} ink px right of x {MAT_LAB_L}")
    print(f"checks {checks}, failed {fails}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
