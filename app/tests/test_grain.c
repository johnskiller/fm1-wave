/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* Grain cloud tests under plain cc.
 *   sh app/tests/run_dsp.sh    (runs all the test binaries)
 *
 * The properties worth testing are not "does it produce numbers" but the four
 * invariants the design rests on:
 *
 *   1. SPRAY=0 is a sample player. Not approximately: the tiling identity makes the
 *      cloud reproduce the material, so a chop played granular at zero spray has to
 *      match the chop played as a chop.
 *   2. Level does not breathe. A cloud whose window sum ripples has a tremolo at the
 *      grain rate; the sum has to be flat over the whole note, not just on average.
 *   3. It does not run off the material. A grain clamped at the edge is the edge;
 *      one that wraps is a wrong hit at full volume.
 *   4. SPRAY actually scatters, and scatters differently per voice.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fastmath.h"
#include "wv_grain.h"
#include "wv_sample.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int fails, checks;

static void ck(int cond, const char *what)
{
    checks++;
    if (!cond) {
        fails++;
        printf("  FAIL %s\n", what);
    }
}

/* ─── material ────────────────────────────────────────────────────────────── */

#define NMAT 32768
static int16_t mat[NMAT];

/* A ramp is the sharpest test material there is: every output sample is different,
 * so any misalignment, double-read or lost sample shows up immediately, where a sine
 * would hide it behind its own symmetry. */
static void fill_ramp(void)
{
    int i;
    for (i = 0; i < NMAT; i++)
        mat[i] = (int16_t)((i % 4096) * 8 - 16384);
}

/* A DC test material: every sample identical, so "the window sum" can be measured
 * directly as the output value. This is how the no-breathing check works. */
static void fill_dc(int16_t v)
{
    int i;
    for (i = 0; i < NMAT; i++)
        mat[i] = v;
}

static wv_sample smp;

static void make_sample(int n, float gain)
{
    smp.data = mat;
    smp.len = (uint32_t)n;
    smp.rate = 44100;
    smp.bits = 16;
    smp.root = 60;
    smp.gain = (uint16_t)(gain * 256.0f);
}

/* ─── 1. the window identity ──────────────────────────────────────────────── */

static void test_window_tiles(void)
{
    printf("window:\n");
    /* sin²(pi*t) + sin²(pi*(t+0.5)) == 1 for every t. This is the whole reason the
     * overlap is 2, so it is checked directly rather than inferred from audio. */
    int i;
    float worst = 0.0f;
    for (i = 0; i <= 1000; i++) {
        float t = (float)i * 0.001f;
        float a = fm_sin_turns(t * 0.5f);
        float b = fm_sin_turns((t + 0.5f) * 0.5f);
        float e = fabsf(a * a + b * b - 1.0f);
        if (e > worst)
            worst = e;
    }
    ck(worst < 0.002f, "sin2 pair sums to 1 (< 0.2%)");
    printf("  worst deviation %.5f\n", worst);
}

/* ─── 2. SPRAY=0 is a sample player ───────────────────────────────────────── */

static void test_spray0_is_a_sample_player(void)
{
    wv_grains gr;
    int i, worst_at = -1;
    float worst = 0.0f;
    int n = 2000;

    printf("spray=0 as a sample player:\n");
    fill_ramp();
    make_sample(n, 1.0f);

    /* A long grain (200 ms = 8820 samples) so the whole 2000-sample window sits well
     * inside the first grain pair and the tiling is in its steady state. */
    wv_grains_start(&gr, &smp, 0, 0, 1.0f, 0, 12345u);

    for (i = 0; i < n; i++) {
        /* Beyond the first few samples the two grains are the tiling pair. */
        float got = wv_grains_next(&gr, &smp, 8820.0f, 0.0f);
        if (i > 4) {
            float want = (float)mat[i] * (1.0f / 32768.0f) * (float)smp.gain * (1.0f / 256.0f);
            float e = fabsf(got - want);
            if (e > worst) {
                worst = e;
                worst_at = i;
            }
        }
    }
    ck(worst < 0.002f, "output equals the material (< 0.2%)");
    printf("  worst error %.5f at sample %d\n", worst, worst_at);

    /* And it is not silent at the start either: the two-grain start is what stops the
     * fade-in, so sample 0 has to be at level. */
    wv_grains_start(&gr, &smp, 0, 0, 1.0f, 0, 12345u);
    {
        float first = wv_grains_next(&gr, &smp, 8820.0f, 0.0f);
        float want = (float)mat[0] * (1.0f / 32768.0f);
        ck(fabsf(first - want) < 0.002f, "first sample is at level, no fade-in");
        ck(wv_grains_on(&gr) == 2u, "a note starts with exactly two grains");
    }
}

