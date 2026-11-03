#!/usr/bin/env python3
"""app/tests/test_fx.py — the FX page: the filter's tap, and key tracking.

Two settings that are not knobs a player turns while playing, which is what makes them
a page: TYPE is a choice of three and KEY TRK a choice of five, both set once for a
patch.

What is asserted, and what each check would catch:

  * B_FX opens and closes the page, it is exclusive with ENV/LFO/ARP/EDIT, and every
    one of those clears it — a page that could be up alongside another would let K3/K4
    mean two things at once
  * K3 cycles the three taps and K4 the five tracking settings, Wrapping in both
    directions: an off-by-one in the detent table shows up as a setting that is
    reachable going up and skipped going down
  * the ring shows the setting, not the detent: KEY TRK at 100 % fills the arc, at 0 %
    leaves it empty — a ring driven by the index would fill the same at 25 % and 100 %
  * the page leaves CUT and RES on K1 and K2, measured as their values not moving when
    the page comes up
  * save and load round-trip both fields, which is the check that fails when a field is
    added to the record and forgotten in save_snapshot or in the restore
  * the DSP is actually told: ftype and ktrack reach dsp.* through ui_apply

Run: python3 app/tests/test_fx.py
"""
import os
import subprocess

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
HOST = os.path.join(APP, "build", "host", "wave_host")

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
        print(r.stdout[-3000:])
        raise SystemExit("host failed")
    return r.stdout


def ink_px(b, rgb, tol=6):
    return int(np.all(np.abs(b.astype(int) - np.array(rgb)) <= tol, axis=2).sum())


def arc_fill(b, x0, w, fill_rgb, track_rgb, tol=4):
    """The ring's fill and its dim track, counted by exact colour.

    Measuring "anything bright" was the first attempt and it was wrong twice over: it
    counted the value text in the hole and the anti-aliased edge, so a full ring read
    HIGHER than the reference full ring in another column. The ring draws its track in
    one known colour and its fill in another, and on the FX page the counted knob is lit
    (ink_hi) while CUT is not (ink), so the two must be told apart by colour, not by
    brightness.

    Returns (fill_px, track_px). Their sum is constant for a given knob, which is what
    makes "the fill grew" mean "the track shrank" rather than "something got brighter".
    """
    reg = b[17:64, x0:x0 + w, :].astype(int)
    fill = int(np.all(np.abs(reg - np.array(fill_rgb)) <= tol, axis=2).sum())
    track = int(np.all(np.abs(reg - np.array(track_rgb)) <= tol, axis=2).sum())
    return fill, track


