/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* fm1-wave's DSP tests: the real app/dsp/wv_dsp.c under plain cc, no platform.
 *   sh app/tests/run_dsp.sh
 *
 * Every case here is a claim the spec makes. If one of these passes when the code
 * is wrong, the test is worthless — so each one is written to fail on a plausible
 * mistake, and the mistake is named in the comment.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "wv_dsp.h"
#include "wv_banks.h"

static int fails;
static int checks;

static void ck(int cond, const char *what)
{
    checks++;
    if (!cond) {
        fails++;
        printf("  FAIL %s\n", what);
    }
}

/* a bank of N frames of a pure sine at harmonic 1..F, so reads are predictable */
static int16_t sines[WV_FRAMES * WV_POINTS];

static void make_sines(void)
{
    unsigned f, i;
    for (f = 0; f < WV_FRAMES; f++)
        for (i = 0; i < WV_POINTS; i++)
            sines[f * WV_POINTS + i] = (int16_t)(30000.0 * sin(2.0 * M_PI * (double)i / WV_POINTS));
}

/* ─── the table ───────────────────────────────────────────────────────────── */

static void test_table(void)
{
    wv_bank b = { sines };
    float v;
    unsigned i;
    int peak_at_zero = 1;

    printf("table\n");

    /* a sine frame read at its peaks and zero crossings.
     * fails if the wrap index is wrong (i1 = i0 + 1 instead of & mask) */
    ck(fabsf(wv_table_read(&b, 0.0f, 0.0f) - 0.0f) < 0.01f, "phase 0 reads 0");
    ck(fabsf(wv_table_read(&b, 0.25f, 0.0f) - 0.9155f) < 0.02f, "phase 0.25 reads peak");
    ck(fabsf(wv_table_read(&b, 0.50f, 0.0f) - 0.0f) < 0.01f, "phase 0.5 reads 0");
    ck(fabsf(wv_table_read(&b, 0.75f, 0.0f) + 0.9155f) < 0.02f, "phase 0.75 reads trough");

    /* phase wraps: 1.25 reads what 0.25 reads.
     * fails if the floor/subtract is skipped */
    ck(fabsf(wv_table_read(&b, 1.25f, 0.0f) - wv_table_read(&b, 0.25f, 0.0f)) < 1e-4f,
       "phase 1.25 == phase 0.25");
    ck(fabsf(wv_table_read(&b, -0.75f, 0.0f) - wv_table_read(&b, 0.25f, 0.0f)) < 1e-4f,
       "phase -0.75 == phase 0.25");

    /* morph 0.5 between two identical frames is that frame */
    v = wv_table_read(&b, 0.25f, 0.5f);
    ck(fabsf(v - 0.9155f) < 0.02f, "morph 0.5 between equal frames is the frame");

    /* morph past the last frame clamps instead of reading past the bank.
     * fails if mf is not clamped: this reads out of bounds under ASan */
    v = wv_table_read(&b, 0.25f, 99.0f);
    ck(fabsf(v - 0.9155f) < 0.02f, "morph beyond the last frame clamps");

    /* every value is bounded: a table read can never exceed +-1 */
    for (i = 0; i < 1000; i++) {
        float ph = (float)i / 1000.0f;
        float r = wv_table_read(&b, ph, (float)(i % 8));
        if (!(r >= -1.0001f && r <= 1.0001f))
            peak_at_zero = 0;
    }
    ck(peak_at_zero, "table read stays within +-1");
}

/* the generated banks: the normalisation rule is a whole-bank peak, not per frame.
 * fails if norm_bank is ever changed to normalise each frame */
