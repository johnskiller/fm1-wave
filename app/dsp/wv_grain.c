/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* The grain cloud. See wv_grain.h for why the window is sin² and why the overlap
 * is exactly 2.
 *
 * No platform, no allocation, no globals: this unit is compiled into the firmware
 * (and into a job that may run on the second core) as well as into the host tests,
 * so it may assume nothing about where it runs.
 *
 * ─── the spawn clock ─────────────────────────────────────────────────────────
 *
 * Spawns are scheduled by a countdown in whole output samples, not by a float
 * accumulator compared against a float hop. The accumulator works but drifts by one
 * sample every few thousand grains whenever the hop is not exactly representable,
 * and a cloud that spawns a sample early now and then is a cloud with a slow wobble
 * in it. A countdown cannot drift.
 *
 * ─── why a note starts with two grains ───────────────────────────────────────
 *
 * The tiling that makes SPRAY=0 an ordinary sample player is: grains every `hop`
 * samples, each at a phase `hop/dt` apart. With an overlap of 2 that is half a grain
 * apart, so sin² + cos² = 1 always holds.
 *
 * A note that starts with a single grain at phase 0 therefore starts at window 0 —
 * silence — and reaches full level half a grain later. That is a fade-in on every
 * drum hit, and it is wrong. The fix is to place the grain that *would* have been
 * spawned a hop before the note existed: same position (it has already read its
 * material by then), phase 0.5, so it is at full window on the first sample while
 * the grain that starts at phase 0 is at zero window. Their sum is the source sample
 * on sample one, and it stays that way for the life of the note.
 *
 * That is why wv_grains_start leaves `started` clear and the first call places two.
 */

#include <stdint.h>

#include "fastmath.h"
#include "wv_grain.h"

float wv_grain_len(uint32_t size127)
{
    /* 10 ms .. 200 ms exponential: 10 * 20^(s/127), then in output samples. */
    float t = (float)(size127 > 127u ? 127u : size127) * (1.0f / 127.0f);
    float ms = WV_GRAIN_MIN_MS * fm_exp2f(t * 4.321928f);      /* log2(200/10) */
    return ms * (WV_RATE / 1000.0f);
}

/* The hop in whole output samples. A hop under one sample means a grain every
 * sample, which is what the countdown degrades to. */
static uint32_t hop_samples(float hop)
{
    return hop > 1.0f ? (uint32_t)(hop + 0.5f) : 0u;
}

/* A deterministic per-voice noise: xorshift32, so a test reproduces a cloud exactly
 * and two voices never scatter the same way. */
static float rnd01(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return (float)(x >> 8) * (1.0f / 16777216.0f);              /* 0..1 */
}

/* Linear read. Past the end it is SILENCE, not the last sample holding.
 *
 * That is a correction, not a preference. Holding the last sample makes a grain that
 * ran past the end read a constant, and a windowed constant summed over two grains
 * is a plain DC offset — so a granular one-shot, instead of dying when the material
 * ran out, sat on the material's final sample value at full level. Measured on the
 * demo tone that was a note that stopped changing pitch and just sat there.
 *
 * Silence is the honest reading of "there is no material here", and with the window
 * still running it fades rather than clicks. The start position is still clamped (in
 * spawn), which is what stops a scattered grain from bursting in from the beginning.
 */
static float read_at(const wv_sample *smp, float pos)
{
    uint32_t i0, i1;
    float frac, a, b, g = (float)smp->gain * (1.0f / 256.0f);

    if (pos < 0.0f || pos >= (float)smp->len)
        return 0.0f;
    if (pos >= (float)(smp->len - 1u)) {
        i0 = smp->len - 1u;
        i1 = i0;
    } else {
        i0 = (uint32_t)pos;
        i1 = (i0 + 1u < smp->len) ? i0 + 1u : i0;
    }
    frac = (i1 > i0) ? pos - (float)i0 : 0.0f;
    a = (float)smp->data[i0] * (1.0f / 32768.0f);
    b = (float)smp->data[i1] * (1.0f / 32768.0f);
    return (a + (b - a) * frac) * g;
}

