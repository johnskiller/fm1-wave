#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 johnskiller
"""Pack samples into fm1-wave's resource blob.

    python3 tools/pack_samples.py OUT.bin [--slot SPEC]... [--limit KB]

A slot is `NAME:FILE:FMT`, FMT one of the names in RATES below:

    kick:kick.aiff:R22050        22050 Hz, 16-bit, root C4 (60)
    hat:hat.wav:R44100/72        44100 Hz, root note 72 (C5)
    pad:pad.wav:R11025/48        11025 Hz, root note 48 (C3)
    dry:sub.wav:R22050/G0.5      22050 Hz, halve the gain (see below)
    out:take.wav:R22050/G2       22050, double it

Two rules the format enforces and this tool cannot:

1. **The whole blob must fit the region.** It prints the total and refuses to
   write past --limit (276 KB by default: 0x97000..0xDC000 minus the 32-byte
   platform header).
2. **A slot's declared rate is a promise.** Nothing on the device resamples between
   slots; the reader indexes the samples at `rate/44100 * 2^((note-root)/12)` and
   interpolates. Declaring 11025 for material that was 44100 makes it play four
   times slow, which sounds like a broken firmware, not like a wrong flag.

What it does to the audio, all of which is reported:
  - mono (the engine is mono into the master bus in v1)
  - resampled to the declared rate with a windowed-sinc kernel, because a naive
    decimation of 44.1 -> 11.025 folds everything above 5.5 kHz back down as
    aliasing, and a wavetable source is exactly the material that has highs
  - peak-normalised to -1 dBFS, then the slot's gain applied on top
  - a short fade-out at the end if the material ends on a non-zero sample (a chop
    that cuts mid-waveform clicks every time it is triggered)

No dependencies beyond the standard library: AIFF and WAV readers are here, and
the only reason this is not a one-liner is that the format is the deliverable.
"""
import argparse
import math
import struct
import sys

MAGIC = b"FMWV"
VERSION = 1
ENTRY = 20
MAX_SLOTS = 8
HEAD = 8 + ENTRY * MAX_SLOTS
DEFAULT_LIMIT_KB = 276


# ─── reading audio ────────────────────────────────────────────────────────────