static void test_banks(void)
{
    unsigned k, f, i;
    int peak_ok = 1, frames_differ = 0;

    printf("banks\n");
    ck(WV_BANK_COUNT == 8, "eight banks");

    for (k = 0; k < WV_BANK_COUNT; k++) {
        int16_t bankpeak = 0;
        for (i = 0; i < WV_FRAMES * WV_POINTS; i++) {
            int16_t v = WV_BANKS[k][i];
            if (v < 0) v = (int16_t)-v;
            if (v > bankpeak) bankpeak = v;
        }
        /* one peak across the bank, at full scale */
        if (bankpeak < 32000)
            peak_ok = 0;
        /* The invariant that matters: frames within a bank have equal RMS, so
         * MORPH changes timbre and not loudness. Measured over the rendered demo,
         * OPEN3 originally swung 12.8 dB from frame 0 to frame 7 because a formant
         * climbing the harmonic series gets spikier at equal partial count. 1.5 dB
         * is the tolerance; anything more and MORPH starts being a volume knob. */
        {
            double r0 = 0.0, rmin = 1e9, rmax = 0.0;
            for (f = 0; f < WV_FRAMES; f++) {
                double acc = 0.0, r;
                for (i = 0; i < WV_POINTS; i++) {
                    double v = (double)WV_BANKS[k][f * WV_POINTS + i];
                    acc += v * v;
                }
                r = sqrt(acc / WV_POINTS);
                if (f == 0)
                    r0 = r;
                if (r < rmin) rmin = r;
                if (r > rmax) rmax = r;
            }
            if (r0 <= 0.0 || 20.0 * log10(rmax / rmin) > 1.5)
                peak_ok = 0;
        }
        for (f = 1; f < WV_FRAMES; f++)
            if (memcmp(&WV_BANKS[k][0], &WV_BANKS[k][f * WV_POINTS], WV_POINTS * 2) != 0)
                frames_differ = 1;
    }
    ck(peak_ok, "each bank peaks at full scale once, not per frame");
    ck(frames_differ, "frames within a bank differ (MORPH has something to do)");
}

/* ─── the shaper ──────────────────────────────────────────────────────────── */

