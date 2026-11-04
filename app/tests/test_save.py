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
tap GLO
turn K4 3
tap LFO
tap LFO
turn SELECT 3
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
expect morph_mod 12
expect arp_rate 8
expect arp_type 1
expect arp_oct 2
expect arp_gate 52
""", "all_knobs")
for what, want in [("cut", 111), ("res", 24), ("shape", 1), ("amount", 8),
                   ("eg_d", 76), ("eg_s", 119), ("lfo_rate", 28), ("lfo_depth", 20),
                   ("morph_mod", 12), ("arp_rate", 8), ("arp_type", 1), ("arp_oct", 2), ("arp_gate", 52)]:
    ok(probe(out, what) == want, f"{what} survived the reboot")

# ── 6. the region and its loop, per slot ────────────────────────────────────────
out = run("""
tap SEL
tap EDIT
turn K1 5
turn K3 -3
turn PRESET 1
tap HOME
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
expect saved 0
expect cut 127
""", "plant_v1_record")
ok(probe(out, "saved") == 0,
   "a version-1 record is refused: the layout gained morph_mod, so its bytes are off by one field")
ok(probe(out, "cut") == 127, "and the defaults boot instead of that record's values")

# A version-2 record is ALSO refused, and this is the assertion that keeps the version
# bump honest: version 3 gave every LFO its own rate/shape/trig/sync and seven amounts, so
# a v2 record's single lfo_depth sits where v3 reads lfo_shape[0] -- a record that would
# boot with a plausible but wrong LFO rather than with defaults.
out = run("""
plant 2
reboot
expect saved 0
expect cut 127
""", "plant_v2_record")
ok(probe(out, "saved") == 0,
   "a version-2 record is refused: two LFOs reshaped the LFO fields, so its bytes mean something else")
ok(probe(out, "cut") == 127, "and the defaults boot instead of that record's values")

out = run("""
plant 3
reboot
expect saved 1
expect cut 3
""", "plant_right_version")
ok(probe(out, "saved") == 1, "a record of THIS version is accepted (the gate is not just paranoia)")
ok(probe(out, "cut") == 3, "and its values are what boot")

# ── 10. the save line: length is how much, colour is how long ──────────────────
#
# The mark used to be the words "NOT SAVED" in the top-left of the material band. It was
# condemned by measurement, not by taste: in its own 13 x 62 box it erased 443 px of
# SYNTH's waveform (403 px of SMPL's and GRAN's), in the brightest part of the picture.
#
# It is now two rows in the padding the band already had, directly under the scope's
# 0 dBFS line, and it carries two numbers with two axes: LENGTH = the count of groups
# that differ from flash, COLOUR = how long they have been left that way.
#
# What this can and cannot see, measured rather than assumed:
#
#   wrong row            -> FAIL on the row check. The band's padding is 6 rows above the
#                           picture and the wave reaches y 90, so the line must stay in 84..85.
#   ink_hi for the colour -> FAIL: ink_hi is (255,186,65), the same colour a knob arc fills
#                           with, and a save line in it reads as a fifth control.
#   length not clearing   -> FAIL: "gone once saved" compares the two screenshots.
#
# The brightness ramp is asserted through the probe (save_ink, 0..3), not through pixels:
# the ramp is 5 minutes long and a screenshot taken mid-ramp would pin an arbitrary step.
def band_px(path, bg, y0, y1, x0=0, x1=240):
    b = np.array(Image.open(path).convert("RGB"))
    sub = b[y0:y1, x0:x1]
    m = np.any(sub != np.array(bg, dtype=np.uint8), axis=2)
    return int(m.sum())


def line_extent(path, bg, y):
    b = np.array(Image.open(path).convert("RGB"))
    m = np.any(b[y] != np.array(bg, dtype=np.uint8), axis=1)
    xs = np.where(m)[0]
    return (int(xs.min()), int(xs.max())) if len(xs) else None


out = run("""
expect save_line 0
turn K1 -4
master 3000
expect save_line 30
shot dirty.png
tap SAVE
master 3000
expect save_line 0
shot saved.png
""", "line")
# One group is 240/8 = 30 px: a single edit has to move the line visibly, or it reads broken.
sl = probes(out, "save_line")
ok(sl[0] == 0 and sl[1] == 30 and sl[2] == 0,
   "the line is 0 px, then 30 px for one changed group, then 0 again after the save")

SYNTH_BG = (41, 28, 8)
d = band_px(os.path.join(OUT, "dirty.png"), SYNTH_BG, 87, 89)
v = band_px(os.path.join(OUT, "saved.png"), SYNTH_BG, 87, 89)
ok(d == 60, "the line is drawn on both of its rows while unsaved", f"{d} px")
ok(v == 0, "and nothing at all is drawn once saved", f"{v} px")

# It must not touch the picture. The wave's highest ink is y 90 and the band's picture
# starts at MAT_PAD = 6, so rows 86..89 are the air that keeps a growing line from
# becoming a lid on the waveform.
# The line sits at band rows 3..4 (screen 87..88) for a measured reason: at rows 0..1 it
# was one row under the scope's full-width 0 dBFS graticule with no air between them, and
# two 240 px bars on consecutive rows read as one crowded block.
ok(band_px(os.path.join(OUT, "dirty.png"), SYNTH_BG, 84, 87) == 0,
   "the three rows directly under the scope stay empty (the line is not crowding it)")
ok(band_px(os.path.join(OUT, "dirty.png"), SYNTH_BG, 89, 91) == 0,
   "and one row of air is left between the line and the wave")

# Colour, not length, carries the age -- and it must top out at `ink`, never `ink_hi`.
# dim(ink, 5, 5) is ink itself: (238,137,24) on SYNTH, quantised.
INK = (238, 137, 24)
out = run("""
turn K1 -4
master 3000
""" + "master 3000\nwait 1000\n" * 300 + """
expect save_ink 3
shot old.png
""", "ramp")
ok(probe(out, "save_ink") == 3, "an edit left unsaved for 5 minutes is at full brightness")
b = np.array(Image.open(os.path.join(OUT, "old.png")).convert("RGB"))
row = b[87]
xs = np.where(np.any(row != np.array(SYNTH_BG, dtype=np.uint8), axis=1))[0]
cols = set(tuple(int(c) for c in row[x]) for x in xs) if len(xs) else set()
ok(INK in cols, "and it reaches exactly `ink`, the knob arc's colour", f"{sorted(cols)}")
ok((255, 186, 65) not in cols, "never `ink_hi`: a save line at ink_hi reads as a fifth control")

# ── 11. B_SAVE's lamp says the same thing, on the button you would press ────────
#
# Three levels, because that is what the matrix can show: lit, mid (felucca-bsp's steady
# ~12 % level), and the glow every unlit button already has. The top level is tied to
# SAVE_LINE_FULL_MS rather than a threshold of its own, so "the line is full" and "the
# button is fully lit" are one event said twice and cannot drift apart.
# The two boundaries, not just the ends: a lamp on its own clock would pass a check that
# only looked at t=0 and t=310 s. Measured against the line's own numbers -- mid below the
# top step, lit at it -- 299 s and 301 s straddle SAVE_LINE_FULL_MS (300 s).
out = run("""
wait 200
expect save_led 0
turn K1 -4
wait 200
expect save_led 1
""" + "wait 1000\n" * 298 + """
expect save_led 1
""" + "wait 1000\n" * 4 + """
expect save_led 2
tap SAVE
wait 200
expect save_led 0
""", "lamp")
led = probes(out, "save_led")
ok(led == [0, 1, 1, 2, 0],
   "the lamp is dark when saved, mid while an edit waits (still mid a second before the "
   "line fills), lit at the line's own threshold, dark again after the save", f"{led}")

# The flag is not the light. The platform is told through two different calls -- lit comes
# via plat_leds, mid via plat_lights_mid -- and a flag that is set while the wrong call is
# made would pass the check above and leave the button dark on the device.
# WHAT THIS CANNOT SEE, measured rather than assumed: plat_lights_mid's button argument.
# plat_host.c's plat_lights_mid takes (b, k) and does `(void)b; mid_keys = k & ~lit_keys` --
# mid is only recorded for KEYS, so the simulator cannot show a mid BUTTON at all. Injecting
# "delete the plat_lights_mid call" leaves this file at 55/55 green, which is the honest
# state of it: the app's own state machine is covered, the lamp's mid path is not, and a
# B_SAVE that sits at glow while an edit waits would be caught by eye on the device and by
# nothing here. Fixing that means teaching the host to record mid buttons, which is a change
# to platform/ and out of bounds for this repo.
out = run("""
turn K1 -4
wait 200
leds
""", "lamp_mid")
ok("SAVE" not in out.split("leds:")[1],
   "an unsaved edit does not put SAVE in the lit set (it is mid, not lit)")

out = run("""
turn K1 -4
""" + "wait 1000\n" * 310 + """
leds
""", "lamp_lit")
ok("SAVE" in out.split("leds:")[1], "and it does reach the lit set once the line is full")

# The line takes the source's own colour, so it belongs to whichever skin is up.
for name, pre, bg in (("smpl", "tap SEL\n", (8, 40, 16)),
                      ("gran", "tap SEL\ntap SEL\n", (8, 36, 49))):
    out = run(pre + "turn K1 -4\nmaster 3000\nshot " + name + ".png\n", name)
    ext = line_extent(os.path.join(OUT, name + ".png"), bg, 87)
    # 60 px, not 30: switching SOURCE is itself a saved field, so this is two groups --
    # `src` and the one region the knob moved. Length counts groups, not knobs.
    ok(ext is not None and ext[1] - ext[0] + 1 == 60, f"the line is drawn in {name.upper()} too",
       f"{ext}")

print(f"\n{checks} checks, {failed} failed")
sys.exit(1 if failed else 0)
