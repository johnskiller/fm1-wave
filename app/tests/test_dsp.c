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

    /* drive: amount 0 is unity, full amount is much hotter */
    ck(fabsf(wv_shaper(WV_SHAPE_SOFT, 0.5f, 0.0f) - wv_shape_curve(WV_SHAPE_SOFT, 0.5f)) < 1e-5f,
       "amount 0 leaves the signal alone");
    ck(wv_shaper(WV_SHAPE_HARD, 0.5f, 1.0f) > wv_shaper(WV_SHAPE_HARD, 0.5f, 0.1f),
       "more amount drives the shaper harder");
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

int main(void)
{
    make_sines();
    printf("fm1-wave DSP tests\n");
    test_table();
    test_banks();
    test_shaper();
    test_voices();
    test_render();
    printf("%s (%d checks, %d failed)\n", fails ? "FAILED" : "ALL PASSED", checks, fails);
    return fails ? 1 : 0;
}
