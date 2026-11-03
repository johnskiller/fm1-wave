/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* Render the sample path to a WAV, so it can be listened to and not only asserted
 * on. Uses the real wv_dsp/wv_sample/wv_slice and the real demo pool, so what comes
 * out is what the firmware's reader produces.
 *   sh app/tools/render_smpl.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wv_dsp.h"
#include "wv_sample.h"
#include "wv_slice.h"

#define SR      44100
#define MAXSEC  30
#define BLK     256

static float buf[MAXSEC * SR];

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

/* 16-bit stereo PCM. The data is mono in the app, so both channels carry it. */
static void write_wav(const char *path, const float *x, int n)
{
    FILE *f = fopen(path, "wb");
    int i;

    if (!f) { perror(path); exit(1); }
    fwrite("RIFF", 1, 4, f); put32(f, 36u + (uint32_t)n * 4u); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32(f, 16u); put16(f, 1u); put16(f, 2u);
    put32(f, SR); put32(f, SR * 4u); put16(f, 4u); put16(f, 16u);
    fwrite("data", 1, 4, f); put32(f, (uint32_t)n * 4u);

    for (i = 0; i < n; i++) {
        float v = x[i];
        int16_t s;
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        s = (int16_t)(v * 32767.0f);
        put16(f, (uint16_t)s);
        put16(f, (uint16_t)s);
    }
    fclose(f);
}

/* The engine in 256-frame blocks, as app_render calls it. */
static void render_at(wv_state *st, wv_bank *bk, float *dst, int n)
{
    int i;
    for (i = 0; i + BLK <= n; i += BLK)
        wv_render(st, bk, dst + i, BLK, 4096.0f);
}

static unsigned char *read_all(const char *path, long *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *p;
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END); *len = ftell(f); fseek(f, 0, SEEK_SET);
    p = malloc((size_t)*len);
    if (fread(p, 1, (size_t)*len, f) != (size_t)*len) { perror(path); exit(1); }
    fclose(f);
    return p;
}

int main(int argc, char **argv)
{
    const char *poolp = argc > 1 ? argv[1] : "app/build/gen/samples.bin";
    const char *outp  = argc > 2 ? argv[2] : "app/build/smpl.wav";
    unsigned char *pool;
    long pool_len;
    wv_sample smp[8];
    wv_slice sl[WV_SLICE_MAX];
    uint32_t nslot, ns, i;
    wv_state st;
    wv_bank bk = { 0 };
    int at = 0;

    pool = read_all(poolp, &pool_len);
    nslot = wv_smp_parse(pool, (uint32_t)pool_len, smp, 8);
    printf("pool: %u slots\n", nslot);
    if (!nslot) { free(pool); return 1; }
    for (i = 0; i < nslot; i++)
        printf("  [%u] %-8s len %-6u rate %-5u root %-3u %.2f s\n",
               i, smp[i].name, smp[i].len, smp[i].rate, smp[i].root,
               (double)smp[i].len / smp[i].rate);

    memset(&bk, 0, sizeof bk);
    memset(buf, 0, sizeof buf);

    /* 1) The four chops in order, each left to ring out. Half a second each, which is
     * longer than a chop: the point is to hear that it ends, not to pad it. */
    ns = wv_slices_split(&smp[0], 0.02f, 200, sl, WV_SLICE_MAX);
    printf("slices: %u\n", ns);
    for (i = 0; i < ns && i < 4u; i++) {
        wv_init(&st);
        printf("  chop %u: %u..%u (%u samples, %.3f s)\n", i, sl[i].start, sl[i].end,
               sl[i].end - sl[i].start, (double)(sl[i].end - sl[i].start) / smp[0].rate);
        wv_note_on_slice(&st, &smp[0], (uint8_t)(53 + i), 100, sl[i].start, sl[i].end, 0);
        render_at(&st, &bk, buf + at, SR / 2);
        at += SR / 2;
    }

    /* 2) The tone as a pitched instrument: at its root, an octave up, an octave down.
     * Same slot, same reader, only the increment changes. */
    {
        static const uint8_t notes[3] = { 57, 69, 45 };
        for (i = 0; i < 3u; i++) {
            wv_init(&st);
            wv_note_on_sample(&st, &smp[1], notes[i], 110, 0, 0, 1);
            render_at(&st, &bk, buf + at, SR * 3 / 4);
            at += SR * 3 / 4;
            wv_note_off(&st, notes[i]);
            render_at(&st, &bk, buf + at, SR / 8);
            at += SR / 8;
        }
    }

    /* 3) Four chops and the tone at once, full velocity: the bus limiter's job. */
    wv_init(&st);
    for (i = 0; i < ns && i < 4u; i++)
        wv_note_on_slice(&st, &smp[0], (uint8_t)(53 + i), 127, sl[i].start, sl[i].end, 0);
    wv_note_on_sample(&st, &smp[1], 69, 127, 0, 0, 1);
    render_at(&st, &bk, buf + at, SR);
    at += SR;

    write_wav(outp, buf, at);
    printf("%s: %.2f s\n", outp, (double)at / SR);
    free(pool);
    return 0;
}
