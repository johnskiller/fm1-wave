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
/* The LFO's rate: 0.02..20 Hz, exponential, the same curve shape as CUT and GRAIN
 * SIZE because a linear rate control spends most of its travel in the fast end where
 * the difference is least musical. 0 is off, which is why the range starts at 1.
 *
 * 0.02 Hz is a 50 s cycle: slower than any note lasts, so at the bottom of the travel
 * the LFO is a slow drift rather than a vibrato. That is deliberate -- a filter that
 * walks over half a minute is a different sound from one that wobbles, and the knob
 * reaches both.
 */
float wv_lfo_hz(uint32_t v127)
{
    if (v127 == 0)
        return 0.0f;
    /* 0.02 * (20/0.02)^(v/127) = 0.02 * 2^(9.966 * v/127) */
    return 0.02f * fm_exp2f(9.9658f * u7_to_f(v127));
}

/* Depth: 0..4 octaves either side. Octaves, not Hz, because CUT is exponential over
 * nine octaves and a sweep that is linear in Hz spends most of its travel in the top
 * two octaves where nothing is happening. */
static float lfo_oct(uint32_t v127)
{
    return 4.0f * u7_to_f(v127);
}

/* The LFO's value for this block: a sine in -1..1, and 1.0 (no modulation) when it is
 * off. CUT is scaled by 2^(value * depth), so the depth is symmetric in octaves. */
static float lfo_cut_scale(const wv_state *s)
{
    if (s->lfo_rate == 0 || s->lfo_depth == 0)
        return 1.0f;
    return fm_exp2f(fm_sin_pi(s->lfo_ph) * lfo_oct(s->lfo_depth));
}

static float svf_g(float hz)
{
    float w = FM_PI * hz * (1.0f / 44100.0f);
    return w * (1.0f + w * w * 0.3333f);          /* tan(w) ~ w + w^3/3 */
}

/* ─── the table ───────────────────────────────────────────────────────────── */

/* The frame pair for a morph position: the two frames the position sits between, and
 * how far between them.
 *
 * wv_table_read recomputes this from `mf` for every sample, which is a clamp, an int
 * conversion, a float subtract and a compare per sample -- for a value that does not
 * change inside a block. Hoisting it out is what a caller that renders a whole block
 * wants; the per-sample function stays because the tests and the odd one-off read
 * want it. */
void wv_frames_at(const wv_bank *b, float mf, const int16_t **fa, const int16_t **fb, float *t)
{
    int32_t f0, f1;

    if (mf <= 0.0f)
        mf = 0.0f;
    else if (mf >= (float)(WV_FRAMES - 1u))
        mf = (float)(WV_FRAMES - 1u);
    f0 = (int32_t)mf;
    *t = mf - (float)f0;
    f1 = f0 + 1 < (int32_t)WV_FRAMES ? f0 + 1 : f0;
    *fa = b->frames + f0 * (int32_t)WV_POINTS;
    *fb = (f1 != f0) ? b->frames + f1 * (int32_t)WV_POINTS : 0;
}

/* One sample out of the pair wv_frames_at picked, with the frames already resolved.
 * When `fb` is null the morph position sits exactly on a frame and only one table is
 * read -- which is the common case, since MORPH is a knob and a knob parked on a frame
 * is not a special mode. */
float wv_table_read2(const int16_t *fa, const int16_t *fb, float ph, float t)
{
    float x, ff, a;
    int32_t i0, i1;

    ph = ph - fm_floorf(ph);
    x = ph * (float)WV_POINTS;
    i0 = (int32_t)x;
    if (i0 >= (int32_t)WV_POINTS)
        i0 = (int32_t)WV_POINTS - 1;
    i1 = (i0 + 1) & ((int32_t)WV_POINTS - 1);
    ff = x - (float)i0;

    a = (float)fa[i0];
    a += ((float)fa[i1] - a) * ff;
    if (fb) {
        float c = (float)fb[i0];
        c += ((float)fb[i1] - c) * ff;
        a += (c - a) * t;
    }
    return a * (1.0f / 32768.0f);
}

