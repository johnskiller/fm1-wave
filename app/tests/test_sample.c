/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* Sample reader tests: parser and reader under plain cc.
 *   sh app/tests/run_dsp.sh    (runs both test binaries)
 *
 * The blob is built here byte by byte rather than by a Python tool, so this file
 * is also the format's executable specification: if the layout in wv_sample.h
 * changes, the bytes below are what has to change with it.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "wv_dsp.h"
#include "wv_sample.h"

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

/* ─── building a blob ─────────────────────────────────────────────────────── */

typedef struct { int rate, bits, root, gain; const char *name; } slot_desc;

#define BLOB_MAX 16384
static uint8_t blob[BLOB_MAX];

/* Writes `nslot` slots, each `n` samples of a sine at its own frequency, and
 * returns the blob's length. `n` is per slot (varying lengths is the point). */
static uint32_t build_blob(const slot_desc *d, const int *ns, int nslot)
{
    uint32_t at;
    int i, k;
    memset(blob, 0, sizeof blob);
    memcpy(blob, "FMWV", 4);
    blob[4] = WV_SMP_VERSION;
    blob[5] = (uint8_t)nslot;
    at = 8u + (uint32_t)nslot * WV_SMP_ENTRY;
    for (i = 0; i < nslot; i++) {
        uint8_t *e = blob + 8 + i * WV_SMP_ENTRY;
        uint32_t off = at;
        for (k = 0; k < 4; k++)
            e[k] = (uint8_t)(off >> (8 * k));
        for (k = 0; k < 4; k++)
            e[4 + k] = (uint8_t)((uint32_t)ns[i] >> (8 * k));
        e[8] = (uint8_t)(d[i].rate & 0xFF);
        e[9] = (uint8_t)(d[i].rate >> 8);
        e[10] = (uint8_t)d[i].bits;
        e[11] = (uint8_t)d[i].root;
        e[12] = (uint8_t)(d[i].gain & 0xFF);
        e[13] = (uint8_t)((unsigned)d[i].gain >> 8);
        {
            /* the name is padded with spaces (0x20), which is what the tools write.
             * Copied with strncpy-style semantics: reading name[k] past its NUL is
             * a real overrun of a string literal, and ASan found it here. */
            size_t nl = d[i].name ? strlen(d[i].name) : 0;
            for (k = 0; k < 6; k++)
                e[14 + k] = (uint8_t)((size_t)k < nl ? d[i].name[k] : ' ');
        }
        for (k = 0; k < ns[i]; k++) {
            double v = sin(2.0 * M_PI * (k % 32) / 32.0) * 30000.0;
            int16_t s = (int16_t)v;
            blob[at + 2 * k] = (uint8_t)((uint16_t)s & 0xFF);
            blob[at + 2 * k + 1] = (uint8_t)((uint16_t)s >> 8);
        }
        at += (uint32_t)ns[i] * 2u;
    }
    return at;
}

/* ─── the parser ──────────────────────────────────────────────────────────── */

