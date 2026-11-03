/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* The VCA envelope, measured. Every claim here is about *time* or *level*, never
 * about which field the code writes, because the thing being replaced was two
 * hardcoded coefficients and a test that reads `estage` would have passed against
 * the old code too.
 *
 * Each case drives a real wv_state and measures the rendered audio, so a wrong
 * coefficient, an off-by-one in the stage walk, or a release that never frees the
 * voice all show up as a number.
 *
 *   sh app/tests/run_dsp.sh
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "wv_dsp.h"
#include "wv_banks.h"

static int fails, checks;

static void ck(int cond, const char *what)
{
    checks++;
    if (!cond) {
        fails++;
        printf("  FAIL %s\n", what);
    }
}

static int16_t sines[WV_FRAMES * WV_POINTS];

static void make_sines(void)
{
    unsigned f, i;
    for (f = 0; f < WV_FRAMES; f++)
        for (i = 0; i < WV_POINTS; i++)
            sines[f * WV_POINTS + i] =
                (int16_t)(28000.0 * sin(2.0 * M_PI * (double)(f + 1) * (double)i / (double)WV_POINTS));
}

#define BLK 256u
static float buf[BLK];

/* Render n samples and report the level at the END of them. */
static float render_end(wv_state *s, const wv_bank *b, uint32_t n)
{
    uint32_t i;
    float pk;
    float last = 0.0f;
    while (n) {
        uint32_t k = n < BLK ? n : BLK;
        memset(buf, 0, sizeof buf);
        wv_render(s, b, buf, k, 4096.0f);
        pk = 0.0f;
        for (i = 0; i < k; i++)
            if (fabsf(buf[i]) > pk)
                pk = fabsf(buf[i]);
        last = pk;
        n -= k;
    }
    return last;
}

static void setup(wv_state *s, wv_bank *b)
{
    wv_init(s);
    s->cut = 127;
    s->res = 0;
    s->sh_amount = 0;
    b->frames = sines;
    b->foff = 0;               /* no per-frame offsets: the path every bank took before
                                * wv_bank had the member at all */
}

/* ── 1. the time curves are the ranges the header claims ──────────────────── */
static void test_times(void)
{
    printf("time curves:\n");
    {
        float a0 = wv_eg_time(WV_EG_ATTACK, 0);
        float a127 = wv_eg_time(WV_EG_ATTACK, 127);
        float d0 = wv_eg_time(WV_EG_DECAY, 0);
        float d127 = wv_eg_time(WV_EG_DECAY, 127);
        float r0 = wv_eg_time(WV_EG_RELEASE, 0);
        float r127 = wv_eg_time(WV_EG_RELEASE, 127);
        printf("  attack  %.4f .. %.3f s\n", a0, a127);
        printf("  decay   %.4f .. %.3f s\n", d0, d127);
        printf("  release %.4f .. %.3f s\n", r0, r127);
        ck(fabsf(a0 - 0.001f) < 0.0002f, "attack's floor is 1 ms");
        ck(fabsf(a127 - 4.0f) < 0.05f, "attack's ceiling is 4 s");
        ck(fabsf(d0 - 0.002f) < 0.0004f, "decay's floor is 2 ms");
        ck(fabsf(d127 - 8.0f) < 0.1f, "decay's ceiling is 8 s");
        ck(fabsf(r127 - 8.0f) < 0.1f, "release's ceiling is 8 s");
        /* monotonic in the knob: a control that is not monotonic is unusable */
        ck(wv_eg_time(WV_EG_ATTACK, 64) > a0 && wv_eg_time(WV_EG_ATTACK, 64) < a127,
           "attack is monotonic in the knob");
    }
    {
        printf("  sustain %.3f .. %.3f\n", wv_eg_sustain(0), wv_eg_sustain(127));
        ck(wv_eg_sustain(0) == 0.0f, "sustain 0 is silence");
        ck(fabsf(wv_eg_sustain(127) - 1.0f) < 1e-6f, "sustain 127 is full level");
        ck(fabsf(wv_eg_sustain(64) - 64.0f / 127.0f) < 1e-6f, "sustain is linear in the knob");
    }
}

