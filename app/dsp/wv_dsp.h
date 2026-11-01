/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* fm1-wave's DSP: the voice chain, with no platform in it.
 *
 * This unit is compiled twice — by the firmware at -O2 (fm1.toml: o2) and by the
 * host unit tests at -O2 under plain cc — so it may include nothing but
 * <stdint.h>, <string.h> and fastmath.h. No plat.h, no globals that the platform
 * owns, no allocation. Everything it needs arrives in wv_state.
 *
 * The chain, per voice:
 *
 *   oscillator ─► waveshaper ─► SVF low-pass ─► VCA
 *   (table, morph)  (shape, amount)  (cut, res)    (env)
 *
 * The oscillator is deliberately the cheap one: a phase accumulator, one frame
 * interpolation (MORPH), one sample interpolation. It aliases; see
 * docs/background-wavetable-and-waveshaper.md for why that is the v1 trade and
 * what the fix costs.
 */

#ifndef WV_DSP_H
#define WV_DSP_H

#include <stdint.h>

#define WV_VOICES      8u

/* Per-voice gain: 1/sqrt(8). Eight voices that do not correlate sum in power, so
 * this puts the expected peak near full scale instead of eight times over it. It
 * costs a solo note about 9 dB against a single-voice design — the alternative
 * is per-voice gain 1.0 and a limiter, which colours every note to protect the
 * rare eight-note chord. See the clamp in wv_render(). */
#define WV_VOICE_GAIN  0.35355339f
#define WV_FRAMES      8u      /* frames per bank (MORPH's range) */
#define WV_POINTS      256u    /* points per frame */
#define WV_BANK_BYTES  (WV_FRAMES * WV_POINTS * 2u)   /* int16 per point: 4 KB */

/* the four waveshaper curves, in panel order */
enum { WV_SHAPE_SOFT, WV_SHAPE_HARD, WV_SHAPE_FOLD, WV_SHAPE_SINE, WV_SHAPE_N };

/* one voice's state. Pitch is a phase increment in turns per sample (float), not
 * a note number: everything downstream wants the increment, and computing it
 * once at note-on keeps exp2 out of the render. */
typedef struct {
    float    phase;        /* 0..1, one cycle */
    float    inc;          /* pitch: turns per sample */
    float    level;        /* VCA, 0..1 */
    float    target;       /* VCA target (the envelope's block value) */
    float    lpf_s1;       /* SVF state */
    float    lpf_s2;
    uint8_t  active;
    uint8_t  note;
    uint8_t  vel;          /* 0..127, the VCA ceiling scales with it */
    uint8_t  pad;
    uint32_t age;          /* for oldest-note stealing */
} wv_voice;

typedef struct {
    wv_voice v[WV_VOICES];
    uint32_t age;          /* the counter note-on hands out */
    /* parameters, 0..127 unless noted (the panel's own units) */
    uint8_t  morph;        /* 0..127 across WV_FRAMES-1 */
    uint8_t  cut;
    uint8_t  res;
    uint8_t  sh_amount;
    uint8_t  shape;        /* WV_SHAPE_* */
    uint8_t  pad;
    /* the block's smoothed values; turning a knob must not click */
    float    cut_z;
    float    amount_z;
} wv_state;

/* ─── tables ──────────────────────────────────────────────────────────────── */

/* The bank the voices read: WV_BANK_BYTES of int16, frames back to back. In the
 * firmware this points into flash (XIP) and is never copied. */
typedef struct {
    const int16_t *frames;   /* WV_BANK_BYTES bytes: frame f at + f*WV_POINTS */
} wv_bank;

/* Read the bank at phase `ph` (0..1) and morph position `mf` (0..WV_FRAMES-1),
 * both linearly interpolated. Returns -1..1. */
float wv_table_read(const wv_bank *b, float ph, float mf);

/* ─── waveshaper ──────────────────────────────────────────────────────────── */

/* The transfer curve alone, no drive: x in -1..1 (values outside are legal — a
 * driven signal is), result bounded to -1..1 except for fold, which is bounded by
 * construction. Exposed so the tests can check each curve's shape directly. */
float wv_shape_curve(uint32_t shape, float x);

/* curve(amount * x), with amount smoothed by the caller (see wv_block). */
float wv_shaper(uint32_t shape, float x, float amount);

/* ─── voice lifecycle ─────────────────────────────────────────────────────── */

void wv_init(wv_state *s);

/* Start a note. Returns the voice index used. Steals the oldest when all eight
 * are busy; the steal fades the old voice out over WV_STEAL_SAMPLES rather than
 * jumping (a stolen voice clicking is the most audible defect a poly synth has).
 * `inc` is the phase increment (freq / 44100), `vel` 1..127. */
uint32_t wv_note_on(wv_state *s, uint8_t note, uint8_t vel, float inc);

/* Release a note: the voice fades out and frees itself. */
void wv_note_off(wv_state *s, uint8_t note);

/* How many voices are sounding (for the UI and for the tests). */
uint32_t wv_active(const wv_state *s);

/* ─── the render ──────────────────────────────────────────────────────────── */

/* Render `n` frames of the whole polyphonic voice pool into `out` (mono, added to
 * what is there: the caller may mix several parts). `master` is 0..4096 as the
 * platform's master_q12. */
void wv_render(wv_state *s, const wv_bank *b, float *out, uint32_t n, float master);

#endif /* WV_DSP_H */
