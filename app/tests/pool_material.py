#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 johnskiller
"""Which material is in the resource region right now?

Several tests measure the SHAPE of the shipped demo material -- a drum transient
that decays, a tone that sustains, a playhead that parks at a known x. Those tests
are meaningful only while the pool is the demo pool. Once real material is packed
with pack_samples.py the shapes are whatever was recorded, and asserting the demo
shapes there reports a failure that is not a defect.

    from pool_material import is_demo_pool, describe

`is_demo_pool()` reads the pool header that the firmware itself reads
(wv_sample.h: 8 B header, 20 B per slot, name in the last 6 bytes of each entry),
so the answer comes from the same bytes the reader sees, not from a build flag.
"""
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
POOL = os.path.join(ROOT, "app", "build", "gen", "samples.bin")

ENTRY = 20
HEADER = 8


def slots(path=POOL):
    """[{name, frames, rate, root, gain}] from the pool header, or [] if absent."""
    if not os.path.exists(path):
        return []
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"FMWV":
        return []
    n = d[5]
    out = []
    for i in range(n):
        o = HEADER + ENTRY * i
        if o + ENTRY > len(d):
            break
        _off, ln, rate, _bits, root, gain = struct.unpack("<IIHBBh", d[o:o + 14])
        name = d[o + 14:o + ENTRY].split(b"\0")[0].decode("ascii", "replace").strip()
        out.append(dict(name=name, frames=ln, rate=rate, root=root, gain=gain))
    return out


def is_demo_pool(path=POOL):
    """True when every slot is one of the synthesised demo slots."""
    s = slots(path)
    return bool(s) and all(x["name"].startswith("DEMO") for x in s)


def describe(path=POOL):
    return ", ".join(f"{x['name']} {x['frames'] / x['rate']:.2f}s@{x['rate']}"
                     for x in slots(path)) or "no pool"


def skip_unless_demo(test_name):
    """Print the reason and return True when the caller should stop."""
    if is_demo_pool():
        return False
    print(f"  SKIP {test_name}: the pool holds real material ({describe()}),")
    print("       not the demo kit these shape checks were written against.")
    print("       Delete app/build/gen/samples.bin to get the demo pool back.")
    return True
