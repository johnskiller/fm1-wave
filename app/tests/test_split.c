/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* The two-core split, and the granular voice inside it.
 *   sh app/tests/run_dsp.sh
 *
 * The claim the split makes is narrow and testable: *the same audio, faster*. Not
 * "similar", not "the same spectrum" — the same samples. Anything else means the
 * listener hears a different instrument depending on whether the second core is up,
 * which on real hardware is a difference they cannot control: a unit that had a bad
 * start runs on one core until it is power-cycled.
 *
 * So these tests compare wv_render_split(one core) against wv_render_split(two cores)
 * sample by sample, with a fake second core that runs the job inline. That inline
 * call is exactly the shape the host simulator uses for the real API (plat_host.c:
 * a job runs at once, in order), so the fake here is not a stand-in for something
 * untested — it is the identical semantics.
 *
 * What is deliberately *not* claimed: that the split halves the time. That needs the
 * device (the 8 ms job timeout, the stack, the flash fetch cost of a job that runs
 * from XIP while CPU0 reads the same flash). Those are Gate B measurements and they
 * are listed as unmeasured in docs/spec.md.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "fastmath.h"
#include "wv_dsp.h"
#include "wv_grain.h"
#include "wv_sample.h"

static int fails, checks;

static void ck(int cond, const char *what)
{
    checks++;
    if (!cond) {
        fails++;
        printf("  FAIL %s\n", what);
    }
}

/* ─── a fake second core with the host simulator's semantics ──────────────── */

static int jobs_run;

static int fake_run(void (*fn)(uint32_t), uint32_t arg)
{
    jobs_run++;
    fn(arg);
    return 1;
}

static int fake_wait(void) { return 0; }

/* A second core that runs the job at `wait` instead of at `run`.
 *
 * The plain fake runs the job inside run(), so it finishes before the caller renders its
 * own half: the two halves never overlap, and a race between them cannot be seen. That
 * is exactly why the device had a hiss the simulator did not: both halves were adding
 * into the same buffer (`out[k] += amp`), which is fine one after the other and is a
 * lost-update race when they overlap.
 *
 * This fake keeps the job and runs it at wait, so the caller's half is already in `out`
 * when the job starts -- the worst case of the overlap, and enough to fail any code that
 * lets the second half write the caller's buffer. */
static void (*held_fn)(uint32_t);
static int held_run(void (*fn)(uint32_t), uint32_t arg)
{
    (void)arg;
    jobs_run++;
    held_fn = fn;
    return 1;
}
static int held_wait(void)
{
    if (held_fn) {
        held_fn(0u);
        held_fn = 0;
    }
    return 0;
}
static void fake_sync(void) {}

/* A second core that refuses the job: the app must still render everything. */
static int fake_run_refuse(void (*fn)(uint32_t), uint32_t arg)
{
    (void)fn;
    (void)arg;
    return 0;
}

/* A second core that takes the job and then dies: wait() reports the failure, and
 * the app must render the half itself or half the voices go missing. */
static int dead_run(void (*fn)(uint32_t), uint32_t arg)
{
    (void)fn;
    (void)arg;
    return 1;
}
static int dead_wait(void) { return -1; }

/* ─── material ────────────────────────────────────────────────────────────── */

#define NMAT 6000
static int16_t mat[NMAT];

static void fill_material(void)
{
    int i;
    for (i = 0; i < NMAT; i++) {
        /* something with a bit of everything: a tone, a harmonic, a little noise by
         * way of an integer hash so the cloud has transients to scatter */
        uint32_t h = (uint32_t)i * 2654435761u;
        float v = sinf((float)i * 0.071f) * 0.6f + sinf((float)i * 0.23f) * 0.25f;
        v += ((float)(h >> 24) * (1.0f / 255.0f) - 0.5f) * 0.08f;
        mat[i] = (int16_t)(v * 24000.0f);
    }
}

static wv_sample smp;
static wv_bank bank;

static void setup(void)
{
    fill_material();
    smp.data = mat;
    smp.len = NMAT;
    smp.rate = 44100;
    smp.bits = 16;
    smp.root = 60;
    smp.gain = 256;
    bank.frames = 0;                /* no table needed by these tests */
}

/* ─── 1. the split is sample-identical to one core ────────────────────────── */

