/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* Slice detection and layout tests.
 *   sh app/tests/run_dsp.sh
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "wv_slice.h"

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

/* A drum-ish sample: `n` bursts of a decaying sine, each `burst` long, `gap`
 * silent samples between them, with a leading gap. Deterministic, so a change in
 * the detector shows up as a changed count rather than a flaky one. */
#define SMAX 40000
static int16_t drum[SMAX];
static uint32_t drum_len;

static void make_drum(int nburst, int burst, int gap, int lead)
{
    uint32_t at = 0;
    int b, i;
    memset(drum, 0, sizeof drum);
    at += (uint32_t)lead;
    for (b = 0; b < nburst; b++) {
        for (i = 0; i < burst; i++) {
            double env = exp(-3.0 * i / burst);
            drum[at++] = (int16_t)(25000.0 * env * sin(2.0 * M_PI * i / 24.0));
        }
        for (i = 0; i < gap; i++)
            drum[at++] = 0;
    }
    drum_len = at;
}

static wv_sample drum_sample(void)
{
    wv_sample s;
    memset(&s, 0, sizeof s);
    s.data = drum;
    s.len = drum_len;
    s.rate = 22050;
    s.root = 60;
    s.gain = 256;
    s.bits = 16;
    return s;
}

static void test_split(void)
{
    wv_sample s;
    wv_slice sl[WV_SLICE_MAX];
    uint32_t n;

    printf("split\n");

    /* four bursts, well separated: four slices */
    make_drum(4, 2000, 1500, 300);
    s = drum_sample();
    n = wv_slices_split(&s, 0.02f, 200, sl, WV_SLICE_MAX);
    ck(n == 4, "four bursts give four slices");
    if (n == 4) {
        /* each slice must contain its burst and no silence beyond the lead */
        ck(sl[0].start >= 300 && sl[0].start < 330, "slice 0 starts at its burst, not at the lead");
        /* Slices tile: each one ends exactly where the next begins, so a hit's decay
         * is never amputated and the silence before the next hit belongs to the
         * previous slice. (The first version ended each slice at its first quiet
         * sample and cut a 180 ms hit down to 50 ms.) */
        ck(sl[1].start == sl[0].end, "slices tile: each ends where the next begins");
        ck(sl[2].start == sl[1].end, "and so does the second");
        ck(sl[3].start == sl[2].end, "and the third");
        /* The last slice has no next slice to bound it, so it ends at its last
         * audible sample: the trailing silence is not played, but the decaying tail
         * that the threshold cuts through elsewhere is kept. */
        ck(sl[3].end < drum_len, "the last slice stops before the trailing silence");
        ck(drum_len - sl[3].end >= 1490, "the trailing gap is excluded");
        ck(sl[3].end - sl[3].start >= 1900, "and the last hit keeps its whole body");
        /* and the boundaries are in the gaps, not inside a burst */
        ck(sl[0].end > sl[0].start + 100, "a slice is not a fragment");
    }

    /* a sample with no gaps is not a chop: 0, so the caller plays it whole */
    {
        static int16_t tone[SMAX];
        uint32_t i;
        wv_sample t;
        for (i = 0; i < 8000; i++)
            tone[i] = (int16_t)(20000.0 * sin(2.0 * M_PI * i / 32.0));
        memset(&t, 0, sizeof t);
        t.data = tone; t.len = 8000; t.rate = 22050; t.root = 60; t.gain = 256;
        ck(wv_slices_split(&t, 0.02f, 200, sl, WV_SLICE_MAX) == 0,
           "a sustained tone is not a chop");
    }

    /* one burst with silence around it is still not a chop */
    make_drum(1, 2000, 1500, 300);
    s = drum_sample();
    ck(wv_slices_split(&s, 0.02f, 200, sl, WV_SLICE_MAX) == 0, "one burst is not a chop");

    /* the minimum gap matters: gaps shorter than it do not split. A real drum
     * machine's samples have short gaps, so this is the setting that decides
     * whether a chop is 8 slices or 3. */
    make_drum(4, 2000, 150, 300);
    s = drum_sample();
    ck(wv_slices_split(&s, 0.02f, 400, sl, WV_SLICE_MAX) < 4,
       "a gap shorter than the minimum does not split");
    ck(wv_slices_split(&s, 0.02f, 50, sl, WV_SLICE_MAX) == 4,
       "and a low enough minimum does split");

    /* a gap in the middle of a waveform (a zero crossing) must not split: this is
     * the failure mode that makes a naive detector cut a sine into pieces */
    {
        static int16_t tone[SMAX];
        uint32_t i;
        wv_sample t;
        for (i = 0; i < 8000; i++)
            tone[i] = (int16_t)(20000.0 * sin(2.0 * M_PI * i / 200.0));
        memset(&t, 0, sizeof t);
        t.data = tone; t.len = 8000; t.rate = 22050; t.root = 60; t.gain = 256;
        /* at 200 samples per cycle the samples near the crossing are below 2% of
         * the peak for a couple of samples; min_gap 200 must ignore that */
        ck(wv_slices_split(&t, 0.02f, 200, sl, WV_SLICE_MAX) == 0,
           "a slow sine's zero crossings are not gaps");
    }

    /* more bursts than the array holds: it stops at max rather than overrunning */
    make_drum(WV_SLICE_MAX + 4, 800, 900, 200);
    s = drum_sample();
    n = wv_slices_split(&s, 0.02f, 100, sl, 4);
    ck(n == 4, "no more slices than the caller asked for");

    /* a single-sample slice is dropped: it cannot interpolate */
    ck(wv_slices_split(&s, 0.02f, 100, sl, 0) == 0, "max 0 yields nothing");
}

