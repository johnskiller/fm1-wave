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
    float    inc;          /* pitch: turns per sample, INCLUDING this block's PITCH route */
    /* The pitch the note was struck at, before modulation. PITCH is a destination, so
     * inc has to be re-derived every block from something; keeping the base here is what
     * lets a route return to concert pitch exactly when its amount returns to zero,
     * instead of accumulating rounding. */
    float    inc_base;
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

/* ─── modulation destinations ───────────────────────────────────────────────
 *
 * One LFO, seven slots. Which slots are LIVE depends on the source, and that is a
 * fact about wv_dsp.c's read sites, not a policy: MORPH is read only on the table
 * path (wv_dsp.c:729 resolves the frame pair for WV_SRC_TABLE voices), GRAIN SIZE and
 * SPRAY only on the granular path (:832), PITCH/RES/CUT/SHAPE AMT everywhere.
 *
 *   SYNTH  CUT RES SHAPE MORPH PITCH        (5)
 *   SMPL   CUT RES SHAPE PITCH              (4)
 *   GRAN   CUT RES SHAPE GRAIN SPRAY PITCH  (6)   <- the most any source needs
 *
 * Six is why seven slots is enough for two LFOs to share one page: the seventh is
 * RATE, so both LFOs always have their rate in reach without going back to screen 1
 * (the player's objection, 2026-11-04: "第二页切换到LFO后，RATE没地方调").
 */
enum {
    WV_DEST_CUT = 0,   /* the filter's corner, in octaves: 0..4 either side */
    WV_DEST_RES,       /* resonance, in the same units */
    WV_DEST_SHAPE,     /* the shaper's drive */
    WV_DEST_MORPH,     /* the wavetable position, in frames (UNIPOLAR, see morph_mod) */
    WV_DEST_GRAIN,     /* grain length */
    WV_DEST_SPRAY,     /* grain scatter */
    WV_DEST_PITCH,     /* the voice's pitch, in semitones */
    WV_DESTS
};

/* HOW FAR EACH ROUTE REACHES at full amount (127), in the unit its ring prints. One copy,
 * here, because app.c's dest_text() also has to print it: the ring promises a sweep and the
 * DSP has to make exactly that sweep. Two literals in two files is how a readout starts
 * lying, which is the same failure the MORPH readout had (it re-derived the position in
 * app.c instead of asking the DSP).
 *
 *   CUT    octaves, BIPOLAR  -- the corner moves either side of the knob
 *   MORPH  frames,  UNIPOLAR -- 0..7, the travel the bank has
 *   PITCH  semitones, UNIPOLAR -- 0..+6, UP only
 *
 * PITCH was 12. The player cut it to 6: "pitch 范围太大，不实用了，细微的 pitch 抖动才是
 * 有用的，先看一半，±6 吧，这样可以调整的更精细一点". One encoder detent is now 5 cents
 * instead of 10, and a usable vibrato sits in the middle of the ring instead of its first
 * pixel. An octave either way is still a key, which is where an octave belongs. */
#define WV_CUT_SPAN    4.0f
#define WV_MORPH_SPAN  ((float)(WV_FRAMES - 1u))
#define WV_PITCH_SPAN  6.0f

#define WV_LFOS 2

/* The LFO's waveform. SINE is what every route was built on; TRI is the second shape a
 * slow sweep wants (a linear rise is easier to hear as movement than a sine's ease-in),
 * and S&H is the one a sample-and-hold step is. The step holds its value for a whole
 * period, which for a slow LFO is a sequence of pitches rather than a sweep. */
enum { WV_LFOSH_SINE, WV_LFOSH_TRI, WV_LFOSH_SH, WV_LFOSH_N };

/* Retrigger: does a note-on reset this LFO's phase?
 *
 *   FREE     no. The LFO has been running since boot and every note lands wherever it is.
 *            This is what the LFO did before the switch existed, so the default keeps
 *            every existing patch sounding the same.
 *   RETRIG   yes, on every note-on. Because the phase is per LFO and not per voice
 *            (eight copies of one sine would be eight times the cost for one sine), a
 *            new note moves the sweep for the voices already sounding too -- a chord
 *            played one finger at a time restarts the sweep on each finger. That is the
 *            audible difference between this and a per-voice LFO, and it is the reason
 *            the switch is worth having rather than a detail.
 */
enum { WV_LFO_FREE, WV_LFO_RETRIG, WV_LFO_TRIG_N };

/* One LFO. Two of these in wv_state, each with its own rate, phase, shape and its own
 * seven amounts -- an LFO that shared amounts with another would not be a second LFO,
 * it would be a second phase for the first one. */