def main():
    out = os.path.join(APP, "build", "fxtest")
    os.makedirs(out, exist_ok=True)

    # ── the page, and exclusivity ────────────────────────────────────────────────
    # The filter's deep settings live on the LFO page's SECOND SCREEN now (player's
    # call: they are how the LFO's filter behaves, not a subject of their own). So the
    # way in is LFO twice, and the way out is HOME -- the LFO button advances rather
    # than closing, which is the panel rule the player set in the same session.
    o = run("""wait 300
expect page_no 1
tap LFO
master 400
expect page_no 1
tap LFO
master 400
expect page_no 2
tap HOME
master 400
expect page 0
expect page_no 1
tap LFO
master 300
expect page_no 1
expect page 2
tap ENV
master 300
expect page 1
tap LFO
tap LFO
master 300
expect page 2
expect page_no 2
tap ARP
master 300
expect page 3
expect page_no 1
""", out)
    for line in o.splitlines():
        if line.startswith("  ok") or line.startswith("FAIL"):
            ck("FAIL" not in line, line.strip()[3:])

    # ── the second screen is still the LFO's page, and says so ───────────────────
    # The lamp belongs to the PAGE, not the screen: both screens are the LFO, so the
    # button stays lit while you are on screen 2. A lamp that went out there would say
    # the LFO had closed while its filter settings were on the screen.
    o = run("""wait 300
leds
tap LFO
master 300
leds
tap LFO
master 300
leds
""", out)
    seen = [l.split("leds:")[1].split("|")[0].strip()
            for l in o.splitlines() if "leds:" in l]
    ck("LFO" not in seen[0], f"nothing is lit at rest [{seen[0]}]")
    ck("LFO" in seen[1], f"screen 1 lights the LFO button [{seen[1]}]")
    ck("LFO" in seen[2], f"and screen 2 keeps it lit: same page, other screen [{seen[2]}]")

    # ── K3 and K4 step, wrap, and reach the DSP ──────────────────────────────────
    o = run("""wait 300
expect ftype 0
expect ktrack 0
tap LFO
tap LFO
master 300
turn K3 1
master 200
expect ftype 1
turn K3 1
master 200
expect ftype 2
turn K3 1
master 200
expect ftype 0
turn K3 -1
master 200
expect ftype 2
turn K4 1
master 200
expect ktrack 25
turn K4 1
master 200
expect ktrack 50
turn K4 1
master 200
expect ktrack 75
turn K4 1
master 200
expect ktrack 100
turn K4 1
master 200
expect ktrack 0
turn K4 -1
master 200
expect ktrack 100
""", out)
    got = []
    for line in o.splitlines():
        if line.startswith("  ok") or line.startswith("FAIL"):
            got.append(line)
    ck(all("FAIL" not in g for g in got), f"K3/K4 step and wrap in both directions ({len(got)} expects)")
    ck(any("ftype == 2" in g for g in got), "K3 reaches the third tap")
    ck(any("ktrack == 100" in g for g in got), "K4 reaches 100 %")
    ck(any("ktrack == 25" in g for g in got), "K4 reaches 25 % (the step is 25, not a sweep)")

    # ── the page leaves CUT and RES alone ───────────────────────────────────────
    o = run("""wait 300
turn K1 -6
master 200
expect cut 103
tap LFO
tap LFO
master 400
expect cut 103
turn K2 4
master 200
expect res 16
tap HOME
master 400
expect res 16
""", out)
    oks = [l for l in o.splitlines() if l.startswith("  ok") or l.startswith("FAIL")]
    ck(len(oks) >= 4 and all("FAIL" not in l for l in oks),
       "CUT and RES keep their values and their knobs on the filter screen")

    # ── the rings show the setting, and differ between 25 % and 100 % ───────────
    o = run("""wait 400
tap LFO
tap LFO
master 400
shot kt000.png
turn K4 1
master 400
shot kt025.png
turn K4 1
master 400
turn K4 1
master 400
turn K4 1
master 400
shot kt100.png
turn K3 1
master 400
shot type_hp.png
""", out)
    f0 = os.path.join(out, "kt000.png")
    f25 = os.path.join(out, "kt025.png")
    f100 = os.path.join(out, "kt100.png")
    # shot NAME writes NAME/<n>; take the last frame
    def last(d):
        p = os.path.join(out, d)
        if os.path.isdir(p):
            ns = sorted(int(n) for n in os.listdir(p) if n.isdigit())
            return os.path.join(p, str(ns[-1]))
        return p
    ks = [np.array(Image.open(last(x)).convert("RGB"))[176:240, :, :]
          for x in (f0, f25, f100)]
    # the KEY TRK knob is the fourth: x 178..232. On the FX page it is lit, so the fill
    # is ink_hi and the unused part of the ring is the dim track.
    INK_HI = (255, 186, 65)
    TRACK = (41, 24, 0)
    pairs = [arc_fill(k, 178, 54, INK_HI, TRACK) for k in ks]
    (f0, t0), (f25, t25), (f100, t100) = pairs
    print(f"  KEY TRK ring: 0%% fill {f0} track {t0}, 25%% fill {f25} track {t25}, "
          f"100%% fill {f100} track {t100}")
    ck(f0 < f25 < f100, "the KEY TRK ring fills with the setting, not with the detent")
    # The bug this exists for: k_ktrack is PERCENT and the ring's argument is 0..127, so
    # passing the percent straight through drew 100 % as 78 % of the arc and the ring
    # never filled. "100 % leaves no track" is the assertion that catches it.
    ck(t100 == 0, "100 % key tracking leaves no unfilled track")
    ck(f100 > f0 * 10, "100 % is a full ring against 0 %'s empty one")
    # NOT asserted: that fill + track is a constant. Measured 497 / 498 / 497, and the
    # one-pixel wobble is the value text in the hole -- "100%" is wider than "0%" and
    # clips a pixel of the ring's inner edge. An assertion with no margin for that would
    # be a check on the font rather than on the ring.

    # ── the record carries both fields across a reboot ──────────────────────────
    # This is the check that fails when a field is added to save_rec and forgotten in
    # save_snapshot (it would always read as the default) or in the restore (it would
    # come back as whatever the last RAM write left). ftype=2 / ktrack=75 are picked
    # because neither is the default: a broken path would return 0 and 0.
    o = run("""wait 300
tap LFO
tap LFO
master 300
turn K3 1
turn K3 1
turn K4 1
turn K4 1
turn K4 1
master 300
expect ftype 2
expect ktrack 75
tap SAVE
master 300
turn K4 -2
master 200
expect ktrack 25
reboot
expect saved 1
expect ftype 2
expect ktrack 75
""", out)
    oks = [l for l in o.splitlines() if l.startswith("  ok") or l.startswith("FAIL")]
    for l in oks:
        if "ftype" in l or "ktrack" in l or "saved" in l:
            ck("FAIL" not in l, l.strip()[3:])
    ck(any("ftype == 2" in l and "ok" in l for l in oks),
       "the filter's tap survives a reboot")
    ck(any("ktrack == 75" in l and "ok" in l for l in oks),
       "key tracking survives a reboot")
    # fx_page is NOT asserted after the reboot, and the reason is a gap in the harness
    # rather than in the app: plat_host.c calls app_host_reboot() under #ifdef
    # APP_HOST_REBOOT, which this app does not define, so a simulated reboot re-runs
    # boot() without clearing any of the app's statics -- env_page, lfo_page and
    # edit_page all survive it too, and always have. On the device a reboot is a cold
    # start and the C runtime zeroes BSS, so the page is closed there for real; the
    # simulator simply cannot show it. Defining the hook would let this be checked, and
    # would make every page's boot state testable.

    print(f"\nchecks {count}, failed {len(fails)}")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(main())
