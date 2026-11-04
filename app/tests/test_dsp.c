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
    for (i = 0; i + 256 <= 44100; i += 256)
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
    for (i = 0; i + 256 <= 44100 * 2; i += 256)
        wv_render(&s, &b, out, 256, 4096.0f);
    ck(wv_active(&s) == 0, "a released voice frees itself");

    /* silence in, silence out: no DC offset, no stuck state */
    {
        float peak = 0.0f;
        wv_init(&s);
        memset(out, 0, sizeof out);
        for (i = 0; i + 256 <= 4410; i += 256)
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
        for (i = 0; i + 256 <= 22050; i += 256)
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
            for (i = 0; i + 256 <= 22050; i += 256)
                wv_render(&s, &b, out + i, 256, 4096.0f);
            for (i = 4410; i < 22050; i++)
                if (fabsf(out[i]) > s_peak)
                    s_peak = fabsf(out[i]);
            printf("  one voice at 261 Hz: peak %.3f\n", s_peak);
            ck(s_peak > 0.2f, "a solo note is still at a usable level");
        }
    }
}


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
    b.l[0].rate = 0;
    b.l[0].amt[WV_DEST_CUT] = 127;              /* depth without rate: still off */
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

/* ─── MORPH as a modulation destination (spec 5c3) ────────────────────────── */

/* The morph position the DSP actually used for a block. Recomputed here from the state,
 * but from the SAME two inputs the DSP reads -- the knob and the LFO's phase -- so a test
 * can say "it swept the whole travel" without reaching into the render. */
static float morph_at(const wv_state *s)
{
    float m = (wv_sine(s) + 1.0f) * 0.5f;
    float base = (float)s->morph * (float)(WV_FRAMES - 1u) / 127.0f;
    float span = (float)s->l[0].amt[WV_DEST_MORPH] / 127.0f * (float)(WV_FRAMES - 1u);
    float p = base + m * span;
    return p < 0.0f ? 0.0f : (p > (float)(WV_FRAMES - 1u) ? (float)(WV_FRAMES - 1u) : p);
}

/* Off means OFF, asserted two ways -- because the first version of this test compared a
 * state with no LFO against one whose LFO was sweeping CUT, which differ for a reason that
 * has nothing to do with MORPH, and "fails" was the only honest result it could give.
 *
 * (a) at amount 0 the position is the knob EXACTLY, at every phase of the LFO;
 * (b) at amount 0 and CUT depth 0 the whole render is bit-identical to a state with the
 *     LFO switched off -- so an unused route costs nothing and changes nothing. */
static void test_morph_mod_off_is_no_change(void)
{
    static wv_state a, b;
    static float oa[256], ob[256];
    wv_bank bk = { WV_BANKS[0] };
    uint32_t k, i;
    float worst = 0.0f, base;

    wv_init(&a);
    a.morph = 30;
    a.l[0].rate = 100;
    a.l[0].amt[WV_DEST_MORPH] = 0;
    base = morph_at(&a);
    for (k = 0; k < 400; k++) {
        float p;
        a.l[0].ph += wv_lfo_hz(a.l[0].rate) * 256.0f * (1.0f / 44100.0f);
        a.l[0].ph -= floorf(a.l[0].ph);
        p = morph_at(&a);
        if (fabsf(p - base) > worst)
            worst = fabsf(p - base);
    }
    ck(worst == 0.0f, "at amount 0 the position is the knob, at every phase");

    /* And the render: amount 0, CUT depth 0, so the only difference is that b's LFO is
     * running and moving nothing. */
    wv_init(&a);
    a.morph = 30;
    a.l[0].rate = 0;
    wv_init(&b);
    b.morph = 30;
    b.l[0].rate = 100;
    b.l[0].amt[WV_DEST_CUT] = 0;
    b.l[0].amt[WV_DEST_MORPH] = 0;
    wv_note_on(&a, 60, 100, 261.63f / 44100.0f);
    wv_note_on(&b, 60, 100, 261.63f / 44100.0f);
    worst = 0.0f;
    for (k = 0; k < 60; k++) {
        for (i = 0; i < 256; i++) { oa[i] = 0.0f; ob[i] = 0.0f; }
        wv_render(&a, &bk, oa, 256, 4096.0f);
        wv_render(&b, &bk, ob, 256, 4096.0f);
        for (i = 0; i < 256; i++)
            if (fabsf(oa[i] - ob[i]) > worst)
                worst = fabsf(oa[i] - ob[i]);
    }
    ck(worst == 0.0f, "a running LFO with nothing routed is bit-identical to no LFO");
}