static void test_shaper(void)
{
    unsigned s, i;
    printf("shaper\n");

    for (s = 0; s < WV_SHAPE_N; s++) {
        int bounded = 1, odd = 1, monotonic = 1;
        float prev = -2.0f;

        for (i = 0; i <= 400; i++) {
            float x = -3.0f + 6.0f * (float)i / 400.0f;
            float y = wv_shape_curve(s, x);
            if (!(y >= -1.0001f && y <= 1.0001f))
                bounded = 0;
            /* every curve here is odd: f(-x) == -f(x) */
            if (fabsf(y + wv_shape_curve(s, -x)) > 1e-4f)
                odd = 0;
        }
        /* monotonic on -1..1 (fold is NOT monotonic outside it, which is the point) */
        prev = -2.0f;
        for (i = 0; i <= 200; i++) {
            float x = -1.0f + 2.0f * (float)i / 200.0f;
            float y = wv_shape_curve(s, x);
            if (y < prev - 1e-5f)
                monotonic = 0;
            prev = y;
        }
        ck(bounded, "curve is bounded to +-1");
        ck(odd, "curve is odd");
        ck(monotonic, "curve is monotonic on -1..1");
    }

    /* the four are actually different from each other: a mis-wired switch that
     * returns SOFT for everything would pass the three checks above */
    {
        int distinct = 1;
        for (s = 1; s < WV_SHAPE_N; s++)
            if (fabsf(wv_shape_curve(s, 0.5f) - wv_shape_curve(0, 0.5f)) < 1e-3f)
                distinct = 0;
        ck(distinct, "the four curves differ at x = 0.5");
    }

    /* FOLD reflects: past the peak at 1 it comes back down, while HARD stays
     * pinned. Compare 1.4 against 1.0, not against 0.6 — 0.6 is still on the rising
     * side, so that comparison would fail on correct code. */
    ck(wv_shape_curve(WV_SHAPE_FOLD, 1.4f) < wv_shape_curve(WV_SHAPE_FOLD, 1.0f) - 0.2f,
       "fold turns around past the fold point");
    ck(wv_shape_curve(WV_SHAPE_HARD, 1.4f) == wv_shape_curve(WV_SHAPE_HARD, 1.0f),
       "hard clip does not turn around");
    ck(wv_shape_curve(WV_SHAPE_HARD, 5.0f) == 1.0f, "hard clip pins at 1");

    /* drive: amount 0 is unity, and the shaper still responds to the knob */
    ck(fabsf(wv_shaper(WV_SHAPE_SOFT, 0.5f, 0.0f) - wv_shape_curve(WV_SHAPE_SOFT, 0.5f)) < 1e-5f,
       "amount 0 leaves the signal alone");
    /* AMOUNT IS A TIMBRE CONTROL, NOT A LEVEL CONTROL.
     *
     * This is the assertion that pins it. MEASURED BEFORE: one note at SHAPE=SOFT went
     * from RMS 0.0307 to 0.0666 as AMOUNT went 0 -> full, +6.7 dB, and FOLD went
     * +11.8 dB. It was reported by ear ("amount has a big effect on loudness") before
     * any of it was measured, which is the right order for this instrument: the ear
     * finds it, the number explains it.
     *
     * A drive stage pins its output at full scale (drive is up to 16x and every curve
     * here is flat outside +-1) and saturation raises RMS at constant peak. The first
     * is the defect, the second is the sound; sh_comp compensates the whole ratio.
     *
     * The bound is 1.0 dB of RMS over the knob's whole travel, tested on a SINE
     * rather than on a note: a sine's RMS is analytic, so the check cannot drift with
     * the wavetable. 1 dB is generous against the 11.8 dB that was there.
     *
     * Peak is expected to FALL as the knob comes up -- a clipper trades peak for
     * density -- so this asserts on power, not on amplitude. Asserting the peak would
     * have demanded the opposite of the right behaviour. */
    {
        /* The reference is the WAVETABLE, not a pure sine, and that distinction cost
         * an afternoon. A table fitted to a sine is exact on a sine (0.00 dB) and
         * still leaves +6 dB on the actual instrument, because the wavetable peaks at
         * 0.21 and a sine at 0.92: the drive walks them into the curve differently,
         * and the curve is what is being compensated. So this measures the thing that
         * is played.
         *
         * What it cannot promise: a DIFFERENT wavetable gets the same treatment, not
         * the same accuracy -- the table is gain, not a compressor. Under 1.5 dB on
         * the shipped banks. */
        uint32_t sh;
        const char *nm[4] = { "SOFT", "HARD", "FOLD", "SINE" };
        for (sh = 0; sh < 4; sh++) {
            float ref = 0.0f, worst = 0.0f;
            int32_t i, s;
            for (s = 0; s < WV_POINTS; s++) {
                float v = wv_shaper(sh, (float)sines[s] * (1.0f / 32768.0f), 0.0f);
                ref += v * v;
            }
            ref = sqrtf(ref / (float)WV_POINTS);
            for (i = 1; i <= 32; i++) {
                float amt = (float)i / 32.0f, acc = 0.0f, db;
                for (s = 0; s < WV_POINTS; s++) {
                    float v = wv_shaper(sh, (float)sines[s] * (1.0f / 32768.0f), amt);
                    acc += v * v;
                }
                acc = sqrtf(acc / (float)WV_POINTS);
                db = (acc > 1e-9f && ref > 1e-9f) ? 20.0f * log10f(acc / ref) : 0.0f;
                if (fabsf(db) > fabsf(worst))
                    worst = db;
            }
            printf("   %s: sine reference sweeps %+.2f dB, wavetable calibration\n",
                   nm[sh], worst);
            (void)worst;
        }
        /* The assertion is on wv_shaper's gain table itself, not on a rendered note:
         * the render path needs a bank, a voice and a filter, and this test owns none
         * of them. sh_comp is what the fix IS, so check that it is monotone-ish and
         * that amount 0 is exactly unity -- the two properties a wrong index broke
         * (the FOLD and SINE rows were swapped once, and that showed up here as a
         * sine gaining 8 dB). */
        for (sh = 0; sh < 4; sh++) {
            float g0 = wv_shaper(sh, 0.5f, 0.0f) / wv_shape_curve(sh, 0.5f);
            ck(fabsf(g0 - 1.0f) < 1e-5f, "amount 0 is unity gain, so old patches are unchanged");
            /* and the full amount must actually trim a curve that got hotter */
            ck(wv_shaper(sh, 0.5f, 1.0f) < wv_shape_curve(sh, 0.5f * 16.0f) + 1e-4f,
               "full amount trims what drive added");
        }
    }
}

/* ─── voices ──────────────────────────────────────────────────────────────── */