void wv_grains_start(wv_grains *gr, const wv_sample *smp, uint32_t start, uint32_t end,
                     float rate, int loop, uint32_t seed)
{
    uint32_t i;

    if (end == 0 || end > smp->len)
        end = smp->len;
    if (start >= end)
        start = 0;
    if (end - start < 2u) {
        start = 0;
        end = smp->len;
        loop = 0;
    }

    for (i = 0; i < WV_GRAINS; i++) {
        gr->g[i].on = 0;
        gr->g[i].t = 1.0f;
    }
    gr->play = (float)start;
    gr->loop_start = (float)start;
    gr->loop_end_k = (float)(end - 1u);
    gr->loop_end = gr->loop_end_k;
    gr->rate = rate;
    gr->left = 0;
    gr->next = 0;
    gr->seed = seed ? seed : 0x9E3779B9u;
    gr->started = 0;
    gr->loop = (uint8_t)(loop ? 1 : 0);
}

void wv_grains_loop(wv_grains *gr, uint32_t start, uint32_t end)
{
    float span;

    if (end <= start + 1u)
        return;
    gr->loop_start = (float)start;
    gr->loop_end_k = (float)(end - 1u);
    gr->loop_end = gr->loop_end_k;
    gr->loop = 1;

    span = gr->loop_end - gr->loop_start;
    if (span <= 0.0f)
        return;
    /* bring the playhead into the range without restarting the cloud: a note that
     * resumes outside its loop is a note reading the wrong material */
    while (gr->play < gr->loop_start)
        gr->play += span;
    while (gr->play > gr->loop_end)
        gr->play -= span;
}

/* Spawn one grain at the playhead.
 *
 * `t0` is where in its own window the grain begins and `back` is how much material
 * it is rewound by, in the slot's samples — a grain that starts its window half way
 * in must also read from half a grain earlier, or it is the same sample twice.
 *
 * A grain never reads outside the slot: clamping (not wrapping) is deliberate — wrapping turns a start near the end
 * of the material into a burst from the beginning, which on a drum kit is a wrong
 * hit at full volume, while clamping is the material's own edge.
 */
static void spawn(wv_grains *gr, const wv_sample *smp, float len_out, float spray,
                  float t0, float back)
{
    wv_grain *g = &gr->g[gr->next];
    float pos = gr->play - back;

    /* SPRAY scatters the start position, 0..127 from the knob mapped to a fraction
     * of the material. At 0 the grain starts exactly where the playhead is, which is
     * the tiling case; at 127 a grain can start anywhere in the slot.
     *
     * The normalisation is here and not at the call site because it is part of this
     * function's contract: passing a raw 0..127 without it scatters by 127 * len / 2,
     * which is far outside the slot, so every grain clamps to the same two edges and
     * two different seeds produce identical clouds. */
    if (spray > 0.0f) {
        float amt = fm_minf(spray, 127.0f) * (1.0f / 127.0f);
        float d = (rnd01(&gr->seed) * 2.0f - 1.0f) * amt * (float)smp->len * 0.5f;
        pos += d;
    }

    if (pos < 0.0f)
        pos = 0.0f;
    if (pos > (float)(smp->len - 1u))
        pos = (float)(smp->len - 1u);

    g->pos = pos;
    g->inc = gr->rate;
    g->t = t0;
    g->dt = len_out > 1.0f ? 1.0f / len_out : 1.0f;
    g->on = 1;

    gr->next = (gr->next + 1u) % WV_GRAINS;
}

