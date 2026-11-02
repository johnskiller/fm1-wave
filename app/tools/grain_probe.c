/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* How many grains are actually sounding, and where are they?
 *
 * The screen draws one tick per live grain, so "how many ticks" is a DSP question before
 * it is a drawing question. With WV_GRAIN_OVERLAP 2 the cloud should be steady at two
 * grains once it has started -- sin²/cos² sum to 1 with exactly two. If the drawing shows
 * one, either the DSP has one or the two are at the same position and their ticks overlap.
 *
 *   cc -O2 -I app/dsp -o /tmp/gq app/tools/grain_probe.c app/dsp/wv_grain.c \
 *      app/dsp/wv_sample.c -lm && /tmp/gq
 */
#include <stdio.h>
#include <string.h>

#include "wv_grain.h"
#include "wv_sample.h"

static float blob[1 << 20];

/* a 1 s ramp-free tone at 11.025 kHz, the demo pool's rate, so pos/len is meaningful */
static uint32_t make_tone(wv_sample *s)
{
    static int16_t data[11025];
    int i;
    for (i = 0; i < 11025; i++)
        data[i] = (int16_t)(12000.0 * (i < 30 ? (float)i / 30.0 : 1.0));   /* a short fade-in */
    s->data = data;
    s->len = 11025;
    s->rate = 11025;
    s->root = 57;
    s->bits = 16;
    s->gain = 256;
    strcpy(s->name, "TONE");
    return s->len;
}

int main(void)
{
    wv_sample s;
    wv_grains gr;
    uint32_t n = make_tone(&s);
    int i, sample, spray;

    (void)blob;

    /* SPRAY 0: the tiling case, two grains half a grain apart */
    memset(&gr, 0, sizeof gr);
    wv_grains_start(&gr, &s, 0, n, 1.0f, 1, 12345u);

    printf("grain length 64 ms -> %.0f output samples, hop %.0f\n",
           wv_grain_len(64), wv_grain_len(64) / 2.0f);
    printf("\n  n     on   positions (samples)            gaps\n");
    for (sample = 0; sample < 9000; sample++) {
        float y = wv_grains_next(&gr, &s, wv_grain_len(64), 0.0f);
        (void)y;
        if (sample % 900 == 0) {
            uint32_t on = wv_grains_on(&gr);
            printf("%5d  %3u   ", sample, on);
            for (i = 0; i < (int)WV_GRAINS; i++)
                if (gr.g[i].on)
                    printf("%8.1f(t=%.2f) ", gr.g[i].pos, gr.g[i].t);
            if (on == 2) {
                float a = -1, b = -1;
                for (i = 0; i < (int)WV_GRAINS; i++)
                    if (gr.g[i].on) {
                        if (a < 0) a = gr.g[i].pos; else b = gr.g[i].pos;
                    }
                printf(" gap %.1f", b - a);
            }
            printf("\n");
        }
    }

    /* the tick size question: how far does a grain travel in 176 px of a 1 s slot */
    /* SPRAY on: the ticks must spread. This is the whole reason the picture has ticks. */
    for (spray = 0; spray <= 127; spray = spray ? spray * 4 : 1) {
        if (spray > 127) spray = 127;
        memset(&gr, 0, sizeof gr);
        wv_grains_start(&gr, &s, 0, n, 1.0f, 1, 12345u);
        for (sample = 0; sample < 4000; sample++) {
            float y = wv_grains_next(&gr, &s, wv_grain_len(64), (float)spray);
            (void)y;
        }
        printf("spray %3d: on=%u  ", spray, wv_grains_on(&gr));
        for (i = 0; i < (int)WV_GRAINS; i++)
            if (gr.g[i].on)
                printf("pos %6.0f (%3.0f px) ", gr.g[i].pos, gr.g[i].pos / ((double)n / 176.0));
        printf("\n");
        if (spray == 127) break;
    }

    printf("\n176 px over %u samples = %.1f samples/px\n", n, (double)n / 176.0);
    printf("a 64 ms grain at 1x covers %.0f samples = %.1f px\n",
           wv_grain_len(64), wv_grain_len(64) / ((double)n / 176.0));
    printf("two grains of a tiling pair are %.0f samples apart = %.1f px\n",
           wv_grain_len(64) / 2.0, (wv_grain_len(64) / 2.0) / ((double)n / 176.0));
    return 0;
}
