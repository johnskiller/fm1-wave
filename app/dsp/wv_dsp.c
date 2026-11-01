/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* fm1-wave's DSP. See wv_dsp.h for the contract and the chain.
 *
 * Compiled by the firmware at -O2 (fm1.toml: o2 = ["dsp/wv_dsp.c"]) and by the host
 * tests under plain cc. Nothing here knows the platform exists.
 */

#include "wv_dsp.h"
#include "fastmath.h"

/* ─── small helpers ───────────────────────────────────────────────────────── */

static float u7_to_f(uint32_t v) { return (float)v * (1.0f / 127.0f); }

/* the panel's 0..127 CUT to a one-pole-ish corner, exponential 30 Hz .. 16 kHz */
static float cut_hz(uint32_t cut)
{
    /* 30 * (16000/30)^(cut/127) = 30 * 2^(9.06 * cut/127) */
    return 30.0f * fm_exp2f(9.0589f * u7_to_f(cut));
}

/* the SVF's g for a corner at fs = 44100: tan(pi*f/fs), approximated by the
 * small-angle form plus one correction (the corner never exceeds ~16 kHz, where
 * tan() is still well behaved) */
static float svf_g(float hz)
{
    float w = FM_PI * hz * (1.0f / 44100.0f);
    return w * (1.0f + w * w * 0.3333f);          /* tan(w) ~ w + w^3/3 */
}

/* ─── the table ───────────────────────────────────────────────────────────── */

float wv_table_read(const wv_bank *b, float ph, float mf)
{
    float x, ff;
    int32_t i0, i1, f0, f1;
    float a, c, t;

    /* the two frames around the morph position */
    if (mf <= 0.0f)
        mf = 0.0f;
    else if (mf >= (float)(WV_FRAMES - 1u))
        mf = (float)(WV_FRAMES - 1u);
    f0 = (int32_t)mf;
    t = mf - (float)f0;
    f1 = f0 + 1 < (int32_t)WV_FRAMES ? f0 + 1 : f0;

    /* the two points around the phase */
    ph = ph - fm_floorf(ph);                       /* 0..1 */
    x = ph * (float)WV_POINTS;
    i0 = (int32_t)x;
    if (i0 >= (int32_t)WV_POINTS)
        i0 = (int32_t)WV_POINTS - 1;               /* (ph == 1.0 after rounding) */
    i1 = (i0 + 1) & ((int32_t)WV_POINTS - 1);      /* the table wraps: cheaper than an add and a compare */
    ff = x - (float)i0;

    a = (float)b->frames[f0 * (int32_t)WV_POINTS + i0];
    a += ((float)b->frames[f0 * (int32_t)WV_POINTS + i1] - a) * ff;
    if (f1 != f0) {
        c = (float)b->frames[f1 * (int32_t)WV_POINTS + i0];
        c += ((float)b->frames[f1 * (int32_t)WV_POINTS + i1] - c) * ff;
        a += (c - a) * t;
    }
    return a * (1.0f / 32768.0f);
}

/* ─── the shaper ──────────────────────────────────────────────────────────── */

float wv_shape_curve(uint32_t shape, float x)
{
    switch (shape) {
    case WV_SHAPE_SOFT:
        return fm_tanhf(x);
    case WV_SHAPE_HARD:
        return fm_clampf(x, -1.0f, 1.0f);
    case WV_SHAPE_SINE:
        /* sin(x * pi/2): monotonic on -1..1, pinned at +-1 outside, odd, and
         * smooth at the join (its slope there is 0, so it does not kink) */
        if (x <= -1.0f)
            return -1.0f;
        if (x >= 1.0f)
            return 1.0f;
        return fm_sin_pi(x * 0.5f);
    case WV_SHAPE_FOLD:
    default:
        /* triangle fold, |((x + 3) mod 4) - 2| - 1: the identity on -1..1, and past
         * the fold point it comes back down. The triangle must be centred on +2,
         * not on 0 — centring it on 0 gives -f(x), which is a phase inversion
         * dressed up as a fold. (That is what this code did at first; the "odd" and
         * "monotonic" checks in tests/test_dsp.c are what caught it.) */
        {
            float u = (x + 3.0f) - 4.0f * fm_floorf((x + 3.0f) * 0.25f);
            return fm_fabsf(u - 2.0f) - 1.0f;
        }
    }
}

float wv_shaper(uint32_t shape, float x, float amount)
{
    /* amount 0..1 from the panel: a mild pre-gain curve, so the knob is usable
     * across its whole travel (linear drive puts almost all the audible change
     * in the first third) */
    float drive = 1.0f + 15.0f * amount * amount;
    return wv_shape_curve(shape, x * drive);
}

/* ─── voice lifecycle ─────────────────────────────────────────────────────── */

/* No <string.h> on the target (the AC79 libc is JieLi's): a byte loop, which the
 * compiler turns into stores anyway. */
static void wv_clear(void *p, unsigned n)
{
    unsigned char *b = (unsigned char *)p;
    while (n--)
        *b++ = 0;
}

