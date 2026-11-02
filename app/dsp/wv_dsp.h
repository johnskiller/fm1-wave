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

#include "wv_grain.h"
#include "wv_sample.h"

#define WV_VOICES      8u

/* What a voice reads. A slice is not a separate source: it is a sample reader
 * with a start and an end, so the difference between a slice and a whole sample is
 * where the reader was pointed and whether it loops. */
enum { WV_SRC_TABLE, WV_SRC_SAMPLE, WV_SRC_SLICE, WV_SRC_GRAIN };

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
    float    phase;        /* 0..1, one cycle (the table source) */
    float    inc;          /* pitch: turns per sample */
    float    level;        /* VCA, 0..1 — the envelope's output */
    float    target;       /* the level the current envelope stage heads for */
    uint8_t  estage;       /* WV_EG_*: the stage `level` is travelling through */
    uint8_t  epad[3];
    float    lpf_s1;       /* SVF state */
    float    lpf_s2;
    /* the sample source. A voice is playing either a table or a sample, never
     * both, and the shaper/filter/VCA are shared: that is why wv_source exists as
     * one field instead of two parallel chains. */
    wv_reader rd;          /* position in the sample, in that slot's samples */
    const wv_sample *smp;  /* the slot being played, 0 for a table voice */
    /* a granular voice reads the same slot as a sample voice but through a cloud of
     * grains instead of one playhead. It is a voice field, not a second engine: the
     * shaper, filter and VCA below are the same ones. */
    wv_grains gr;
    uint8_t  active;
    uint8_t  note;
    uint8_t  vel;          /* 0..127, the VCA ceiling scales with it */
    uint8_t  src;          /* WV_SRC_* */
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
    uint8_t  grain_size;   /* 0..127: grain length, 10..200 ms (wv_grain_len) */
    uint8_t  spray;        /* 0..127: grain start scatter; 0 makes it a sample player */
    /* the VCA envelope's four stages, in the panel's units. Before this existed the
     * shape was two #defines (~2 ms attack, ~10x slower release) with no sustain at
     * all, so a held note was a gate: it reached full level and stayed there, and
     * "note off" was the only thing that ever moved the level down. */
    uint8_t  eg_a, eg_d, eg_s, eg_r;
    /* the block's smoothed values; turning a knob must not click */
    float    cut_z;
    float    amount_z;
} wv_state;

/* ─── the VCA envelope ────────────────────────────────────────────────────── */

/* Stages. A note begins in ATTACK and walks A → D → S; note-off sends it to R from
 * wherever it is. That is a one-way walk with one branch, which is why it needs a
 * stage field and not a timer: the stage decides the target, and the target is what
 * the per-sample slew follows. */
enum { WV_EG_ATTACK, WV_EG_DECAY, WV_EG_SUSTAIN, WV_EG_RELEASE, WV_EG_OFF };

/* Stage times, from the panel's 0..127, exponential — the same curve shape as
 * GRAIN SIZE and CUT, because a linear time control spends most of its travel in
 * the long end where the difference is least audible.
 *
 *     attack   1 ms .. 4 s
 *     decay    2 ms .. 8 s
 *     release  2 ms .. 8 s
 *
 * A is deliberately the shortest at the low end: the bottom of the range has to be
 * fast enough that a key press is not late, and 1 ms is.
 *
 * SUSTAIN is a level, not a time: 0..127 maps to 0..1 linearly.
 *
 * At the default settings (A=0, D=64, S=127, R=32) this reproduces the old
 * behaviour — a ~2 ms attack and no decay, because sustain is full — so a patch
 * made before the envelope existed still sounds the same. */
float wv_eg_time(uint32_t stage, uint32_t v127);
float wv_eg_sustain(uint32_t v127);

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

/* Start a table note. Returns the voice index used. Steals the oldest when all
 * eight are busy; the steal fades the old voice out over WV_STEAL_SAMPLES rather
 * than jumping (a stolen voice clicking is the most audible defect a poly synth
 * has). `inc` is the phase increment (freq / 44100), `vel` 1..127. */