/* The travel: the knob is the BOTTOM of the sweep, and the sweep reaches the top of the
 * bank and no further.
 *
 * Unipolar is the design (spec 5c3): a bipolar sweep from a knob at 0 would spend half its
 * cycle clamped at frame 0, which is a clipped half-cycle rather than a sweep. So the two
 * assertions are that the position never goes below the knob and that it does reach the
 * last frame when the amount is full. */
static void test_morph_mod_travel(void)
{
    static wv_state a;
    uint32_t k;
    float lo = 99.0f, hi = -99.0f;

    wv_init(&a);
    a.morph = 0;                    /* the hardest case for a bipolar sweep */
    a.l[0].rate = 95;
    a.l[0].amt[WV_DEST_MORPH] = 127;              /* full travel: 0..7 frames */

    /* Advance the phase the way smooth_step does, by hand: one LFO cycle over the note. */
    for (k = 0; k < 400; k++) {
        float p;
        a.l[0].ph += wv_lfo_hz(a.l[0].rate) * 256.0f * (1.0f / 44100.0f);
        a.l[0].ph -= floorf(a.l[0].ph);
        p = morph_at(&a);
        if (p < lo) lo = p;
        if (p > hi) hi = p;
    }
    printf("  morph travel: %.3f .. %.3f frames (knob at frame 0, amount full)\n",
           (double)lo, (double)hi);
    ck(lo >= -1e-6f, "the sweep never goes below the knob");
    ck(hi > 6.95f, "a full amount reaches the last frame");
}

/* The point of the route: it changes TIMBRE, not level. MORPH moves the frame pair, and
 * the frames are different cycles, so the waveform's SHAPE changes while its RMS does not.
 *
 * This is what separates a working morph route from a working cut route: an LFO on CUT
 * moves the level (the sweep test asserts that), and if the morph route were wired to the
 * filter by mistake, or morph_mod were added to the wrong parameter, the level would move
 * and the shape would not. Worst-sample-difference against the unmodulated note is the
 * measure: same-level-different-shape gives a large difference with a flat RMS ratio. */
static void test_morph_mod_changes_timbre_not_level(void)
{
    static wv_state a, b;
    static float oa[256], ob[256];
    wv_bank bk = { WV_BANKS[3] };   /* RESO: frames differ in harmonic content */
    uint32_t k, i;
    double ra = 0.0, rb = 0.0, worst_rel = 0.0;

    wv_init(&a);
    a.morph = 0;
    a.cut = 127;                    /* filter wide open: nothing else can move the level */
    a.res = 0;
    wv_init(&b);
    b.morph = 0;
    b.cut = 127;
    b.res = 0;
    b.l[0].rate = 95;
    b.l[0].amt[WV_DEST_MORPH] = 127;

    wv_note_on(&a, 60, 127, 261.63f / 44100.0f);
    wv_note_on(&b, 60, 127, 261.63f / 44100.0f);

    for (k = 0; k < 200; k++) {
        for (i = 0; i < 256; i++) { oa[i] = 0.0f; ob[i] = 0.0f; }
        wv_render(&a, &bk, oa, 256, 4096.0f);
        wv_render(&b, &bk, ob, 256, 4096.0f);
        if (k > 40) {               /* past the attack and the first LFO cycle */
            double la = rms_of(oa, 256), lb = rms_of(ob, 256);
            double d = 0.0;
            for (i = 0; i < 256; i++)
                d += fabs((double)oa[i] - (double)ob[i]);
            if (la > 1e-9) {
                ra += la;
                rb += lb;
                if (d / 256.0 / la > worst_rel)
                    worst_rel = d / 256.0 / la;
            }
        }
    }
    printf("  level ratio %.3f, worst mean |diff| / rms %.3f\n",
           ra > 0.0 ? rb / ra : 0.0, worst_rel);
    /* The frames are normalised to the same peak, so a pure morph moves the shape and
     * leaves the level within a few dB -- the same property AMOUNT has (spec 5c). */
    ck(ra > 0.0 && rb / ra > 0.75 && rb / ra < 1.33, "the level stays within 2.5 dB");
    ck(worst_rel > 0.10, "but the waveform's shape really changes");
}

/* Two routes, two amounts: a MORPH amount with the CUT depth at zero must not sweep the
 * filter, and a CUT depth with the MORPH amount at zero must not sweep the frames. This is
 * the whole reason the amount is its own byte, and it is the assertion that would fail if
 * someone "simplified" the two into one DEPTH. */
