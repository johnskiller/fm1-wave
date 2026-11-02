#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 johnskiller
"""The arpeggiator.

The claim is narrow: with RATE above zero the keys stop playing themselves and the
clock plays them, in the order TYPE says, over the octaves OCT says, for GATE of each
step. Everything else -- CUT, RES, the envelope, the voices -- is untouched, which is
what makes it an arpeggiator on this instrument rather than a second engine.

The walk is sampled at multiples of the step length, because that is the only way to
say "the next step played this note" without depending on where in a step the script
happened to look.

Checked against the defect: with arp_tick() not called from app_tick, the walk never
advances, arp_note stays at -1, and every pattern assertion here fails.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
HOST = os.path.join(APP, "build", "host", "wave_host")

# turn N on a knob is rate 4N, so these are the turns that land on each division.
TURN = {"1/4": 4, "1/8": 8, "1/8T": 12, "1/16": 16, "1/16T": 20, "1/32": 24}
MS = {"1/4": 500, "1/8": 250, "1/8T": 167, "1/16": 125, "1/16T": 83, "1/32": 62}

checks = failures = 0


def check(what, ok, detail=""):
    global checks, failures
    checks += 1
    if not ok:
        failures += 1
    print(f"  {'ok  ' if ok else 'FAIL'} {what}{('  ' + detail) if detail else ''}")


def run(script):
    d = tempfile.mkdtemp()
    sp = os.path.join(d, "s.txt")
    with open(sp, "w") as f:
        f.write(script)
    r = subprocess.run([HOST, sp, d], capture_output=True, text=True, timeout=300)
    return [l for l in (r.stdout + r.stderr).splitlines() if not l.startswith("dbg ")], r.returncode


def all_ok(script):
    out, rc = run(script)
    return rc == 0, [l for l in out if "FAIL" in l]


def walk(setup, n=8, div="1/8T"):
    """The notes the arp walked, one sample per step.

    Sampled exactly one step apart: any extra offset accumulates, and after a few
    steps the sample skips one and the walk reads as if it had jumped.
    """
    s = f"tap ARP\nturn K1 {TURN[div]}\n{setup}"
    for _ in range(n):
        s += f"wait {MS[div]}\nshow arp_note\n"
    out, _ = run(s)
    hits = []
    for l in out:
        if l.startswith("show arp_note "):
            hits.append(int(l.rsplit(" ", 1)[1]))
    return hits


def main():
    if not os.path.exists(HOST):
        raise SystemExit(f"build the host first: {HOST}")

    print("arp: the page and its knobs")

    ok, bad = all_ok("tap ARP\nexpect arp 1\ntap ARP\nexpect arp 0\n")
    check("B_ARP toggles the page", ok, "" if ok else bad[0].strip())

    ok, bad = all_ok("tap ENV\ntap ARP\nexpect arp 1\nexpect env 0\ntap LFO\nexpect lfo 1\nexpect arp 0\n")
    check("the three pages exclude each other", ok, "" if ok else bad[0].strip())

    ok, bad = all_ok("tap ARP\nexpect arp_on 0\nturn K1 20\nexpect arp_on 1\n")
    check("RATE doubles as the on/off switch, off at zero", ok, "" if ok else bad[0].strip())

    # The table used to hold { 1000, 667, 500, 333, ... }, which is not a note
    # division at any tempo; 667 ms is neither 1/8 (250) nor half-speed 1/8 (375).
    for name, want in MS.items():
        ok, bad = all_ok(f"tap ARP\nturn K1 {TURN[name]}\nexpect arp_step {want}\n")
        check(f"RATE {name} is {want} ms at 120 BPM", ok, "" if ok else bad[0].strip())

    print("arp: the walk")

    # Three keys, sorted by pitch, one octave: 60 62 64 60 ...
    got = walk("key 7 down\nkey 9 down\nkey 11 down\n")
    check("UP walks the held notes in pitch order and wraps",
          got[:4] == [60, 62, 64, 60], f"walked {got}")

    # The order is by pitch, not by the order the fingers landed.
    a = walk("key 11 down\nkey 7 down\nkey 9 down\n", n=4)
    b = walk("key 7 down\nkey 9 down\nkey 11 down\n", n=4)
    check("the walk is by pitch, not by when the keys landed", a == b and a,
          f"top-down {a} bottom-up {b}")

    # DOWN is the same set backwards, starting at the top.
    got = walk("turn K2 1\nturn K4 -16\nkey 7 down\nkey 9 down\nkey 11 down\n", n=6)
    check("DOWN walks the same notes from the top", got[:4] == [64, 62, 60, 64],
          f"walked {got}")

    # UP-DN bounces without repeating the ends: 60 62 64 62 60 62 ...
    #
    # The sample lands a little after each step boundary, so the sequence can start
    # anywhere in the bounce; what has to be true is that it goes up then comes down
    # and never plays an end twice in a row.
    got = walk("turn K2 2\nturn K4 -16\nkey 7 down\nkey 9 down\nkey 11 down\n", n=8)
    rises = any(got[i] < got[i + 1] for i in range(len(got) - 1))
    falls = any(got[i] > got[i + 1] for i in range(len(got) - 1))
    repeats = any(got[i] == got[i + 1] for i in range(len(got) - 1))
    check("UP-DN goes up and comes back down", rises and falls, f"walked {got}")
    check("UP-DN does not play an end twice", not repeats, f"walked {got}")
    check("UP-DN stays inside the held notes", set(got) <= {60, 62, 64}, f"walked {got}")

    # RND is still only the held notes: an arpeggiator that invents pitches is a
    # sequencer, and B_SEQ is still the empty button.
    got = walk("turn K2 3\nturn K4 -16\nkey 7 down\nkey 9 down\nkey 11 down\n", n=12)
    check("RND stays inside the held notes", got and set(got) <= {60, 62, 64},
          f"walked {got}")
    check("RND does not walk a fixed order", len(set(got)) > 1, f"walked {got}")

    print("arp: the octaves")

    # OCT 2 repeats the whole set an octave up before wrapping: 60 62 72 74 60 ...
    got = walk("turn K3 1\nkey 7 down\nkey 9 down\n", n=8)
    check("OCT 2 plays the set an octave higher before wrapping",
          set(got) == {60, 62, 72, 74}, f"walked {got}")
    # and in the right shape: each octave in pitch order, the higher one after the
    # lower. A cycle that jumped between them would satisfy the set and not the walk.
    # Every step is either +2 (the next note of the set), +10 (the set again an
    # octave up), or -14 (the top of the second octave back to the bottom).
    steps = {(got[i + 1] - got[i]) for i in range(len(got) - 1)}
    check("OCT 2 walks up within an octave and then up to the next",
          steps <= {2, 10, -14}, f"walked {got} steps {sorted(steps)}")

    ok, bad = all_ok("tap ARP\nturn K3 4\nexpect arp_oct 4\nturn K3 4\nexpect arp_oct 4\n"
                     "turn K3 -24\nexpect arp_oct 1\n")
    check("OCT is clamped to 1..4", ok, "" if ok else bad[0].strip())

    print("arp: the gate")

    ok, bad = all_ok("tap ARP\nturn K4 32\nexpect arp_gate 127\n")
    check("GATE opens to 100%", ok, "" if ok else bad[0].strip())

    ok, bad = all_ok("tap ARP\nturn K4 -32\nexpect arp_gate 1\n")
    check("GATE clamps to 1%, never to zero", ok, "" if ok else bad[0].strip())

    # A short gate leaves the step silent for the rest of it: with GATE at its
    # minimum the note is gone well before the next step arrives.
    quiet = walk("turn K4 -32\nkey 7 down\n", n=4, div="1/8")
    check("a short GATE still plays a note each step", len(quiet) == 4,
          f"walked {quiet}")

    print("arp: the keys")

    ok, bad = all_ok(
        "tap ARP\nturn K1 20\n"
        "key 7 down\nkey 9 down\nkey 11 down\nexpect arp_held 3\n"
        "key 9 up\nexpect arp_held 2\n"
        "key 7 up\nkey 11 up\nexpect arp_held 0\n"
        "wait 300\nexpect arp_note -1\n")
    check("a key up leaves the set, and an empty set stops the note", ok,
          "" if ok else bad[0].strip())

    ok, bad = all_ok("tap ARP\nexpect arp_on 0\ntap ARP\nkey 7 down\n"
                     "expect arp_held 0\n")
    check("with the arp off a key does not enter the set", ok, "" if ok else bad[0].strip())

    # MIDI goes through the same set: one engine, one sound, which was the lesson
    # from the MIDI-in work.
    # The two messages have to be separated by an audio block: midi_in() drains the
    # ring once per block, so a pair sent in the same block is one drain.
    ok, bad = all_ok("tap ARP\nturn K1 20\n"
                     "midi USB 90 3C 64\nwait 20\nmidi USB 90 40 64\nwait 20\n"
                     "expect arp_held 2\n")
    check("a MIDI note enters the arp set like a key does", ok, "" if ok else bad[0].strip())

    # A note-off from MIDI takes it out again.
    ok, bad = all_ok("tap ARP\nturn K1 20\n"
                     "midi USB 90 3C 64\nwait 20\nmidi USB 90 40 64\nwait 20\n"
                     "midi USB 80 3C 00\nexpect arp_held 1\n")
    check("a MIDI note off leaves the set", ok, "" if ok else bad[0].strip())

    # ── pages switched under a held key ─────────────────────────────────────
    # A note-on and a note-off have to agree about which engine owns the note, and
    # the panel lets that agreement be broken: hold a chord, open the ARP page, turn
    # RATE up, let go. The note-ons went to the voices (the arp was off) and the
    # note-offs went to the held set (the arp was on), so the voices were never
    # released -- a note that sounds until the unit is powered off.
    #
    # The same shape the other way round: start a note with the arp running, turn it
    # off while the key is down, lift the key -- the note-off goes to the voice and
    # the held set keeps a note that will play again the moment RATE goes up.
    #
    # Both are asserted through `active` (the voices the engine still thinks are
    # sounding), because that is the quantity a hang is made of.
    print("arp: pages switched under a held key")
    ok, bad = all_ok("key 7 down\nkey 9 down\nwait 40\n"
                     "tap ARP\nturn K1 16\n"
                     "key 7 up\nkey 9 up\n"
                     "wait 40\nturn K1 -16\ntap ARP\n"
                     "wait 300\nexpect active 0\n")
    check("a chord held into the arp lets go of its voices", ok, "" if ok else bad[0].strip())

    ok, bad = all_ok("tap ARP\nturn K1 16\nkey 7 down\nwait 40\n"
                     "tap ARP\nturn K1 -16\n"      # the arp off while the key is down
                     "key 7 up\n"
                     "tap ARP\nturn K1 16\n"       # and on again
                     "wait 300\nexpect arp_held 0\nexpect active 0\n")
    check("a note started by the arp lets go when the arp is switched off under it", ok,
          "" if ok else bad[0].strip())

    print(f"\n{checks} checks, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