float wv_table_read(const wv_bank *b, float ph, float mf)
{
    const int16_t *fa, *fb;
    float t;
    wv_frames_at(b, mf, &fa, &fb, &t);
    return wv_table_read2(fa, fb, ph, t);
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
    /* The envelope's defaults are the old hardcoded behaviour: a ~1 ms attack, full
     * sustain (so decay has nowhere to go), and a release of about the 30 ms the
     * fixed coefficient gave. A patch made before A/D/S/R existed therefore sounds
     * the same after it, which is the only way adding a feature does not silently
     * change every sound. */
    s->eg_a = 0;
    s->eg_d = 64;
    s->eg_s = 127;
    s->eg_r = 32;
    /* LFO off, so a patch made before it existed sounds the same after it */
    s->lfo_rate = 0;
    s->lfo_depth = 0;
    s->lfo_ph = 0.0f;
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
    /* the envelope starts at the bottom of its attack. A re-used voice restarts it
     * from wherever the level is, which is what makes voice stealing a crossfade
     * rather than a click: the level is continuous, only the stage resets. */
    v->estage = WV_EG_ATTACK;
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

uint32_t wv_note_on_grain(wv_state *s, const wv_sample *smp, uint8_t note, uint8_t vel,
                          uint32_t start, uint32_t end)
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
    voice_begin(s, pick, note, vel, 0.0f);
    v = &s->v[pick];
    v->src = WV_SRC_GRAIN;
    v->smp = smp;
    /* The seed is the voice index and the note, so two voices on the same note and
     * two notes on the same voice scatter differently, and a test that replays the
     * same sequence gets the same cloud. */
    /* loop: a granular note sustains. See wv_grains_start for why an unwrapped
     * playhead is not a granular instrument. */
    wv_grains_start(&v->gr, smp, start, end, wv_smp_inc(smp, (float)note), 1,
                    (uint32_t)pick * 2654435761u ^ ((uint32_t)note << 8));
    return pick;
}

void wv_note_off(wv_state *s, uint8_t note)
{
    uint32_t i;
    for (i = 0; i < WV_VOICES; i++)
        if (s->v[i].active && s->v[i].note == note) {
            s->v[i].estage = WV_EG_RELEASE;        /* from any stage: see eg_step */
            s->v[i].target = 0.0f;                 /* the render frees it below the floor */
        }
}

uint32_t wv_active(const wv_state *s)
{
    uint32_t i, n = 0;
    for (i = 0; i < WV_VOICES; i++)
        n += s->v[i].active ? 1u : 0u;
    return n;
}

/* ─── the render ──────────────────────────────────────────────────────────── */

/* ─── the VCA envelope ────────────────────────────────────────────────────── */

/* A stage's per-sample slew: the fraction of the remaining distance covered each
 * sample, chosen so the stage covers 1.0 in exactly `sec`. A one-pole reaches its
 * target asymptotically, so "covers 1.0" is defined as "arrives within -60 dB",
 * which is what these two constants set (1 - 0.001 = 0.999).
 *
 *     k = 1 - exp(ln(0.001) / (sec * fs))  ~=  6.9078 / (sec * fs)
 *
 * A linear ramp is the obvious alternative and it is wrong here: a per-sample
 * increment has to be recomputed when the stage changes mid-note, while the
 * coefficient depends only on the time, so a held note can have its attack
 * lengthened under it without a discontinuity. */
static float eg_coef(float sec)
{
    float n = sec * WV_RATE;
    if (n < 1.0f)
        n = 1.0f;
    return 1.0f - fm_exp2f(-9.96578428f / n);   /* ln(1/0.001) = 6.90776 = 9.96578 * ln2 */
}

float wv_eg_time(uint32_t stage, uint32_t v127)
{
    float t = u7_to_f(v127 > 127u ? 127u : v127);
    switch (stage) {
    case WV_EG_ATTACK:  return 0.001f * fm_exp2f(t * 11.9658f);   /* 1 ms .. 4 s */
    case WV_EG_DECAY:   return 0.002f * fm_exp2f(t * 11.9658f);   /* 2 ms .. 8 s */
    case WV_EG_RELEASE: return 0.002f * fm_exp2f(t * 11.9658f);   /* 2 ms .. 8 s */
    default:            return 0.0f;
    }
}

float wv_eg_sustain(uint32_t v127) { return u7_to_f(v127 > 127u ? 127u : v127); }