static void test_parse(void)
{
    static wv_sample s[WV_SMP_MAX];
    slot_desc d[3];
    int ns[3];
    uint32_t len, n;

    printf("parse\n");

    d[0] = (slot_desc){ 22050, 16, 60, 256, "HIT" };
    d[1] = (slot_desc){ 11025, 16, 48, 128, "LOOP1" };
    d[2] = (slot_desc){ 44100, 16, 72, 512, "TICK" };
    ns[0] = 1200; ns[1] = 800; ns[2] = 40;
    len = build_blob(d, ns, 3);

    n = wv_smp_parse(blob, len, s, WV_SMP_MAX);
    ck(n == 3, "three slots parse");
    ck(s[0].len == 1200 && s[1].len == 800 && s[2].len == 40, "each slot keeps its own length");
    ck(s[0].rate == 22050 && s[1].rate == 11025 && s[2].rate == 44100, "each slot keeps its own rate");
    ck(s[0].root == 60 && s[1].root == 48 && s[2].root == 72, "root notes");
    ck(s[0].gain == 256 && s[1].gain == 128 && s[2].gain == 512, "gains are signed Q8");
    /* The name is padded to 6 with spaces. Trailing spaces are kept: they are a
     * legitimate character, and trimming them would be the display's business, not
     * the parser's. What the parser must do is stop at the first non-printable. */
    ck(strncmp(s[0].name, "HIT", 3) == 0, "the name is read");
    ck(strlen(s[0].name) == 6, "and padded to six characters with spaces");
    ck(strcmp(s[1].name, "LOOP1 ") == 0, "a full-length name keeps its padding");
    ck(s[1].data && s[2].data && s[0].data, "data pointers are filled");

    /* the samples themselves: s[0] is a 32-sample sine, so its first peak is at 8 */
    ck(s[0].data[0] == 0, "sample 0 starts at 0");
    ck(s[0].data[8] > 29000, "sample 8 is the sine's peak");

    /* Every rejection, and each is a way the file can be wrong in practice */
    ck(wv_smp_parse(0, len, s, WV_SMP_MAX) == 0, "null blob");
    ck(wv_smp_parse(blob, 4, s, WV_SMP_MAX) == 0, "truncated before the header");
    {
        uint8_t t[BLOB_MAX];
        memcpy(t, blob, 8);
        t[0] = 'X';
        ck(wv_smp_parse(t, len, s, WV_SMP_MAX) == 0, "wrong magic");
        memcpy(t, blob, 8);
        t[4] = 99;
        ck(wv_smp_parse(t, len, s, WV_SMP_MAX) == 0, "wrong version");
        memcpy(t, blob, 8);
        t[5] = 0;
        ck(wv_smp_parse(t, len, s, WV_SMP_MAX) == 0, "zero slots");
        memcpy(t, blob, 8);
        t[5] = WV_SMP_MAX + 1;
        ck(wv_smp_parse(t, len, s, WV_SMP_MAX) == 0, "more slots than the format allows");
    }

    /* an entry whose samples run past the end of the blob: the failure mode of a
     * write cut short, and it must not hand out a pointer into nothing */
    {
        uint8_t t[BLOB_MAX];
        memcpy(t, blob, len);
        t[8 + 4] = 0xFF;                       /* slot 0's len */
        t[8 + 5] = 0x7F;
        ck(wv_smp_parse(t, len, s, WV_SMP_MAX) == 0, "a slot longer than the blob is refused");
    }
    /* and one that points into the header */
    {
        uint8_t t[BLOB_MAX];
        memcpy(t, blob, len);
        t[8] = 0;
        t[9] = 0;
        t[10] = 0;
        t[11] = 0;
        ck(wv_smp_parse(t, len, s, WV_SMP_MAX) == 0, "a slot pointing into the header is refused");
    }
    /* an impossible rate */
    {
        uint8_t t[BLOB_MAX];
        memcpy(t, blob, len);
        t[8 + 8] = 0; t[8 + 9] = 0;
        ck(wv_smp_parse(t, len, s, WV_SMP_MAX) == 0, "rate 0 is refused");
    }

    /* the count clamp: a blob with 8 slots asked into a buffer of 2 is refused, not
     * half-filled */
    {
        slot_desc many[WV_SMP_MAX];
        int many_n[WV_SMP_MAX];
        int i;
        for (i = 0; i < WV_SMP_MAX; i++) {
            many[i] = (slot_desc){ 16000, 16, 60, 256, "S" };
            many_n[i] = 16;
        }
        len = build_blob(many, many_n, WV_SMP_MAX);
        ck(wv_smp_parse(blob, len, s, 2) == 0, "more slots than the caller's buffer is refused");
    }
}

/* ─── the increment ───────────────────────────────────────────────────────── */

