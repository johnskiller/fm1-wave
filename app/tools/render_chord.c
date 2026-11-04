/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* Render C4, E4, G4 alone and then all three together, from the real DSP, so the
 * hardware's recording of the same four takes can be compared partial by partial.
 *
 * The question this answers is not "does it sound right" -- that is the player's ear and
 * the hardware's recording -- but "does the firmware produce the spectrum the hardware
 * produced". If the two agree to the noise floor, the chord's character is what the DSP
 * says it is, and anything left over is downstream of the render.
 *
 *   sh app/tools/render_chord.sh [out.wav]
 *
 * Same render order as the take: three single notes, each held, then the chord. Equal
 * windows, so an FFT of a single take and of the chord have the same gain.
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
    unsigned i, bytes = n * 2u;

    if (!fp) {
        perror(path);
        exit(1);
    }
    fwrite("RIFF", 1, 4, fp);
    put32(fp, 36u + bytes);
    fwrite("WAVEfmt ", 1, 8, fp);
    put32(fp, 16); put16(fp, 1); put16(fp, 1);
    put32(fp, SR); put32(fp, SR * 2u); put16(fp, 2); put16(fp, 16);
    fwrite("data", 1, 4, fp);
    put32(fp, bytes);
    for (i = 0; i < n; i++) {
        float v = x[i];
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        put16(fp, (unsigned)((int)(v * 32767.0f)) & 0xFFFFu);
    }
    fclose(fp);
}

/* the MIDI note for a frequency, so the pitch comes from the note table the panel uses
 * rather than from a ratio computed here */
static float hz_of(int note)
{
    return 440.0f * powf(2.0f, (float)(note - 69) / 12.0f);
}

static void render(wv_state *s, const wv_bank *b, unsigned frames)
{
    unsigned done = 0;

    while (done < frames) {
        unsigned n = frames - done;
        if (n > BLK) n = BLK;
        wv_render(s, b, buf + len + done, n, 4096.0f);
        done += n;
    }
    len += frames;
}

int main(int argc, char **argv)
{
    static wv_state s;
    wv_bank b;
    const char *out = argc > 1 ? argv[1] : "chord.wav";
    const int notes[3] = { 60, 64, 67 };       /* C4, E4, G4 */
    const unsigned HOLD = SR * 2u;             /* the take holds each note ~2 s */
    int i;

    memset(buf, 0, sizeof buf);
    memset(&s, 0, sizeof s);
    wv_init(&s);
    b.frames = WV_BANKS[0];
    b.foff = 0;                                /* the take did not touch the EDIT page */

    /* three singles, one at a time, in the take's order */
    for (i = 0; i < 3; i++) {
        wv_note_on(&s, (uint8_t)notes[i], 127, hz_of(notes[i]) / (float)SR);
        render(&s, &b, HOLD);
        wv_note_off(&s, (uint8_t)notes[i]);
        render(&s, &b, SR / 10u);              /* the gap between takes */
    }

    /* then all three together, held */
    for (i = 0; i < 3; i++)
        wv_note_on(&s, (uint8_t)notes[i], 127, hz_of(notes[i]) / (float)SR);
    render(&s, &b, SR * 5u);

    wav(out, buf, len);
    printf("%s: %u frames, %.2f s\n", out, len, (double)len / SR);
    return 0;
}
