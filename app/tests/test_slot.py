#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""EN_PRESET must change the SOUND in SMPL, not only the label.

The regression this exists for: the screen said SLOT 2/2 and the sound was SLOT 1/2.
EN_PRESET moved `smp_i` while the chops stayed on the boot slot (slices_from was set once
at boot) and the no-chop fallback was hardcoded `smp[0]`. GRAN did not have the bug because
it reads `smp[smp_i]` directly -- one source honouring the knob and one not is what made it
visible at all.

Why a WAV and not a level check: measured at the same key and velocity the two demo slots
peak 0.7 dB apart (-14.4 and -15.1 dBFS), so any peak threshold loose enough to pass the
right sample also passes the wrong one. The shapes, however, are not close:

    slot 0 (DEMO KIT)   4 of 16 segments then silence   -- a drum transient
    slot 1 (DEMO TONE)  11 of 16 segments flat at 32     -- a 220 Hz sustained tone
    zero-lag correlation between them: 0.0001

So the check is on the envelope's SHAPE, which is what actually distinguishes the slots.

    python3 app/tests/test_slot.py          # needs app/build/host/wave_host
"""
import math
import os
import struct
import subprocess
import sys
import tempfile
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
HOST = os.path.join(ROOT, "app", "build", "host", "wave_host")

# key 20 is above the demo kit's chops (they land on 53..56 = keys 0..3), so this is the
# no-chop fallback -- the path that was hardcoded to slot 0. Testing a chop key would also
# exercise the bug, but through reslice() rather than through the fallback, and the two are
# separate defects that were both fixed.
KEY = 20
FRAMES = 16

fails = 0
checks = 0


def ck(cond, what, detail=""):
    global fails, checks
    checks += 1
    if not cond:
        fails += 1
        print(f"  FAIL {what}{(' -- ' + detail) if detail else ''}")


def read_wav(path):
    w = wave.open(path)
    n, ch, sw = w.getnframes(), w.getnchannels(), w.getsampwidth()
    data = w.readframes(n)
    w.close()
    if sw == 2:
        s = struct.unpack("<%dh" % (len(data) // 2), data)
    elif sw == 4:
        s = struct.unpack("<%di" % (len(data) // 4), data)
    else:
        raise SystemExit(f"unexpected sample width {sw} in {path}")
    return [s[i] for i in range(0, len(s), ch)]


def envelope(x, frames=FRAMES):
    """RMS per segment, quantised to 0..255.

    An envelope rather than the raw samples because the two slots are at different pitches
    and phases; comparing samples directly would fail for a correct implementation the
    moment anything shifted by one frame. The envelope's SHAPE is what the ear uses to tell
    a drum hit from a sustained tone.
    """
    seg = max(1, len(x) // frames)
    out = []
    for i in range(frames):
        chunk = x[i * seg:(i + 1) * seg]
        r = math.sqrt(sum(v * v for v in chunk) / max(1, len(chunk)))
        out.append(int(255.0 * r / 32768.0))
    return out


def record(slot_turns, name, out):
    script = f"""wait 600
master 4096
wait 100
press SEL
wait 80
release SEL
wait 300
"""
    for _ in range(slot_turns):
        script += "turn PRESET 1\nwait 400\n"
    script += f"""wav {name}.wav
key {KEY} down
wait 900
key {KEY} up
wait 200
wavstop
"""
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        f.write(script)
        path = f.name
    try:
        r = subprocess.run([HOST, path, out], capture_output=True, text=True, timeout=300, cwd=out)
        ck(r.returncode == 0, f"host ran for {name}", (r.stdout + r.stderr).strip()[-200:])
    finally:
        os.unlink(path)
    p = os.path.join(out, f"{name}.wav")
    ck(os.path.exists(p), f"{name}.wav was written")
    return envelope(read_wav(p)) if os.path.exists(p) else []


def main():
    if not os.path.exists(HOST):
        sys.exit(f"{HOST} is missing: build it with `python3 platform/tools/build.py app --host`")

    out = tempfile.mkdtemp(prefix="fm1slot")
    # one turn of EN_PRESET from the boot slot, and back again
    slot0 = record(0, "slot0", out)
    slot1 = record(1, "slot1", out)
    if fails:
        print(f"checks {checks}, failed {fails}")
        return 1

    print(f"slot 0 envelope: {slot0}")
    print(f"slot 1 envelope: {slot1}")

    # the drum transient decays within the first quarter; the sustained tone does not
    quiet_at = next((i for i, v in enumerate(slot0) if v == 0), len(slot0))
    ck(quiet_at <= FRAMES // 2, "slot 0 is a transient that decays (DEMO KIT)",
       f"silent from segment {quiet_at}")

    loud_segments = sum(1 for v in slot1 if v > 8)
    ck(loud_segments >= FRAMES // 2, "slot 1 sustains (DEMO TONE)",
       f"{loud_segments} of {FRAMES} segments above 8")

    differ = max(abs(a - b) for a, b in zip(slot0, slot1)) if slot0 and slot1 else 0
    ck(differ >= 15, "the two slots are different material, not one at two levels",
       f"largest envelope difference {differ}/255")

    # ...and explicitly: the shapes must not be near-identical, which is what the shipped
    # bug produced (whichever slot was selected, the same sample played)
    same = sum(1 for a, b in zip(slot0, slot1) if abs(a - b) <= 2)
    ck(same < FRAMES // 2, "the envelopes are not the same shape",
       f"{same} of {FRAMES} segments within 2/255")

    print(f"checks {checks}, failed {fails}")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