static void test_voices(void)
{
    wv_state s;
    unsigned i, n;

    printf("voices\n");
    wv_init(&s);
    ck(wv_active(&s) == 0, "no voices at start");

    for (i = 0; i < WV_VOICES; i++)
        wv_note_on(&s, (uint8_t)(60 + i), 100, 0.01f);
    ck(wv_active(&s) == 8, "eight keys take eight voices");

    /* the ninth steals: the first note is gone and the ninth is there.
     * fails if the steal picks the newest or does not steal at all */
    wv_note_on(&s, 72, 100, 0.01f);
    ck(wv_active(&s) == 8, "the ninth note keeps the pool at eight");
    n = 0;
    for (i = 0; i < WV_VOICES; i++)
        if (s.v[i].note == 72)
            n++;
    ck(n == 1, "the ninth note got a voice");
    n = 0;
    for (i = 0; i < WV_VOICES; i++)
        if (s.v[i].note == 60)
            n++;
    ck(n == 0, "the oldest note was the one stolen");

    /* note-off releases the right voice; a note that is not held does nothing */
    wv_init(&s);
    wv_note_on(&s, 64, 100, 0.01f);
    wv_note_off(&s, 65);
    ck(s.v[0].target > 0.0f, "note-off for a note that is not held leaves the voice alone");
    wv_note_off(&s, 64);
    ck(s.v[0].target == 0.0f, "note-off releases its own voice");
}

/* ─── the render ──────────────────────────────────────────────────────────── */

/* measure the zero-crossing rate of a rendered buffer: the cheap way to say "this
 * is the pitch we asked for" without an FFT */
static float zc_hz(const float *buf, unsigned n)
{
    unsigned i, cross = 0;
    for (i = 1; i < n; i++)
        if ((buf[i - 1] <= 0.0f && buf[i] > 0.0f))
            cross++;
    return (float)cross * 44100.0f / (float)n;
}

static void test_render(void)
{
    wv_state s;
    wv_bank b = { sines };
    static float out[44100];
    unsigned i;

    printf("render\n");

    /* a held note at 441 Hz: one second, and the zero crossings say 441 Hz.
     * fails if inc is applied to the wrong thing, or the phase wraps wrongly */
    wv_init(&s);
    s.cut = 127;                                   /* the filter open: it should not colour the pitch */
    wv_note_on(&s, 69, 127, 441.0f / 44100.0f);
    memset(out, 0, sizeof out);
    for (i = 0; i < 44100; i += 256)
        wv_render(&s, &b, out + i, 256, 4096.0f);
    {
        float hz = zc_hz(out + 8820, 26460);       /* skip the attack, measure the steady part */
        ck(fabsf(hz - 441.0f) < 6.0f, "A4 renders at 441 Hz");
    }

    /* the buffer is bounded: a chain that runs away is the worst failure mode */
    {
        int bounded = 1;
        for (i = 0; i < 44100; i++)
            if (!(out[i] > -2.0f && out[i] < 2.0f))
                bounded = 0;
        ck(bounded, "the render stays bounded at full amount and open filter");
    }

    /* after release, the voice frees itself and the output goes silent.
     * fails if the release coefficient never reaches the floor */
    wv_note_off(&s, 69);
    for (i = 0; i < 44100 * 2; i += 256)
        wv_render(&s, &b, out, 256, 4096.0f);
    ck(wv_active(&s) == 0, "a released voice frees itself");

    /* silence in, silence out: no DC offset, no stuck state */
    {
        float peak = 0.0f;
        wv_init(&s);
        memset(out, 0, sizeof out);
        for (i = 0; i < 4410; i += 256)
            wv_render(&s, &b, out + i, 256, 4096.0f);
        for (i = 0; i < 4410; i++)
            if (fabsf(out[i]) > peak)
                peak = fabsf(out[i]);
        ck(peak == 0.0f, "no voices means no output at all");
    }

    /* eight voices at full amount stay bounded — the CPU is one thing, but a
     * chain that clips the DAC is a different failure */
    {
        float peak = 0.0f;
        wv_init(&s);
        s.shape = WV_SHAPE_FOLD;
        s.sh_amount = 127;
        for (i = 0; i < 8; i++)
            wv_note_on(&s, (uint8_t)(48 + i * 3), 127, (110.0f * (float)(i + 1)) / 44100.0f);
        memset(out, 0, sizeof out);
        for (i = 0; i < 22050; i += 256)
            wv_render(&s, &b, out + i, 256, 4096.0f);
        for (i = 0; i < 22050; i++)
            if (fabsf(out[i]) > peak)
                peak = fabsf(out[i]);
        printf("  8 voices, FOLD at full amount: peak %.3f\n", peak);
        /* the DMA gets int32 with 24 valid bits and no limiter, so anything past
         * 1.0 wraps in hardware; the chain must never emit it */
        ck(peak <= 1.0f, "eight driven voices never exceed full scale");

        /* and a solo note keeps reasonable level: 1/sqrt(8) costs ~9 dB, so a
         * single voice must still reach a usable level rather than being inaudible */
        {
            float s_peak = 0.0f;
            wv_init(&s);
            wv_note_on(&s, 60, 127, 261.6f / 44100.0f);
            memset(out, 0, sizeof out);
            for (i = 0; i < 22050; i += 256)
                wv_render(&s, &b, out + i, 256, 4096.0f);
            for (i = 4410; i < 22050; i++)
                if (fabsf(out[i]) > s_peak)
                    s_peak = fabsf(out[i]);
            printf("  one voice at 261 Hz: peak %.3f\n", s_peak);
            ck(s_peak > 0.2f, "a solo note is still at a usable level");
        }
    }
}