/* A scene: eight voices, mixed sources, so the split has something to divide. */
static void scene(wv_state *s)
{
    uint32_t i;
    wv_init(s);
    s->morph = 40;
    s->cut = 96;
    s->res = 30;
    s->shape = WV_SHAPE_FOLD;
    s->sh_amount = 70;
    s->grain_size = 60;
    s->spray = 0;

    for (i = 0; i < 4; i++)
        wv_note_on_sample(s, &smp, (uint8_t)(55 + i * 3), 100, 0, 0, 0);
    for (i = 0; i < 2; i++)
        wv_note_on_grain(s, &smp, (uint8_t)(60 + i * 5), 90, 0, 0);
    wv_note_on_slice(s, &smp, 40, 110, 100, 2000, 0);
}

#define BLK 256

static void test_split_is_identical(void)
{
    static wv_state a, b;
    static float oa[BLK], ob[BLK];
    float worst = 0.0f, energy = 0.0f;
    uint32_t k;
    int ok;

    printf("split:\n");
    setup();

    scene(&a);
    scene(&b);

    for (k = 0; k < 40; k++) {
        memset(oa, 0, sizeof oa);
        memset(ob, 0, sizeof ob);
        wv_render(&a, &bank, oa, BLK, 4096.0f);
        jobs_run = 0;
        wv_render_split(&b, &bank, ob, BLK, 4096.0f, 1, fake_run, fake_wait, fake_sync);
        ck(jobs_run == 1, "the second core was handed one job per block");
        {
            uint32_t i;
            for (i = 0; i < BLK; i++) {
                float d = fabsf(oa[i] - ob[i]);
                if (d > worst) worst = d;
                energy += oa[i] * oa[i];
            }
        }
    }
    ck(worst < 1e-6f, "two cores produce the same samples as one");
    printf("  worst difference %.9f over 40 blocks, rms %.4f\n",
           worst, sqrtf(energy / (40.0f * BLK)));
    ck(sqrtf(energy / (40.0f * BLK)) > 0.01f, "there is actual audio to compare");
    /* The split must not be a no-op that happens to match: check it moved work. */
    ck(jobs_run == 1, "the split path was taken, not the single-core one");
    (void)ok;
}

/* The knobs glide: a block in which CUT moves must still match, because the
 * smoothing is advanced once per block and not once per voice range. */
static void test_split_with_moving_knobs(void)
{
    static wv_state a, b;
    static float oa[BLK], ob[BLK];
    float worst = 0.0f;
    uint32_t k, i;

    printf("split, moving knobs:\n");
    setup();
    scene(&a);
    scene(&b);

    for (k = 0; k < 60; k++) {
        /* a knob turn every few blocks, which is what the smoother is for */
        if (k % 4 == 0) {
            a.cut = b.cut = (uint8_t)(40 + k);
            a.sh_amount = b.sh_amount = (uint8_t)(30 + k);
        }
        memset(oa, 0, sizeof oa);
        memset(ob, 0, sizeof ob);
        wv_render(&a, &bank, oa, BLK, 4096.0f);
        wv_render_split(&b, &bank, ob, BLK, 4096.0f, 1, fake_run, fake_wait, fake_sync);
        for (i = 0; i < BLK; i++) {
            float d = fabsf(oa[i] - ob[i]);
            if (d > worst) worst = d;
        }
    }
    ck(worst < 1e-6f, "a moving knob glides at the same rate on both");
    printf("  worst difference %.9f\n", worst);
}

/* The cut has to follow the ACTIVE voices, not the pool's middle.
 *
 * The allocator hands out voice 0 first, so four notes land in 0..3: cutting the pool
 * in half left the second core idle and the first doing all four. Measured on the
 * device, GRAN read 51% at four notes and 51% at eight -- the same four voices' work,
 * because that is exactly what it was.
 *
 * What this checks is the sound, not the balance: an odd number of voices cuts BETWEEN
 * them instead of through the middle, and the two cores must still sum to one core's
 * output. Fewer voices than the pool is the case that was broken before, so it is the
 * case that gets its own test.
 *
 * The comparison is against wv_render (one core over the whole pool), and the tolerance
 * is one 24-bit LSB rather than 1e-6: the split changes the ORDER the voices are summed
 * in, and float addition is not associative, so a voice that lands on the other core is
 * added at a different point. That is inherent to splitting, not a defect -- measured on
 * the host, three voices differ in 6 of 35328 samples by 1 LSB (-90 dBFS), and eight
 * voices (where the cut is the same as it always was) are bit-identical. */
