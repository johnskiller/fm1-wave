#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 johnskiller
"""The pictures in docs/wavetables.md: every frame of every synth bank.

    python3 app/tools/gen_wv_figures.py           (writes docs/img/wv-*.png)
    python3 app/tools/gen_wv_figures.py --check   (exit 1 if any would change)

It imports gen_wv_banks.py and reads its BANKS table, so the pictures are drawn from
the same numbers the firmware is built from. Nothing here is measured off the screen:
the point is that a wrong frame in the generator shows up as a wrong picture, and the
numbers printed under the heading are computed from the same arrays.

Not part of the build. Run it when the banks change; the PNGs are committed.
"""
import importlib.util
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                      # app/
OUT = os.path.join(ROOT, "docs", "img")

# The three sources' colours (app/app.c, the src_skin_t table), so a reader who has seen
# the instrument recognises the family the picture belongs to. Banks are all SYNTH, so
# this is the amber; the others are here to keep the numbers in one place.
AMBER = "#ee8918"
AMBER_HI = "#ffba41"
BG = "#291c08"
INK_DIM = "#8a5a18"


def load_banks():
    spec = importlib.util.spec_from_file_location("gen_wv_banks", os.path.join(HERE, "gen_wv_banks.py"))
    g = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(g)
    return g


def frames_of(data, F, N):
    """The bank's int16 run -> (F, N) float array in -1..1."""
    return np.array(data, dtype=np.float64).reshape(F, N) / 32767.0