/* ─── 3. level does not breathe ───────────────────────────────────────────── */

static void test_level_is_flat(void)
{
    wv_grains gr;
    int i, n = 12000;
    float lo = 1e9f, hi = -1e9f;

    printf("level:\n");
    fill_dc(20000);
    /* The material has to outlast the measurement: an unwrapped cloud past the end of
     * its material reads silence now (not the last sample held), so a 4096-sample
     * material measured over 12000 samples would measure the decay, not the ripple. */
    make_sample(14000, 1.0f);

    wv_grains_start(&gr, &smp, 0, 0, 1.0f, 0, 999u);
    for (i = 0; i < n; i++) {
        float v = wv_grains_next(&gr, &smp, 1500.0f, 0.0f);
        if (i > 3000) {                 /* past the start, in the steady state */
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    /* DC in, DC out: the output *is* the window sum. A ripple here is the tremolo. */
    ck(lo > 0.60f && hi < 0.62f, "window sum is flat (dc 20000 of 32768 = 0.610)");
    printf("  steady-state %.5f .. %.5f (ripple %.5f)\n", lo, hi, hi - lo);
    ck((hi - lo) < 0.006f, "ripple under 1%");
}

/* A grain hop that is not an integer: the countdown must still tile, not drift. */
static void test_fractional_hop(void)
{
    wv_grains gr;
    int i, n = 20000;
    float lo = 1e9f, hi = -1e9f;

    printf("fractional hop:\n");
    fill_dc(16384);
    make_sample(24000, 1.0f);           /* see the note in test_level_is_flat */

    /* 1337.7 samples: hop = 668.85, which is not representable. */
    wv_grains_start(&gr, &smp, 0, 0, 1.0f, 0, 7u);
    for (i = 0; i < n; i++) {
        float v = wv_grains_next(&gr, &smp, 1337.7f, 0.0f);
        if (i > 4000) {
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    ck(lo > 0.48f && hi < 0.52f, "fractional hop stays tiled");
    printf("  steady-state %.5f .. %.5f\n", lo, hi);
    /* The countdown cannot drift, so the level at the end matches the level at the
     * start. A float accumulator loses about one sample per 3000 grains here. */
    ck((hi - lo) < 0.01f, "no drift: ripple under 1% over 20k samples");
}

/* ─── 4. it stays inside the material ─────────────────────────────────────── */

static void test_clamps_at_the_edge(void)
{
    wv_grains gr;
    int i, n = 20000;
    float peak = 0.0f;

    printf("edges:\n");
    fill_ramp();
    make_sample(1000, 1.0f);            /* short material, long note */

    /* Playhead starts near the end and keeps going, so every grain runs off it. */
    wv_grains_start(&gr, &smp, 900, 0, 1.0f, 0, 3u);
    for (i = 0; i < n; i++) {
        float v = wv_grains_next(&gr, &smp, 8820.0f, 0.0f);
        if (fabsf(v) > peak)
            peak = fabsf(v);
    }
    /* Clamped at the last sample: the tail value, not a burst from sample 0 (which
     * is -16384 in the ramp and would show as a full-level step). */
    ck(peak < 1.05f, "no burst when the playhead runs off the end");

    /* Spray at full from a start near the end: scatter must clamp too. */
    wv_grains_start(&gr, &smp, 990, 0, 1.0f, 0, 3u);
    peak = 0.0f;
    for (i = 0; i < n; i++) {
        float v = wv_grains_next(&gr, &smp, 400.0f, 127.0f);
        if (fabsf(v) > peak)
            peak = fabsf(v);
    }
    ck(peak < 1.05f, "spray near the end does not burst");
    printf("  peak at the edge %.3f\n", peak);
}

/* ─── 5. spray ────────────────────────────────────────────────────────────── */

static float run_cloud(uint32_t seed, float spray, int n, float *buf)
{
    wv_grains gr;
    int i;
    float acc = 0.0f;
    fill_ramp();
    make_sample(4096, 1.0f);
    wv_grains_start(&gr, &smp, 500, 0, 1.0f, 0, seed);
    for (i = 0; i < n; i++) {
        buf[i] = wv_grains_next(&gr, &smp, 800.0f, spray);
        acc += fabsf(buf[i]);
    }
    return acc / (float)n;
}

static void test_spray_scatters(void)
{
    static float a[4000], b[4000], c[4000];
    float ma, mb, mc;
    int i, diff_ab = 0, diff_ac = 0;

    printf("spray:\n");
    ma = run_cloud(111u, 0.0f, 4000, a);
    mb = run_cloud(111u, 40.0f, 4000, b);
    mc = run_cloud(222u, 40.0f, 4000, c);

    for (i = 500; i < 4000; i++) {
        if (fabsf(a[i] - b[i]) > 0.01f) diff_ab++;
        if (fabsf(b[i] - c[i]) > 0.01f) diff_ac++;
    }
    ck(diff_ab > 3000, "spray changes the output");
    ck(diff_ac > 3000, "two voices with the same spray settings differ");
    /* Same seed, same settings: reproducible, which is what makes an audible test
     * replayable. */
    printf("  mean |out|: spray0 %.4f, spray40 %.4f (seed 111), %.4f (seed 222)\n",
           ma, mb, mc);
    ck(fabsf(ma - mb) < 0.6f, "spray does not change the average level much");
}

static void test_reproducible(void)
{
    static float a[2000], b[2000];
    int i, same = 1;

    printf("determinism:\n");
    run_cloud(4242u, 60.0f, 2000, a);
    run_cloud(4242u, 60.0f, 2000, b);
    for (i = 0; i < 2000; i++)
        if (a[i] != b[i]) same = 0;
    ck(same, "same seed and settings reproduce the cloud exactly");
}

/* ─── 6. grain length mapping ─────────────────────────────────────────────── */

static void test_grain_len(void)
{
    printf("grain length:\n");
    float lo = wv_grain_len(0), mid = wv_grain_len(63), hi = wv_grain_len(127);
    /* 10 ms .. 200 ms at 44.1 kHz. */
    ck(fabsf(lo - 441.0f) < 2.0f, "size 0 is 10 ms");
    ck(fabsf(hi - 8820.0f) < 20.0f, "size 127 is 200 ms");
    ck(mid > lo && mid < hi, "size 63 is between");
    /* Exponential means the midpoint is the geometric mean: sqrt(10*200) = 44.7 ms,
     * which is 1971 output samples, not the 105 ms a linear map would give. */
    ck(fabsf(mid - 1971.0f) < 40.0f, "size 63 is the geometric middle (~44.7 ms)");
    printf("  10 ms = %.0f, mid = %.1f, 200 ms = %.0f output samples\n", lo, mid, hi);
}

/* ─── 7. a cloud over a short sample sustains ─────────────────────────────── */

/* This is the bug the WAV found and the unit tests did not: with an unwrapped
 * playhead, a cloud over a 1.2 s tone went silent after 1.2 s, and it went silent
 * *earlier* the more spray was on, because a scattered grain that clamps at the last
 * sample is silence. The first chapter of test_spray_scatters passed anyway, because
 * it only looked at the first 4000 samples of an 8192-sample material. */
static void test_cloud_sustains_over_short_material(void)
{
    wv_grains gr;
    int i, n = 44100 * 3;               /* three seconds over 0.2 s of material */
    float early = 0.0f, late = 0.0f;
    int span_e = 0, span_l = 0;

    printf("sustain:\n");
    fill_ramp();
    make_sample(8000, 1.0f);            /* 0.18 s of material */

    /* Spray at 90: the case that used to die fastest. */
    wv_grains_start(&gr, &smp, 0, 0, 1.0f, 1, 42u);
    for (i = 0; i < n; i++) {
        float v = wv_grains_next(&gr, &smp, 1200.0f, 90.0f);
        if (i > 4410 && i < 8820) { early += fabsf(v); span_e++; }
        if (i > n - 8820)         { late  += fabsf(v); span_l++; }
    }
    early /= (float)span_e;
    late /= (float)span_l;
    ck(late > 0.5f * early, "a looped cloud is still sounding 3 s later");
    printf("  mean |out| at 0.1-0.2 s: %.4f, at 2.8-3.0 s: %.4f\n", early, late);

    /* And the unwrapped one still decays, because that is a one-shot's job. */
    wv_grains_start(&gr, &smp, 0, 0, 1.0f, 0, 42u);
    {
        float e2 = 0.0f, l2 = 0.0f;
        int se = 0, sl2 = 0;
        for (i = 0; i < 44100; i++) {
            float v = wv_grains_next(&gr, &smp, 1200.0f, 0.0f);
            if (i < 4410) { e2 += fabsf(v); se++; }
            if (i > 39690) { l2 += fabsf(v); sl2++; }
        }
        ck(e2 / (float)se > 0.05f, "an unwrapped cloud sounds at first");
        ck(l2 / (float)sl2 < 0.1f * (e2 / (float)se), "and decays when the material runs out");
        printf("  one-shot: %.4f -> %.4f\n", e2 / (float)se, l2 / (float)sl2);
    }
}


/* The wrap must not be audible.
 *
 * A grain spawned at the playhead reads FORWARD for its whole window, so a playhead
 * that wraps at the material's last sample spawns grains in the final window-length
 * with nothing left to read. read_at() returns silence there, the window keeps running,
 * and the result is a level dip once per slot pass: measured on the demo tone, 0.7 dB
 * at GRAIN 16, 3.5 dB at GRAIN 64 and 11.4 dB at GRAIN 127 -- a periodic thump whose
 * depth grows with the grain length.
 *
 * The fix is that the wrap point backs off by one window. This measures the outcome,
 * not the mechanism: over four passes of a 0.25 s slot, no 20 ms window may be more
 * than 1 dB below the loudest. A silent or short-slot case is excluded by the first
 * check.
 *
 * The tolerance is 0.5 dB because the fixed cloud measures exactly 0.00 dB on DC
 * material -- the window sum is 1 by construction, so a constant must come out
 * constant. The injected bug measured 0.52 / 2.03 / 12.51 dB, and a 1 dB threshold
 * let the smallest grain through: an assertion a bug can pass is not an assertion.
 */
static void test_wrap_has_no_dip(void)
{
    static const uint8_t grains[3] = { 16u, 64u, 127u };
    int gi;

    printf("wrap:\n");
    for (gi = 0; gi < 3; gi++) {
        wv_grains gr;
        float lo = 1e30f, hi = -1e30f, mean = 0.0f;
        int i, n = 44100 * 2;           /* 2 s over 0.25 s of material: 8 passes */
        /* One full period of fill_ramp's sawtooth. A window that does not hold whole
         * periods measures the waveform's own shape, not the cloud's level: at 882
         * samples the same cloud read -7 to -26 dB with nothing wrong with it. */
        int win = 4096;
        int nwin = 0;
        double acc = 0.0;

        /* DC material, on purpose. With a window sum of exactly 1 (the sin²/cos²
         * identity), a constant sample must come out at a constant level, so any drop
         * in the output can only come from reading where there is no material -- which
         * is exactly the defect under test. Ramp or sine material fails this: the
         * sawtooth read 2.8-4.7 dB of ripple with no wrap involved at all, because the
         * measurement was reading the waveform. */
        fill_dc(16384);
        make_sample(11025, 1.0f);       /* 0.25 s */

        wv_grains_start(&gr, &smp, 0, 0, 1.0f, 1, 4242u);
        for (i = 0; i < n; i++) {
            float v = wv_grains_next(&gr, &smp, wv_grain_len(grains[gi]), 0.0f);
            acc += (double)v * (double)v;
            if (++nwin == win) {
                float r = (float)sqrt(acc / (double)win);
                if (r > hi) hi = r;
                if (r < lo) lo = r;
                mean += r;
                acc = 0.0;
                nwin = 0;
            }
        }
        {
            float db = 20.0f * log10f(hi > 1e-9f ? lo / hi : 1e-9f);
            /* the first window is the note's own fade-in; it is not a wrap dip */
            printf("  GRAIN %3u: quietest window %.2f dB below the loudest\n", grains[gi], -db);
            ck(db > -0.5f, "no window of a looped cloud drops more than 0.5 dB");
        }
    }
}

int main(void)
{
    test_window_tiles();
    test_spray0_is_a_sample_player();
    test_level_is_flat();
    test_fractional_hop();
    test_clamps_at_the_edge();
    test_spray_scatters();
    test_reproducible();
    test_grain_len();
    test_cloud_sustains_over_short_material();
    test_wrap_has_no_dip();

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