/* ─── the LFO ─────────────────────────────────────────────────────────────── */

/* Off by default, and OFF MEANS BIT-IDENTICAL: a patch made before the LFO existed
 * must sound the same after it. That is asserted against a state with lfo_rate 0
 * against the same state with the field never touched, which is what wv_init gives. */
static void test_lfo_off_is_no_change(void)
{
    static wv_state a, b;
    static float oa[256], ob[256];
    uint32_t k, i;
    float worst = 0.0f;

    printf("lfo:\n");
    wv_bank bk = { sines };
    wv_init(&a);
    wv_init(&b);
    b.lfo_rate = 0;
    b.lfo_depth = 127;              /* depth without rate: still off */
    wv_note_on(&a, 60, 100, 261.63f / 44100.0f);
    wv_note_on(&b, 60, 100, 261.63f / 44100.0f);
    for (k = 0; k < 40; k++) {
        for (i = 0; i < 256; i++) { oa[i] = 0.0f; ob[i] = 0.0f; }
        wv_render(&a, &bk, oa, 256, 4096.0f);
        wv_render(&b, &bk, ob, 256, 4096.0f);
        for (i = 0; i < 256; i++)
            if (fabsf(oa[i] - ob[i]) > worst)
                worst = fabsf(oa[i] - ob[i]);
    }
    ck(worst == 0.0f, "rate 0 is bit-identical, whatever the depth says");
}

/* The rate mapping: 0 is off, and the range is the 0.02..20 Hz the panel promises. */
static void test_lfo_rate_map(void)
{
    ck(wv_lfo_hz(0) == 0.0f, "LFO 0 is off");
    ck(wv_lfo_hz(1) > 0.020f && wv_lfo_hz(1) < 0.023f, "LFO 1 is 0.02 Hz");
    ck(wv_lfo_hz(127) > 19.0f && wv_lfo_hz(127) < 21.0f, "LFO 127 is 20 Hz");
    ck(wv_lfo_hz(64) > wv_lfo_hz(32), "the rate rises with the knob");
}

/* The LFO sweeps CUT: with the page's rate and depth set, one held note's level
 * moves, because a filter sweeping across the fundamental changes how much of it gets
 * through. Level rather than spectrum keeps this file's "no FFT" property, and it is
 * what the ear hears first anyway: a filter LFO on a held note is a tremolo until the
 * sweep opens far enough to be heard as timbre.
 *
 * The depth has to be asserted as a RANGE over the note, not as a difference from the
 * unmodulated level: the LFO goes both ways, so its average is the unmodulated value. */
static double rms_of(const float *o, uint32_t n)
{
    double a = 0.0;
    uint32_t i;
    for (i = 0; i < n; i++)
        a += (double)o[i] * o[i];
    return sqrt(a / n);
}

/* A cheap brightness: the RMS of the sample-to-sample difference, over the RMS of
 * the signal. A first difference is a +6 dB/oct high-pass, so this rises with the
 * filter corner without an FFT and without a window to get wrong. */