static void test_inc(void)
{
    wv_sample s;
    memset(&s, 0, sizeof s);

    printf("increment\n");

    /* an 11.025 kHz slot at its root reads a quarter of the engine's rate */
    s.rate = 11025; s.root = 60;
    ck(fabsf(wv_smp_inc(&s, 60.0f) - 0.25f) < 1e-5f, "11.025 kHz at its root reads 0.25 x 44.1k");
    /* an octave up doubles, exactly: the reason 11025 was chosen */
    ck(fabsf(wv_smp_inc(&s, 72.0f) - 0.5f) < 1e-4f, "an octave up doubles the increment");
    ck(fabsf(wv_smp_inc(&s, 48.0f) - 0.125f) < 1e-5f, "an octave down halves it");

    /* a 22.05 kHz slice at its root reads at 1x — a slice is not transposed */
    s.rate = 22050; s.root = 60;
    ck(fabsf(wv_smp_inc(&s, 60.0f) - 0.5f) < 1e-5f, "22.05 kHz at its root reads 0.5");

    /* a 44.1 kHz slot is exactly 1.0: sample-for-sample, for material recorded at
     * the engine's own rate */
    s.rate = 44100; s.root = 60;
    ck(fabsf(wv_smp_inc(&s, 60.0f) - 1.0f) < 1e-6f, "44.1 kHz at its root reads exactly 1.0");

    /* the root matters independently of the rate: the same material declared an
     * octave higher must read twice as fast to sound the same */
    s.rate = 22050;
    {
        float a = wv_smp_inc(&s, 72.0f);   /* root 60 */
        s.root = 72;
        ck(fabsf(wv_smp_inc(&s, 72.0f) - 0.5f) < 1e-5f, "at its own root a slot reads its own rate");
        ck(a > wv_smp_inc(&s, 72.0f), "declaring the root higher makes the same note read slower");
    }
}

/* ─── the reader ──────────────────────────────────────────────────────────── */

static int16_t ramp[64];      /* 0, 1000, 2000, ... a straight line: easy to check */

static void make_ramp(void)
{
    int i;
    for (i = 0; i < 64; i++)
        ramp[i] = (int16_t)(i * 1000);
}

static wv_sample ramp_sample(void)
{
    wv_sample s;
    memset(&s, 0, sizeof s);
    s.data = ramp;
    s.len = 64;
    s.rate = 44100;
    s.root = 60;
    s.gain = 256;
    s.bits = 16;
    return s;
}

