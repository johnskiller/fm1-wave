/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* Two questions about where a granular note starts, answered with numbers.
 *
 *   cc -O2 -I app/dsp -I platform/include -o /tmp/q app/tools/grain_startq.c \
 *      app/dsp/wv_grain.c app/dsp/wv_sample.c -lm && /tmp/q
 *
 * Q1  If the playhead RESUME does what it says, a second note on percussive material
 *     starts near the end of the sample and plays the tail. How bad is "the tail"?
 *     The answer is a level, so this reads the material itself: for every start
 *     position, the RMS of the first 100 ms of the cloud at SPRAY 0 (which by the
 *     tiling property is exactly the material).
 *
 * Q2  If the playhead RESETS to 0, what happens to the scatter? `spawn` clamps a grain
 *     whose scattered position is negative, so at the start of a looping sample half the
 *     cloud has nowhere to go but the first sample. Measured as the fraction of grains
 *     that land on index 0.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "wv_grain.h"
#include "wv_sample.h"

#define LEN 9256            /* the demo kit, 11.025 kHz, 0.84 s */
#define RATE 11025

static int16_t kit[LEN];

/* A drum-ish shape: a loud transient then a decay to about 3 % of the peak. Enough to
 * answer the question, and shaped like the thing that makes the question matter. */
static void make_kit(void)
{
    int i;
    for (i = 0; i < LEN; i++) {
        double t = (double)i / RATE;
        double env = exp(-t / 0.12);
        kit[i] = (int16_t)(30000.0 * env * sin(2.0 * 3.14159265 * 220.0 * t));
    }
}

static wv_sample smp;

/* RMS of the first 100 ms of a cloud started at `from`. */
static double first100ms(uint32_t from, float spray)
{
    wv_grains gr;
    double acc = 0.0;
    int n = RATE / 10, i;

    memset(&gr, 0, sizeof gr);
    wv_grains_start(&gr, &smp, from, 0, 1.0f, 1, 999u);
    for (i = 0; i < n; i++) {
        double v = wv_grains_next(&gr, &smp, wv_grain_len(64), spray);
        acc += v * v;
    }
    return sqrt(acc / n);
}

int main(void)
{
    int i;

    make_kit();
    memset(&smp, 0, sizeof smp);
    smp.data = kit;
    smp.len = LEN;
    smp.rate = RATE;
    smp.root = 53;
    smp.bits = 16;
    smp.gain = 256;
    strcpy(smp.name, "KIT");

    printf("Q1  what the first 100 ms sounds like, by start position (SPRAY 0)\n");
    printf("    the tiling property makes this the material itself\n\n");
    printf("    start      %% of slot   RMS      dBFS    vs start of sample\n");
    for (i = 0; i <= 8; i++) {
        uint32_t from = (uint32_t)((double)LEN * i / 8.0);
        if (from >= LEN - 1)
            from = LEN - 2;
        double r = first100ms(from, 0.0f);
        printf("    %5u      %3d %%      %.5f  %6.1f   %+6.1f dB\n", from, i * 100 / 8, r,
               20.0 * log10(r + 1e-12), 20.0 * log10((r + 1e-12) / (first100ms(0, 0.0f) + 1e-12)));
    }

    printf("\nQ2  where the cloud sits just after the note starts, spray 127\n");
    printf("    (spawn clamps a scattered position below 0 onto sample 0, so a start at 0\n");
    printf("     has nowhere to scatter backwards)\n\n");
    {
        uint32_t starts[2];
        int s;
        starts[0] = 0;
        starts[1] = LEN / 2;
        for (s = 0; s < 2; s++) {
            wv_grains gr;
            int first5 = 0, total = 0, step;
            memset(&gr, 0, sizeof gr);
            wv_grains_start(&gr, &smp, starts[s], 0, 1.0f, 1, 4242u);
            for (step = 0; step < 2000; step++) {
                int g;
                wv_grains_next(&gr, &smp, wv_grain_len(64), 127.0f);
                for (g = 0; g < (int)WV_GRAINS; g++)
                    if (gr.g[g].on) {
                        total++;
                        if (gr.g[g].pos < (float)LEN * 0.05f)
                            first5++;
                    }
            }
            printf("    start %-5u  grains in the first 5%% of the material: %d/%d = %.1f %%\n",
                   starts[s], first5, total, total ? 100.0 * first5 / total : 0.0);
        }
        printf("\n    uniform would be about 5 %%: a start at the middle is near uniform,\n");
        printf("    a start at 0 is not.\n");
    }

    return 0;
}