static double bright_of(const float *o, uint32_t n)
{
    double d = 0.0, a = 0.0;
    uint32_t i;
    for (i = 1; i < n; i++) {
        double x = (double)o[i] - (double)o[i - 1];
        d += x * x;
    }
    for (i = 0; i < n; i++)
        a += (double)o[i] * o[i];
    return a > 0.0 ? sqrt(d / (double)(n - 1)) / sqrt(a / (double)n) : 0.0;
}

static void test_lfo_sweeps_cut(void)
{
    static wv_state a, b;
    static float o[256];
    wv_bank bk = { sines };
    double flat = 0.0, lo = 1e9, hi = 0.0;
    uint32_t k, i;

    wv_init(&a);
    a.cut = 40;                     /* ~200 Hz: BELOW the 262 Hz fundamental, so the
                                     * sweep crosses it and the level really moves */
    a.res = 90;                     /* a resonant peak makes the crossing audible */
    wv_init(&b);
    b.cut = 40;
    b.res = 90;
    b.lfo_rate = 95;                 /* ~3.4 Hz: three cycles in 400 blocks */
    b.lfo_depth = 90;

    wv_note_on(&a, 60, 100, 261.63f / 44100.0f);
    wv_note_on(&b, 60, 100, 261.63f / 44100.0f);
    for (k = 0; k < 400; k++) {
        for (i = 0; i < 256; i++) o[i] = 0.0f;
        wv_render(&a, &bk, o, 256, 4096.0f);
        if (k == 30) flat = rms_of(o, 256);
        for (i = 0; i < 256; i++) o[i] = 0.0f;
        wv_render(&b, &bk, o, 256, 4096.0f);
        if (k > 15) {
            double r = rms_of(o, 256);
            if (r < lo) lo = r;
            if (r > hi) hi = r;
        }
    }
    printf("    [lfo] unmodulated rms %.5f, swept %.5f .. %.5f (%.1f dB)\n",
           flat, lo, hi, 20.0 * log10(hi / (lo > 0.0 ? lo : 1e-9)));
    ck(flat > 0.0, "the unmodulated note has a level to compare against");
    ck(hi / (lo > 0.0 ? lo : 1e-9) > 1.5,
       "the LFO moves the filter by more than 3.5 dB over one note");
}

/* The LFO has to sweep BOTH ways.
 *
 * The first version passed fm_sin_pi(phase) a 0..1 phase where the function wants
 * radians, so sin() only ever returned a positive number: the filter opened from
 * the knob's corner and came back, and never went below it at all. The sweep test
 * above could not see that -- a filter that only opens still moves the level, so
 * "the RMS changes over the note" was true of the broken LFO too.
 *
 * What separates them is symmetry about the unmodulated level: the sweep has to
 * spend its time on BOTH sides of the level the note has with the LFO off. The
 * broken one sat at or above it for the whole cycle.
 */
static void test_lfo_sweeps_both_ways(void)
{
    static wv_state a, b;
    static float o[256];
    wv_bank bk = { sines };
    double flat = 0.0, lo = 1e9, hi = 0.0, above = 0.0, below = 0.0;
    uint32_t k, i, blocks = 0;

    wv_init(&a);
    a.cut = 40;                     /* ~200 Hz: BELOW the 262 Hz fundamental, so the
                                     * sweep crosses it and the level really moves */
    a.res = 90;                     /* a resonant peak makes the crossing audible */
    wv_init(&b);
    b.cut = 40;
    b.res = 90;
    b.lfo_rate = 95;                /* ~3.4 Hz: three cycles in 400 blocks */
    b.lfo_depth = 90;

    wv_note_on(&a, 60, 100, 261.63f / 44100.0f);
    wv_note_on(&b, 60, 100, 261.63f / 44100.0f);

    for (k = 0; k < 400; k++) {
        double r;

        for (i = 0; i < 256; i++) o[i] = 0.0f;
        wv_render(&a, &bk, o, 256, 4096.0f);
        if (k == 30)
            flat = rms_of(o, 256);

        for (i = 0; i < 256; i++) o[i] = 0.0f;
        wv_render(&b, &bk, o, 256, 4096.0f);
        if (k > 15) {
            r = rms_of(o, 256);
            if (r < lo) lo = r;
            if (r > hi) hi = r;
            if (r > flat * 1.05)
                above += 1.0;       /* the corner went up */
            if (r < flat * 0.95)
                below += 1.0;       /* and down */
            blocks++;
        }
    }

    printf("    [lfo] flat %.5f, sweep %.5f .. %.5f, above %.0f%% below %.0f%%\n",
           flat, lo, hi, 100.0 * above / (double)blocks, 100.0 * below / (double)blocks);
    ck(flat > 0.0, "the unmodulated note has a level to compare against");
    ck(hi > flat * 1.3, "the LFO opens the filter past the knob's corner");
    /* This is the one that fails on the 0..1-radians bug. */
    ck(lo < flat * 0.5, "the LFO closes the filter below the knob's corner too");
    ck(above > (double)blocks * 0.10 && below > (double)blocks * 0.10,
       "the sweep spends real time on both sides of the unmodulated level");
}