def read_wav(path):
    """Mono float samples and a rate. PCM 8/16/24/32 and float32, which covers what
    a phone recorder, a DAW and sox all produce."""
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"RIFF" or d[8:12] != b"WAVE":
        raise SystemExit("%s: not a WAV" % path)
    at = 12
    fmt = None
    data = None
    while at + 8 <= len(d):
        cid = d[at:at + 4]
        n = struct.unpack("<I", d[at + 4:at + 8])[0]
        body = d[at + 8:at + 8 + n]
        if cid == b"fmt ":
            fmt = body
        elif cid == b"data":
            data = body
        at += 8 + n + (n & 1)
    if fmt is None or data is None:
        raise SystemExit("%s: no fmt or data chunk" % path)
    tag, ch, rate = struct.unpack("<HHI", fmt[:8])
    bits = struct.unpack("<H", fmt[14:16])[0]
    if tag == 0xFFFE and len(fmt) >= 40:            # WAVE_FORMAT_EXTENSIBLE
        tag = struct.unpack("<H", fmt[24:26])[0]
    raw = data
    if tag == 3 and bits == 32:
        vals = struct.unpack("<%df" % (len(raw) // 4), raw)
    elif tag == 1 and bits == 16:
        vals = struct.unpack("<%dh" % (len(raw) // 2), raw)
        vals = [v / 32768.0 for v in vals]
    elif tag == 1 and bits == 24:
        vals = [(int.from_bytes(raw[i:i + 3], "little", signed=True)) / 8388608.0
                for i in range(0, len(raw) - 2, 3)]
    elif tag == 1 and bits == 32:
        vals = struct.unpack("<%di" % (len(raw) // 4), raw)
        vals = [v / 2147483648.0 for v in vals]
    elif tag == 1 and bits == 8:
        vals = [(b - 128) / 128.0 for b in raw]
    else:
        raise SystemExit("%s: WAV format %d/%d bit is not read here" % (path, tag, bits))
    if ch > 1:
        vals = [sum(vals[i:i + ch]) / ch for i in range(0, len(vals) - ch + 1, ch)]
    return list(vals), rate


def read_aiff(path):
    """AIFF and AIFC (PCM), which is what a Mac's own tools write."""
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"FORM":
        raise SystemExit("%s: not an AIFF" % path)
    at = 12
    comm = ssnd = None
    while at + 8 <= len(d):
        cid = d[at:at + 4]
        n = struct.unpack(">I", d[at + 4:at + 8])[0]
        body = d[at + 8:at + 8 + n]
        if cid == b"COMM":
            comm = body
        elif cid == b"SSND":
            ssnd = body
        at += 8 + n + (n & 1)
    if comm is None or ssnd is None:
        raise SystemExit("%s: no COMM or SSND" % path)
    ch, nframes, bits = struct.unpack(">HIH", comm[:8])
    rate = aiff_extended(comm[8:18])
    offset = struct.unpack(">I", ssnd[:4])[0]
    raw = ssnd[8 + offset:]
    if bits == 16:
        vals = struct.unpack(">%dh" % (len(raw) // 2), raw)
        vals = [v / 32768.0 for v in vals]
    elif bits == 24:
        vals = [int.from_bytes(raw[i:i + 3], "big", signed=True) / 8388608.0
                for i in range(0, len(raw) - 2, 3)]
    elif bits == 8:
        vals = [(int.from_bytes(raw[i:i + 1], "big", signed=True)) / 128.0
                for i in range(len(raw))]
    else:
        raise SystemExit("%s: %d-bit AIFF is not read here" % (path, bits))
    if ch > 1:
        vals = [sum(vals[i:i + ch]) / ch for i in range(0, len(vals) - ch + 1, ch)]
    return list(vals), rate


def aiff_extended(b):
    """An 80-bit IEEE 754 extended float, which is how AIFF stores a sample rate."""
    expon = struct.unpack(">H", b[:2])[0]
    mant = int.from_bytes(b[2:10], "big")
    if expon == 0 and mant == 0:
        return 0
    sign = -1 if expon & 0x8000 else 1
    expon = (expon & 0x7FFF) - 16383
    return sign * mant * 2.0 ** (expon - 63)


def read_audio(path):
    low = path.lower()
    if low.endswith((".wav", ".wave")):
        return read_wav(path)
    if low.endswith((".aif", ".aiff", ".aifc")):
        return read_aiff(path)
    raise SystemExit("%s: give the file a .wav or .aiff extension" % path)


# ─── resampling ───────────────────────────────────────────────────────────────

def resample(x, src, dst, taps=24):
    """Windowed-sinc resampling, because decimating without a filter folds the high
    band back down. `taps` is the kernel's half-width in output samples."""
    if src == dst or not x:
        return list(x)
    n_out = max(1, int(round(len(x) * float(dst) / src)))
    ratio = float(src) / dst
    out = [0.0] * n_out
    half = taps
    for i in range(n_out):
        t = i * ratio
        c = int(t)
        acc = 0.0
        wsum = 0.0
        for k in range(c - half + 1, c + half + 1):
            if k < 0 or k >= len(x):
                continue
            dt = t - k
            if abs(dt) > half:
                continue
            # Lanczos window: a good compromise between the passband ripple of a
            # rectangular window and the dullness of a Hann one
            s = 1.0 if dt == 0 else math.sin(math.pi * dt) / (math.pi * dt)
            w = 1.0 if dt == 0 else math.sin(math.pi * dt / half) / (math.pi * dt / half)
            c0 = s * w
            acc += x[k] * c0
            wsum += c0
        out[i] = acc / wsum if wsum else 0.0
    return out


# ─── the slot spec ────────────────────────────────────────────────────────────

def parse_slot(spec, limit_peak=True):
    """NAME:FILE:RATE[/ROOT][/Gx]  ->  (name, samples, rate, root, gain)"""
    parts = spec.split(":")
    if len(parts) != 3:
        raise SystemExit("%s: expected NAME:FILE:FMT" % spec)
    name, path, fmt = parts
    opts = fmt.split("/")
    r = opts[0]
    if not r.startswith("R") or not r[1:].isdigit():
        raise SystemExit("%s: the format must start with R<rate>" % spec)
    rate = int(r[1:])
    if rate not in (8000, 11025, 16000, 22050, 32000, 44100, 48000):
        raise SystemExit("%s: %d Hz is not a supported slot rate" % (spec, rate))
    root = 60
    gain = 1.0
    for o in opts[1:]:
        if o.startswith("G") and len(o) > 1:
            gain = float(o[1:])
        elif o.isdigit():
            root = int(o)
        else:
            raise SystemExit("%s: do not understand option %r (root note or G<gain>)" % (spec, o))
    if root < 0 or root > 127:
        raise SystemExit("%s: root %d is outside MIDI" % (spec, root))
    x, src = read_audio(path)
    if not x:
        raise SystemExit("%s: %s is empty" % (spec, path))
    y = resample(x, src, rate)
    peak = max(abs(v) for v in y) or 1.0
    # -1 dBFS, so the 16-bit conversion never clips on rounding
    k = 0.891 / peak if limit_peak else 1.0
    y = [max(-1.0, min(1.0, v * k * gain)) for v in y]
    # a fade at a hard ending: a chop that stops mid-waveform clicks on every hit
    if len(y) > 8 and abs(y[-1]) > 0.005:
        fade = min(64, len(y) // 4)
        for i in range(fade):
            y[len(y) - fade + i] *= 1.0 - (i + 1.0) / fade
    # `k` has already been applied to the samples above, so it must NOT also be
    # written to the gain field: the reader multiplies by that field at playback
    # (wv_sample.c: v * gain / 256), and writing k there normalises twice. Measured
    # on a pad whose peak was 0.162: k = 5.51, so the reader emitted 0.891 * 5.51 =
    # 4.91 -- 4.9x over full scale, and the bus limiter turned the whole thing into
    # a soft-clipped wall. A loud source hides it (k ~ 0.89 is -1 dB) which is why
    # the demo pool, whose gain field is a flat 256, never showed it.
    return name, y, rate, root, gain


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("out")
    ap.add_argument("--slot", action="append", default=[])
    ap.add_argument("--limit", type=int, default=DEFAULT_LIMIT_KB, help="KB, default 276")
    ap.add_argument("--list", action="store_true", help="report the budget and exit")
    a = ap.parse_args()

    limit = a.limit * 1024
    if a.list:
        print("region %d KB, header %d B, %d slots max" % (a.limit, HEAD, MAX_SLOTS))
        return 0
    if not a.slot:
        raise SystemExit("give at least one --slot NAME:FILE:R<rate>[/<root>][/G<gain>]")
    if len(a.slot) > MAX_SLOTS:
        raise SystemExit("%d slots, the format holds %d" % (len(a.slot), MAX_SLOTS))

    slots = []
    for s in a.slot:
        name, y, rate, root, g = parse_slot(s)
        slots.append((name, y, rate, root, g))
        print("  %-6s %6d Hz  root %3d  %8d frames  %6.2f s  gain %5.2f"
              % (name, rate, root, len(y), len(y) / float(rate), g))

    # the layout: header, then the samples back to back
    at = 8 + ENTRY * len(slots)
    entries = []
    payload = bytearray()
    for (name, y, rate, root, g) in slots:
        samples = [max(-32768, min(32767, int(round(v * 32767.0)))) for v in y]
        entries.append((at, len(samples), rate, 16, root, int(round(g * 256.0)), name))
        payload += struct.pack("<%dh" % len(samples), *samples)
        at += len(samples) * 2

    total = 8 + ENTRY * len(slots) + len(payload)
    print("total %d B of %d KB (%d slots, %d B of samples)"
          % (total, a.limit, len(slots), len(payload)))
    if total > limit:
        raise SystemExit("over the region by %d B: shorten a slot or lower its rate" % (total - limit))

    out = bytearray()
    out += MAGIC
    out.append(VERSION)
    out.append(len(slots))
    out += struct.pack("<H", 0)
    for (off, n, rate, bits, root, gain, name) in entries:
        out += struct.pack("<IIHBBh", off, n, rate, bits, root, gain)
        nm = name.encode("ascii", "replace")[:6].ljust(6, b" ")
        out += nm
    out += payload
    with open(a.out, "wb") as f:
        f.write(out)
    print("wrote %s, %d B" % (a.out, len(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
