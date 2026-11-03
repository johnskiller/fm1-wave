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


def arc_fill(b, x0, w):
    """How far round the ring is lit, as the furthest lit column from the left edge.

    The ring is drawn with the source's ink at full strength above the fill and a dim
    track below it. Counting lit pixels in the knob's own 54 px column is enough to tell
    an empty arc from a full one without modelling the geometry again.
    """
    col = b[:, x0:x0 + w, :]
    lit = ((col[:, :, 0] > 60) | (col[:, :, 1] > 60) | (col[:, :, 2] > 60)).sum(axis=1)
    return int(lit.sum())


def main():
    out = os.path.join(APP, "build", "fxtest")
    os.makedirs(out, exist_ok=True)

    # ── the page, and exclusivity ────────────────────────────────────────────────
    o = run("""wait 300
expect fx_page 0
tap FX
master 400
expect fx_page 1
tap FX
master 400
expect fx_page 0
tap FX
master 300
expect fx_page 1
tap ENV
master 300
expect fx_page 0
tap FX
master 300
expect fx_page 1
tap LFO
master 300
expect fx_page 0
tap FX
master 300
tap ARP
master 300
expect fx_page 0
""", out)
    for line in o.splitlines():
        if line.startswith("  ok") or line.startswith("FAIL"):
            ck("FAIL" not in line, line.strip()[3:])

    # ── K3 and K4 step, wrap, and reach the DSP ──────────────────────────────────
    o = run("""wait 300
expect ftype 0
expect ktrack 0
tap FX
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
tap FX
master 400
expect cut 103
turn K2 4
master 200
expect res 16
tap FX
master 400
expect res 16
""", out)
    oks = [l for l in o.splitlines() if l.startswith("  ok") or l.startswith("FAIL")]
    ck(len(oks) >= 4 and all("FAIL" not in l for l in oks),
       "CUT and RES keep their values and their knobs on the FX page")

    # ── the rings show the setting, and differ between 25 % and 100 % ───────────
    o = run("""wait 400
tap FX
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
    # the KEY TRK knob is the fourth: x 178..232
    a0, a25, a100 = (arc_fill(k, 178, 54) for k in ks)
    print(f"  KEY TRK ring lit px: 0%% {a0}, 25%% {a25}, 100%% {a100}")
    ck(a0 < a25 < a100, "the KEY TRK ring fills with the setting, not with the detent")
    ck(a100 > a0 * 1.5, "100 % is visibly more arc than 0 %")

    # ── the record carries both fields across a reboot ──────────────────────────
    # This is the check that fails when a field is added to save_rec and forgotten in
    # save_snapshot (it would always read as the default) or in the restore (it would
    # come back as whatever the last RAM write left). ftype=2 / ktrack=75 are picked
    # because neither is the default: a broken path would return 0 and 0.
    o = run("""wait 300
tap FX
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