static void test_split_follows_the_active_voices(void)
{
    static wv_state a, b;
    static float oa[BLK], ob[BLK];
    static const uint32_t counts[] = {1, 2, 3, 5, 6, 7};
    float one_lsb = 1.0f / 8388608.0f;
    uint32_t c, k, i;
    int bad = 0;

    printf("split, partial voices:\n");
    setup();    /* bank.frames is 0 here: these tests play the material, not a table */
    for (c = 0; c < sizeof counts / sizeof counts[0]; c++) {
        float worst = 0.0f, peak = 0.0f;

        wv_init(&a);
        wv_init(&b);
        a.cut = b.cut = 60;
        a.res = b.res = 20;
        a.sh_amount = b.sh_amount = 40;
        for (i = 0; i < counts[c]; i++) {
            wv_note_on_sample(&a, &smp, (uint8_t)(57 + i * 2), 100, 0, 0, 0);
            wv_note_on_sample(&b, &smp, (uint8_t)(57 + i * 2), 100, 0, 0, 0);
        }

        for (k = 0; k < 20; k++) {
            memset(oa, 0, sizeof oa);
            memset(ob, 0, sizeof ob);
            wv_render(&a, &bank, oa, BLK, 4096.0f);
            jobs_run = 0;
            wv_render_split(&b, &bank, ob, BLK, 4096.0f, 1, fake_run, fake_wait, fake_sync);
            for (i = 0; i < BLK; i++) {
                float d = fabsf(oa[i] - ob[i]);
                if (d > worst) worst = d;
                if (fabsf(oa[i]) > peak) peak = fabsf(oa[i]);
            }
        }
        /* Where the cut went, not just that the audio matched. A cut through the
         * middle of the pool renders the same samples (so the audio check cannot see
         * it) but leaves core 1 idle at four notes, which is the defect being fixed:
         * four notes occupied 0..3 and the pool was cut at 4. Half of the active
         * voices is the target, so the two cores each get about the same work. */
        {
            uint32_t low = 0, high = 0;
            for (i = 0; i < wv_last_split(); i++)
                if (a.v[i].active)
                    low++;
            for (i = wv_last_split(); i < WV_VOICES; i++)
                if (a.v[i].active)
                    high++;
            printf("  %2u voices: worst %.2f LSB, peak %.3f, cut at %u (%u|%u)\n",
                   counts[c], worst / one_lsb, peak, wv_last_split(), low, high);
            if (counts[c] >= 2 && (high == 0 || low > high + 1))
                bad = 1;
        }
        if (peak < 0.01f || worst > 2.0f * one_lsb)
            bad = 1;
    }
    ck(bad == 0, "a partial voice pool splits to the same audio, balanced across the cores");
}

/* The second core must not write the caller's buffer.
 *
 * With the job run at wait (held_run/held_wait) the two halves overlap the way they do
 * on the device. If the job adds into `out` -- which it did, `out[k] += amp` -- then its
 * writes land on top of samples CPU0 has already written, and updates are lost. The
 * audio is then the same signal with broadband noise on it: hiss, worse with more
 * voices, absent on the host's inline fake. That is the bug this test exists for.
 *
 * It compares against wv_render (one core, one buffer), so any lost or doubled update
 * shows up as a difference far larger than the 1-LSB summation-order noise. */
static void test_split_does_not_race_on_the_output(void)
{
    static wv_state a, b;
    static float oa[BLK], ob[BLK];
    float worst = 0.0f, peak = 0.0f;
    uint32_t k, i;
    int aliased = 0;

    printf("split, the job's buffer:\n");
    setup();
    scene(&a);
    scene(&b);

    for (k = 0; k < 20; k++) {
        memset(oa, 0, sizeof oa);
        memset(ob, 0, sizeof ob);
        wv_render(&a, &bank, oa, BLK, 4096.0f);
        jobs_run = 0;
        wv_render_split(&b, &bank, ob, BLK, 4096.0f, 1, held_run, held_wait, fake_sync);
        if (wv_last_job_buffer() == (const float *)ob)
            aliased = 1;
        for (i = 0; i < BLK; i++) {
            float d = fabsf(oa[i] - ob[i]);
            if (d > worst) worst = d;
            if (fabsf(oa[i]) > peak) peak = fabsf(oa[i]);
        }
    }
    printf("  job buffer %s the caller's, worst difference %.2f LSB, peak %.3f\n",
           aliased ? "IS" : "is not", worst * 8388608.0f, peak);
    ck(!aliased, "the second core renders into its own buffer, not the caller's");

    /* And the sum must still be one core's audio. */
    ck(worst <= 2.0f / 8388608.0f, "the two buffers sum to one core's audio");
}