static void test_reader(void)
{
    wv_sample s = ramp_sample();
    wv_reader r;
    int done, i;

    printf("reader\n");

    /* inc 1.0 reads the samples one after another */
    wv_reader_start(&r, &s, 0, 1.0f);
    ck(fabsf(wv_reader_next(&r, &s, &done) - 0.0f) < 1e-4f, "sample 0");
    ck(fabsf(wv_reader_next(&r, &s, &done) - 1000.0f / 32768.0f) < 1e-4f, "sample 1");
    ck(!done, "not done yet");

    /* inc 0.5 interpolates halfway: the whole reason for linear interpolation */
    wv_reader_start(&r, &s, 0, 0.5f);
    wv_reader_next(&r, &s, &done);
    ck(fabsf(wv_reader_next(&r, &s, &done) - 500.0f / 32768.0f) < 1e-4f,
       "inc 0.5 lands halfway between samples 0 and 1");

    /* inc 2.0 skips every other sample */
    wv_reader_start(&r, &s, 0, 2.0f);
    wv_reader_next(&r, &s, &done);
    ck(fabsf(wv_reader_next(&r, &s, &done) - 2000.0f / 32768.0f) < 1e-4f, "inc 2.0 skips one");

    /* a start offset begins at that sample, not at 0 */
    wv_reader_start(&r, &s, 10, 1.0f);
    ck(fabsf(wv_reader_next(&r, &s, &done) - 10000.0f / 32768.0f) < 1e-4f, "a start offset lands on its sample");

    /* a one-shot stops at the end and stays stopped: a slice that kept reading past
     * its data would read the next slot's samples, or past the blob */
    wv_reader_start(&r, &s, 60, 1.0f);
    for (i = 0; i < 10; i++)
        wv_reader_next(&r, &s, &done);
    ck(done, "a one-shot sets done at the end");
    ck(wv_reader_next(&r, &s, &done) == 0.0f, "and stays silent");
    ck(done, "done stays set");

    /* a loop wraps without a gap: inc 1.0 over a 4-sample loop reads 0,1,2,3,0,1.. */
    wv_reader_start(&r, &s, 8, 1.0f);
    wv_reader_loop(&r, 8, 12);
    {
        float got[8];
        int ever_done = 0;
        for (i = 0; i < 8; i++) {
            done = 0;                      /* fresh each call: `done` is an output, and the
                                            * previous case above left it set */
            got[i] = wv_reader_next(&r, &s, &done) * 32768.0f;
            if (done)
                ever_done = 1;
        }
        ck(fabsf(got[0] - 8000.0f) < 500.0f && fabsf(got[3] - 11000.0f) < 500.0f,
           "the loop starts where it was told");
        ck(fabsf(got[4] - 8000.0f) < 500.0f, "and wraps back to its start");
        ck(fabsf(got[5] - 9000.0f) < 500.0f, "and continues from there, not from 0");
        ck(!ever_done, "a looping reader never sets done");
    }

    /* a loop shorter than two samples is ignored rather than made to divide by zero */
    wv_reader_start(&r, &s, 8, 1.0f);
    wv_reader_loop(&r, 8, 9);
    ck(r.loop_end < 0, "a one-sample loop is refused");

    /* the slot's gain scales the output: Q8, so 128 is half */
    s.gain = 128;
    wv_reader_start(&r, &s, 8, 1.0f);
    ck(fabsf(wv_reader_next(&r, &s, &done) - 8000.0f / 32768.0f * 0.5f) < 1e-3f,
       "gain 128 halves the output");
    s.gain = 0;
    wv_reader_start(&r, &s, 8, 1.0f);
    ck(wv_reader_next(&r, &s, &done) == 0.0f, "gain 0 silences the slot");
    s.gain = 256;

    /* reverse: a granular grain running backwards walks down the ramp */
    wv_reader_start(&r, &s, 20, 1.0f);
    r.dir = -1;
    ck(fabsf(wv_reader_next(&r, &s, &done) - 20000.0f / 32768.0f) < 1e-3f, "reverse starts at its position");
    {
        float a = wv_reader_next(&r, &s, &done);
        ck(a < 20000.0f / 32768.0f, "and walks downwards");
    }
    /* reverse with no loop runs off the start and stops */
    wv_reader_start(&r, &s, 1, 1.0f);
    r.dir = -1;
    wv_reader_next(&r, &s, &done);
    wv_reader_next(&r, &s, &done);
    ck(done, "reverse without a loop stops at 0");
}

/* the last sample holds instead of wrapping to sample 0: wrapping there is a click
 * at the end of every one-shot */
static void test_endpoint(void)
{
    static int16_t sq[8] = { 30000, -30000, 30000, -30000, 30000, -30000, 30000, -30000 };
    wv_sample s;
    wv_reader r;
    int done;

    printf("endpoint\n");
    memset(&s, 0, sizeof s);
    s.data = sq; s.len = 8; s.rate = 44100; s.root = 60; s.gain = 256;

    wv_reader_start(&r, &s, 6, 1.0f);
    wv_reader_next(&r, &s, &done);                 /* 6 */
    wv_reader_next(&r, &s, &done);                 /* 7 */
    ck(fabsf(wv_reader_next(&r, &s, &done)) < 1e-6f, "the end returns 0 and sets done");
    ck(done, "done is set at the last sample");
}


/* ─── range: a slice stops at its own end ─────────────────────────────────── */