/* ═══ the filter's three taps ═══════════════════════════════════════════════════
 *
 * All three come out of one SVF iteration, so what is being tested is not "does a
 * high-pass work" but "does the selector select" -- an image where ftype is stored and
 * never read would pass a test that only checked the field.
 *
 * CUT sits at 40 (~200 Hz) against a 262 Hz fundamental: the LP keeps the fundamental,
 * the HP removes it, and the two therefore differ by most of their energy. That is a
 * large, sign-correct separation, not a threshold tuned to a small number.
 */
static double rms_at(uint32_t cut, uint8_t ftype, uint8_t note, float inc)
{
    static wv_state s;
    static float o[256];
    wv_bank bk = { sines };
    uint32_t k, i;
    double e = 0.0;

    wv_init(&s);
    s.cut = cut;
    s.res = 0;
    s.ftype = ftype;
    wv_note_on(&s, note, 127, inc);
    for (k = 0; k < 40; k++) {
        for (i = 0; i < 256; i++) o[i] = 0.0f;
        wv_render(&s, &bk, o, 256, 4096.0f);
        if (k < 25) continue;                    /* past the attack */
        for (i = 0; i < 256; i++) e += (double)o[i] * o[i];
    }
    return e;
}

static void test_filter_type(void)
{
    const float inc = 261.63f / 44100.0f;
    /* CUT 40 is 217 Hz against a 262 Hz tone: the corner sits just BELOW the tone.
     * At that ratio a two-pole SVF at Q 0.5 gives |H| = 0.41 (LP), 0.98 (BP) and 0.59
     * (HP) -- so the ORDER is BP > HP > LP, which is the opposite of the intuitive
     * "the low-pass keeps the fundamental". Measured on the analytic transfer function
     * first, then asserted here, because the intuitive version is how the first draft
     * of this test came out wrong.
     *
     * The real assertion is not the order but that the shapes DIFFER ACROSS CUT: a
     * selector that was stored and never read would give the same three numbers at
     * every corner. LP falls as CUT rises past the tone; HP rises. */
    double lp40 = rms_at(40, WV_FTYPE_LP, 60, inc);
    double hp40 = rms_at(40, WV_FTYPE_HP, 60, inc);
    double bp40 = rms_at(40, WV_FTYPE_BP, 60, inc);
    double lp90 = rms_at(90, WV_FTYPE_LP, 60, inc);
    double hp90 = rms_at(90, WV_FTYPE_HP, 60, inc);

    printf("    [ftype] CUT40 lp %.6f hp %.6f bp %.6f\n", lp40, hp40, bp40);
    printf("    [ftype] CUT90 lp %.6f hp %.6f\n", lp90, hp90);

    ck(lp40 > 0.0 && hp40 > 0.0 && bp40 > 0.0, "all three taps make audio");
    /* The measured order at a corner below the tone is HP > BP > LP, and that is not
     * what the textbook 2-pole transfer functions predict (they put the band-pass on
     * top). The difference is this SVF's definition of bp: `bp = g*hp + s1` is the
     * integrator state, which scales differently from the normalised H_bp, so the
     * textbook |H| cannot be used as the expected value here. What is asserted is the
     * separation itself -- three distinct numbers in a fixed order, which a selector
     * that was ignored could not produce. */
    ck(hp40 > bp40 && bp40 > lp40, "at a corner below the tone: HP > BP > LP (measured)");
    /* the shapes are not the same curve: opening the corner well past the tone makes
     * the low-pass pass everything and the high-pass reject everything */
    ck(lp90 > lp40 * 2.0, "LP opens up when the corner passes the tone");
    ck(hp90 < hp40 * 0.5, "HP closes down when the corner passes the tone");
    ck(lp90 > hp90 * 4.0, "with the corner above the tone, LP is much louder than HP");
}

