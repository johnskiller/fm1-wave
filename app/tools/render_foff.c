/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* The per-frame phase offset, as a file you can A/B.
 *   sh app/tools/render_foff.sh [out.wav]
 *
 * The point is that every pair below is the SAME note, the same bank, the same MORPH
 * position and the same shaper -- the only difference is whether the eight frames carry a
 * phase offset. Anything you hear between the two halves of a pair is the offset.
 *
 * The offsets are a ramp: frame f is turned f*step points, so MORPH slides the pair
 * through each other as it moves. 32 points per frame is a quarter of a cycle between
 * neighbours, which is the setting most likely to be heard as movement rather than as a
 * new timbre; 8 and 64 bracket it.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wv_dsp.h"
#include "wv_banks.h"

#define SR  44100
#define BLK 256
#define MAXF (SR * 60)

static float buf[MAXF];
static unsigned len;

static void put16(FILE *fp, unsigned v) { fputc((int)(v & 0xFFu), fp); fputc((int)((v >> 8) & 0xFFu), fp); }
static void put32(FILE *fp, unsigned v) { put16(fp, v & 0xFFFFu); put16(fp, (v >> 16) & 0xFFFFu); }

static void wav(const char *path, const float *x, unsigned n)
{
    FILE *fp = fopen(path, "wb");
    unsigned i;
    if (!fp) { perror(path); exit(1); }
    fwrite("RIFF", 1, 4, fp); put32(fp, 36u + n * 2u);
    fwrite("WAVEfmt ", 1, 8, fp); put32(fp, 16); put16(fp, 1); put16(fp, 1);
    put32(fp, SR); put32(fp, SR * 2); put16(fp, 2); put16(fp, 16);
    fwrite("data", 1, 4, fp); put32(fp, n * 2u);
    for (i = 0; i < n; i++) {
        float v = x[i];
        short s;
        if (v > 0.891f) v = 0.891f;
        if (v < -0.891f) v = -0.891f;
        s = (short)lrintf(v * 32767.0f);
        put16(fp, (unsigned short)s);
    }
    fclose(fp);
    printf("  %-34s %5.2f s\n", path, (double)n / SR);
}

/* wv_render ADDS into `out` (wv_dsp.h: "added to what is there: the caller may mix
 * several parts"), so a buffer that is reused has to be cleared per block. The platform
 * gets this for free because its buffer is the DMA bank it just handed back; a file
 * renderer is the caller that has to remember it. Getting it wrong is not subtle once
 * measured -- five identical takes gave rms 0.17, 0.33, 0.46, 0.56, 0.62 -- and it would
 * have made every A/B in this file meaningless.
 */
static void render(wv_state *s, const wv_bank *b, unsigned frames)
{
    unsigned i, k;
    if (len + frames > MAXF) frames = MAXF - len;
    for (i = 0; i < frames; i += BLK) {
        unsigned n = frames - i < BLK ? frames - i : BLK;
        for (k = 0; k < n; k++)
            buf[len + i + k] = 0.0f;
        wv_render(s, b, buf + len + i, n, 4096.0f);
    }
    len += frames;
}

static void wav(const char *path, const float *x, unsigned n);

/* One note of exactly NOTE_LEN samples, so the two halves of a pair line up at the file's
 * midpoint and a player (or a script) can compare them sample for sample. The attack is
 * 5 ms so there is no click at the start; the release is a sixth of a second and the
 * sustain fills whatever is left. */
#define NOTE_LEN (SR * 3 / 2)

static void note(wv_state *s, const wv_bank *b, float hz)
{
    unsigned attack = SR / 200, release = SR / 6;

    wv_note_on(s, 69, 127, hz / SR);
    render(s, b, attack);
    render(s, b, NOTE_LEN - attack - release);
    wv_note_off(s, 69);
    render(s, b, release);
}

/* A pair: the same note with the offsets off, then on. Rendered one after the other so a
 * player can hear the change without touching anything. */
static const char *g_dir;
static unsigned g_n;

static void pair(const char *label, unsigned bank, uint8_t morph, uint8_t shape,
                 uint8_t amount, unsigned step)
{
    static uint8_t off[WV_FRAMES];
    wv_state s;
    unsigned f;
    wv_bank plain, shifted;

    printf("%s\n", label);

    plain.frames = WV_BANKS[bank];
    plain.foff = 0;
    for (f = 0; f < WV_FRAMES; f++)
        off[f] = (uint8_t)((f * step) % WV_POINTS);
    shifted.frames = WV_BANKS[bank];
    shifted.foff = off;

    wv_init(&s);
    s.morph = morph;
    s.shape = shape;
    s.sh_amount = amount;
    s.cut = 127;
    note(&s, &plain, 220.0f);

    wv_init(&s);
    s.morph = morph;
    s.shape = shape;
    s.sh_amount = amount;
    s.cut = 127;
    note(&s, &shifted, 220.0f);
    {
        char path[512];
        snprintf(path, sizeof path, "%s/%u.wav", g_dir, g_n++);
        printf("  %s\n", path);
        wav(path, buf, len);
        len = 0;
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s OUTDIR\n", argv[0]);
        return 2;
    }
    g_dir = argv[1];

    /* SAW SQR (bank 0): frame 0 saw, frame 7 square. MORPH at 64 sits between two
     * frames, which is the position the offset changes most -- on a frame boundary the
     * second table is not read at all. */
    pair("SAW SQR, MORPH 64, SOFT 60, 32 pt/frame  [off then on]",
         0, 64, WV_SHAPE_SOFT, 60, 32);
    pair("SAW SQR, MORPH 64, SOFT 60,  8 pt/frame  [off then on]",
         0, 64, WV_SHAPE_SOFT, 60, 8);
    pair("SAW SQR, MORPH 64, SOFT 60, 64 pt/frame  [off then on]",
         0, 64, WV_SHAPE_SOFT, 60, 64);
    /* OPEN2 (bank 6) is the one whose frames differ only in the PHASE of the odd
     * harmonics, so it is the bank where an index offset is most of what MORPH has. */
    pair("OPEN2,   MORPH 64, HARD 90, 32 pt/frame  [off then on]",
         6, 64, WV_SHAPE_HARD, 90, 32);
    /* Through FOLD, which is not monotonic: the offset moves the fold points, so the
     * difference is not just a phase. */
    pair("VOX,     MORPH 64, FOLD 100, 32 pt/frame  [off then on]",
         3, 64, WV_SHAPE_FOLD, 100, 32);

    return 0;
}