void wv_init(wv_state *s)
{
    wv_clear(s, sizeof *s);
    s->morph = 0;
    s->cut = 127;                                  /* open: a new instrument must make a bright sound */
    s->res = 0;
    s->shape = WV_SHAPE_SOFT;
    s->sh_amount = 0;
    s->cut_z = cut_hz(s->cut);
    s->amount_z = 0.0f;
}

/* Pick a voice: a free one, or the oldest note when all eight are busy. Shared by
 * both note-on paths, because "which voice" must not depend on what the voice is
 * going to read. */
static uint32_t voice_alloc(wv_state *s)
{
    uint32_t i, pick = 0, oldest = 0xFFFFFFFFu;

    for (i = 0; i < WV_VOICES; i++)
        if (!s->v[i].active)
            return i;

    for (i = 0; i < WV_VOICES; i++)
        if (s->v[i].age < oldest)
            { oldest = s->v[i].age; pick = i; }
    return pick;
}

/* The part of note-on both sources share: pitch, velocity, VCA, age. */
static void voice_begin(wv_state *s, uint32_t pick, uint8_t note, uint8_t vel, float inc)
{
    wv_voice *v = &s->v[pick];
    int was = v->active;

    /* a fresh voice starts its phase a third of a cycle apart, so two voices of the
     * same note do not sum to a 6 dB peak on sample 0 */
    if (!was)
        v->phase = (float)pick * (1.0f / 3.0f);
    v->inc = inc;
    v->note = note;
    /* A zero velocity is not a quiet note, it is a missing reading. Velocity is a
     * control input (the touch sensor), not the sample's amplitude, so a missing one
     * falls back to a playable value rather than to 1/127 of one: `vel ? vel : 1` is
     * how a chop ended up 42 dB down when the answer was "no touch data". */
    v->vel = vel ? vel : 100;
    /* Full velocity reaches a full VCA. Do NOT put a headroom factor here as well
     * as in WV_VOICE_GAIN: that is how a solo note ended up at -19 dBFS when this
     * was first written (two 6 dB cuts that neither knew about the other).
     * Headroom is polyphony's problem, and WV_VOICE_GAIN owns it. */
    v->target = u7_to_f(v->vel);
    if (!was) {
        v->lpf_s1 = 0.0f;
        v->lpf_s2 = 0.0f;
        v->level = 0.0f;
    }
    v->active = 1;
    v->age = ++s->age;
}

uint32_t wv_note_on(wv_state *s, uint8_t note, uint8_t vel, float inc)
{
    uint32_t pick = voice_alloc(s);
    voice_begin(s, pick, note, vel, inc);
    s->v[pick].src = WV_SRC_TABLE;
    s->v[pick].smp = 0;
    return pick;
}

/* The common body of both sample note-ons. `inc` is the whole difference between
 * a chop and a pitched sample, so it is a parameter and not a mode. */
static uint32_t sample_voice(wv_state *s, const wv_sample *smp, uint8_t note, uint8_t vel,
                             uint32_t start, uint32_t end, int loop, float inc)
{
    uint32_t pick;
    wv_voice *v;

    if (!smp || !smp->data || !smp->len)
        return 0xFFFFFFFFu;
    if (end == 0 || end > smp->len)
        end = smp->len;
    if (start >= end)
        start = 0;

    pick = voice_alloc(s);
    /* a sample voice's position is its own, in the slot's samples: the reader owns
     * it, and the engine's phase accumulator is not involved at all */
    voice_begin(s, pick, note, vel, 0.0f);
    v = &s->v[pick];
    v->src = loop ? WV_SRC_SAMPLE : WV_SRC_SLICE;
    v->smp = smp;
    wv_reader_start(&v->rd, smp, start, inc);
    wv_reader_range(&v->rd, start, end);
    if (loop)
        wv_reader_loop(&v->rd, start, end);
    return pick;
}

uint32_t wv_note_on_sample(wv_state *s, const wv_sample *smp, uint8_t note, uint8_t vel,
                           uint32_t start, uint32_t end, int loop)
{
    if (!smp || !smp->data || !smp->len)
        return 0xFFFFFFFFu;
    return sample_voice(s, smp, note, vel, start, end, loop, wv_smp_inc(smp, (float)note));
}

uint32_t wv_note_on_slice(wv_state *s, const wv_sample *smp, uint8_t note, uint8_t vel,
                          uint32_t start, uint32_t end, int loop)
{
    if (!smp || !smp->data || !smp->len)
        return 0xFFFFFFFFu;
    /* "1x" is the slot's own rate against the engine's, not the literal 1.0: the
     * reader's position is in the slot's samples and the engine renders at 44100, so
     * an 11.025 kHz chop advances a quarter of a sample per output sample. Written as
     * 1.0f (the first version) a 210 ms chop played in 52 ms — four times too fast,
     * which sounds like a truncated sample rather than like a wrong rate. */
    return sample_voice(s, smp, note, vel, start, end, loop,
                        (float)smp->rate * (1.0f / 44100.0f));
}

