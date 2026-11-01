/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* Render a demo WAV from the real DSP: no platform, no device.
 *   sh app/tools/render_demo.sh [out.wav]
 *
 * The point is to hear the chain before the UI exists, and to hear the spec's
 * claims rather than read them: that MORPH sweeps instead of snapping, that SHAPE
 * changes character rather than volume, and that AMOUNT decides how much of the
 * shaper you get.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wv_dsp.h"
#include "wv_banks.h"

#define SR  44100
#define BLK 256
#define MAXF (SR * 32)

static float buf[MAXF];
static unsigned len;

static void put16(FILE *fp, unsigned v)
{
    fputc((int)(v & 0xFFu), fp);
    fputc((int)((v >> 8) & 0xFFu), fp);
}

static void put32(FILE *fp, unsigned v)
{
    put16(fp, v & 0xFFFFu);
    put16(fp, (v >> 16) & 0xFFFFu);
}

static void wav(const char *path, const float *x, unsigned n)
{
    FILE *fp = fopen(path, "wb");
    unsigned i;
    if (!fp) {
        perror(path);
        exit(1);
    }
    fwrite("RIFF", 1, 4, fp);
    put32(fp, 36u + n * 2u);
    fwrite("WAVEfmt ", 1, 8, fp);
    put32(fp, 16);
    put16(fp, 1);                                  /* PCM */
    put16(fp, 1);                                  /* mono */
    put32(fp, SR);
    put32(fp, SR * 2);
    put16(fp, 2);
    put16(fp, 16);
    fwrite("data", 1, 4, fp);
    put32(fp, n * 2u);
    for (i = 0; i < n; i++) {
        float v = x[i];
        short s;
        if (v > 0.891f) v = 0.891f;                /* -1 dBFS: this file is for listening */
        if (v < -0.891f) v = -0.891f;
        s = (short)lrintf(v * 32767.0f);
        put16(fp, (unsigned short)s);
    }
    fclose(fp);
    printf("%-28s %6.2f s\n", path, (double)n / SR);
}

static void render(wv_state *s, const wv_bank *b, unsigned frames)
{
    unsigned i;
    if (len + frames > MAXF)
        frames = MAXF - len;
    for (i = 0; i < frames; i += BLK) {
        unsigned n = frames - i < BLK ? frames - i : BLK;
        wv_render(s, b, buf + len + i, n, 4096.0f);
    }
    len += frames;
}

/* one note, held `hold` frames, with a short attack and a long release */
static void note(wv_state *s, const wv_bank *b, float hz, unsigned hold)
{
    unsigned attack = SR / 200, release = SR / 6;
    wv_note_on(s, 60, 127, hz / SR);
    render(s, b, attack);
    render(s, b, hold > attack ? hold - attack : 1);
    wv_note_off(s, 60);
    render(s, b, release);
}

int main(int argc, char **argv)
{
    wv_state s;

    /* 1: MORPH across bank 0, one note per step. If MORPH snapped, these would
     *    sound like eight instruments rather than one being turned. */
    {
        unsigned f;
        for (f = 0; f < WV_FRAMES; f++) {
            wv_init(&s);
            s.morph = (uint8_t)(f * 127 / (WV_FRAMES - 1));
            note(&s, &(wv_bank){ WV_BANKS[0] }, 220.0f, SR / 2);
        }
    }

    /* 2: the four shapes at full amount on the same note. If SHAPE were a volume
     *    control these would all sound alike. */
    {
        unsigned sh;
        for (sh = 0; sh < WV_SHAPE_N; sh++) {
            wv_init(&s);
            s.morph = 64;
            s.shape = (uint8_t)sh;
            s.sh_amount = 127;
            note(&s, &(wv_bank){ WV_BANKS[0] }, 220.0f, SR / 2);
        }
    }

    /* 3: AMOUNT swept under FOLD, silent to violent */
    {
        unsigned a;
        for (a = 0; a < 4; a++) {
            wv_init(&s);
            s.morph = 64;
            s.shape = WV_SHAPE_FOLD;
            s.sh_amount = (uint8_t)(a * 42);
            note(&s, &(wv_bank){ WV_BANKS[0] }, 220.0f, SR / 2);
        }
    }

    /* 4: CUT swept with RES up, the filter doing what a filter does */
    {
        unsigned c;
        for (c = 0; c < 4; c++) {
            wv_init(&s);
            s.morph = 32;
            s.res = 100;
            s.cut = (uint8_t)(40 + c * 28);
            note(&s, &(wv_bank){ WV_BANKS[0] }, 220.0f, SR / 2);
        }
    }

    /* 5: all eight banks at their mid frame, four steps each */
    {
        unsigned k;
        for (k = 0; k < WV_BANK_COUNT; k++) {
            wv_init(&s);
            s.morph = 64;
            note(&s, &(wv_bank){ WV_BANKS[k] }, 220.0f, SR / 4);
        }
    }

    /* 6: an eight-note chord at the worst setting, to hear whether the clamp
     *    reads as distortion or merely as loudness */
    {
        static const float hz[8] = { 110, 138.6f, 164.8f, 220, 277.2f, 329.6f, 440, 554.4f };
        unsigned i;
        wv_init(&s);
        s.morph = 64;
        s.shape = WV_SHAPE_FOLD;
        s.sh_amount = 127;
        for (i = 0; i < 8; i++)
            wv_note_on(&s, (uint8_t)(48 + i * 4), 127, hz[i] / SR);
        render(&s, &(wv_bank){ WV_BANKS[0] }, SR);
    }

    wav(argc > 1 ? argv[1] : "demo.wav", buf, len);
    return 0;
}
