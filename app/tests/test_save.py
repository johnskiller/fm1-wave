#!/usr/bin/env python3
"""test_save.py -- B_SAVE is the only writer, and the record survives a reboot.

The host simulator's `reboot` re-runs boot from a simulated flash that outlives it
(plat_host.c: the store is a buffer per object, kept across a reboot), which is exactly
the property this file needs: anything that "persists" only because the variable was
never cleared fails here.

What is deliberately NOT tested: that a save is fast, or that it does not glitch the
audio. Neither is measurable in the simulator (its store is a memcpy), and both are facts
about the device -- storage.c erases a 4 KiB sector and reads it back. What IS tested is
the decision that came from those facts: the app writes when asked and at no other time.
"""
import os
import subprocess
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = os.path.join(HERE, "..", "build", "host", "wave_host")
OUT = os.path.join(HERE, "..", "build", "save")
checks = failed = 0


def run(body, name):
    """Run a host script; return its output."""
    os.makedirs(OUT, exist_ok=True)
    script = os.path.join(OUT, name + ".txt")
    with open(script, "w") as f:
        f.write(body.strip() + "\n")
    r = subprocess.run([HOST, script, OUT], capture_output=True, text=True, timeout=300)
    if r.returncode not in (0, 1):
        print(r.stdout[-3000:], r.stderr[-3000:])
        raise SystemExit(f"{name}: host exited {r.returncode}")
    return r.stdout + r.stderr


def ok(cond, what, detail=""):
    global checks, failed
    checks += 1
    if cond:
        print(f"ok   {what}")
    else:
        failed += 1
        print(f"FAIL {what}" + (f"\n     {detail}" if detail else ""))


def probes(out, what):
    """Every value this probe was checked at, in script order.

    One script can check the same probe twice (before and after a recovery), and those are
    two different assertions; returning only one of them silently checks the wrong one.
    """
    """Read an `expect` line's value out of the host's output.

    The host prints "  ok <what> == <value>" for a check that passed and
    "FAIL <what> == <value> (got ...)" for one that did not; a missing line means the
    probe name does not exist in app_expect, which is a test bug, not a product bug.
    """
    hits = []
    for line in out.splitlines():
        t = line.strip()
        if t.startswith(f"ok {what} ==") or t.startswith(f"FAIL {what} =="):
            hits.append(int(t.split("==", 1)[1].split("(")[0].strip()))
    if not hits:
        raise SystemExit(f"no `expect {what}` line in the output:\n{out[-1500:]}")
    return hits


def probe(out, what):
    h = probes(out, what)
    if len(h) != 1:
        raise SystemExit(f"`{what}` was checked {len(h)} times in one script: use probes() "
                         f"and index it, or the assertion is ambiguous")
    return h[0]


# ── 1. nothing saved at boot: the defaults, and the mark on nothing ─────────────
out = run("""
expect saved 0
expect save_dirty 0
expect save_count 0
expect cut 127
""", "boot")
ok(probe(out, "saved") == 0, "a flash with no record boots on the defaults")
ok(probe(out, "save_count") == 0, "boot writes nothing (no save at power-on)")
ok(probe(out, "cut") == 127, "the default CUT is the engine's own 127 (open)")

# ── 2. a turn makes it dirty, and does NOT write ────────────────────────────────
out = run("""
turn K1 -8
expect save_dirty 1
expect save_count 0
expect cut 95
""", "dirty")
ok(probe(out, "save_dirty") == 1, "turning a knob marks the state unsaved")
ok(probe(out, "save_count") == 0, "and does not write: there is no auto-save")
ok(probe(out, "cut") == 95, "the knob itself landed where it should")

