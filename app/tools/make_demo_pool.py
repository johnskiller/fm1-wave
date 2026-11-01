#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 johnskiller
"""Make the sample pool the firmware ships with.

    python3 tools/make_demo_pool.py build/gen/samples.bin

This exists so that SOURCE=SMPL does something on the first flash instead of
showing an empty page. It is *demo material*, synthesised here, not a recording:
one kit of four transients (for the slice path to have something to chop) and one
sustained tone (for the pitched path). Replace it with pack_samples.py and real
material; nothing in the reader knows the difference.

The pool is only written when it is missing, so a pool packed from real material
is not overwritten by the next build. Delete the file to get the demo back.
"""
import math
import os
import struct
import sys

RATE = 11025          # the wavetable-oriented default (docs/spec.md)
BITS = 16
ENTRY = 20            # wv_sample.h: WV_SMP_ENTRY


def env(n, attack, decay, curve=3.0):
    """A one-shot envelope: linear attack, exponential decay to silence.

    `curve` is the number of e-foldings over the whole length, so the tail is defined
    against the segment's duration and not against a fixed time constant: a long hit
    decays slowly, a short one quickly, and both end at zero. The first version used
    a fixed time constant, so a 0.21 s hit was 40 dB down by 40 ms and the chops
    measured 19 ms long — the engine was right and the material was wrong.
    """
    out = []
    a = max(1, int(attack * RATE))
    d = max(1, n - a)
    for i in range(n):
        if i < a:
            out.append(i / a)
        else:
            x = (i - a) / d
            # exp curve shaped by the final (1-x) so it reaches exactly 0, and the
            # e-folding count scaled so the decay fills the segment
            out.append(math.exp(-curve * x * x) * (1.0 - x))
    return out


def noise(seed):
    """xorshift: reproducible noise without importing random's state machine."""
    s = seed & 0xFFFFFFFF

    def nxt():
        nonlocal s
        s ^= (s << 13) & 0xFFFFFFFF
        s ^= s >> 17
        s ^= (s << 5) & 0xFFFFFFFF
        return (s / 0xFFFFFFFF) * 2.0 - 1.0
    return nxt


def hit(n, f0, f1, decay, seed, click=0.0):
    """A drum-ish transient: a pitch-swept body plus a noise click."""
    rnd = noise(seed)
    e = env(n, 0.0005, 0.0, decay)
    out = []
    ph = 0.0
    for i in range(n):
        t = i / n
        f = f0 * (f1 / f0) ** t
        ph += 2.0 * math.pi * f / RATE
        v = math.sin(ph) * e[i]
        if click > 0.0 and i < n * 0.02:
            v += rnd() * click * (1.0 - i / (n * 0.02))
        out.append(v)
    return out


def tone(n, f):
    """A genuinely sustained, mildly harmonic tone.

    It must NOT decay, and that is a change: this used to fade to 5 % over its 1.2 s,
    which was fine while the tone was only heard as a pitched sample (the VCA decides
    when it ends) but wrong for the granular path, where the material's own envelope
    is part of the instrument. A cloud over a decaying tone gets quieter as its
    playhead walks into the tail, so sweeping SPRAY made the sound *fade* as well as
    scatter, and it looked like a bug in the cloud. It was: the cloud was faithful to
    material that was fading. Sustained material is what grain parameters can be
    judged on.

    The attack and the release are short but present: a tone that starts and ends at
    full amplitude is a click, and a loop of it ticks once per pass.
    """
    a = int(0.01 * RATE)
    r = int(0.01 * RATE)
    out = []
    for i in range(n):
        t = i / RATE
        v = math.sin(2 * math.pi * f * t)
        v += 0.35 * math.sin(2 * math.pi * 2 * f * t)
        v += 0.15 * math.sin(2 * math.pi * 3 * f * t)
        if i < a:
            g = i / a
        elif i > n - r:
            g = (n - i) / r
        else:
            g = 1.0
        out.append(v * g * 0.6)
    return out


def kit():
    """Four hits with silence between them, the way a chop-able bar looks.

    The gaps matter: the slice detector splits on silence, so a kit without gaps
    would come back as one slice and the demo would not demonstrate slicing.
    """
    seg = int(0.18 * RATE)
    gap = int(0.03 * RATE)
    out = []
    # `dec` is the number of e-foldings across the whole segment: 2-4 is a drum with
    # a body, 8+ is a tick. The kit needs bodies, otherwise the chop test measures
    # the envelope rather than the reader.
    for i, (f0, f1, dec) in enumerate([(180, 55, 2.5), (320, 90, 3.0),
                                       (900, 400, 4.5), (2400, 2000, 6.0)]):
        out += hit(seg, f0, f1, dec, 0x1234 + i * 0x9E37, click=0.5 - i * 0.1)
        out += [0.0] * gap
    return out


def normalise(x, peak=0.85):
    m = max(abs(v) for v in x) or 1.0
    return [v * peak / m for v in x]


def to_pcm16(x):
    return [max(-32768, min(32767, int(round(v * 32767.0)))) for v in x]


def main(out_path):
    if os.path.exists(out_path):
        print(f"demo pool: {out_path} exists, left alone")
        return 0

    slots = [
        ("DEMO KIT", kit(), 53),          # C3-ish: the chops land on the low keys
        # 2 s: long enough that a cloud's playhead has material to walk through, and
        # still only 44 KB at 11.025 kHz / 16-bit.
        ("DEMO TONE", tone(int(2.0 * RATE), 220.0), 57),
    ]

    body = bytearray()
    entries = []
    at = 8 + ENTRY * len(slots)
    for name, samples, root in slots:
        pcm = to_pcm16(normalise(samples))
        data = struct.pack(f"<{len(pcm)}h", *pcm)
        entries.append((at, len(pcm), name, root))
        body += data
        at += len(data)

    head = bytearray(b"FMWV")
    head += bytes([1, len(slots), 0, 0])
    for off, n, name, root in entries:
        # 20 bytes: offset, len, rate, bits, root, gain, name(6). The layout is
        # wv_sample.h's WV_SMP_ENTRY; writing a shorter entry is how the first
        # version of this file produced a pool the parser rejected outright.
        head += struct.pack("<IIHBBh", off, n, RATE, BITS, root, 256)
        head += name.encode("ascii")[:6].ljust(6, b"\0")

    os.makedirs(os.path.dirname(out_path) or ".", exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(bytes(head) + bytes(body))

    total = len(head) + len(body)
    print(f"demo pool: {len(slots)} slots, {total} B "
          f"({total / 1024:.1f} KiB of the 276 KiB region)")
    for off, n, name, root in entries:
        print(f"  {name:9s} root {root}  {n:6d} samples  {n / RATE:.2f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "build/gen/samples.bin"))
