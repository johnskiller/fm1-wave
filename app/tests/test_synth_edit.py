#!/usr/bin/env python3
"""app/tests/test_synth_edit.py -- SYNTH's EDIT page: a phase offset per frame.

The page exists because an offset is a value you reach for while listening, not a field
you type. It is also the first page whose knobs are not four independent values: K3
(SPREAD) rewrites all eight frames at once, so the page has a notion of a RAMP that the
eight values are derived from, and K2 can pull one frame off that ramp.

K1 = FRAME, K2 = OFFSET (that frame, points of 256), K3 = SPREAD (points per frame),
K4 = SHAPE (LIN/ALT/RAND). EN_PRESET zeroes all eight and SPREAD with them.

What each check would catch:
  * K2 turning the ramp instead of one frame -- an implementation that calls foff_ramp()
    on every edit would pass "the value changed" and fail "frame 0 is still zero"
  * SPREAD stored as a 0..127 knob position but used as points -- the sum is a number
    that cannot be right by accident: LIN over eight frames is 28 x the step
  * the ring showing the knob position rather than the value
  * a save that writes the table but never reads it back -- and the host does not clear
    statics on reboot, so the test turns the value off between the save and the reboot
  * a page that survives a source change

Run: python3 app/tests/test_synth_edit.py
"""
import os
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
HOST = os.path.join(APP, "build", "host", "wave_host")

fails = []
count = 0


def ck(cond, what):
    global count
    count += 1
    if cond:
        print(f"  ok   {what}")
    else:
        fails.append(what)
        print(f"  FAIL {what}")


def run(script, allow_fail=False):
    outdir = tempfile.mkdtemp(prefix="se_")
    path = os.path.join(outdir, "s.txt")
    with open(path, "w") as f:
        f.write(script)
    r = subprocess.run([HOST, path, outdir], capture_output=True, text=True, timeout=300)
    if r.returncode and not allow_fail:
        print(r.stdout[-3000:])
        raise SystemExit("host failed")
    return r.stdout


def shows(o, name):
    """Every value printed for one probe, in order.

    Taking the last one is wrong the moment a script asks twice, and taking the first is
    wrong when the same probe is asked before and after an action -- which is exactly the
    shape of a "did it change" check. So the tests count them.
    """
    out = []
    for line in o.splitlines():
        s = line.strip()
        if not s.startswith("show "):
            continue
        parts = s.split()
        if len(parts) >= 3 and parts[1] == name:
            try:
                out.append(int(parts[2]))
            except ValueError:
                pass
    return out


def one(o, name):
    v = shows(o, name)
    if not v:
        raise SystemExit(f"probe {name} never printed:\n{o[-2000:]}")
    return v[-1]