float wv_grains_next(wv_grains *gr, const wv_sample *smp, float len_out, float spray)
{
    float sum = 0.0f;
    float hop, wrap;
    uint32_t i;

    if (len_out < 2.0f)
        len_out = 2.0f;

    /* Where the playhead wraps: the material's end, less one window.
     *
     * A grain spawned at the playhead reads FORWARD for `len_out * rate` samples of
     * material. If the playhead wraps at the slot's last sample, a grain spawned
     * anywhere in the last window-length of material reads past the end, where
     * read_at() returns silence, and part of its window is a hole. The window is
     * still running, so it is not a click -- it is a level dip. Measured on the demo
     * tone (2.0 s slot, one wrap every 2.0 s) the dip at the wrap was 0.7 dB at
     * GRAIN 16, 3.5 dB at GRAIN 64 and 11.4 dB at GRAIN 127: a periodic thump whose
     * depth grows with the grain length, which is the parameter the sound is made of.
     *
     * So the wrap point backs off by one window, computed from the length in force
     * rather than stored, because GRAIN SIZE is a live control: a held cloud re-tiles
     * when the knob moves, and the wrap has to move with it. The cost is the last
     * `len_out * rate` samples of material, which at the longest grain and a 3 s slot
     * is 6.7 % of it -- and that is material no grain could have read anyway.
     *
     * The floor keeps a short slot usable: a window longer than the slot degrades to
     * a wrap at the start instead of a zero-length loop. */
    wrap = gr->loop_end_k;
    if (gr->loop) {
        float back = wrap - len_out * gr->rate;
        /* The range must keep a nonzero span. A grain window longer than the whole
         * material (200 ms over a 0.25 s slot, in the test) backs off past the start,
         * and a zero-length span freezes the playhead -- the cloud does not stutter, it
         * goes silent, and it never comes back even after GRAIN SIZE is turned down
         * again. So the start is the floor, plus one sample: the loop keeps going and
         * reads what material there is, holes and all, because a grain longer than the
         * slot cannot be filled and pretending otherwise is what froze it. */
        if (back > gr->loop_start)
            wrap = back;
        else
            wrap = gr->loop_start + 1.0f;
        gr->loop_end = wrap;
    }

    /* Grain length / overlap: with an overlap of 2 the next grain starts when the
     * previous one is half done, which is the sin²/cos² pair. */
    hop = len_out / (float)WV_GRAIN_OVERLAP;

    if (!gr->started) {
        gr->started = 1;
        /* The grain that would have started a hop ago, and the one starting now.
         * See the header comment: one grain alone would fade the note in. */
        spawn(gr, smp, len_out, spray, 1.0f - 1.0f / (float)WV_GRAIN_OVERLAP,
              hop * gr->rate);
        spawn(gr, smp, len_out, spray, 0.0f, 0.0f);
        gr->left = hop_samples(hop);
    } else if (gr->left == 0u) {
        spawn(gr, smp, len_out, spray, 0.0f, 0.0f);
        gr->left = hop_samples(hop);
    }

    for (i = 0; i < WV_GRAINS; i++) {
        wv_grain *g = &gr->g[i];
        float w;

        if (!g->on)
            continue;

        /* sin²(pi*t): the window whose halves are the same curve, so any two grains
         * half a grain apart sum to 1 */
        w = fm_sin_turns(g->t * 0.5f);
        sum += read_at(smp, g->pos) * w * w;

        g->pos += g->inc;
        g->t += g->dt;
        if (g->t >= 1.0f)
            g->on = 0;
    }

    if (gr->left > 0u)
        gr->left--;
    gr->play += gr->rate;
    /* Wrap the playhead at `wrap` (the material's end less one window), so a cloud
     * over a finite sample keeps a cloud without ever spawning a grain that has no
     * material left to read. */
    if (gr->loop) {
        float span = wrap - gr->loop_start;
        if (span > 0.0f) {
            while (gr->play > wrap)
                gr->play -= span;
        }
    }

    return sum;
}

uint32_t wv_grains_on(const wv_grains *gr)
{
    uint32_t i, n = 0;
    for (i = 0; i < WV_GRAINS; i++)
        if (gr->g[i].on)
            n++;
    return n;
}