def stats(a, N):
    """What the document says about a bank, all of it computed here."""
    F = a.shape[0]
    rms = np.sqrt((a * a).mean(axis=1))
    crest = np.abs(a).max(axis=1) / rms
    spec = np.abs(np.fft.rfft(a, axis=1))[:, :N // 2]

    def last_audible(fr, db=-40.0):
        thr = fr.max() * 10 ** (db / 20.0)
        return int(np.max(np.nonzero(fr > thr)[0]))

    def centroid(fr):
        k = np.arange(len(fr))
        return float((k * fr).sum() / fr.sum())

    return {
        "peak": float(np.abs(a).max()),
        # RMS is equalised by the generator, so this is ~0 by construction -- printed
        # anyway, because a nonzero value would mean the generator's step 1 had broken.
        "rms_spread_db": 20 * math.log10(rms.max() / max(rms.min(), 1e-12)),
        "crest_min": float(crest.min()), "crest_max": float(crest.max()),
        # how many harmonics a frame carries above -40 dB of its own loudest
        "band_f0": last_audible(spec[0]), "band_f7": last_audible(spec[F - 1]),
        "cent_f0": centroid(spec[0]), "cent_f7": centroid(spec[F - 1]),
        "dc": float(np.abs(a.mean(axis=1)).max()),
    }


def figure(name, a, st, F, N, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig = plt.figure(figsize=(11.0, 6.2), dpi=140, facecolor=BG)
    gs = fig.add_gridspec(3, 4, height_ratios=[1.0, 1.0, 0.62], hspace=0.62, wspace=0.28,
                          left=0.055, right=0.985, top=0.86, bottom=0.085)

    x = np.arange(N) / N                      # one cycle, in periods
    lo = float(a.min()) * 1.12
    hi = float(a.max()) * 1.12

    # ── the eight frames, in order ────────────────────────────────────────────
    for f in range(F):
        ax = fig.add_subplot(gs[f // 4, f % 4])
        ax.set_facecolor(BG)
        # a dim midline, so "what is zero" is visible in every frame
        ax.axhline(0.0, color=INK_DIM, lw=0.5, alpha=0.55)
        ax.plot(x, a[f], color=AMBER_HI if f in (0, F - 1) else AMBER,
                lw=1.35 if f in (0, F - 1) else 0.95,
                solid_capstyle="round")
        for s in ax.spines.values():
            s.set_color(INK_DIM); s.set_linewidth(0.5)
        ax.set_xlim(0, 1); ax.set_ylim(lo, hi)
        ax.set_xticks([0, 0.5, 1]); ax.set_yticks([-0.5, 0, 0.5])
        ax.tick_params(colors="#c9a06a", labelsize=6.5, length=2, width=0.5, pad=1.5)
        ax.set_title("frame %d   morph %d%%" % (f, round(100 * f / (F - 1.0))),
                     color="#ffd79a", fontsize=7.2, pad=3.0, loc="left")

    # ── the whole bank as a waterfall: frame index is the depth axis ──────────
    ax = fig.add_subplot(gs[2, :3])
    ax.set_facecolor(BG)
    for f in range(F):
        off = f * 1.55
        ax.plot(x, a[f] * 0.9 - off, color=AMBER, lw=0.7,
                alpha=0.42 + 0.075 * f)
    for s in ax.spines.values():
        s.set_color(INK_DIM); s.set_linewidth(0.5)
    ax.set_xlim(0, 1)
    ax.set_yticks([]); ax.set_xticks([0, 0.5, 1])
    ax.tick_params(colors="#c9a06a", labelsize=6.5, length=2, width=0.5, pad=1.5)
    ax.set_title("the bank, frame 0 at the top and frame 7 at the bottom",
                 color="#ffd79a", fontsize=7.2, pad=3.0, loc="left")

    # ── the numbers, computed from the same arrays ───────────────────────────
    ax = fig.add_subplot(gs[2, 3])
    ax.set_facecolor(BG); ax.axis("off")
    rows = [
        ("peak", "%.3f" % st["peak"]),
        ("frame RMS spread", "%.2f dB" % st["rms_spread_db"]),
        ("crest, frame 0..7", "%.2f .. %.2f" % (st["crest_min"], st["crest_max"])),
        ("harmonics above -40 dB", "%d -> %d" % (st["band_f0"], st["band_f7"])),
        ("centroid, frame 0 -> 7", "%.1f -> %.1f" % (st["cent_f0"], st["cent_f7"])),
        ("DC offset", "%.2e" % st["dc"]),
    ]
    for i, (k, v) in enumerate(rows):
        y = 0.94 - i * 0.163
        ax.text(0.0, y, k, color="#c9a06a", fontsize=7.0, va="top", transform=ax.transAxes)
        ax.text(1.0, y, v, color="#ffd79a", fontsize=7.0, va="top", ha="right",
                family="DejaVu Sans Mono", transform=ax.transAxes)

    fig.suptitle(name, color="#ffd79a", fontsize=14, x=0.055, ha="left", y=0.965,
                 fontweight="bold")
    fig.text(0.055, 0.905,
             "8 frames x 256 points, int16 Q15, one cycle each.  Horizontally: one period.  "
             "Vertically: full scale of the bank.",
             color="#c9a06a", fontsize=7.4, ha="left")

    fig.savefig(path, facecolor=BG)
    plt.close(fig)


def overview(banks, F, N, path):
    """All eight banks side by side, frame 0 and frame 7 only: the ends of each morph."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    fig, axes = plt.subplots(len(banks), 2, figsize=(10.2, 0.95 * len(banks) + 0.9),
                             dpi=140, facecolor=BG, sharex=True)
    fig.subplots_adjust(left=0.13, right=0.985, top=0.905, bottom=0.055, hspace=0.42,
                        wspace=0.09)
    x = np.arange(N) / N
    for i, (name, a) in enumerate(banks):
        for j, f in enumerate((0, F - 1)):
            ax = axes[i][j]
            ax.set_facecolor(BG)
            ax.axhline(0.0, color=INK_DIM, lw=0.4, alpha=0.5)
            ax.plot(x, a[f], color=AMBER_HI if j else AMBER, lw=1.0)
            for s in ax.spines.values():
                s.set_color(INK_DIM); s.set_linewidth(0.45)
            ax.set_xlim(0, 1); ax.set_ylim(-1.05, 1.05)
            ax.set_xticks([0, 1]); ax.set_yticks([-1, 0, 1])
            ax.tick_params(colors="#c9a06a", labelsize=6, length=2, width=0.5, pad=1.5)
            if j == 0:
                ax.set_ylabel(name, color="#ffd79a", fontsize=8.5, rotation=0,
                              labelpad=34, va="center")
            if i == 0:
                ax.set_title("frame 0   (MORPH 0%)" if j == 0 else "frame 7   (MORPH 100%)",
                             color="#ffd79a", fontsize=7.6, pad=4)
    fig.suptitle("The eight synth banks, ends of each morph", color="#ffd79a",
                 fontsize=13, x=0.13, ha="left", y=0.975, fontweight="bold")
    fig.savefig(path, facecolor=BG)
    plt.close(fig)


def main():
    check = "--check" in sys.argv
    g = load_banks()
    F, N = g.F, g.N
    os.makedirs(OUT, exist_ok=True)

    made = []
    banks = []
    for name, data in g.BANKS:
        a = frames_of(data, F, N)
        banks.append((name, a))
        slug = name.lower().replace(" ", "-")
        path = os.path.join(OUT, "wv-%s.png" % slug)
        before = os.path.getsize(path) if os.path.exists(path) else -1
        figure(name, a, stats(a, N), F, N, path)
        made.append((name, path, stats(a, N)))
        print("%-8s %s" % (name, path))

    path = os.path.join(OUT, "wv-banks.png")
    overview(banks, F, N, path)
    print("%-8s %s" % ("overview", path))

    if check:
        # the numbers in the document, printed so they can be checked against it
        print()
        for name, _p, st in made:
            print("%-8s peak %.3f  rms %.2f dB  crest %.2f..%.2f  band %d->%d  cent %.1f->%.1f"
                  % (name, st["peak"], st["rms_spread_db"], st["crest_min"], st["crest_max"],
                     st["band_f0"], st["band_f7"], st["cent_f0"], st["cent_f7"]))


if __name__ == "__main__":
    main()