void wv_note_off(wv_state *s, uint8_t note)
{
    uint32_t i;
    for (i = 0; i < WV_VOICES; i++)
        if (s->v[i].active && s->v[i].note == note)
            s->v[i].target = 0.0f;                 /* the render frees it below the floor */
}

uint32_t wv_active(const wv_state *s)
{
    uint32_t i, n = 0;
    for (i = 0; i < WV_VOICES; i++)
        n += s->v[i].active ? 1u : 0u;
    return n;
}

/* ─── the render ──────────────────────────────────────────────────────────── */

/* the VCA's one-pole coefficients: ~2 ms attack, ~10x slower release. Short
 * enough that a key press is not late, long enough that nothing clicks. */
#define WV_ATK 0.0113f
#define WV_REL 0.0012f

void wv_render(wv_state *s, const wv_bank *b, float *out, uint32_t n, float master)
{
    uint32_t i, k;
    float mf = u7_to_f(s->morph) * (float)(WV_FRAMES - 1u);
    float cut_t = cut_hz(s->cut);
    float amt_t = u7_to_f(s->sh_amount);
    /* the SVF's damping: RES 0 is a plain low-pass, RES 127 rings hard */
    float damp = 2.0f - 1.85f * u7_to_f(s->res);
    /* the parameter smoothing per sample, to reach the target in ~2 ms at 44.1 k */
    const float zc = 0.0113f;

    for (k = 0; k < n; k++) {
        float g, sh, env, amp;
        s->cut_z += (cut_t - s->cut_z) * zc;
        s->amount_z += (amt_t - s->amount_z) * zc;
        g = svf_g(s->cut_z);

        for (i = 0; i < WV_VOICES; i++) {
            wv_voice *v = &s->v[i];
            float osel, coef, s1, s2, lp, hp, bp;

            if (!v->active)
                continue;

            coef = v->target > v->level ? WV_ATK : WV_REL;
            v->level += (v->target - v->level) * coef;

            /* The one place the two sources differ. Everything after this line is
             * shared, which is the whole reason a sample voice is a voice and not a
             * second engine. */
            if (v->src == WV_SRC_TABLE) {
                osel = wv_table_read(b, v->phase, mf);
                v->phase += v->inc;
                if (v->phase >= 1.0f)
                    v->phase -= 1.0f;
            } else {
                int fin = 0;
                osel = wv_reader_next(&v->rd, v->smp, &fin);
                /* A one-shot that ran out does not stop the voice: the VCA's
                 * release is what ends it, and that is the note's release, not the
                 * sample's end. A slice that ends early simply goes silent, which
                 * for a drum hit is what "the hit is over" sounds like. */
                if (fin)
                    osel = 0.0f;
            }

            sh = wv_shaper((uint32_t)(s->shape % WV_SHAPE_N), osel, s->amount_z);

            /* Zavalishin's topology-preserving SVF: stable at any g, no
             * coefficient that blows up when the corner goes near Nyquist */
            s1 = v->lpf_s1;
            s2 = v->lpf_s2;
            hp = (sh - (damp + g) * s1 - s2) / (1.0f + g * (g + damp));
            bp = g * hp + s1;
            lp = g * bp + s2;
            v->lpf_s1 = g * hp + bp;
            v->lpf_s2 = g * bp + lp;

            env = v->level;
            amp = lp * env * WV_VOICE_GAIN * master * (1.0f / 4096.0f);
            out[k] += amp;
        }

        /* Bus soft clip, then a hard clamp behind it.
         *
         * The hard clamp is not cosmetic: the platform hands this buffer straight
         * to the ALNK DMA as int32 with 24 valid bits and nothing on the way down
         * clips (main_fm1.c: render_block -> abuf). A sample past full scale there
         * does not distort, it *wraps* — the loudest possible click.
         *
         * But clamping alone distorts too early: measured, eight voices under FOLD
         * at full amount peak at 2.4x and a bare clamp clips 2-9% of samples. tanh
         * fixes that in the only place it can be fixed cheaply, the bus (one call
         * per sample, against eight voices' worth of work). It is ~identity below
         * 0.5, so a solo note and a small chord are untouched and stay ~-16 dBFS,
         * and it compresses the full-eight case to ~0.98 instead of shredding it. */
        {
            float b = fm_tanhf(out[k]);
            if (b > 1.0f)
                b = 1.0f;
            else if (b < -1.0f)
                b = -1.0f;
            out[k] = b;
        }
    }

    /* free the voices that finished releasing (outside the sample loop: a voice
     * that frees itself mid-block would leave a discontinuity in the block) */
    for (i = 0; i < WV_VOICES; i++) {
        wv_voice *v = &s->v[i];
        if (v->active && v->target == 0.0f && v->level < 0.0005f) {
            v->active = 0;
            v->level = 0.0f;
            v->lpf_s1 = 0.0f;
            v->lpf_s2 = 0.0f;
        }
    }
}