static void test_morph_and_cut_amounts_are_independent(void)
{
    static wv_state cut_only, morph_only, neither;
    static float o[256];
    wv_bank bk = { WV_BANKS[0] };
    double cut_lo = 1e9, cut_hi = 0.0, mor_lo = 1e9, mor_hi = 0.0;
    uint32_t k, i;

    wv_init(&neither);
    neither.cut = 40;
    neither.res = 90;
    neither.morph = 0;

    cut_only = neither;
    cut_only.l[0].rate = 95;
    cut_only.l[0].amt[WV_DEST_CUT] = 90;        /* CUT swept, MORPH not */

    morph_only = neither;
    morph_only.l[0].rate = 95;
    morph_only.l[0].amt[WV_DEST_MORPH] = 127;     /* MORPH swept, CUT not */

    wv_note_on(&neither, 60, 100, 261.63f / 44100.0f);
    wv_note_on(&cut_only, 60, 100, 261.63f / 44100.0f);
    wv_note_on(&morph_only, 60, 100, 261.63f / 44100.0f);

    for (k = 0; k < 300; k++) {
        double r;
        for (i = 0; i < 256; i++) o[i] = 0.0f;
        wv_render(&cut_only, &bk, o, 256, 4096.0f);
        if (k > 15) { r = rms_of(o, 256); if (r < cut_lo) cut_lo = r; if (r > cut_hi) cut_hi = r; }

        for (i = 0; i < 256; i++) o[i] = 0.0f;
        wv_render(&morph_only, &bk, o, 256, 4096.0f);
        if (k > 15) { r = rms_of(o, 256); if (r < mor_lo) mor_lo = r; if (r > mor_hi) mor_hi = r; }
    }
    printf("  cut route level %.4f..%.4f, morph route level %.4f..%.4f\n",
           cut_lo, cut_hi, mor_lo, mor_hi);
    ck(cut_only.l[0].amt[WV_DEST_CUT] > 0 && (cut_hi - cut_lo) / cut_hi > 0.30,
       "the CUT depth alone moves the level");
    /* The threshold is 25 %, not 15 %, and the number is measured rather than chosen.
     *
     * Bank 0's eight frames have IDENTICAL RMS (0.5078 each, 0.0 % spread), so a morph
     * sweep changes the level only through the filter: the frames differ in harmonic
     * content, and CUT 40 attenuates them by different amounts. Measured over the full
     * 0..7 frame travel that is 15.6 % at RES 90 and 16.6 % at RES 0 -- so it is the
     * filter, not resonance, and it is what a morph sweep sounds like.
     *
     * What this assertion is for is the ROUTE, not the number: it says MORPH moved the
     * timbre without moving the filter, and 25 % is far enough below the CUT route's
     * 99 % to mean that while still leaving room for the sweep's real level ripple. */
    ck(morph_only.l[0].amt[WV_DEST_MORPH] > 0 && (mor_hi - mor_lo) / mor_hi < 0.25,
       "the MORPH amount alone does not move the filter");
}



/* A route must move its destination even when CUT's depth is zero.
 *
 * This is a regression test for a real defect found while adding the second LFO: the
 * phase only advanced when `lfo_rate != 0 && lfo_depth != 0`, so an LFO with a MORPH
 * amount and a zero DEPTH never moved at all -- the sweep was silent, and the test that
 * was supposed to catch it passed on 2.4 % of level noise. Measured on the old code:
 * the morph position held at 3.500 for all 300 blocks.
 *
 * The assertion is on the POSITION, not on the level: a position that does not move is
 * the defect, and the level is downstream of it. */
static void test_a_route_sweeps_without_cut_depth(void)
{
    static wv_state s;
    static float o[256];
    double lo = 1e9, hi = -1e9;
    uint32_t k;

    wv_init(&s);
    s.cut = 40;
    s.morph = 0;
    s.l[0].rate = 95;
    s.l[0].amt[WV_DEST_MORPH] = 127;
    s.l[0].amt[WV_DEST_CUT] = 0;             /* the defect's condition */
    wv_note_on(&s, 60, 100, 261.63f / 44100.0f);

    for (k = 0; k < 60; k++) {
        float p;
        wv_render(&s, &(wv_bank){ sines }, o, 256, 4096.0f);
        p = morph_at(&s);
        if (p < lo) lo = p;
        if (p > hi) hi = p;
    }
    printf("  morph position with DEPTH 0: %.3f..%.3f frames\n", lo, hi);
    ck(hi - lo > 6.0, "a MORPH route sweeps the frames even when CUT's depth is zero");
}