# ── 3. B_SAVE writes, once, and clears the mark ─────────────────────────────────
# What this can and cannot see, measured rather than assumed:
#
#   removing `dirty = 0` from save_now          -> still 0 failed. save_scan recomputes the
#                                                  flag from the baseline every frame, so it
#                                                  clears itself one frame later. A guard
#                                                  here would be a guard on nothing.
#   removing `rec_written = rec` from save_now  -> FAIL (save_dirty reads 1). This is the
#                                                  real shape of the defect: the write
#                                                  reported SAVED, flash has it, and the
#                                                  panel says "not saved" forever.
#
# So the assertion below is about the baseline, which is what makes the mark honest.
out = run("""
turn K1 -8
tap SAVE
expect save_count 1
expect save_dirty 0
expect save_last 0
""", "save_once")
ok(probe(out, "save_count") == 1, "B_SAVE writes exactly once")
ok(probe(out, "save_dirty") == 0, "and the mark clears at the save, not a frame later")
ok(probe(out, "save_last") == 0, "the write reported success")

# ── 4. the record survives a reboot ─────────────────────────────────────────────
out = run("""
turn K1 -8
turn K2 4
turn ALGO 5
tap SAVE
reboot
expect saved 1
expect cut 95
expect res 16
expect morph 40
expect save_dirty 0
""", "reboot")
ok(probe(out, "saved") == 1, "after a save, boot finds a record")
ok(probe(out, "cut") == 95, "CUT came back from flash")
ok(probe(out, "res") == 16, "RES came back")
ok(probe(out, "morph") == 40, "MORPH came back")
ok(probe(out, "save_dirty") == 0, "and a freshly loaded record counts as saved")

# ── 5. the whole panel set, not just three knobs ────────────────────────────────
out = run("""
turn K1 -4
turn K2 6
turn K3 1
turn K4 2
tap ENV
turn K2 3
turn K3 -2
tap ENV
tap LFO
turn K3 7
turn K4 5
tap LFO
tap ARP
turn K1 2
turn K2 1
turn K3 1
turn K4 -3
tap ARP
tap SAVE
reboot
expect cut 111
expect res 24
expect shape 1
expect amount 8
expect eg_d 76
expect eg_s 119
expect lfo_rate 28
expect lfo_depth 20
expect arp_rate 8
expect arp_type 1
expect arp_oct 2
expect arp_gate 52
""", "all_knobs")
for what, want in [("cut", 111), ("res", 24), ("shape", 1), ("amount", 8),
                   ("eg_d", 76), ("eg_s", 119), ("lfo_rate", 28), ("lfo_depth", 20),
                   ("arp_rate", 8), ("arp_type", 1), ("arp_oct", 2), ("arp_gate", 52)]:
    ok(probe(out, what) == want, f"{what} survived the reboot")

# ── 6. the region and its loop, per slot ────────────────────────────────────────
out = run("""
tap SEL
tap EDIT
turn K1 5
turn K3 -3
turn PRESET 1
tap EDIT
tap SAVE
reboot
expect src 1
expect edit 0
expect region_start 1125
expect region_loope 51694
expect region_on 1
""", "region")
ok(probe(out, "src") == 1, "SOURCE came back as SMPL")
ok(probe(out, "edit") == 0, "the EDIT page is a view, not a saved setting: boot is out of it")
ok(probe(out, "region_start") == 1125, "START came back")
ok(probe(out, "region_loope") == 51694, "LOOP E came back")
ok(probe(out, "region_on") == 1, "LOOP on came back")

# ── 7. a failed write is reported and NOT reported as saved ────────────────────
out = run("""
flash_fail 1
turn K1 -8
tap SAVE
expect save_count 0
expect save_dirty 1
expect save_last -1
flash_fail 0
tap SAVE
expect save_count 1
expect save_dirty 0
""", "fail")
sc = probes(out, "save_count")
sd = probes(out, "save_dirty")
ok(sc[0] == 0, "a failing flash writes nothing")
ok(sd[0] == 1, "and the state stays marked unsaved")
ok(probe(out, "save_last") == -1, "the failure is reported (not swallowed)")
ok(sc[1] == 1, "a later successful save works")
ok(sd[1] == 0, "and the mark clears on the successful one")

# ── 8. saving with notes held: allowed, and that is the point ──────────────────
out = run("""
turn K1 -8
key 6 down
tap SAVE
expect save_count 1
key 6 up
reboot
expect cut 95
""", "save_while_playing")
ok(probe(out, "save_count") == 1, "B_SAVE writes while a note is held (the chord is the sound you want)")
ok(probe(out, "cut") == 95, "and it is what comes back")