typedef struct {
    uint8_t rate;                 /* 0..127; 0 = this LFO is off */
    /* The rate's mode. In Hz the knob is the exponential 0.02..20 Hz curve it always
     * was; in SYNC it snaps to the eight clock divisions the arpeggiator already uses
     * (ARP_STEP_MS), at the tempo the transport is running at. Same knob, two scales:
     * 0.02..20 Hz is the right range for a free sweep and 1/4..1/64 is the right one
     * for a sweep that has to sit on the beat, and neither range contains the other. */
    uint8_t sync;                 /* 0 = Hz, 1 = clock division */
    uint8_t trig;                 /* WV_LFO_FREE / WV_LFO_RETRIG */
    uint8_t shape;                /* WV_LFOSH_* */
    uint8_t amt[WV_DESTS];        /* 0..127 per destination, 0 = route off */
    float   ph;                   /* 0..1 */
    uint32_t cycle;              /* whole cycles elapsed: S&H's value index */} wv_lfo;

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
    float    res_z;      /* RES is a modulation destination, so it smooths like CUT */
    /* The LFOs. TWO of them, each one for every voice rather than one per voice: an
     * LFO is not a voice property, and eight copies of the same sine is eight times the
     * cost for one sine. A phase advances once per block, in wv_lfo_step, so every voice
     * is swept together -- which is what a player expects from a filter LFO.
     *
     * Two is the number the panel can present: the LFO page holds one set of seven
     * destination slots and the REC button swaps which LFO owns them (the player's call,
     * 2026-11-04). A third would need a second slot set, not a third struct. */
    wv_lfo   l[WV_LFOS];
    /* Seconds one clock step lasts, pushed in by the app once per block (0 = no clock).
     * The arpeggiator owns the tempo; the DSP only divides what it is handed, so there is
     * one note-division table in the firmware and it is the app's. */
    float    lfo_step_s;
    /* Which LFO the PAGE is editing. The DSP needs it only for wv_sine(), the waveform
     * the panel draws and the tests assert; every route reads its own LFO by index. */
    uint8_t  lfo_edit;
    /* The filter's output: which of the SVF's three taps the VCA reads.
     *
     * This costs nothing to offer. Zavalishin's topology-preserving SVF computes lp, bp
     * and hp every sample already -- the three are intermediate results of one iteration
     * -- and the voice was reading `lp` and discarding the other two. Selecting a tap is
     * a pointer written once per block, not a second filter.
     */
    uint8_t  ftype;        /* WV_FTYPE_* */
    /* KEY TRACKING: how far the corner follows the note, in percent of one octave per
     * octave played, from C4 (60).
     *
     * Stored as 0, 25, 50, 75, 100 rather than 0..127, because the setting is a choice
     * out of five and not a fine control: a knob that can land on 37 % key tracking is
     * a knob that makes two patches that should match not match. The panel offers the
     * five; the DSP just multiplies.
     */
    uint8_t  ktrack;       /* 0, 25, 50, 75, 100 */
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
    /* PER-FRAME PHASE OFFSET, one byte per frame, in POINTS of the 256-point cycle.
     *
     * This is the whole of SYNTH's EDIT: no flash, no RAM copy, no second table. The
     * table stays exactly where it is in flash and only the *index* moves.
     *
     * Why it matters. wv_table_read2(fa, fb, ph, t) reads both frames at the SAME phase,
     * so MORPH between two frames is a sample-by-point sum of two shapes exactly in
     * phase -- and two shapes in phase sum to a third shape whose partials add or cancel
     * depending on where they line up. With an offset per frame, MORPH becomes a phase
     * scan: the pair slides through each other as the knob turns, so the sum passes
     * through the cancellations instead of starting at one.
     *
     * Measured on SAW SQR through the real SOFT shaper at amount 1.0, blended frames
     * only: no offset holds the spectral centroid inside 1.00x, a 16-point-per-frame
     * ramp spreads it 1.16x. Small, but it is timbre rather than level, and it costs one
     * add and one mask per frame read.
     *
     * Null means every frame at zero, which is bit-identical to the build before this
     * existed -- the same rule MORPH followed, so an old patch is unchanged.
     */
    const uint8_t *foff;
} wv_bank;

/* Read the bank at phase `ph` (0..1) and morph position `mf` (0..WV_FRAMES-1),
 * both linearly interpolated. Returns -1..1. */
float wv_table_read(const wv_bank *b, float ph, float mf);

/* The same read with the two frames already resolved AND their phase offsets applied by
 * the caller's index arithmetic -- see wv_bank.foff. `o0`/`o1` are the two frames'
 * offsets in points, already wrapped to 0..WV_POINTS-1. */