/* ─── 2. the fallbacks ────────────────────────────────────────────────────── */

static void test_refused_job_still_renders(void)
{
    static wv_state a, b;
    static float oa[BLK], ob[BLK];
    float worst = 0.0f, peak = 0.0f;
    uint32_t k, i;

    printf("fallbacks:\n");
    setup();
    scene(&a);
    scene(&b);

    for (k = 0; k < 20; k++) {
        memset(oa, 0, sizeof oa);
        memset(ob, 0, sizeof ob);
        wv_render(&a, &bank, oa, BLK, 4096.0f);
        wv_render_split(&b, &bank, ob, BLK, 4096.0f, 1, fake_run_refuse, fake_wait, fake_sync);
        for (i = 0; i < BLK; i++) {
            float d = fabsf(oa[i] - ob[i]);
            if (d > worst) worst = d;
            if (fabsf(ob[i]) > peak) peak = fabsf(ob[i]);
        }
    }
    ck(worst < 1e-6f, "a refused job renders the whole block on one core");
    ck(peak > 0.01f, "and it is not silent");
}

static void test_dead_core_still_renders(void)
{
    static wv_state a, b;
    static float oa[BLK], ob[BLK];
    float worst = 0.0f, peak = 0.0f;
    uint32_t k, i;

    setup();
    scene(&a);
    scene(&b);

    for (k = 0; k < 20; k++) {
        memset(oa, 0, sizeof oa);
        memset(ob, 0, sizeof ob);
        wv_render(&a, &bank, oa, BLK, 4096.0f);
        /* takes the job, then reports it was retired: the top half never ran */
        wv_render_split(&b, &bank, ob, BLK, 4096.0f, 1, dead_run, dead_wait, fake_sync);
        for (i = 0; i < BLK; i++) {
            float d = fabsf(oa[i] - ob[i]);
            if (d > worst) worst = d;
            if (fabsf(ob[i]) > peak) peak = fabsf(ob[i]);
        }
    }
    ck(worst < 1e-6f, "a core that died mid-job does not lose half the voices");
    ck(peak > 0.01f, "and it is not silent");
}

/* ─── 3. granular is a voice, not a second engine ─────────────────────────── */

static void test_grain_voice_is_a_voice(void)
{
    static wv_state s;
    uint32_t v;

    printf("granular voice:\n");
    setup();
    wv_init(&s);
    s.grain_size = 40;
    s.spray = 0;

    v = wv_note_on_grain(&s, &smp, 60, 100, 0, 0);
    ck(v != 0xFFFFFFFFu, "a grain note starts");
    ck(s.v[v].src == WV_SRC_GRAIN, "it is a grain source");
    ck(wv_active(&s) == 1u, "one voice is active");

    /* It goes through the same shaper and filter: at a low CUT the output has less
     * high end than at a high CUT, which is only true if the chain is shared. */
    {
        static float lo[BLK], hi[BLK];
        float elo = 0.0f, ehi = 0.0f, dlo = 0.0f, dhi = 0.0f;
        uint32_t k, i;
        wv_state a, b;

        wv_init(&a);
        a.grain_size = 40; a.cut = 20; a.spray = 0;
        wv_note_on_grain(&a, &smp, 60, 100, 0, 0);
        wv_init(&b);
        b.grain_size = 40; b.cut = 120; b.spray = 0;
        wv_note_on_grain(&b, &smp, 60, 100, 0, 0);

        for (k = 0; k < 30; k++) {
            memset(lo, 0, sizeof lo);
            memset(hi, 0, sizeof hi);
            wv_render(&a, &bank, lo, BLK, 4096.0f);
            wv_render(&b, &bank, hi, BLK, 4096.0f);
            if (k < 10) continue;              /* past the envelope attack */
            for (i = 0; i < BLK; i++) {
                elo += lo[i] * lo[i];
                ehi += hi[i] * hi[i];
                if (i > 0) {
                    dlo += (lo[i] - lo[i-1]) * (lo[i] - lo[i-1]);
                    dhi += (hi[i] - hi[i-1]) * (hi[i] - hi[i-1]);
                }
            }
        }
        /* A low-pass at CUT 20 removes high frequencies, so the sample-to-sample
         * difference (a crude high-frequency energy measure) is smaller. */
        ck(dlo < dhi * 0.7f, "CUT filters a grain voice like any other voice");
        printf("  high-frequency energy: cut20 %.6f, cut120 %.6f (%.2fx)\n",
               dlo, dhi, dhi > 0 ? dlo / dhi : 0.0f);
        ck(elo > 0.0f && ehi > 0.0f, "both render audio");
    }

    wv_note_off(&s, 60);
    {
        uint32_t k;
        static float o[BLK];
        for (k = 0; k < 200; k++) {
            memset(o, 0, sizeof o);
            wv_render(&s, &bank, o, BLK, 4096.0f);
        }
    }
    ck(wv_active(&s) == 0u, "a grain voice releases and frees itself");
}