# ── 9. a record from a DIFFERENT version falls back to the defaults ────────────
# The platform's CRC catches a torn write. What save_load() adds is the case a CRC cannot:
# a record that is perfectly intact and was written by another build -- a restore from an
# older firmware, which is a thing the backup path makes easy to do.
#
# `plant` (app_host_cmd, host builds only) writes such a record straight into the store,
# bypassing the app. Both directions are checked, because "rejects everything" would pass
# a one-sided test: a stranger's version must be ignored, and this build's own version
# must be believed.
out = run("""
plant 99
reboot
expect saved 0
expect cut 127
""", "plant_wrong_version")
ok(probe(out, "saved") == 0, "an intact record from another version is rejected")
ok(probe(out, "cut") == 127, "and the defaults boot instead of that record's values")

out = run("""
plant 1
reboot
expect saved 1
expect cut 3
""", "plant_right_version")
ok(probe(out, "saved") == 1, "a record of THIS version is accepted (the gate is not just paranoia)")
ok(probe(out, "cut") == 3, "and its values are what boot")

# ── 10. the "not saved" mark is drawn, and it is the panel's only warning ──────
# This is the whole safety of the manual model: an edit in RAM that flash does not have.
# It cannot be heard, so it has to be seen. Measured at 3/5 ink (148,109,32 on SYNTH's
# amber background) in the top-left of the material band, one row under the slot name.
def ink_in(path, rgb, y0, y1, x0, x1):
    b = np.array(Image.open(path).convert("RGB"))
    sub = b[y0:y1, x0:x1]
    return int(((sub[:, :, 0] == rgb[0]) & (sub[:, :, 1] == rgb[1]) & (sub[:, :, 2] == rgb[2])).sum())


out = run("""
turn K1 -8
shot dirty.png
tap SAVE
shot saved.png
""", "mark")
MARK = (148, 109, 32)          # dim(sk.ink_hi, 3, 5) quantised to RGB565 on SYNTH
d = ink_in(os.path.join(OUT, "dirty.png"), MARK, 84, 120, 0, 80)
v = ink_in(os.path.join(OUT, "saved.png"), MARK, 84, 120, 0, 80)
ok(d >= 60, "the NOT SAVED mark is drawn while there are unsaved edits", f"{d} px")
ok(v < d // 3, "and it is gone once saved", f"dirty {d} px, saved {v} px")

# The mark must stay out of SYNTH's label column. That column is the picture's neighbour,
# and a 42 px string anchored to its right edge reached back to x 192 and closed the 12 px
# gap the geometry promises between the picture (ends at 175) and the labels (start at 189)
# -- which test_screen.py caught as a broken gap.
#
# Asserting the gap from THIS file does not work, and that was measured: a clean-columns
# test over the whole band passes with the mark back in the column, because the labels
# themselves already ink those columns. The check has to be about the mark's own extent,
# so it is measured from the two screenshots: everything the mark added, on the row it is
# drawn on, must end left of the picture's edge.
def mark_columns(dirty, saved, y0, y1):
    """Columns where the mark's ink appears in `dirty` and not in `saved`."""
    b = np.array(Image.open(dirty).convert("RGB")).astype(int)
    v = np.array(Image.open(saved).convert("RGB")).astype(int)
    d = np.abs(b - v).sum(axis=2)
    cols = set()
    for y in range(y0, y1):
        for x in range(0, 240):
            if d[y, x] > 8:
                cols.add(x)
    return sorted(cols)


mc = mark_columns(os.path.join(OUT, "dirty.png"), os.path.join(OUT, "saved.png"), 100, 116)
ok(mc and max(mc) < 176, "the mark stays left of SYNTH's picture edge",
   f"mark spans x {mc[0] if mc else '-'}..{max(mc) if mc else '-'}")

print(f"\n{checks} checks, {failed} failed")
sys.exit(1 if failed else 0)