/* ── 2. the attack actually takes the time it says ───────────────────────── */
static void test_attack(void)
{
    wv_state s;
    wv_bank b;
    printf("attack:\n");
    {
        /* a 100 ms attack must reach ~full level at 100 ms and be well short of it
         * at 25 ms. The window is loose on purpose: a one-pole defined as "-60 dB"
         * arrives slightly after its nominal time, and a test that demanded 100 ms
         * exactly would be testing the definition, not the behaviour. */
        setup(&s, &b);
        s.eg_a = (uint8_t)(127.0f * log(0.1f / 0.001f) / log(4.0f / 0.001f));
        printf("  A knob %u = %.3f s\n", s.eg_a, wv_eg_time(WV_EG_ATTACK, s.eg_a));
        ck(fabsf(wv_eg_time(WV_EG_ATTACK, s.eg_a) - 0.1f) < 0.02f,
           "A knob for 100 ms is within 20 ms of it");
        wv_note_on(&s, 60, 127, 1.0f / 100.0f);
        {
            /* 44100, not 4410: the first version of this test asked for 25 ms and got
             * 2.5 ms, so "at 100 ms" was really at 10 ms and the attack looked slow.
             * The engine was right; the arithmetic was not. */
            float at25 = render_end(&s, &b, 44100u * 25u / 1000u);
            float at100 = render_end(&s, &b, 44100u * 75u / 1000u);   /* 25 + 75 = 100 */
            float settled = render_end(&s, &b, 44100u / 2u);
            printf("  level at 25 ms %.3f, at 100 ms %.3f, settled %.3f\n",
                   at25, at100, settled);
            ck(at25 < at100, "the attack rises");
            ck(at100 > 0.93f * settled, "the attack is up by its nominal time");
            /* and it is not instant: at a quarter of the time it must still be short,
             * or the knob's travel means nothing */
            ck(at25 < 0.93f * settled, "the attack is not a step at a quarter of its time");
        }
    }
    {
        /* the shortest attack must not be a click: at 1 ms the level is a ramp over
         * 44 samples, so no single sample jumps more than a fraction of full scale */
        setup(&s, &b);
        s.eg_a = 0;
        wv_note_on(&s, 60, 127, 1.0f / 100.0f);
        {
            float prev = 0.0f, worst = 0.0f;
            uint32_t i, n = BLK;
            memset(buf, 0, sizeof buf);
            wv_render(&s, &b, buf, n, 4096.0f);
            for (i = 1; i < n; i++) {
                float d = fabsf(fabsf(buf[i]) - prev);
                if (d > worst)
                    worst = d;
                prev = fabsf(buf[i]);
            }
            printf("  largest sample-to-sample step at 1 ms attack: %.4f\n", worst);
            ck(worst < 0.35f, "a 1 ms attack does not step");
        }
    }
}

/* ── 3. decay falls to the sustain level and stops there ─────────────────── */
static void test_decay_sustain(void)
{
    wv_state s;
    wv_bank b;
    printf("decay and sustain:\n");
    {
        setup(&s, &b);
        s.eg_a = 0;
        s.eg_d = 40;                            /* short decay */
        s.eg_s = 50;                            /* about 39% */
        s.eg_r = 32;
        wv_note_on(&s, 60, 127, 1.0f / 100.0f);
        {
            float full = 0.0f, sus, later;
            /* peak during the attack's tail, before the decay has moved far */
            full = render_end(&s, &b, 200);
            sus = render_end(&s, &b, 44100);     /* one second: decay is long over */
            later = render_end(&s, &b, 44100);   /* another second: must not move */
            printf("  peak %.3f, sustain %.3f, one second later %.3f\n", full, sus, later);
            ck(sus < full * 0.75f, "the decay audibly lowers the level");
            ck(fabsf(sus - later) < 0.02f, "the sustain holds: it does not keep decaying");
            ck(sus > 0.05f, "sustain 50 is not silence");
        }
    }
    {
        /* sustain 0 is a percussive note: it must decay to silence on its own, with
         * the key still held. Under the old code this was impossible — there was no
         * decay stage at all, so a held key stayed at full level forever. */
        setup(&s, &b);
        s.eg_a = 0;
        s.eg_d = 30;                            /* ~14 ms decay */
        s.eg_s = 0;
        wv_note_on(&s, 60, 127, 1.0f / 100.0f);
        {
            /* 90 samples = 2 ms: early enough that a 14 ms decay has not eaten the
             * note yet. At 400 samples (9 ms) the decay was already 2/3 done, which is
             * correct and was the previous version of this test's mistake. */
            float at0 = render_end(&s, &b, 90);
            float late = render_end(&s, &b, 44100);
            printf("  sustain 0: %.3f after 2 ms, %.4f after 1 s\n", at0, late);
            ck(at0 > 0.05f, "the note starts");
            ck(late < 0.01f, "sustain 0 decays to silence with the key still down");
        }
    }
}