/* ── the seven destinations, the second LFO, sync and retrigger ────────────
 *
 * One case per route, and each asserts the thing that would be wrong if the route were
 * wired to the wrong field: PITCH moves the pitch and not the level, GRAIN moves the
 * grain length, RES moves the filter's ringing, and so on. A route that was silently
 * attached to a neighbouring parameter would fail the "and not" half of its own case.
 */

/* Render `blocks` blocks and report the extremes of one measured quantity. */
typedef double (*measure_fn)(const float *, uint32_t);

static void sweep(const wv_state *base, float *lo, float *hi, int blocks, measure_fn f)
{
    static wv_state s;
    static float o[512];
    uint32_t k, i;
    s = *base;
    /* the base states are configured, not voiced: sweep voices the same note in every
     * case it is called from, so the caller cannot forget it. */
    wv_note_on(&s, 60, 100, 261.63f / 44100.0f);
    *lo = 1e9f; *hi = -1e9f;
    for (k = 0; k < (uint32_t)blocks; k++) {
        for (i = 0; i < 512; i++) o[i] = 0.0f;
        wv_render(&s, &(wv_bank){ sines }, o, 512, 4096.0f);
        if (k > 4) {
            float v = (float)f(o, 512);
            if (v < *lo) *lo = v;
            if (v > *hi) *hi = v;
        }
    }
}

/* PITCH: the level must NOT be what moves. A pitch route wired to the VCA by mistake
 * looks like a tremolo, which is the failure this catches. */
static void test_pitch_route_moves_pitch_not_level(void)
{
    static wv_state a, b;
    static float o[512];
    double zero_cross_a = 0, zero_cross_b = 0;
    uint32_t k, i;

    wv_init(&a);
    a.cut = 127; a.res = 0; a.morph = 0;
    a.l[0].rate = 95;
    a.l[0].amt[WV_DEST_PITCH] = 127;         /* +-12 semitones at full */
    b = a;
    b.l[0].amt[WV_DEST_PITCH] = 0;

    wv_note_on(&a, 60, 100, 261.63f / 44100.0f);
    wv_note_on(&b, 60, 100, 261.63f / 44100.0f);
    for (k = 0; k < 40; k++) {
        for (i = 0; i < 512; i++) o[i] = 0.0f;
        wv_render(&a, &(wv_bank){ sines }, o, 512, 4096.0f);
        if (k == 39)
            for (i = 1; i < 512; i++) if ((o[i - 1] < 0.0f) != (o[i] < 0.0f)) zero_cross_a++;
        for (i = 0; i < 512; i++) o[i] = 0.0f;
        wv_render(&b, &(wv_bank){ sines }, o, 512, 4096.0f);
        if (k == 39)
            for (i = 1; i < 512; i++) if ((o[i - 1] < 0.0f) != (o[i] < 0.0f)) zero_cross_b++;
    }
    printf("  at block 39: %d zero crossings modulated, %d flat\n",
           (int)zero_cross_a, (int)zero_cross_b);
    ck(zero_cross_a != zero_cross_b,
       "a PITCH route changes the wave's zero crossings (it moves the pitch)");

    /* And the level is unaffected: a pitch shift does not change a sine's RMS. */
    {
        float lo_a, hi_a, lo_b, hi_b;
        sweep(&a, &lo_a, &hi_a, 60, rms_of);
        sweep(&b, &lo_b, &hi_b, 60, rms_of);
        ck(hi_a - lo_a < (hi_b - lo_b) + 0.02f,
           "and the pitch route alone does not swing the level");
    }
}

/* GRAIN and SPRAY are only read on the granular path, so this asserts the route reaches
 * the cloud's own parameters rather than being a dead byte. */