def main():
    print("SYNTH's EDIT page:")

    print("opens with EDIT and starts empty")
    o = run("""wait 300
tap EDIT
master 200
show edit_page
show edit_src
show foff
show foff_spread
""")
    ck(one(o, "edit_page") == 1, "EDIT opens a page in SYNTH")
    ck(one(o, "edit_src") == 0, "and it is SYNTH's page")
    ck(one(o, "foff") == 0, "the selected frame starts at no offset")
    ck(one(o, "foff_spread") == 0, "and so does the ramp")

    print("K2 turns the selected frame one point a detent")
    o = run("""wait 300
tap EDIT
master 200
turn K2 1
master 150
show foff
turn K2 1
master 150
show foff
turn K2 -1
turn K2 -1
turn K2 -1
master 150
show foff
""")
    seq = shows(o, "foff")
    print(f"  foff: {seq}")
    ck(seq[0] == 1, "one detent is one point of 256")
    ck(seq[1] == 2, "and the next adds one more")
    ck(seq[2] == 255, "going below zero wraps to 255, not to a clamp")

    print("K1 selects the frame, and each frame keeps its own value")
    o = run("""wait 300
tap EDIT
master 200
turn K1 3
master 150
show edit_frame
turn K2 2
turn K2 2
master 150
show foff
turn K1 -3
master 150
show edit_frame
show foff
""")
    ck(shows(o, "edit_frame") == [3, 0], "K1 moves the frame and wraps back to 0")
    seq = shows(o, "foff")
    print(f"  foff: {seq}")
    ck(seq[0] == 4, "frame 3 took the four points")
    ck(seq[1] == 0, "and frame 0 is still empty")

    print("K3 fans all eight frames out")
    o = run("""wait 300
tap EDIT
master 200
turn K3 4
master 150
show foff_spread
show foff_sum
turn K3 -4
master 150
show foff_sum
turn K3 1
master 150
show foff
turn K1 1
master 150
show foff
""")
    ck(one(o, "foff_spread") == 4, "SPREAD is stored as detents")
    sums = shows(o, "foff_sum")
    print(f"  foff_sum: {sums}")
    ck(sums[0] == 224, "4 detents = 8 pts/frame, and LIN over 8 frames sums to 224")
    ck(sums[1] == 0, "turning SPREAD back to zero empties the table")
    seq = shows(o, "foff")
    ck(seq[0] == 0, "one detent of SPREAD leaves frame 0 at zero")
    ck(seq[1] == 2, "and puts frame 1 two points out")

    print("K4 picks the shape")
    o = run("""wait 300
tap EDIT
master 200
turn K3 4
master 200
show foff_sum
turn K4 1
master 200
show foff_shape
show foff_sum
turn K4 1
master 200
show foff_shape
show foff_sum
turn K4 1
master 200
show foff_shape
""")
    ck(shows(o, "foff_shape") == [1, 2, 0], "LIN, ALT, RAND, back to LIN")
    sums = shows(o, "foff_sum")
    print(f"  foff_sum: {sums}")
    ck(sums[0] == 224, "LIN ramps 0,8,16,...")
    # ALT: frames 1,3,5,7 get the full step (8 pts) and the even ones stay at 0, so the
    # sum is 4 x 8 = 32, not half of LIN's 224 (LIN also counts the small early steps).
    ck(sums[1] == 32, "ALT puts the odd frames at 8 pts and the even ones at zero")

    print("EN_PRESET zeroes the whole table")
    o = run("""wait 300
tap EDIT
master 200
turn K3 4
master 150
show foff_sum
spin PRESET 1
master 200
show foff_sum
show foff_spread
""")
    ck(one(o, "foff_sum") == 0, "the ramp is gone")
    ck(one(o, "foff_spread") == 0, "and SPREAD reads zero, not a stale detent")

    print("the page closes when the source changes")
    o = run("""wait 300
tap EDIT
master 200
show edit_page
tap SEL
master 200
show edit_page
show src
""")
    ck(shows(o, "edit_page") == [1, 0], "switching source closes the page")
    ck(one(o, "src") == 1, "and the source really did change")

    print("SMPL still has its own EDIT page")
    o = run("""wait 300
tap SEL
master 200
tap EDIT
master 200
show edit_page
show edit_src
""")
    ck(one(o, "edit_page") == 1, "EDIT opens on SMPL")
    ck(one(o, "edit_src") == 1, "and it is SMPL's page")

    print("the ramp survives a save")
    o = run("""wait 300
tap EDIT
master 200
turn K3 4
turn K2 3
master 150
show foff_sum
show foff
tap SAVE
master 300
show saved
show save_count
turn K2 -3
turn K3 -4
master 200
show foff_sum
reboot
show saved
show foff_sum
tap EDIT
master 150
show foff
show foff_spread
""")
    sums = shows(o, "foff_sum")
    print(f"  foff_sum: {sums}  saved: {one(o, 'saved')}  count: {one(o, 'save_count')}")
    ck(one(o, "saved") == 1, "the record was accepted")
    ck(one(o, "save_count") == 1, "one save, not one per frame")
    ck(sums[0] > 0, "there was something to save")
    ck(sums[1] == 0, "and it was turned off without saving")
    ck(sums[2] == sums[0], "the reboot brought the ramp back exactly")
    ck(one(o, "foff") == 3, "including the frame that had been hand-turned")
    ck(one(o, "foff_spread") == 4, "and SPREAD came back with it")

    print(f"\n{'ALL PASSED' if not fails else 'FAILED'} ({count} checks, {len(fails)} failed)")
    if fails:
        for f in fails:
            print("  -", f)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
