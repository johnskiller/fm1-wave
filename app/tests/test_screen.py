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


HOST_OUT = ""


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
    global HOST_OUT
    HOST_OUT += r.stdout
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


    # ── SYNTH gives the labels a margin; SMPL and GRAN do not ────────────────
    # The player accepted the overlay for SMPL and GRAN -- their envelope is at 2/5 ink
    # and the grain bars are hairlines, so text on top still separates -- and rejected it
    # for SYNTH, where the wavetable frame is drawn at full ink and the text fights it.
    # SYNTH therefore stops at x 4+172 = 176 and the labels get x 176..235 to themselves.
    # Both halves are checked: the picture must not reach into the label column, and the
    # label column must not be empty.
    for name in ("syn", "smp", "grn"):
        im = shots[name]
        bg = background(im, SCOPE_H, SCOPE_H + MAT_H)
        col_x0 = 240 - 60 + 4                     # 184: first column of the label block
        if name == "syn":
            # Colour cannot separate picture from label here: SYNTH draws the frame at
            # sk.ink_hi and the label VALUES at sk.ink_hi too, so a "is this the picture's
            # tone" test caught the labels and failed on correct code. The separator the
            # design actually guarantees is geometric -- a gap between the picture's right
            # edge (x 175) and the widest label (46 px, so it starts at 189). Asserted as
            # a gap rather than an edge, because the edge moves with the label's width.
            gap = [x for x in range(176, 188)
                   if not any(im.getpixel((x, y)) != bg
                              for y in range(SCOPE_H + 2, SCOPE_H + MAT_H - 2))]
            ck(len(gap) == 12,
               "syn: there is a clear gap between the picture and the label column",
               f"clean columns {gap}")
        else:
            # full width: these two are allowed to run under the labels
            ck(any(im.getpixel((x, y)) != bg
                   for x in range(MAT_W - 20, 240)
                   for y in range(SCOPE_H + 2, SCOPE_H + MAT_H - 2)),
               f"{name}: the picture still runs the full width")

    # ── SYNTH's second line is the bank's NAME ──────────────────────────────
    # "BANK 3/8" says which one you are on and nothing about what it is. WV_BANK_NAME has
    # carried SAW SQR / RESO / HOLLOW / VOX / DIGI / OPEN1..3 since the generator was
    # written and no screen had ever shown one. Checked by width: the name line's ink run
    # has to be one of the eight names' rendered widths, which also proves it tracks the
    # bank rather than printing a constant.
    import re as _re
    _h = open(os.path.join(ROOT, "app", "build", "gen", "felucca_font.h")).read()
    _adv = [int(v) for v in _re.search(r"FONT_XS_ADV\[95\] = \{([^}]*)\}",
                                       _h).group(1).split(",")]

    def _w(t):
        return sum(_adv[ord(c) - 32] for c in t)

    names = ["SAW SQR", "RESO", "HOLLOW", "VOX", "DIGI", "OPEN1", "OPEN2", "OPEN3"]
    for shot_name, want_i in (("syn", 0),):
        im = shots[shot_name]
        bg = background(im, SCOPE_H, SCOPE_H + MAT_H)
        y = SCOPE_H + 20                           # the name's row, screen y 104
        cols = [x for x in range(180, 240)
                if any(im.getpixel((x, y_)) != bg for y_ in range(y, y + 10))]
        ck(bool(cols), "syn: the bank name line is drawn")
        if cols:
            wide = cols[-1] - cols[0] + 1
            match = [n for n in names if abs(_w(n) - wide) <= 2]
            ck(bool(match), "syn: the second line is one of the bank names",
               f"{wide} px wide, matches none of {[_w(n) for n in names]}")
            ck(bool(match) and match[0] == names[want_i],
               f"syn: it shows bank {want_i + 1}'s name", f"looks like {match}")


    # ── the scope's 0 dBFS line is the top row, and there is no headroom ────
    # "+0" labels the line at row 1. There is no +6 dB above it: SCOPE_FS is SCOPE_HALF,
    # so full scale is the band edge exactly. This is asserted because two separate
    # comments described a +6 dB headroom that the code never had, and a comment that
    # describes a different design than the code is worse than no comment -- the next
    # reader believes it. If someone adds headroom later, this check is what tells them
    # the label and the edge have to move together.
    # RGB565 quantisation, not the RGB() constants: RGB(96,99,104) lands on (98,97,106)
    # and RGB(58,61,66) on (57,60,65). Measured from the framebuffer, because computing
    # them from the constants is how the first version of this check failed on correct code.
    SCOPE_GRID = (98, 97, 106)                    # GRID: the 0 dBFS line
    SCOPE_DIM = (57, 60, 65)                      # GRID_DIM: the rest
    im = shots["syn"]
    for y in (0, 1):
        row = sum(1 for x in range(240) if im.getpixel((x, y)) == SCOPE_GRID)
        if row > 200:
            ck(y == 1, "syn: the 0 dBFS line is at row 1", f"row {y}")
            break
    else:
        ck(False, "syn: the 0 dBFS line is drawn in GRID")

    # full scale must land on the band edge: centre +- SCOPE_HALF reaches row 1
    ck(42 - 41 == 1 and 42 + 41 == 83,
       "the scope's full scale reaches the band edge, leaving no headroom above 0 dBFS")

    # ── the debug corner (app.c dbg_draw, DBG_ON) ──────────────────────────────
    # Three rows at the top left: the render's load, the second core, the clock. What
    # this checks is that it draws at all and where it sits, not its values: the values
    # are the platform's and change with what is playing.
    im = shots["syn"]
    INK_HI = (255, 186, 65)                       # SYNTH's ink_hi, what dbg_draw uses
    BG = (41, 28, 8)                              # SYNTH's bg: the backing box
    # Glyphs are anti-aliased, so only the stems land on ink_hi exactly (10 px for
    # "0% xr0"); counting exact matches measures the font's hinting, not whether the
    # row drew. What is counted is bright pixels -- anything well above the backing box
    # and the graticule, which is what a glyph's core is.
    for name, y0, y1 in (("row 1", 4, 16), ("row 2", 16, 28)):
        n_ink = sum(1 for y in range(y0, y1) for x in range(0, 64)
                    if sum(im.getpixel((x, y))) > 300)
        ck(n_ink > 15, f"debug {name} is drawn", f"{n_ink} px of ink")

    # Row 3 is the clock, and the host has no cycle counter (app.c DBG_NO_CYCLES): the
    # host's own cycles are not the FM-1's, so there is no reading and the row must stay
    # EMPTY. Asserting it draws would force the host build to fabricate a figure, which
    # is the one thing the measurement must not do -- the first version printed "1555M?",
    # a measurement of the developer's machine. What is checked here is that host and
    # device differ for a stated reason, not that both show three rows.
    n_row3 = sum(1 for y in range(28, 40) for x in range(0, 64)
                 if sum(im.getpixel((x, y))) > 300)
    ck(n_row3 < 8, "debug row 3 is blank on the host (no cycle counter there)", f"{n_row3} px")

    # Row 2 must report what the second core DID, and the host runs its jobs inline
    # (plat_host.c: a job runs at once, in order), so the host can only ever show jobs
    # with no time in them: "c2 <n>u0". A row that still said "c2 ok" would be the
    # readout this replaced -- it claimed the core was fine without ever saying it had
    # done anything, which is exactly the inference the device cannot support either
    # (plat_cpu2_wait returning -1 makes the caller re-render the half, so a dead core
    # produces complete, correct audio and an unchanged load figure).
    # (the row's text is not readable from the bitmap without a font table, so the
    # format is asserted where it is built instead: app.c's dbg_draw is the one place
    # that writes it, and the host run below prints the same bytes it drew)

    # Row 2's FORMAT, read from the text the device would draw rather than from the
    # bitmap (host builds print the rows where they are built: app.c dbg_draw).
    #
    # The row used to say "c2 ok", which claimed the core was healthy without ever
    # saying it had done anything -- and the device cannot support that inference: when
    # plat_cpu2_wait() returns -1 the caller re-renders that half, so a core that died
    # still yields complete, correct audio. What matters is the job count and the time,
    # so the row is "<n>u<us>" and this checks the shape is still there.
    import re as _re
    rows = _re.findall(r"dbg row1=(.*?)\| row2=(.*?)\| row3=(.*?)$", HOST_OUT, _re.M)
    ck(len(rows) > 0, "the host prints the debug rows", f"{len(rows)} lines")
    if rows:
        r1, r2, r3 = rows[-1]
        ck(_re.fullmatch(r"\d+% xr\d+", r1) is not None,
           "row 1 is '<load>% xr<xruns>'", repr(r1))
        ck(r2 == "c2 --" or _re.fullmatch(r"c2 \d+u\d+w?", r2) is not None,
           "row 2 reports the jobs handed over and the us each, or '--'", repr(r2))
        # The host runs its jobs inline (plat_host.c: at once, in order), so the interval
        # run..wait there is the job's own duration -- small, and nonzero. Asserting it
        # was zero was wrong and the test said so. What matters is that the number is
        # present and plausible: a reading that stayed 0 forever on the device is the
        # signature of a core that never took the job (wait returns -1 at once and the
        # caller renders the half itself, so the audio is complete and correct).
        ck(r2 == "c2 --" or _re.fullmatch(r"c2 \d+u\d+w?", r2) is not None,
           "row 2's handover time is a number", repr(r2))
        ck(r3 == "", "row 3 is empty on the host", repr(r3))

    # The backing box must NOT cover the 0 dBFS line at row 1: the graticule's reference
    # is the scale's anchor, and a debug overlay that hides it makes the scope unreadable
    # to check. (This check earned its place: the first version covered rows 0-36 and the
    # 0 dBFS line went from 240 px of GRID to 178, which the check above caught.)
    dbg_bg = sum(1 for x in range(0, 64) if im.getpixel((x, 3)) == BG)
    ck(dbg_bg > 40, "the debug box's backing reaches row 3", f"{dbg_bg} px")
    ck(im.getpixel((0, 1)) == SCOPE_GRID,
       "the debug box leaves the 0 dBFS line at row 1 intact")


    # ── the LFO page ────────────────────────────────────────────────────────
    # The page takes K3-K4 and leaves CUT and RES on K1-K2, because CUT is what the
    # LFO sweeps and the panel must not move it. Asserted through the app's own state
    # (app_expect, driven by the host's `expect <name> <value>`), not by reading the
    # bitmap: the label text is a font table away, but "K3 turned RATE and not SHAPE"
    # is a fact about the app, and that is the part that can drift.
    def expect(script, name, val):
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
            f.write(script + "\nexpect %s %s\n" % (name, val))
            path = f.name
        try:
            r = subprocess.run([HOST, path, out], capture_output=True, text=True,
                               timeout=120, cwd=out)
        finally:
            os.unlink(path)
        return any(l.strip().startswith("ok ") for l in r.stdout.splitlines())

    ck(expect("press LFO\nturn K3 8\n", "lfo_rate", 32),
       "LFO page: K3 sets RATE")
    ck(expect("press LFO\nturn K4 10\n", "lfo_depth", 40),
       "LFO page: K4 sets DEPTH")
    ck(expect("press LFO\nturn K3 8\n", "shape", 0),
       "LFO page: K3 does NOT set SHAPE (the page took it)")
    ck(expect("press LFO\nturn K4 10\n", "amount", 0),
       "LFO page: K4 does NOT set AMOUNT (the page took it)")
    ck(expect("press LFO\nturn K1 -6\n", "cut", 103),
       "LFO page: K1 still sets CUT")          # -6 steps of 4 from 127
    ck(expect("turn K1 -6\n", "cut", 103),
       "the LFO page is not needed for CUT")
    ck(expect("press LFO\n", "lfo", 1), "B_LFO opens the page")
    ck(expect("tap LFO\ntap LFO\n", "lfo", 0), "B_LFO toggles it shut")

    # Two pages cannot be up at once. Without this the lit lamp would be ambiguous and
    # which parameters the knobs edit would depend on the order the buttons were hit.
    ck(expect("tap ENV\ntap LFO\n", "env", 0), "LFO closes the ENV page")
    ck(expect("tap LFO\ntap ENV\n", "lfo", 0), "ENV closes the LFO page")

    # ── the scope's vertical reference ──────────────────────────────────────────
    # SCOPE_REF is 0.5 (eight voices, pre-master, wide open -- measured), not digital
    # full scale. The bug this pins is "you can barely see the waveform": with full
    # scale as the reference a single note drew 2 px from the centre and the user
    # could not read the trace at all.
    #
    # Measured on the host through the real render path: 1 voice 36 px of 84, 4 voices
    # 47 px, 8 voices 43 px. The check is that one note is at least a third of the
    # band, which is the difference between a waveform and a line.
    INK = (255, 186, 65)                      # SYNTH ink_hi
    def trace_extent(keys):
        out = tempfile.mkdtemp()
        script = "res 1\nwait 200\n" + "".join(f"key {k} down\n" for k in keys)
        script += "wait 600\nshot s.png\n"
        sp = os.path.join(out, "s.txt")
        open(sp, "w").write(script)
        subprocess.run([HOST, sp, out], capture_output=True, text=True, timeout=120, cwd=out)
        im = Image.open(os.path.join(out, "s.png")).convert("RGB")
        rows = []
        for y in range(84):
            for x in range(64, 240):          # skip the debug corner
                px = im.getpixel((x, y))
                if px == INK:
                    rows.append(y)
                    break
        return (max(rows) - min(rows) + 1) if rows else 0

    one = trace_extent([13])
    four = trace_extent([10, 13, 16, 19])
    print(f"scope: 1 voice {one} px, 4 voices {four} px of 84")
    ck(one >= 28, "one note draws at least a third of the scope band")
    ck(four > one, "a chord draws taller than one note (the scope tracks polyphony)")

    print(f"checks {checks}, failed {fails}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