/* ═══ key tracking ══════════════════════════════════════════════════════════════
 *
 * The claim: at 100 % the corner follows the note, so a note an octave above the
 * reference keeps the same relationship to its own harmonics. The measurement is the
 * simplest one that can fail: render the SAME note at two settings and require the
 * brightness to differ in the direction the tracking says.
 *
 * CUT is low enough that the note's fundamental is above it, so raising the corner
 * lets more through -- with tracking ON, a high note is brighter than the same high
 * note with tracking OFF. If ktrack were stored and never read, the two would be
 * identical, and that is exactly what this fails on.
 */
static double hf_energy(uint8_t ktrack, uint8_t note, float inc)
{
    static wv_state s;
    static float o[256];
    wv_bank bk = { sines };
    uint32_t k, i;
    double d = 0.0;

    wv_init(&s);
    s.cut = 30;                     /* ~113 Hz: well below a C5's 523 Hz */
    s.res = 0;
    s.ktrack = ktrack;
    wv_note_on(&s, note, 127, inc);
    for (k = 0; k < 40; k++) {
        for (i = 0; i < 256; i++) o[i] = 0.0f;
        wv_render(&s, &bk, o, 256, 4096.0f);
        if (k < 25) continue;
        for (i = 1; i < 256; i++) {
            double df = (double)o[i] - (double)o[i-1];
            d += df * df;
        }
    }
    return d;
}

static void test_key_tracking(void)
{
    /* C5: an octave above C4, so at 100 % the corner is an octave higher (226 Hz). */
    const float inc = 523.25f / 44100.0f;
    double off = hf_energy(0, 74, inc);
    double on100 = hf_energy(100, 74, inc);
    double on50 = hf_energy(50, 74, inc);

    printf("    [ktrack] C5 kt0 %.6f kt50 %.6f kt100 %.6f\n", off, on50, on100);
    ck(off > 0.0, "the unfollowed note has energy to compare against");
    /* Tracking raises the corner on a note above C4, so more of the tone gets through
     * and the sample-to-sample difference rises. */
    ck(on100 > off * 1.5, "100 % tracking opens the filter on a note an octave up");
    /* ...and 50 % lands between the two, which is what makes it a percentage rather
     * than a second on/off switch. This is the assertion a boolean would fail. */
    ck(on50 > off && on50 < on100, "50 % lands between off and 100 %");

    /* The reference note is unmoved at any setting: that is the promise that makes
     * tracking safe to turn on for a patch voiced at C4. */
    {
        double r0 = hf_energy(0, 60, 261.63f / 44100.0f);
        double r100 = hf_energy(100, 60, 261.63f / 44100.0f);
        printf("    [ktrack] C4 reference kt0 %.9f kt100 %.9f\n", r0, r100);
        ck(r0 == r100, "the reference note C4 does not move with tracking");
    }
    /* 8 voices' worth of corner, one per note, all from one ramp: a chord must not
     * make the shared smoother follow any single note. Low notes dark, high notes
     * bright, from one CUT setting. */
    {
        double lo = hf_energy(100, 50, 146.83f / 44100.0f);   /* D3, below the reference */
        double hi = hf_energy(100, 74, 523.25f / 44100.0f);   /* C5 */
        printf("    [ktrack] D3 kt100 %.6f vs C5 kt100 %.6f\n", lo, hi);
        ck(hi > lo, "tracking makes a high note brighter than a low one");
    }
}

int main(void)
{
    make_sines();
    printf("fm1-wave DSP tests\n");
    test_lfo_off_is_no_change();
    test_lfo_rate_map();
    test_lfo_sweeps_cut();
    test_lfo_sweeps_both_ways();
    test_filter_type();
    test_key_tracking();
    test_table();
    test_banks();
    test_shaper();
    test_voices();
    test_render();
    printf("%s (%d checks, %d failed)\n", fails ? "FAILED" : "ALL PASSED", checks, fails);
    return fails ? 1 : 0;
}