/* Advance one voice's envelope by one sample. The stage decides the target; the
 * slew follows it; arriving moves the stage on. Note-off sets the target to 0 and
 * the stage to RELEASE from *any* stage, so a key released during the attack
 * releases from wherever the level had got to rather than jumping.
 *
 * The decay and release use the same coefficient shape, so a note whose release is
 * shorter than its decay does not click when it changes stage: both are one-poles
 * heading for a level, and the level is continuous across the change. */
static void eg_step(wv_state *s, wv_voice *v)
{
    float k;
    switch (v->estage) {
    case WV_EG_ATTACK:
        v->target = u7_to_f(v->vel);
        k = eg_coef(wv_eg_time(WV_EG_ATTACK, s->eg_a));
        v->level += (v->target - v->level) * k;
        if (v->level >= v->target - 0.0005f) {
            v->level = v->target;
            v->estage = WV_EG_DECAY;
        }
        break;
    case WV_EG_DECAY:
        v->target = u7_to_f(v->vel) * wv_eg_sustain(s->eg_s);
        k = eg_coef(wv_eg_time(WV_EG_DECAY, s->eg_d));
        v->level += (v->target - v->level) * k;
        if (v->level <= v->target + 0.0005f) {
            v->level = v->target;
            v->estage = WV_EG_SUSTAIN;
        }
        break;
    case WV_EG_SUSTAIN:
        /* the sustain level tracks the knob while the key is held: turning S under a
         * held note is the one envelope stage that is a level and not a time */
        v->target = u7_to_f(v->vel) * wv_eg_sustain(s->eg_s);
        v->level += (v->target - v->level) * 0.0113f;
        break;
    default:                                    /* WV_EG_RELEASE */
        v->target = 0.0f;
        k = eg_coef(wv_eg_time(WV_EG_RELEASE, s->eg_r));
        v->level += (v->target - v->level) * k;
        if (v->level < 0.0005f) {
            v->level = 0.0f;
            v->estage = WV_EG_OFF;
        }
        break;
    }
}