uint32_t wv_note_on(wv_state *s, uint8_t note, uint8_t vel, float inc);

/* Start a sample voice at `note`: it plays [start, end) of the slot, looping inside
 * that range if `loop` (a held slice, or a wavetable-source), and once if not (a
 * drum hit). `end` 0 means the slot's end. The increment comes from the slot's own
 * rate and root (wv_smp_inc), so a slot at its root plays at 1x and an octave up is
 * exactly 2x. Returns the voice index used, or 0xFFFFFFFF for a slot with no data. */
uint32_t wv_note_on_sample(wv_state *s, const wv_sample *smp, uint8_t note, uint8_t vel,
                           uint32_t start, uint32_t end, int loop);

/* A chop, at 1x. Same voice, same chain, but the increment is exactly 1: a slice is
 * *played*, not detuned, so the key only chooses which chop. wv_note_on_sample
 * pitches from the slot's root, which is right for a sample used as an instrument
 * and wrong for a drum kit — a kit played chromatically is a kit at the wrong speed,
 * and the first version of this shipped exactly that: the low keys halved the
 * playback rate and the hits came out long and dull. */
uint32_t wv_note_on_slice(wv_state *s, const wv_sample *smp, uint8_t note, uint8_t vel,
                          uint32_t start, uint32_t end, int loop);

/* Start a granular voice: a cloud over [start, end) of the slot, `size` 0..127 grain
 * length, `spray` 0..127 scatter. The voice pitches like a sample (wv_smp_inc), so a
 * cloud over a chop is in tune with the chop itself.
 *
 * SPRAY at 0 is not a special case in this function or in the cloud: the window
 * tiling makes the cloud reproduce the material, so a note started with spray 0 is a
 * sample player with a grain-length window on it. That is the property that lets the
 * granular path be the sample path's fallback rather than a second feature. */
uint32_t wv_note_on_grain(wv_state *s, const wv_sample *smp, uint8_t note, uint8_t vel,
                          uint32_t start, uint32_t end);

/* Release a note: the voice fades out and frees itself. */
void wv_note_off(wv_state *s, uint8_t note);

/* How many voices are sounding (for the UI and for the tests). */
uint32_t wv_active(const wv_state *s);

/* ─── the render ──────────────────────────────────────────────────────────── */

/* Render `n` frames of the whole polyphonic voice pool into `out` (mono, added to
 * what is there: the caller may mix several parts). `master` is 0..4096 as the
 * platform's master_q12. */
void wv_render(wv_state *s, const wv_bank *b, float *out, uint32_t n, float master);

/* Render one voice range of one block: [from, to) of the voice pool, all `n` frames.
 *
 * This is the split the second core takes. It is per *voice*, not per sample, for a
 * reason that is not about performance: a voice's state (a grain cloud's four
 * positions, a filter's two integrators, the VCA's ramp) is written by exactly one
 * core for the whole block. Splitting by sample instead would have both cores touch
 * every voice's floats every block, and the cross-core barrier around that is what
 * makes dual-core audio expensive and fragile.
 *
 * The output buffer is still shared: `out` is cleared by the caller and each voice
 * range adds its own sum into it. Two cores adding to different addresses in the same
 * cache line is the one sharing this accepts, and it is why wv_render_split does the
 * bus limiter itself, once, after both halves are in.
 */
void wv_render_range(wv_state *s, const wv_bank *b, float *out, uint32_t n,
                     float master, uint32_t from, uint32_t to);

/* The whole render, optionally over two cores. With `cpu2` clear this is exactly
 * wv_render (one core, one pass). With it set and a working second core, the voice
 * pool is cut in half, both halves run, and the bus limiter lands after both.
 * Falls back to one core whenever the second one will not take the job, so a unit
 * that cannot run it loses throughput and nothing else. */
uint32_t wv_last_split(void);
void wv_render_split(wv_state *s, const wv_bank *b, float *out, uint32_t n, float master,
                     int cpu2, int (*run)(void (*)(uint32_t), uint32_t),
                     int (*wait)(void), void (*sync)(void));

#endif /* WV_DSP_H */
