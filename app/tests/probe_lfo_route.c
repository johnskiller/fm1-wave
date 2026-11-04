/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller
 *
 * Does a route actually move the sound?
 *
 * The player asked "UI现在连接DSP了吗？" before going to listen in the emu. The panel writes
 * lfo_amt into dsp.l[i].amt and ui_apply() copies it there -- but a byte landing in a struct
 * is not the same thing as a route that changes what comes out of the speaker. So this
 * renders the same note twice, once with the route at 0 and once with it at 100, and
 * compares the audio.
 *
 * A throwaway probe rather than a suite test: test_lfo_off_is_no_change already proves that
 * amount 0 is bit-identical to no LFO at all. What is missing is the other half -- that
 * amount 100 is NOT identical, and in the way the destination implies. CUT is a filter
 * corner, so an oscillating CUT must change brightness; if it only changed level it would be
 * a VCA wearing the wrong label.
 *
 * build: cc -O2 -ffp-contract=off -std=c99 -I app/dsp -o /tmp/probe app/tests/probe_lfo_route.c \
 *            app/dsp/wv_dsp.c app/dsp/wv_sample.c app/dsp/wv_slice.c app/dsp/wv_grain.c -lm
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "wv_dsp.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define N       256
#define BLOCKS  64          /* 64 x 256 = 16384 samples = 0.37 s at 44.1 k */

static int16_t sines[WV_FRAMES * WV_POINTS];
static float   a[N * BLOCKS], b[N * BLOCKS];

/* A SAW, not a sine. The first version used sines and the measurement could not work:
 * a sine has no harmonics, so a moving filter corner only changes its level a little and
 * brightness barely moves. The question is whether CUT is a filter, and a filter is only
 * visible on something with harmonics to remove. */
static void make_bank(void)
{
    int f, i;
    for (f = 0; f < WV_FRAMES; f++)
        for (i = 0; i < WV_POINTS; i++) {
            double ph = (double)i / (double)WV_POINTS;
            sines[f * WV_POINTS + i] = (int16_t)(28000.0 * (2.0 * ph - 1.0));
        }
}

static float rms(const float *x, int n)
{
    double acc = 0.0;
    int i;
    for (i = 0; i < n; i++)
        acc += (double)x[i] * (double)x[i];
    return (float)sqrt(acc / (double)n);
}

/* Energy above the fundamental, by a cheap proxy: the mean absolute first difference grows
 * with high-frequency content. Closing a filter corner kills the harmonics and this falls;
 * a pure gain change leaves it proportional to rms. Reported as the ratio to rms so a level
 * change alone cannot pass for a brightness change. */
static float brightness(const float *x, int n)
{
    double acc = 0.0;
    int i;
    for (i = 1; i < n; i++)
        acc += fabs((double)x[i] - (double)x[i - 1]);
    return (float)(acc / (double)(n - 1));
}

static void play(wv_state *s, const wv_bank *bk, float *out)
{
    int i;
    for (i = 0; i < BLOCKS; i++)
        wv_render(s, bk, out + i * N, N, 4096.0f);  /* 4096 = unity */
}

int main(void)
{
    wv_state sa, sb;
    wv_bank bk;
    float ba, bb, na, nb;
    int i, differ = 0;

    make_bank();
    bk.frames = sines;
    bk.foff = NULL;

    wv_init(&sa);
    wv_init(&sb);

    /* Identical in every field but one, and RATE high enough that a 0.37 s render covers
     * several cycles -- a slow LFO over a short render would look like "no difference" for
     * the boring reason. CUT starts mid-travel so the sweep has room both ways. */
    sa.cut = sb.cut = 64;
    sa.sh_amount = sb.sh_amount = 0;
    sa.l[0].rate = sb.l[0].rate = 127;          /* 20 Hz */
    sa.l[0].shape = sb.l[0].shape = WV_LFOSH_SINE;
    sa.l[0].amt[WV_DEST_CUT] = 0;
    sb.l[0].amt[WV_DEST_CUT] = 100;             /* the one difference */

    wv_note_on(&sa, 57, 127, 220.0f / 44100.0f);
    wv_note_on(&sb, 57, 127, 220.0f / 44100.0f);

    play(&sa, &bk, a);
    play(&sb, &bk, b);

    for (i = 0; i < N * BLOCKS; i++)
        if (a[i] != b[i])
            differ++;

    na = rms(a, N * BLOCKS); ba = brightness(a, N * BLOCKS);
    nb = rms(b, N * BLOCKS); bb = brightness(b, N * BLOCKS);

    printf("amt=0    rms %.5f  bright/rms %.5f\n", na, na > 0 ? ba / na : 0.0f);
    printf("amt=100  rms %.5f  bright/rms %.5f\n", nb, nb > 0 ? bb / nb : 0.0f);
    printf("samples differing: %d of %d\n", differ, N * BLOCKS);

    if (na == 0.0f) {
        printf("VERDICT: SILENT -- the render produced nothing, nothing was compared\n");
        return 1;
    }
    if (differ == 0) {
        printf("VERDICT: NO CHANGE -- the route does nothing\n");
        return 1;
    }
    {
        /* Relative, not absolute: these are ratios around 0.03, so an absolute threshold of
         * 0.05 could never be crossed by any real change. */
        float rel = (na > 0 && nb > 0) ? fabsf((bb / nb) - (ba / na)) / (ba / na) : 0.0f;
        printf("brightness change: %.1f%%\n", rel * 100.0f);
        printf("VERDICT: the CUT route changes the sound%s\n",
               (rel > 0.05f) ? ", and changes its brightness (so it is the filter, not a gain)"
                             : ", but not its brightness -- suspect");
    }
    return 0;
}