static void voice_block(wv_state *s, const wv_bank *b, float *out, uint32_t n,
                       float master, uint32_t from, uint32_t to)
{
    uint32_t i, k;
    float mf = u7_to_f(s->morph) * (float)(WV_FRAMES - 1u);
    /* The frame pair, resolved once per block instead of once per sample: MORPH does
     * not move inside a block, and recomputing the pair for every sample was 12-16% of
     * the read path (measured). wv_frames_at is the same arithmetic wv_table_read
     * does, just hoisted. */
    const int16_t *fa = 0, *fb = 0;
    float ft = 0.0f;
    wv_frames_at(b, mf, &fa, &fb, &ft);
    /* CUT, swept by the LFO. The scale multiplies Hz, so the depth is symmetric in
     * octaves -- see lfo_cut_scale. Read, not advanced: this function runs twice a
     * block (once per core). */
    float cut_t = cut_hz(s->cut) * lfo_cut_scale(s);
    /* past Nyquist the SVF's tan() blows up: 16 kHz * 2^4 is 256 kHz */
    if (cut_t > 18000.0f)
        cut_t = 18000.0f;
    float amt_t = u7_to_f(s->sh_amount);
    /* the SVF's damping: RES 0 is a plain low-pass, RES 127 rings hard */
    float damp = 2.0f - 1.85f * u7_to_f(s->res);
    /* The parameter smoothing per sample, to reach the target in ~2 ms at 44.1 k.
     *
     * These are LOCAL copies of the smoothed values, and voice_block does not write
     * them back. That is what makes the two-core split sum to the same audio as one
     * core: every voice range renders the same n samples, so every range has to see
     * the same cut and amount at every sample — the same ramp, starting from the same
     * place. If each range wrote back, the second half would advance the smoother a
     * second time for the same n samples and the knobs would glide at twice the rate
     * whenever the split was on. The caller advances the real values once per block. */
    const float zc = 0.0113f;
    float cut_z = s->cut_z;
    float amount_z = s->amount_z;

    for (k = 0; k < n; k++) {
        float g, sh, env, amp;
        cut_z += (cut_t - cut_z) * zc;
        amount_z += (amt_t - amount_z) * zc;
        g = svf_g(cut_z);

        for (i = from; i < to; i++) {
            wv_voice *v = &s->v[i];
            float osel, s1, s2, lp, hp, bp;

            if (!v->active)
                continue;

            eg_step(s, v);

            /* The one place the two sources differ. Everything after this line is
             * shared, which is the whole reason a sample voice is a voice and not a
             * second engine. */
            if (v->src == WV_SRC_TABLE) {
                osel = wv_table_read2(fa, fb, v->phase, ft);
                v->phase += v->inc;
                if (v->phase >= 1.0f)
                    v->phase -= 1.0f;
            } else if (v->src == WV_SRC_GRAIN) {
                /* The cloud renders its own output sample; the grain length is a
                 * parameter, not the voice's, because the panel can change it while a
                 * note is held (a held cloud re-tiles at the new length, which is the
                 * point of being able to turn it). */
                osel = wv_grains_next(&v->gr, v->smp, wv_grain_len(s->grain_size),
                                      (float)s->spray);
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

            sh = wv_shaper((uint32_t)(s->shape % WV_SHAPE_N), osel, amount_z);

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

    }

    /* free the voices that finished releasing (outside the sample loop: a voice
     * that frees itself mid-block would leave a discontinuity in the block) */
    for (i = from; i < to; i++) {
        wv_voice *v = &s->v[i];
        if (v->active && (v->estage == WV_EG_OFF || (v->level < 0.0005f && v->target == 0.0f))) {
            v->active = 0;
            v->level = 0.0f;
            v->lpf_s1 = 0.0f;
            v->lpf_s2 = 0.0f;
        }
    }
}

/* Bus soft clip, then a hard clamp behind it. It runs ONCE, over the whole buffer,
 * after all the voices that are going to write to it have written.
 *
 * The hard clamp is not cosmetic: the platform hands this buffer straight to the
 * ALNK DMA as int32 with 24 valid bits and nothing on the way down clips
 * (main_fm1.c: render_block -> abuf). A sample past full scale there does not
 * distort, it *wraps* — the loudest possible click.
 *
 * But clamping alone distorts too early: measured, eight voices under FOLD at full
 * amount peak at 2.4x and a bare clamp clips 2-9% of samples. tanh fixes that in the
 * only place it can be fixed cheaply, the bus (one call per sample, against eight
 * voices' worth of work). It is ~identity below 0.5, so a solo note and a small
 * chord are untouched and stay ~-16 dBFS, and it compresses the full-eight case to
 * ~0.98 instead of shredding it.
 *
 * It also cannot live inside voice_block: with the pool split across two cores each
 * half would limit its own partial sum, and tanh(a) + tanh(b) != tanh(a + b) — the
 * split would change the sound, not just the speed. */
/* Tap, then the listening level, then the limiter -- in that order. See
 * render_split_tap's comment: this order is the fix for the hiss a chord had. */
static void bus_apply(float *out, uint32_t n, float master, float *tap)
{
    uint32_t k;
    float m = master * (1.0f / 4096.0f);

    if (tap) {
        for (k = 0; k < n; k++)
            tap[k] = out[k];
    }
    for (k = 0; k < n; k++) {
        float b = fm_tanhf(out[k] * m);
        if (b > 1.0f)
            b = 1.0f;
        else if (b < -1.0f)
            b = -1.0f;
        out[k] = b;
    }
}


/* Advance the smoothed parameters by one block. Called once per block by whichever
 * entry point ran, never by voice_block: see the note there. */
static void smooth_step(wv_state *s, uint32_t n)
{
    const float zc = 0.0113f;
    float cut_t = cut_hz(s->cut);
    float amt_t = u7_to_f(s->sh_amount);
    uint32_t k;
    for (k = 0; k < n; k++) {
        s->cut_z += (cut_t - s->cut_z) * zc;
        s->amount_z += (amt_t - s->amount_z) * zc;
    }
    /* The LFO's phase advances ONCE per block, here, and not in voice_block.
     *
     * voice_block runs twice per block (once per core), so advancing the phase there
     * would run the LFO at twice its rate whenever the second core is up -- the same
     * shape of mistake as the smoothers in voice_block, which are deliberately local
     * copies for exactly this reason. smooth_step is the one place that runs once per
     * block, so this is the one place the phase may move.
     *
     * A block is all one LFO value: 5.8 ms per step is not a stair the ear hears, and
     * a per-sample phase would cost a sine per voice per sample for a value every
     * voice shares. */
    if (s->lfo_rate != 0 && s->lfo_depth != 0) {
        s->lfo_ph += wv_lfo_hz(s->lfo_rate) * (float)n * (1.0f / 44100.0f);
        s->lfo_ph -= fm_floorf(s->lfo_ph);
    }
}

void wv_render_range(wv_state *s, const wv_bank *b, float *out, uint32_t n,
                     float master, uint32_t from, uint32_t to)
{
    if (from >= WV_VOICES || from >= to)
        return;
    if (to > WV_VOICES)
        to = WV_VOICES;
    voice_block(s, b, out, n, master, from, to);
}

/* The job the second core runs. The platform hands it a single uint32, so the whole
 * argument set has to be reachable from one: a file-static pointer to the block in
 * flight is the only way to do that without inventing a queue. That is safe because
 * the render is synchronous — the job is handed over and waited for inside one
 * app_render call, so there is exactly one job in flight at any time, and the second
 * core is only ever handed the top half of the pool. */
/* The second core's own output. It must NOT write the caller's buffer.
 *
 * Both halves used to add into `out` (`out[k] += amp`), and on one core that is right.
 * Across two cores it is a race on every sample: both do a read-modify-write of the same
 * float, and the interleaving loses updates at random -- which is white noise, louder
 * with more voices, and it cannot happen on the host because the host's second core runs
 * the job inline. So the device had a hiss the simulator could not reproduce, from four
 * notes up (the cut only starts putting work on core 1 once there are enough active
 * voices), and that threshold is what identified it.
 *
 * Now each core writes only its own memory and CPU0 sums the two after the wait. The
 * sum is in a fixed order, so it is deterministic; the cost is one add per sample, done
 * once. */
#define WV_JOB_BLOCK_MAX 256u   /* the app's BLK_MAX; wv_render_split clamps to it */
static float job_out[WV_JOB_BLOCK_MAX];

static struct {
    wv_state *s;
    const wv_bank *b;
    uint32_t n;
    float master;
    uint32_t mid;       /* the split: the second core renders [mid, WV_VOICES) */
} job;

static void job_half(void *arg)
{
    uint32_t k;

    (void)arg;
    /* its own buffer, cleared here so the sum below needs no clearing step */
    for (k = 0; k < job.n; k++)
        job_out[k] = 0.0f;
    voice_block(job.s, job.b, job_out, job.n, job.master, job.mid, WV_VOICES);
}

/* The cut the last split used, for the tests: the balance is invisible in the audio
 * (both cuts render the same samples), so the only way to assert that four notes no
 * longer all land on core 0 is to look at where the cut was put. */
uint32_t wv_last_split(void) { return job.mid; }

/* The buffer the next job will render into, for the tests: the invariant is that it is
 * NOT the caller's `out`. Two cores adding into one buffer lose updates at random, which
 * is hiss; it cannot be shown by the audio on the host because the host's second core
 * runs the job inline, so the property has to be read directly. */
const float *wv_last_job_buffer(void) { return job_out; }

/* The platform's signature: void (*)(uint32_t). */
static void job_half_u32(uint32_t arg)
{
    (void)arg;
    job_half(0);
}

static void render_split_tap(wv_state *s, const wv_bank *b, float *out, uint32_t n, float master,
                             int cpu2, int (*run)(void (*)(uint32_t), uint32_t),
                             int (*wait)(void), void (*sync)(void), float *tap)
{
    /* `master` is the app's 0..4096 listening level. It is applied to the summed
     * voices and BEFORE the limiter, because the limiter is the one thing on the
     * bus that is not linear: with master applied after it, the limiter saw the
     * full-scale sum whatever the pot said, and four voices were enough to bend
     * the bus while the instrument played at -30 dBFS. Measured: a four-note chord
     * at MASTER 12 o'clock carried -41 dB of non-harmonic content, and -82 dB with
     * the limiter bypassed. That 40 dB was audible as hiss on every note, and it
     * was the whole reason a chord sounded dirty and a single note did not.
     *
     * `tap`, when given, takes the sum as it is before master and the limiter: the
     * scope must not shrink when the listening level is turned down, and it has to
     * be able to show past 0 dBFS, which the limiter's output never does. */
    if (n > WV_JOB_BLOCK_MAX)
        n = WV_JOB_BLOCK_MAX;

    if (cpu2 && run && wait) {
        /* Where to cut the voice pool.
         *
         * Cutting it in half is the obvious thing and it is wrong for a player: the
         * allocator hands out voice 0 first, so four notes land in 0..3 and the second
         * core is idle while the first does all of them. Measured on the device: GRAN at
         * four notes read 51% and at eight also 51% -- the same four voices' work in
         * both cases, and the fix is not more throughput but using the core that is
         * already there.
         *
         * The cut is now between the ACTIVE voices, as near half of them as the
         * contiguity allows: every voice in [0, mid) is lower-numbered than every voice
         * in [mid, VOICES), and each is rendered by exactly one core -- that is the
         * property the split has always needed, since two cores adding the same voice
         * would double it. The quiet voices in each half cost almost nothing (the loop
         * skips them), so the two halves are close to balanced by voice count.
         *
         * A cut that lands differently from the previous block is harmless: a voice's
         * state floats (phase, filter, envelope, grain positions) is carried in the
         * state, not in the core, and each block renders all n frames of its own
         * voices. Whatever the cut, the sample a voice produces is the same -- the
         * audio is bit-identical, which is what the regression below checks. */
        uint32_t act = 0, seen = 0, mid = WV_VOICES / 2u;
        uint32_t i;

        for (i = 0; i < WV_VOICES; i++)
            if (s->v[i].active)
                act++;
        if (act > 1u) {
            for (i = 0; i < WV_VOICES; i++) {
                if (s->v[i].active && ++seen == act / 2u) {
                    mid = i + 1u;
                    break;
                }
            }
        }

        job.s = s;
        job.b = b;
        job.n = n;
        job.master = master;
        job.mid = mid;

        /* Publish the job's argument block BEFORE handing it over. The platform's
         * own barrier covers its mailbox words, not this struct, and the two cores
         * share their caches: without this the second core can read a stale `job`
         * and render the previous block's audio — which sounds like a stutter, not
         * like a race. */
        if (sync)
            sync();

        /* Hand the top half over first, then render the bottom half here, then wait.
         * That order is the whole point: the two halves run at the same time. Waiting
         * first would serialise them and cost more than one core, because the handover
         * is not free. */
        if (run(job_half_u32, 0u)) {
            voice_block(s, b, out, n, 4096.0f, 0u, mid);

            /* The wait can fail: the core was retired (cpu2_fallback) and the job
             * dropped. It is never half-done in that case — a fault restarts the
             * device without the fallback, and with it the job is dropped whole
             * before it runs — so re-rendering the top half adds nothing to `out`.
             * Without this the top four voices would simply be missing, and that is
             * the difference between "slower" and "wrong". */
            if (wait() != 0)
                voice_block(s, b, out, n, 4096.0f, mid, WV_VOICES);
            else {
                uint32_t k;
                if (sync)
                    sync();             /* drain its writes to job_out before reading it */
                for (k = 0; k < n; k++)
                    out[k] += job_out[k];
            }

            smooth_step(s, n);
            bus_apply(out, n, master, tap);
            return;
        }
    }

    /* one core: exactly what wv_render does */
    voice_block(s, b, out, n, 4096.0f, 0u, WV_VOICES);
    smooth_step(s, n);
    bus_apply(out, n, master, tap);
}

void wv_render_split(wv_state *s, const wv_bank *b, float *out, uint32_t n, float master,
                     int cpu2, int (*run)(void (*)(uint32_t), uint32_t),
                     int (*wait)(void), void (*sync)(void), float *tap)
{
    render_split_tap(s, b, out, n, master, cpu2, run, wait, sync, tap);
}

void wv_render(wv_state *s, const wv_bank *b, float *out, uint32_t n, float master)
{
    render_split_tap(s, b, out, n, master, 0, 0, 0, 0, 0);
}