/* ── 4. release: from any stage, and it frees the voice ──────────────────── */
static void test_release(void)
{
    wv_state s;
    wv_bank b;
    printf("release:\n");
    {
        setup(&s, &b);
        s.eg_a = 0;
        s.eg_d = 64;
        s.eg_s = 127;                           /* no decay: hold at full */
        s.eg_r = 60;
        wv_note_on(&s, 60, 127, 1.0f / 100.0f);
        render_end(&s, &b, 2000);
        wv_note_off(&s, 60);
        {
            float just = render_end(&s, &b, 200);   /* still audible */
            uint32_t n = 0;
            printf("  200 ms after note-off: %.3f\n", just);
            ck(just > 0.05f, "the release starts from the level the note was at");
            /* a released voice must free itself, or eight notes exhaust the pool */
            while (wv_active(&s) > 0 && n < 44100u * 6u) {
                render_end(&s, &b, BLK);
                n += BLK;
            }
            printf("  freed after %.2f s, active %u\n", (double)n / 44100.0, wv_active(&s));
            ck(wv_active(&s) == 0, "a released voice frees itself");
            ck(n < 44100u * 5u, "release 60 is under 5 s");
        }
    }
    {
        /* note-off during the attack: the level must not jump to full first. This is
         * the case that separates "release from wherever it is" from "release from
         * the sustain level". */
        setup(&s, &b);
        s.eg_a = 100;                           /* slow attack */
        s.eg_d = 64;
        s.eg_s = 127;
        s.eg_r = 30;
        wv_note_on(&s, 60, 127, 1.0f / 100.0f);
        {
            float during = render_end(&s, &b, 2205);    /* 50 ms into a long attack */
            float after;
            wv_note_off(&s, 60);
            after = render_end(&s, &b, 441);            /* 10 ms later */
            printf("  mid-attack %.3f, 10 ms after note-off %.3f\n", during, after);
            ck(during < 0.9f, "the attack had not finished");
            ck(after < during, "releasing during the attack goes down, not up");
        }
    }
}

/* ── 5. the defaults reproduce the old behaviour ─────────────────────────── */
static void test_defaults(void)
{
    wv_state s;
    wv_bank b;
    printf("defaults:\n");
    {
        setup(&s, &b);
        printf("  A=%u D=%u S=%u R=%u\n", s.eg_a, s.eg_d, s.eg_s, s.eg_r);
        ck(s.eg_s == 127, "sustain defaults to full, so decay has nowhere to go");
        ck(wv_eg_time(WV_EG_ATTACK, s.eg_a) < 0.005f, "the default attack is under 5 ms");
        /* a held note at the defaults is steady: the old code's gate behaviour */
        wv_note_on(&s, 60, 127, 1.0f / 100.0f);
        {
            float a = render_end(&s, &b, 4410);
            float c = render_end(&s, &b, 44100);
            printf("  default patch: %.3f at 100 ms, %.3f at 1 s\n", a, c);
            ck(fabsf(a - c) < 0.05f, "the default patch holds its level, as the old gate did");
        }
    }
}

/* ── 6. the envelope is per voice, and stealing restarts it ──────────────── */
static void test_polyphony(void)
{
    wv_state s;
    wv_bank b;
    printf("per voice:\n");
    {
        uint32_t i;
        float peaks[WV_VOICES];
        setup(&s, &b);
        s.eg_a = 80;
        s.eg_s = 40;
        for (i = 0; i < WV_VOICES; i++)
            wv_note_on(&s, (uint8_t)(48 + i), 127, 1.0f / 100.0f);
        ck(wv_active(&s) == WV_VOICES, "eight notes fill eight voices");
        /* release one and the others must keep their level: a shared envelope would
         * drop them all */
        wv_note_off(&s, 48);
        render_end(&s, &b, 4410);
        for (i = 0; i < WV_VOICES; i++)
            peaks[i] = s.v[i].level;
        printf("  levels after releasing voice 0: %.2f %.2f %.2f\n", peaks[0], peaks[1], peaks[2]);
        ck(peaks[0] < peaks[1], "the released voice is below the held ones");
        ck(peaks[1] > 0.05f, "the held voices keep sounding");
    }
}

int main(void)
{
    make_sines();
    printf("fm1-wave ADSR tests\n");
    test_times();
    test_attack();
    test_decay_sustain();
    test_release();
    test_defaults();
    test_polyphony();
    printf("%s (%d checks, %d failed)\n", fails ? "FAILED" : "ALL PASSED", checks, fails);
    return fails ? 1 : 0;
}
