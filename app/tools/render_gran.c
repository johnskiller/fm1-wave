/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* Render the granular path to a WAV, so it can be listened to and not only asserted
 * on. Uses the real wv_dsp/wv_grain and the real demo pool.
 *   sh app/tools/render_gran.sh
 *
 * The material is the second slot of the demo pool (a tone, not a drum), because a
 * cloud over a pitched sound is where the parameters are audible: a drum's own
 * envelope hides grain length and hides the difference between scattering and not.
 *
 * What each section is for:
 *   1  SPRAY=0, three grain lengths. These must sound like the material, only
 *      windowed: that is the tiling property, and if it is wrong this section is a
 *      tremolo or a pile of clicks.
 *   2  GRAIN held, SPRAY swept. The sound should go from "the sample" to a texture
 *      that loses the original pitch without losing the original colour.
 *   3  SPRAY held, GRAIN swept. Small grains are a buzz at the grain rate, large
 *      grains are the sample again. If the pitch of the buzz does not change with
 *      grain size, something is quantising.
 *   4  eight voices, full spray: the bus limiter's job, and the reason the gating of
 *      this feature is a CPU measurement and not a musical one.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wv_dsp.h"
#include "wv_grain.h"
#include "wv_sample.h"

#define SR      44100
#define MAXSEC  30
#define BLK     256

static float buf[MAXSEC * SR];

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

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

/* One section: a note at `note` with the given parameters, rendered for `n` frames. */
static void section(wv_state *st, const wv_sample *s, wv_bank *bk, float *dst, int n,
                    uint8_t note, uint8_t grain, uint8_t spray, uint8_t cut)
{
    wv_init(st);
    st->grain_size = grain;
    st->spray = spray;
    st->cut = cut;
    st->res = 0;
    st->sh_amount = 0;
    st->shape = WV_SHAPE_SOFT;
    wv_note_on_grain(st, s, note, 110, 0, 0);
    render_at(st, bk, dst, n);
}

int main(int argc, char **argv)
{
    const char *poolp = argc > 1 ? argv[1] : "app/build/gen/samples.bin";
    const char *outp  = argc > 2 ? argv[2] : "app/build/gran.wav";
    unsigned char *pool;
    long pool_len;
    wv_sample smp[8];
    uint32_t nslot, i;
    wv_state st;
    wv_bank bk = { 0 };
    const wv_sample *tone;
    int at = 0;

    pool = read_all(poolp, &pool_len);
    nslot = wv_smp_parse(pool, (uint32_t)pool_len, smp, 8);
    printf("pool: %u slots\n", nslot);
    if (nslot < 2u) { printf("need a pitched slot\n"); free(pool); return 1; }
    for (i = 0; i < nslot; i++)
        printf("  [%u] %-8s len %-6u rate %-5u root %-3u %.2f s\n",
               i, smp[i].name, smp[i].len, smp[i].rate, smp[i].root,
               (double)smp[i].len / smp[i].rate);

    tone = &smp[1];
    memset(&bk, 0, sizeof bk);
    memset(buf, 0, sizeof buf);

    /* 1) SPRAY 0 at three grain lengths: the material, windowed. */
    {
        static const uint8_t gs[3] = { 80, 50, 20 };    /* ~80, ~37, ~17 ms */
        for (i = 0; i < 3u; i++) {
            printf("  spray 0, grain %u -> %u samples (%.1f ms)\n", gs[i],
                   (unsigned)wv_grain_len(gs[i]),
                   (double)wv_grain_len(gs[i]) / (SR / 1000.0));
            section(&st, tone, &bk, buf + at, SR * 2 / 3, 60, gs[i], 0, 127);
            at += SR * 2 / 3;
        }
    }

    /* 2) GRAIN held, SPRAY swept 0 -> 127 over two seconds. */
    {
        int swept = SR * 2;
        uint32_t n = 0;
        wv_init(&st);
        st.grain_size = 55;
        st.cut = 127;
        st.spray = 0;
        wv_note_on_grain(&st, tone, 60, 110, 0, 0);
        while ((int)n < swept) {
            st.spray = (uint8_t)((n * 127u) / (uint32_t)swept);
            wv_render(&st, &bk, buf + at + n, BLK, 4096.0f);
            n += BLK;
        }
        printf("  spray swept 0..127 over 2 s\n");
        at += swept;
    }

    /* 3) SPRAY 100, GRAIN swept 10 -> 120 over two seconds: the buzz's pitch should
     *    fall as the grains get longer. */
    {
        int swept = SR * 2;
        uint32_t n = 0;
        wv_init(&st);
        st.spray = 100;
        st.cut = 127;
        st.grain_size = 10;
        wv_note_on_grain(&st, tone, 60, 110, 0, 0);
        while ((int)n < swept) {
            st.grain_size = (uint8_t)(10u + (n * 110u) / (uint32_t)swept);
            wv_render(&st, &bk, buf + at + n, BLK, 4096.0f);
            n += BLK;
        }
        printf("  grain swept 10..120 over 2 s\n");
        at += swept;
    }

    /* 4) eight grain voices, full spray, FOLD: the limiter. */
    wv_init(&st);
    st.grain_size = 45;
    st.spray = 90;
    st.cut = 110;
    st.shape = WV_SHAPE_FOLD;
    st.sh_amount = 60;
    for (i = 0; i < WV_VOICES; i++)
        wv_note_on_grain(&st, i & 1u ? &smp[0] : &smp[1], (uint8_t)(48 + i * 2), 120, 0, 0);
    render_at(&st, &bk, buf + at, SR);
    printf("  eight grain voices, spray 90, FOLD\n");
    at += SR;

    write_wav(outp, buf, at);
    printf("%s: %.2f s\n", outp, (double)at / SR);
    free(pool);
    return 0;
}