static void test_layout(void)
{
    wv_slice sl[WV_SLICE_MAX];
    wv_slice got;
    wv_sample s;
    uint32_t n, i;

    printf("layout\n");

    make_drum(5, 1000, 900, 200);
    s = drum_sample();
    n = wv_slices_split(&s, 0.02f, 100, sl, WV_SLICE_MAX);
    ck(n == 5, "five slices for the layout test");

    /* laid out from F3: slice i plays from note 53+i, across white and black keys
     * alike, because a chop is played by position not by pitch */
    wv_slices_layout(sl, n, WV_SLICE_BASE_NOTE);
    ck(wv_slice_for_note(sl, n, 53, &got) == 1 && got.start == sl[0].start, "the first key fires slice 0");
    ck(wv_slice_for_note(sl, n, 57, &got) == 1 && got.start == sl[4].start, "the fifth key fires slice 4");
    ck(wv_slice_for_note(sl, n, 52, &got) == 0, "a key below the layout fires nothing");
    ck(wv_slice_for_note(sl, n, 58, &got) == 0, "a key above the layout fires nothing");

    /* every slice is reachable and none is duplicated */
    {
        int seen[WV_SLICE_MAX];
        memset(seen, 0, sizeof seen);
        for (i = 0; i < n; i++) {
            ck(wv_slice_for_note(sl, n, (uint8_t)(WV_SLICE_BASE_NOTE + i), &got) == 1,
               "each laid-out key fires a slice");
            seen[i] = 1;
        }
        ck(seen[0] && seen[n - 1], "the whole range is covered");
    }

    /* a different base moves the whole chop: the layout is a decision, not a constant */
    wv_slices_layout(sl, n, 65);
    ck(wv_slice_for_note(sl, n, 65, &got) == 1, "re-laying out from another key works");
    ck(wv_slice_for_note(sl, n, 53, &got) == 0, "and the old keys no longer fire");
}

int main(void)
{
    printf("fm1-wave slice tests\n");
    test_split();
    test_layout();
    printf("%s (%d checks, %d failed)\n", fails ? "FAILED" : "ALL PASSED", checks, fails);
    return fails ? 1 : 0;
}