static void test_range(void)
{
    int16_t data[100];
    wv_sample s;
    wv_reader r;
    float v = 0.0f;
    int done = 0, n, seen_past = 0;

    printf("range\n");
    for (n = 0; n < 100; n++)
        data[n] = (int16_t)(10000 - n * 100);      /* a ramp: position is readable */
    memset(&s, 0, sizeof s);
    s.data = data; s.len = 100; s.rate = 11025; s.bits = 16; s.root = 60;
    s.gain = 256;

    /* 20..40 at 1x: about 20 output samples, whatever the slot's length */
    wv_reader_start(&r, &s, 20, 1.0f);
    wv_reader_range(&r, 20, 40);
    for (n = 0; n < 200 && !done; n++) {
        v = wv_reader_next(&r, &s, &done);
        if (r.pos > 40.5f)
            seen_past = 1;
    }
    ck(!seen_past, "range: the reader never walks past its end");
    ck(n >= 19 && n <= 22, "range: 20 samples at 1x takes about 20 calls");

    /* inside the range it reads the right sample: data[29] after ten steps */
    wv_reader_start(&r, &s, 20, 1.0f);
    wv_reader_range(&r, 20, 40);
    done = 0;
    for (n = 0; n < 9 && !done; n++)
        v = wv_reader_next(&r, &s, &done);
    v = wv_reader_next(&r, &s, &done);
    ck(fabsf(v - 7100.0f / 32768.0f) < 0.003f, "range: reads the right sample inside it");

    /* end == len must behave exactly like no range: no life lost, no value changed */
    {
        wv_reader a, b;
        int fa = 0, fb = 0, k;
        float va = 0, vb = 0;

        wv_reader_start(&a, &s, 0, 1.0f);
        wv_reader_start(&b, &s, 0, 1.0f);
        wv_reader_range(&b, 0, 100);
        for (k = 0; k < 150; k++) {
            va = wv_reader_next(&a, &s, &fa);
            vb = wv_reader_next(&b, &s, &fb);
            if (fa != fb || (!fa && fabsf(va - vb) > 1e-6f))
                break;
        }
        ck(k == 150, "range: end==len is indistinguishable from no range");
    }

    /* a range that would be inverted is refused, not obeyed */
    wv_reader_start(&r, &s, 50, 1.0f);
    wv_reader_range(&r, 50, 20);
    ck(r.end < 0, "range: an inverted range is ignored");
}


/* ─── a chop plays at its slot's rate, not at 1.0 ─────────────────────────── */

static void test_slice_rate(void)
{
    int16_t data[200];
    wv_sample s;
    wv_state st;
    wv_bank bk;
    float blk[256];
    int n, fill = 0;
    uint32_t vi;

    printf("slice rate\n");
    for (n = 0; n < 200; n++)
        data[n] = 20000;                    /* a step: the reader never drops out */
    memset(&s, 0, sizeof s);
    s.data = data; s.len = 200; s.root = 60; s.bits = 16; s.gain = 256;
    s.rate = 11025;                          /* a quarter of the engine's rate */
    memset(&bk, 0, sizeof bk);

    /* A chop of the whole slot at 1x of its own rate: the engine renders 44100, the
     * slot holds 11025, so the reader advances 0.25 per output sample and 200 source
     * samples last 800 output samples. If the increment were the literal 1.0 the
     * voice would run out after 200 — four times too fast. */
    wv_init(&st);
    vi = wv_note_on_slice(&st, &s, 60, 127, 0, 200, 0);
    ck(vi == 0, "slice voice allocated");
    ck(st.v[vi].active, "slice voice is active");
    ck(fabsf(st.v[vi].rd.inc - 0.25f) < 1e-4f,
          "a chop at 11025 advances 0.25 per output sample, not 1.0");

    /* Run it and count how long the *reader* lasts. The voice outlives it (a held
     * note's VCA is the note's, not the sample's), so waiting for the voice to be
     * released would measure the envelope, not the material. */
    while (fill < 4000 && !st.v[vi].rd.done) {
        wv_render(&st, &bk, blk, 256, 4096.0f);
        fill += 256;
    }
    ck(st.v[vi].rd.done, "the reader reaches the end of the slot");
    ck(fill >= 700, "a 200-sample slot at 11025 lasts ~800 engine samples");
    ck(fill <= 1200, "and not much longer");

    /* the same slot played as a pitched sample at its root is the same increment:
     * that equality is what "1x" means in both paths */
    wv_init(&st);
    wv_note_on_sample(&st, &s, 60, 127, 0, 0, 0);
    ck(fabsf(st.v[0].rd.inc - 0.25f) < 1e-4f,
          "root note and a chop share the same 1x increment");
}

int main(void)
{
    make_ramp();
    printf("fm1-wave sample tests\n");
    test_parse();
    test_inc();
    test_reader();
    test_endpoint();
    test_range();
    test_slice_rate();
    printf("%s (%d checks, %d failed)\n", fails ? "FAILED" : "ALL PASSED", checks, fails);
    return fails ? 1 : 0;
}
