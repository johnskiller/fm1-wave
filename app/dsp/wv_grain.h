/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* Grains: a sample read as a cloud instead of as a tape.
 *
 * A grain is a short windowed read of the material at its own position, its own
 * rate and its own envelope. Overlapping grains make a texture out of any sound;
 * a single grain per voice is just a sample player with a window on it.
 *
 * ─── why the window is sin² and why the overlap is exactly 2 ─────────────────
 *
 * The obvious choice is a window lookup table. The better one is sin²(pi*t), for a
 * reason that is arithmetic rather than taste:
 *
 *     sin²(pi*t) + sin²(pi*(t + 0.5)) = sin² + cos² = 1
 *
 * So two grains half a grain apart sum to *exactly* one, at every sample, with no
 * approximation and no table. That is the condition that makes granular play back
 * at unity gain instead of breathing: any other overlap, or any peak-normalised
 * table, leaves a ripple that is audible as a slow tremolo when the grain rate
 * beats against the pitch.
 *
 * It also gives the property the spec asks for — SPRAY at 0 behaves like an
 * ordinary sample player — as a consequence rather than as a tuning: when the
 * grains tile, every live grain reads the *same* source sample at the same instant,
 * so the sum of their windows times that one sample is exactly that sample.
 *
 * One honest caveat: at the very start of a note there is only one grain, so the
 * first half grain is a fade-in (sin² rising 0 → 1) rather than unity. That is a
 * deliberate fade and not a defect — a cloud that reached full level on its first
 * sample would begin with a step — but it means the SPRAY=0 equivalence is exact
 * only once the cloud is steady, which is what the test checks.
 *
 * ─── what a grain reads ──────────────────────────────────────────────────────
 *
 * Position is in the slot's own samples, like wv_reader, and advances by `rate`
 * per output sample. `rate` is the same wv_smp_inc the sample path uses, so a
 * granular voice is in tune with the chop path and with the synth.
 *
 * Grain length is in *output* samples (how long you hear it) while the material a
 * grain consumes is length * rate (how much of the slot it uses). Both are needed:
 * a grain at 2x consumes half as much material as it takes time.
 */

#ifndef WV_GRAIN_H
#define WV_GRAIN_H

#include <stdint.h>

#include "wv_sample.h"

/* Grain slots per voice. An overlap of 2 means exactly two grains sound at any
 * instant, so two would be enough; four leaves room for a spawn that lands in the
 * same sample as another grain ends, and for a future overlap other than 2. */
#define WV_GRAINS 4u

/* Grains overlapping at any instant. 2 is not a taste: see the header comment — it
 * is the value at which sin² windows sum to exactly 1. Any other value needs the
 * gain compensated by the actual window sum, which v1 does not do. */
#define WV_GRAIN_OVERLAP 2u

/* Grain length, from GRAIN SIZE 0..127: 10 ms .. 200 ms, exponential. A linear
 * millisecond range spends most of its travel in the long grains, where the
 * difference between one long grain and the next is least audible. */
#define WV_GRAIN_MIN_MS 10.0f
#define WV_GRAIN_MAX_MS 200.0f

/* The engine's rate. wv_grain_len needs it to turn milliseconds into samples, and
 * the DSP has no other reason to know it (the caller holds the block's rate). */
#define WV_RATE 44100.0f

typedef struct {
    float pos;      /* in the slot's samples, fractional */
    float inc;      /* advance per output sample */
    float t;        /* 0..1 through the grain */
    float dt;       /* 1 / length, per output sample */
    uint8_t on;
    uint8_t pad[3];
} wv_grain;

typedef struct {
    wv_grain g[WV_GRAINS];
    float    play;      /* the playhead grains are spawned at, in the slot's samples */
    float    rate;      /* the source advance per output sample */
    float    loop_start, loop_end;   /* the range the playhead wraps inside */
    uint32_t left;      /* output samples until the next spawn (0 = this sample) */
    uint32_t next;      /* round-robin slot */
    uint32_t seed;      /* xorshift state for SPRAY; per voice, so two voices on the
                         * same note do not scatter identically */
    uint8_t  started;   /* the first grain has been placed */
    uint8_t  loop;      /* the playhead wraps at loop_end instead of running off */
    uint8_t  pad[2];
} wv_grains;

/* The grain length in output samples for a GRAIN SIZE of 0..127. */
float wv_grain_len(uint32_t size127);

/* Start the cloud over [start, end) of the slot, reading at `rate`.
 *
 * `loop` is what makes this an instrument rather than a one-shot: a cloud whose
 * playhead runs off the end of the material dies the moment it gets there, which on
 * the demo tone (1.2 s) meant the sound faded to silence after 1.2 s of holding a
 * key — and it faded exactly when SPRAY was turned up, because a scattered grain that
 * clamps at the last sample is silence, while an unscattered one keeps reading before
 * the end. Wrapped, a cloud over a 1.2 s sample sustains for as long as the key is
 * down, which is the behaviour a granular instrument has to have.
 *
 * With `loop` clear the playhead clamps and the cloud decays as the material runs
 * out: that is what a granular *one-shot* is, and it is still reachable. */
void wv_grains_start(wv_grains *gr, const wv_sample *smp, uint32_t start, uint32_t end,
                     float rate, int loop, uint32_t seed);

/* One output sample of the cloud. `len_out` is the grain length in output samples
 * (wv_grain_len), `spray` 0..127 scatters grain starts. */
float wv_grains_next(wv_grains *gr, const wv_sample *smp, float len_out, float spray);

/* How many grains are sounding (the UI and the tests). */
uint32_t wv_grains_on(const wv_grains *gr);

#endif /* WV_GRAIN_H */