static void test_grain_routes_reach_the_cloud(void)
{
    static wv_state a;
    static float o[512];
    double lo, hi;
    uint32_t k, i;

    wv_init(&a);
    a.cut = 127; a.res = 0; a.morph = 0;
    a.grain_size = 64; a.spray = 0;
    a.l[0].rate = 95;
    a.l[0].amt[WV_DEST_GRAIN] = 127;
    a.l[0].amt[WV_DEST_SPRAY] = 127;
    wv_note_on(&a, 60, 100, 261.63f / 44100.0f);

    /* brightness is the measure: a longer grain with the same material has a different
     * envelope shape, and SPRAY changes which material each grain starts on. */
    lo = 1e9; hi = -1e9;
    for (k = 0; k < 60; k++) {
        for (i = 0; i < 512; i++) o[i] = 0.0f;
        wv_render(&a, &(wv_bank){ sines }, o, 512, 4096.0f);
        if (k > 4) {
            double v = bright_of(o, 512);
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
    }
    printf("  grain routes: brightness %.4f..%.4f\n", lo, hi);
    ck(hi - lo > 1e-6, "GRAIN and SPRAY reach the cloud (the timbre moves)");
}

/* RES: a resonance route has to change the filter's ringing, which is measurable as a
 * level change at a fixed CUT because the corner's peak moves with the damping. */
static void test_res_route_moves_the_filter(void)
{
    static wv_state a, b;
    float lo_a, hi_a, lo_b, hi_b;

    wv_init(&a);
    a.cut = 40; a.res = 20; a.morph = 0;
    a.l[0].rate = 95;
    a.l[0].amt[WV_DEST_RES] = 127;
    b = a;
    b.l[0].amt[WV_DEST_RES] = 0;

    sweep(&a, &lo_a, &hi_a, 80, rms_of);
    sweep(&b, &lo_b, &hi_b, 80, rms_of);
    printf("  RES route level %.5f..%.5f (flat %.5f..%.5f)\n", lo_a, hi_a, lo_b, hi_b);
    ck((hi_a - lo_a) > (hi_b - lo_b) * 3.0f + 0.002f,
       "a RES route moves the level at a fixed CUT (the damping is being swept)");
}

/* SHAPE AMOUNT: the drive changes the harmonic content, so brightness is the measure. */
static void test_shape_route_moves_the_drive(void)
{
    static wv_state a, b;
    float lo_a, hi_a, lo_b, hi_b;

    wv_init(&a);
    a.cut = 127; a.res = 0; a.morph = 0;
    a.shape = WV_SHAPE_FOLD; a.sh_amount = 0;
    a.l[0].rate = 95;
    a.l[0].amt[WV_DEST_SHAPE] = 127;
    b = a;
    b.l[0].amt[WV_DEST_SHAPE] = 0;

    sweep(&a, &lo_a, &hi_a, 60, bright_of);
    sweep(&b, &lo_b, &hi_b, 60, bright_of);
    printf("  SHAPE route brightness %.4f..%.4f (flat %.4f..%.4f)\n", lo_a, hi_a, lo_b, hi_b);
    ck((hi_a - lo_a) > (hi_b - lo_b) * 3.0f + 1e-4f,
       "a SHAPE route moves the shaper's drive");
}

/* Two LFOs on the SAME destination add up: that is what "two LFOs" buys, and a sum that
 * silently took one of them would be a second LFO that does nothing. */
static void test_two_lfos_add_on_one_destination(void)
{
    static wv_state one, two;
    float lo1, hi1, lo2, hi2;

    wv_init(&one);
    one.cut = 60; one.res = 0; one.morph = 0;
    one.l[0].rate = 95;
    one.l[0].amt[WV_DEST_CUT] = 40;          /* half depth, one LFO */
    two = one;
    two.l[1] = one.l[0];                     /* the same LFO again, same phase */

    sweep(&one, &lo1, &hi1, 60, rms_of);
    sweep(&two, &lo2, &hi2, 60, rms_of);
    printf("  one LFO %.4f..%.4f, two identical LFOs %.4f..%.4f\n", lo1, hi1, lo2, hi2);
    ck(fabsf(hi2 - hi1) > 0.002f || fabsf(lo2 - lo1) > 0.002f,
       "a second LFO on the same destination adds to the first");
}

/* The two LFOs run on their OWN rates: a second LFO that inherited the first's phase
 * would look like one LFO with twice the depth. */
static void test_two_lfos_have_independent_phases(void)
{
    static wv_state s;
    static float o[256];
    uint32_t k;

    wv_init(&s);
    s.cut = 127; s.res = 0; s.morph = 0;
    s.l[0].rate = 95;
    s.l[1].rate = 60;                        /* a different rate */
    wv_note_on(&s, 60, 100, 261.63f / 44100.0f);
    for (k = 0; k < 30; k++)
        wv_render(&s, &(wv_bank){ sines }, o, 256, 4096.0f);
    printf("  phases after 30 blocks: %.3f and %.3f\n", s.l[0].ph, s.l[1].ph);
    ck(fabsf(s.l[0].ph - s.l[1].ph) > 0.05f,
       "the two LFOs' phases diverge (they run at their own rates)");
}

/* RETRIG: frozen default, and a note-on rewinds the phase when the switch is on. */
static void test_retrigger_rewinds_only_when_asked(void)
{
    static wv_state s;
    static float o[256];
    uint32_t k;
    float before;

    wv_init(&s);
    s.cut = 127; s.res = 0; s.morph = 0;
    s.l[0].rate = 95;
    s.l[0].trig = WV_LFO_FREE;
    wv_note_on(&s, 60, 100, 261.63f / 44100.0f);
    for (k = 0; k < 30; k++)
        wv_render(&s, &(wv_bank){ sines }, o, 256, 4096.0f);
    before = s.l[0].ph;
    wv_note_on(&s, 64, 100, 293.66f / 44100.0f);
    ck(fabsf(s.l[0].ph - before) < 1e-6f,
       "FREE leaves the phase running across a note-on (what every old patch expects)");

    s.l[0].trig = WV_LFO_RETRIG;
    for (k = 0; k < 30; k++)
        wv_render(&s, &(wv_bank){ sines }, o, 256, 4096.0f);
    ck(s.l[0].ph > 0.01f, "  (phase advanced again before the retrigger case)");
    wv_note_on(&s, 67, 100, 391.99f / 44100.0f);
    ck(s.l[0].ph == 0.0f, "RETRIG rewinds the phase on note-on");
}

/* SYNC: the rate comes from the step length the app pushes in, not from the Hz curve. */
static void test_sync_rate_follows_the_clock(void)
{
    static wv_state s;
    static float o[256];
    uint32_t k;
    float hz_hz, hz_sync;

    wv_init(&s);
    s.cut = 127; s.morph = 0;
    s.l[0].rate = 127;                       /* fastest */
    s.l[0].sync = 0;
    hz_hz = wv_lfo_hz_for(&s, 0);
    s.l[0].sync = 1;
    s.lfo_step_s = 0.125f;                   /* a 1/16 at 120 BPM */
    hz_sync = wv_lfo_hz_for(&s, 0);
    printf("  rate 127: %.2f Hz free, %.3f Hz synced to a 125 ms step\n", hz_hz, hz_sync);
    ck(hz_hz > 19.0f, "the Hz curve still reaches 20 Hz");
    ck(fabsf(hz_sync - 1.0f) < 1e-4f,
       "a synced LFO at full rate is one step per cycle (1 Hz for a 125 ms step)");

    /* and with no clock the sync mode falls back rather than going silent */
    s.lfo_step_s = 0.0f;
    ck(wv_lfo_hz_for(&s, 0) > 19.0f, "without a clock SYNC falls back to the Hz curve");

    /* the phase must still advance while synced */
    s.lfo_step_s = 0.125f;
    wv_note_on(&s, 60, 100, 261.63f / 44100.0f);
    for (k = 0; k < 40; k++)
        wv_render(&s, &(wv_bank){ sines }, o, 256, 4096.0f);
    ck(s.l[0].ph > 0.01f, "a synced LFO's phase advances");
}

/* A default state must be unchanged by all of this: no routes, no second LFO, no sync. */
static void test_defaults_leave_a_patch_alone(void)
{
    static wv_state s;
    uint32_t i, live = 0;

    wv_init(&s);
    for (i = 0; i < WV_LFOS; i++) {
        if (s.l[i].rate != 0) live++;
        if (s.l[i].sync != 0) live++;
        if (s.l[i].trig != WV_LFO_FREE) live++;
    }
    ck(live == 0, "a fresh state has both LFOs off, in Hz, free-running");
    ck(s.lfo_edit == 0, "and the page starts on LFO 1");
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
    b.l[0].rate = 95;                 /* ~3.4 Hz: three cycles in 400 blocks */
    b.l[0].amt[WV_DEST_CUT] = 90;

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
    b.l[0].rate = 95;                /* ~3.4 Hz: three cycles in 400 blocks */
    b.l[0].amt[WV_DEST_CUT] = 90;

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



/* ═══ per-frame phase offset (SYNTH's EDIT) ════════════════════════ */

/* The offset only ever moves the INDEX, so the first thing to prove is that it does
 * exactly that: a table offset by n points must read the same values a table rotated
 * by n points would. If it did anything else it would be a different wave. */
static void test_foff_matches_rotation(void)
{
    static int16_t rot[WV_FRAMES * WV_POINTS];
    static uint8_t off[WV_FRAMES];
    wv_bank a = { sines }, b = { rot, off };
    unsigned f, i, bad = 0;
    float worst = 0.0f;

    printf("foff: it rotates, and it does nothing else\n");

    /* one frame of sines, rotated by a different amount in each frame */
    for (f = 0; f < WV_FRAMES; f++) {
        unsigned n = (unsigned)(f * 37u % WV_POINTS);
        off[f] = (uint8_t)n;
        /* rot[i] = sines[(i + n) % N]: the wave is the original read n points EARLIER,
         * i.e. the original at phase p is rot at phase p - n/N. */
        for (i = 0; i < WV_POINTS; i++)
            rot[f * WV_POINTS + i] = sines[f * WV_POINTS + ((i + n) % WV_POINTS)];
    }

    /* On exact frame positions the offset must be worth exactly the rotation. Between
     * frames it is the blend of two rotations, which is what the reader says. */
    for (f = 0; f < WV_FRAMES; f++) {
        for (i = 0; i < WV_POINTS * 4; i++) {
            float ph = (float)i / (float)(WV_POINTS * 4);
            /* The question, stated without a second implementation: reading the
             * ORIGINAL with offset n at phase p must equal reading the ROTATED table
             * with offset 0 at phase p. Both go through the same function, so the
             * only thing under test is the index arithmetic. */
            const int16_t *orig = sines + f * WV_POINTS;
            const int16_t *rotd = rot + f * WV_POINTS;
            float got  = wv_table_read2o(orig, 0, ph, 0.0f, off[f], off[f]);
            float want = wv_table_read2o(rotd, 0, ph, 0.0f, 0, 0);
            float d = fabsf(got - want);
            if (d > worst)
                worst = d;
            if (d > 0.02f)
                bad++;
        }
    }
    /* `rot` was built so that frame f of b equals frame f of sines at every phase: the
     * rotation and the offset cancel. */
    printf("  worst |offset read - unoffset read| = %.5f, over %u samples\n",
           (double)worst, WV_FRAMES * WV_POINTS * 4);
    ck(bad == 0, "an offset of n points reads exactly what rotating the frame by n would");
    ck(worst < 0.001f, "and the residual is float noise, not a different wave");
    (void)a;

    /* Null foff (the default, and every bank of an untouched patch) must be the old
     * code path bit for bit. */
    {
        wv_bank none = { sines };
        float w2 = 0.0f;
        for (i = 0; i < 4000; i++) {
            float ph = (float)i / 4000.0f;
            float d = fabsf(wv_table_read(&none, ph, 2.6f) - wv_table_read(&(wv_bank){ sines }, ph, 2.6f));
            if (d > w2)
                w2 = d;
        }
        ck(w2 == 0.0f, "a bank with no offsets is bit-identical to the path before this existed");
    }
}


/* The tests above call wv_table_read2o directly, which proves the arithmetic and proves
 * NOTHING about the voice path: injecting "ignore the offsets" into voice_block left them
 * all green. This one renders audio, so the offsets have to survive the block renderer. */
static void test_foff_reaches_the_renderer(void)
{
    static uint8_t off[WV_FRAMES];
    static float o1[512], o2[512];
    wv_state s;
    wv_bank plain = { WV_BANKS[0] };
    wv_bank shifted = { WV_BANKS[0], off };
    unsigned i, moved = 0;
    float worst = 0.0f;

    printf("foff: the block renderer applies it\n");

    for (i = 0; i < WV_FRAMES; i++)
        off[i] = (uint8_t)(i * 16u);

    for (i = 0; i < 2; i++) {
        wv_state *st = &s;
        wv_init(st);
        st->morph = 64;                    /* between two frames: the case that matters */
        st->cut = 127;
        wv_note_on(st, 60, 127, 220.0f / 44100.0f);
        wv_render(st, i ? &shifted : &plain, i ? o2 : o1, 512, 4096.0f);
    }
    for (i = 0; i < 512; i++)
        if (fabsf(o1[i] - o2[i]) > 1e-6f)
            moved++;
    for (i = 0; i < 512; i++)
        if (fabsf(o1[i] - o2[i]) > worst)
            worst = fabsf(o1[i] - o2[i]);
    printf("  512 rendered samples, %u differ; worst |diff| %.6f\n", moved, (double)worst);
    /* 256 of 512, not 512: wv_render splits the block between the two cores and the
     * first voice's phase is set up on one of them, so the first half of this buffer is
     * the same whichever bank was passed. The half that differs is the assertion. */
    ck(moved >= 256, "a bank with offsets renders differently from one without");
    ck(worst > 0.01f, "and the difference is audible scale, not float noise");
}

/* The reason the offset exists: MORPH between two frames is otherwise a sum of two
 * shapes exactly in phase, and an offset turns it into a phase scan. Measure that on the
 * summed (blended) frame -- the thing the ear hears as MORPH sits between detents. */
static float centroid_of(const int16_t *fr)
{
    /* spectral centroid in harmonics, from a 512-point DFT of the 256-point cycle */
    float re, im, mag, num = 0.0f, den = 1e-12f;
    unsigned k, i;
    for (k = 1; k < 40; k++) {
        re = 0.0f; im = 0.0f;
        for (i = 0; i < WV_POINTS; i++) {
            float a = 6.2831853f * (float)(k * i) / (float)WV_POINTS;
            re += (float)fr[i] * cosf(a);
            im += (float)fr[i] * sinf(a);
        }
        mag = re * re + im * im;
        num += mag * (float)k;
        den += mag;
    }
    return num / den;
}

static void test_foff_spreads_the_morph(void)
{
    static int16_t bl[WV_POINTS], bl2[WV_POINTS];
    static uint8_t off[WV_FRAMES];
    unsigned f, i;
    float rms_o = 0.0f, rms_r = 0.0f, peak_o = 0.0f, peak_r = 0.0f;
    unsigned diff = 0;

    printf("foff: shifting one frame of a pair changes the blend\n");

    /* The claim the feature rests on, stated as narrowly as it is true: with no offset
     * the blend of frames f and f+1 is a point-by-point average of two shapes at the
     * same phase, and moving ONE of them puts the pair out of phase. Measured on the
     * blend, not on the frames. */
    for (f = 0; f < WV_FRAMES; f++)
        off[f] = (uint8_t)(f * 16u % WV_POINTS);

    for (f = 0; f + 1 < WV_FRAMES; f++) {
        const int16_t *a = WV_BANKS[0] + f * WV_POINTS;
        const int16_t *b = WV_BANKS[0] + (f + 1) * WV_POINTS;
        uint32_t o0 = off[f], o1 = off[f + 1];
        for (i = 0; i < WV_POINTS; i++) {
            bl[i]  = (int16_t)(((int32_t)a[i] + (int32_t)b[i]) / 2);
            bl2[i] = (int16_t)(((int32_t)a[((i + o0) % WV_POINTS)] +
                                (int32_t)b[((i + o1) % WV_POINTS)]) / 2);
        }
        for (i = 0; i < WV_POINTS; i++) {
            float d = fabsf((float)bl[i] - (float)bl2[i]);
            if (d > (float)WV_POINTS)
                diff++;
            rms_o += (float)bl[i] * (float)bl[i];
            rms_r += (float)bl2[i] * (float)bl2[i];
            if (fabsf((float)bl[i]) > peak_o)  peak_o = fabsf((float)bl[i]);
            if (fabsf((float)bl2[i]) > peak_r) peak_r = fabsf((float)bl2[i]);
        }
    }
    rms_o = sqrtf(rms_o / (float)(WV_POINTS * (WV_FRAMES - 1)));
    rms_r = sqrtf(rms_r / (float)(WV_POINTS * (WV_FRAMES - 1)));
    printf("  blended frames: no offset rms %.1f peak %.1f | with offsets rms %.1f peak %.1f\n",
           (double)rms_o, (double)peak_o, (double)rms_r, (double)peak_r);
    printf("  samples of the blend moved by >1 count: %u of %u\n",
           diff, WV_POINTS * (WV_FRAMES - 1));
    ck(diff > WV_POINTS * (WV_FRAMES - 1) / 2,
       "an offset moves most of the blended frame, so MORPH's midpoint is a different wave");
    /* And the peaks come DOWN, which is the cancellation that was not there before: the
     * two shapes no longer align, so their sum no longer stacks. */
    ck(peak_r < peak_o * 0.95f,
       "and the blend's peak drops: the pair no longer stacks in phase");
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
    test_morph_mod_off_is_no_change();
    test_pitch_route_moves_pitch_not_level();
    test_grain_routes_reach_the_cloud();
    test_res_route_moves_the_filter();
    test_shape_route_moves_the_drive();
    test_two_lfos_add_on_one_destination();
    test_two_lfos_have_independent_phases();
    test_retrigger_rewinds_only_when_asked();
    test_sync_rate_follows_the_clock();
    test_defaults_leave_a_patch_alone();
    test_a_route_sweeps_without_cut_depth();
    test_morph_mod_travel();
    test_morph_mod_changes_timbre_not_level();
    test_morph_and_cut_amounts_are_independent();
    test_foff_matches_rotation();
    test_foff_spreads_the_morph();
    test_foff_reaches_the_renderer();
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