/* A granular voice at spray 0 over a chop must sound like the chop: same envelope
 * shape, same pitch. This is the spec's "SPRAY=0 degrades to a sample player" as a
 * test rather than as a hope. */
static void test_grain_matches_chop_at_spray0(void)
{
    static wv_state g, c;
    static float og[BLK], oc[BLK];
    float worst = 0.0f;
    uint32_t k, i;

    setup();

    wv_init(&g);
    g.grain_size = 127;                 /* 200 ms: long enough to cover the window */
    g.spray = 0;
    g.cut = 127; g.res = 0; g.sh_amount = 0; g.shape = WV_SHAPE_SOFT;
    wv_note_on_grain(&g, &smp, 60, 100, 0, 0);

    wv_init(&c);
    c.cut = 127; c.res = 0; c.sh_amount = 0; c.shape = WV_SHAPE_SOFT;
    wv_note_on_sample(&c, &smp, 60, 100, 0, 0, 0);

    for (k = 0; k < 20; k++) {
        memset(og, 0, sizeof og);
        memset(oc, 0, sizeof oc);
        wv_render(&g, &bank, og, BLK, 4096.0f);
        wv_render(&c, &bank, oc, BLK, 4096.0f);
        if (k < 2) continue;            /* the attack differs: see wv_grain.h */
        for (i = 0; i < BLK; i++) {
            float d = fabsf(og[i] - oc[i]);
            if (d > worst) worst = d;
        }
    }
    /* Not bit-identical: the shaper at amount 0 and the filter at CUT 127 are near
     * but not exactly identity. Close enough that the two paths are the same sound. */
    ck(worst < 0.35f, "spray 0 tracks the chop (same level, same material)");
    printf("  worst deviation from the chop path %.4f\n", worst);
}

/* ─── 4. the grain cloud does not blow the budget ─────────────────────────── */

static void test_eight_grain_voices(void)
{
    static wv_state s;
    static float o[BLK];
    float peak = 0.0f;
    uint32_t k, i, made = 0;

    printf("eight grain voices:\n");
    setup();
    wv_init(&s);
    s.grain_size = 50;
    s.spray = 60;
    s.cut = 100;
    s.shape = WV_SHAPE_FOLD;
    s.sh_amount = 60;

    for (i = 0; i < WV_VOICES; i++)
        if (wv_note_on_grain(&s, &smp, (uint8_t)(48 + i * 2), 110, 0, 0) != 0xFFFFFFFFu)
            made++;
    ck(made == WV_VOICES, "eight grain voices start");

    for (k = 0; k < 40; k++) {
        memset(o, 0, sizeof o);
        wv_render(&s, &bank, o, BLK, 4096.0f);
        for (i = 0; i < BLK; i++)
            if (fabsf(o[i]) > peak) peak = fabsf(o[i]);
    }
    /* The bus limiter is why this can be asserted at all: 32 grains summing without
     * it would be well past full scale and would wrap in the DMA. */
    ck(peak <= 1.0f, "32 grains stay inside full scale (bus limiter holds)");
    ck(peak > 0.30f, "and they are not squashed to nothing");
    printf("  peak %.3f\n", peak);
}

int main(void)
{
    test_split_is_identical();
    test_split_with_moving_knobs();
    test_split_follows_the_active_voices();
    test_split_does_not_race_on_the_output();
    test_refused_job_still_renders();
    test_dead_core_still_renders();
    test_grain_voice_is_a_voice();
    test_grain_matches_chop_at_spray0();
    test_eight_grain_voices();

    printf("\n%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