float wv_table_read2o(const int16_t *fa, const int16_t *fb, float ph, float t,
                      uint32_t o0, uint32_t o1);

/* The same read, with the frame pair resolved once (wv_frames_at) and the samples
 * taken from it (wv_table_read2). A block renderer should hoist: MORPH does not change
 * inside a block, and resolving the pair per sample is 12-16% of the read path. `fb`
 * null means the position sits on a frame and only one table is read. */
void  wv_frames_at(const wv_bank *b, float mf, const int16_t **fa, const int16_t **fb, float *t);
float wv_table_read2(const int16_t *fa, const int16_t *fb, float ph, float t);

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
/* A pitched sample held with a loop whose bounds are NOT the region's: what sounds is
 * [start, end), what repeats is [loop_start, loop_end). The loop lives inside the
 * region, and 0/0 means "the whole region" (what wv_note_on_sample always did).
 *
 * This exists because the two pairs are genuinely two: a sample that plays an intro
 * and then sustains a tail loop is the ordinary use, and one range cannot express it.
 * wv_note_on_sample is kept as the "loop the whole thing" form so the existing
 * callers (tests, offline renderers) do not have to change. */
uint32_t wv_note_on_sample_loop(wv_state *s, const wv_sample *smp, uint8_t note, uint8_t vel,
                                uint32_t start, uint32_t end,
                                uint32_t loop_start, uint32_t loop_end, int loop);

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

/* What the second core contributed (wv_dsp.c): the blocks it returned, the ones it
 * did not, and the largest sample it ever rendered. For the device only -- the host's
 * second core runs inline, so these are always "correct" there. */
uint32_t wv_c2_job_count(void);
uint32_t wv_c2_fail_count(void);
float    wv_c2_peak(void);

/* How many voices are sounding (for the UI and for the tests). */
uint32_t wv_active(const wv_state *s);

/* The LFO's rate in Hz for a panel value 0..127 (0 = off). Exposed so the panel can
 * print the rate instead of a percentage: "0.4 Hz" is a rate, "37" is not. */
float wv_lfo_hz(uint32_t v127);
/* The rate one LFO is running at now, honouring its Hz/SYNC mode. */
float wv_lfo_hz_for(const wv_state *s, uint32_t which);
/* The LFO's raw value for the current phase, in -1..1, and 0 when the rate is 0.
 *
 * Exposed because two routes now read the same waveform (CUT via lfo_cut_scale, MORPH via
 * morph_mod) and a test that re-derived the sine itself would be asserting its own copy of
 * the arithmetic rather than the DSP's. What is NOT exposed is how either route scales it. */
float wv_sine(const wv_state *s);
/* The same waveform for a NAMED LFO. Two of them run at once and a route reads its own,
 * so the panel and the tests need to ask for one by index rather than by "the one the
 * page happens to be editing". */
float wv_lfo_value(const wv_state *s, uint32_t which);

/* The morph position the readers are using this block, in frames 0..WV_FRAMES-1: the knob
 * plus whatever the MORPH route is adding, clamped to the travel. Exported so the panel can
 * draw what is SOUNDING instead of what the knob says -- with an LFO on MORPH the two are
 * different numbers, and a readout showing the knob while the sound sweeps is a readout that
 * disagrees with the ear.
 *
 * PURE: it reads the LFO phases and does not advance them. Calling this once per frame from
 * the renderer therefore cannot change the modulation rate, which is the whole reason it is
 * allowed out of the DSP at all. */
float wv_morph_pos(const wv_state *s);

/* The filter's tap. LP is the default and the only one this app had; HP and BP are the
 * two the SVF was already computing and throwing away. */
enum { WV_FTYPE_LP, WV_FTYPE_HP, WV_FTYPE_BP, WV_FTYPE_N };

/* KEY TRACKING, as the corner's multiplier for one note: 2^((note-60)/12 * pct/100).
 * At 100 % a note an octave up has its corner an octave higher; at 0 % this is 1.0 and
 * every note has the corner the CUT knob says. 50 % is half an octave per octave, which
 * is the setting that keeps a filter sounding even across the keyboard rather than
 * opening up as you climb. */
float wv_ktrack_scale(uint32_t note, uint32_t pct);

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
const float *wv_last_job_buffer(void);
/* `tap`, when non-null, receives the voice sum before MASTER and the limiter: the
 * scope reads it so that turning the listening level down does not shrink the trace.
 * Pass 0 when the caller does not draw a scope. */
void wv_render_split(wv_state *s, const wv_bank *b, float *out, uint32_t n, float master,
                     int cpu2, int (*run)(void (*)(uint32_t), uint32_t),
                     int (*wait)(void), void (*sync)(void), float *tap);

#endif /* WV_DSP_H */
