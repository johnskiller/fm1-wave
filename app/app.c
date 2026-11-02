/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* fm1-wave, first playable: the keys play the wavetable engine, CUT/RES/SHAPE/
 * AMOUNT are live, and the screen shows the rendered waveform. No menus, no
 * sample page, no envelope page yet — this build exists to answer the questions
 * that only a device can answer (see docs/spec.md, Gate A), and to be playable
 * while the rest is written.
 *
 * Part of the unity build, after the platform: gfx.c (cv_*) and announce.c
 * (app_say) are in scope.
 */
#include "fastmath.h"
#include "touch.h"
#include "wv_dsp.h"
#include "wv_sample.h"
#include "wv_slice.h"
#include "wv_banks.h"

#define WAVE_HZ   44100.0f
#define BLK_MAX   256u
#define SMP_MAX   8

static wv_state dsp;
static wv_bank  bank = { WV_BANKS[0] };

/* The sample pool, as the platform handed it over. `pool` points into the resource
 * blob, not a copy: the blob is 276 KB of flash and there is no RAM for a copy. */
static const uint8_t *pool;
static uint32_t       pool_len;
static wv_sample      smp[SMP_MAX];
static uint8_t        smp_count;
static wv_slice        slices[WV_SLICE_MAX];
static uint8_t        slice_count;
static uint8_t        smp_i;      /* which slot EN_PRESET picked, in SMPL */
/* Where each slot's cloud resumes. The picture is meant to be readable while you play,
 * and at 210 px/s on the demo kit a bar crosses the band in 0.84 s -- faster than the
 * eye resolves, so what you see is a blur and then nothing when the note stops.
 *
 * So the playhead position is kept per slot and the next granular note starts there
 * instead of at the loop start. That has two effects and the second is the point: the
 * picture is legible when still, and consecutive notes continue the material instead of
 * jumping back to 0 -- which is also how a granular instrument should behave, since
 * retriggering a texture usually means "read on", not "restart the sample".
 *
 * Per slot, because the position belongs to the material: switching slots and back
 * returns to where that sample was left. */

static uint8_t        slices_from; /* the slot the current chops came out of */

/* SOURCE: which thing a key plays. SYNTH reads the wavetable, SMPL reads the pool as
 * a sampler, GRAN reads the same pool as a grain cloud. Three sources, one switch:
 * GRAN is the pool with a different reader, not a different instrument, so it shares
 * the knob set and the key map. */
enum { SRC_SYNTH, SRC_SMPL, SRC_GRAN };
static uint8_t src = SRC_SYNTH;

/* Held notes as the UI sees them, and the velocity each arrived with. The DSP gets
 * note on/off from here; nothing in the audio path reads plat_keys, because the
 * interrupt must not consult the matrix scanner. */
static uint32_t held_prev;

/* The scope's window is pitch-synchronous: it shows SCOPE_CYCLES whole periods of the
 * note being played, which is what makes a held note stand still. The alternative --
 * a fixed window -- was measured shifting -14/+14/0/+27/+14/+40 px between consecutive
 * frames on a held A440, which is the "sometimes three cycles, sometimes three and a
 * half" the fixed 384-sample window produced: 440 Hz is not a divisor of the frame
 * rate, so every frame caught a different phase.
 *
 * `scope_note_hz` is set by play_note(), which already computes the Hz for the table
 * path; the sample paths have no pitch to lock to (a chop is played at 1x, a cloud has
 * no period), so they leave it alone and the scope keeps the last melodic pitch. */
static float scope_note_hz;
static uint8_t scope_note_live;

/* how many periods the scope shows. Three is the number that answers "is this a saw"
 * with room to see the ramp twice and the edge once, without squeezing anything above
 * the middle of the keyboard into a comb. */
#define SCOPE_CYCLES 3
/* columns one period gets. 60 puts three periods on a 240 px screen with each period
 * 60 px wide: enough to see a saw's ramp and its edge, not so many that the shape is
 * compressed into a comb. */
#define SCOPE_COLS_PER_CYCLE 60
static uint8_t  bank_i;

/* the panel's four live knobs, as 0..127 */
static uint8_t k_cut = 127, k_res = 0, k_shape = WV_SHAPE_SOFT, k_amount = 0;
static uint8_t k_morph = 0;
/* granular: grain length and scatter. SPRAY is on EN_SELECT in GRAN (the only free
 * encoder) so that every other knob stays where it is in every mode. */
static uint8_t k_grain = 64;      /* 0..127: 10..200 ms */
static uint8_t k_spray = 0;       /* 0..127: 0 makes a grain voice a sample player */

/* The ENV page: B_ENV toggles it and K1-K4 become A/D/S/R while it is up.
 *
 * A page is not a mode. The distinction matters and is the whole reason this is a
 * toggle and not a fourth SOURCE: a page has a button that is lit while it is up and
 * a fixed set of four knobs it owns, so leaving it always returns every knob to the
 * parameter it had. That is why the envelope is allowed to take K1-K4 and GRAIN SIZE
 * is not: there is no button that says "you are now editing the grain".
 *
 * The defaults are the old hardcoded VCA: ~1 ms attack, full sustain, ~30 ms release.
 * A patch made before the envelope existed sounds the same after it. */
static uint8_t env_page;
static uint8_t k_eg_a = 0, k_eg_d = 64, k_eg_s = 127, k_eg_r = 32;

/* ─── the scope ─────────────────────────────────────────────────────────────
 *
 * The screen is 240 columns and the band is 112 px. Two decisions decide whether
 * the result looks like a waveform or like a filled rectangle, and the first
 * version of this got both wrong:
 *
 * 1. **A column is a min and a max, not a peak of |x|.** The old code stored
 *    |x|'s maximum per column and drew it mirrored about the centre, which for any
 *    harmonically rich signal puts nearly every column at full height: a saw, or
 *    anything through the shaper, becomes a solid block. A column has to remember
 *    which way the signal was going, so it keeps both extremes and draws the line
 *    between them.
 *
 * 2. **The window is ~2.7 cycles of the note, not 21.8 ms of anything.** This is
 *    the part that decides whether a shape can be read at all. With 240 columns and
 *    a fixed time window, the cycles a shape occupies is set by the window, and 22 ms
 *    spent on a 220 Hz note after the shaper gives 4.8 cycles over 240 columns: an
 *    eleven-column cycle whose vertical extent is a smear of four samples, which is
 *    why it read as a filled band. 120 samples is 2.7 ms and 2 samples per column,
 *    so a 220 Hz note is 54 columns a cycle and its shape is legible; the top of the
 *    keyboard (784 Hz) is 15 columns a cycle, still readable.
 *
 *    A shorter window is what a musical scope wants, and it is the opposite of the
 *    usual intuition: a long window is for finding a level, a short one is for seeing
 *    a shape. This screen is showing a shape.
 *
 * The reader takes a snapshot of the write index and reads *backwards* from it, so
 * a block arriving mid-read cannot tear the picture: the worst case is that the
 * frame shows data one block old. */
/* ─── the three bands ─────────────────────────────────────────────────────────
 *
 * Every height here is derived from what the band has to draw. The first version had
 * the numbers as literals and got this wrong in a way that does not look like a bug:
 * the material band was cut from 108 to 84 while its contents stayed where they were,
 * so src_shape drew to y=103 inside an 84 px canvas and its bottom 20 px were clipped,
 * and the "ENV PAGE" line at y=92 vanished entirely. gfx clips at the canvas edge
 * (cv_rect: `if (y1 > cv_y1) y1 = cv_y1;`) with no warning, so an overrun reads as a
 * missing element rather than as an error.
 *
 * So: content first, height second, and the sum is checked by an assertion below.
 */
#define KNOB_TOP   4                                  /* knob(): where its ink starts */
#define KNOB_INK   (KNOB_TOP + 34 + 8)                /* label, value, bar = 46 px */
#define KNOB_H     64                                 /* 18 px spare: the round knob art
                                                       * that replaces the bar is not written yet */

#define MAT_PAD    6                                  /* above and below the picture */
#define MAT_WAVE   80                                 /* the picture's own height */
#define MAT_H      (MAT_PAD + MAT_WAVE + MAT_PAD)     /* 92 */

/* The picture runs the full band, and the three knob values sit ON it, in the corners
 * the knobs themselves occupy:
 *
 *                              n/m          top right    (bank or slot)
 *          SPRAY n             ...          bottom left  (GRAN only)
 *                              45ms         bottom right  (morph or grain size)
 *
 * No knob NAME is printed. The panel has three labelled knobs around this screen's top
 * edge and four below it, and the four below are drawn as CUT/RES/SHAPE/AMOUNT without
 * their encoder numbers -- the position is the name. The same argument applies here:
 * the top-right value is the top-right knob. Printing "SEL" over a corner that IS the
 * SELECT knob's corner says nothing the position has not already said.
 *
 * The values are printed over the picture instead of beside it, which costs a clear
 * rectangle of waveform in one corner of each label. That is the trade the player asked
 * for after seeing the side-by-side version: a wider picture with three small labels on
 * top of it reads better than a narrow picture with a clean margin beside it.
 *
 * The picture is drawn first and the text over it, so a label always wins; each one gets
 * its own backdrop-free position rather than a box, because a box is a third colour on
 * screen and this band only has two. */
#define MAT_X0     4
#define MAT_W      232                                /* the picture, x 4..235 */
#define MAT_LAB_L  (MAT_X0 + 2)                       /* the left corners' edge */
#define MAT_LAB_R  (MAT_X0 + MAT_W - 2)               /* the right corners' edge */

/* SYNTH is the one source that needs a clean margin, and the reason is measurable: its
 * picture is drawn at full ink (a wavetable frame IS the picture), while SMPL and GRAN
 * draw their envelope at 2/5 and put 1 px bars on top of it. Overlaying bright text on a
 * bright trace is what the player objected to -- "和标签冲突的厉害". So SYNTH gives up the
 * right-hand 60 px instead, and the labels get a background of their own.
 *
 * 60 px is not a round number picked for looks; it is the widest label plus slack,
 * measured with the font's own advance table rather than estimated:
 *
 *     BANK 1/8    46 px        SAW SQR    42 px   (the widest bank name)
 *     MORPH 0     42 px        MORPH 100  54 px   <- the worst case
 *
 * 180 + 60 is the whole screen with no margins at all, so the picture is 172 and the
 * 60 px column sits at x 176..235 with a 4 px gap after the picture and 4 px at the
 * screen edge. The widest string is 54 px, leaving 6 px of slack. */
#define SYN_W      172                                /* SYNTH's picture, x 4..175 */
#define SYN_COL_W  60                                 /* the labels' column */
#define SYN_COL_R  (240 - 4)                          /* its right edge: x 235 */
#define SYN_COL_X  (SYN_COL_R - SYN_COL_W)            /* 175 */
#define SYN_ROW_2  (MAT_ROW_TOP + 12)                 /* the bank name's own line */
#define MAT_ROW_TOP 8                                 /* screen y 92: SELECT's corner */
#define MAT_ROW_BOT 62                                /* screen y 146: the lower corners */

#define SCOPE_H    (240 - MAT_H - KNOB_H)             /* 84 */
#define SCOPE_MID  (SCOPE_H / 2)                      /* 42 */
#define SCOPE_HALF ((SCOPE_H - 2) / 2)                /* 41: 0 dBFS stays inside the band */

#if (MAT_H + KNOB_H + SCOPE_H) != 240
#error "the three bands must fill the 240 px screen"
#endif

#define WF_W   240
#define WF_SPAN 768                      /* samples held: 3 windows' worth, so the reader never wraps mid-frame */
#define WF_VIEW 384                      /* samples on screen: 8.7 ms, 1.6 columns per sample */
static volatile int16_t wf_buf[WF_SPAN];
static volatile uint16_t wf_idx;

/* Chop the selected slot, and record that the chops came from it.
 *
 * The two have to move together. EN_PRESET changed `smp_i` and left the chops where they
 * were, so SMPL kept playing the boot slot's slices: the screen said SLOT 2/2 and the
 * sound was SLOT 1/2, which is exactly the defect this function exists to make
 * impossible. GRAN differed because it reads `smp[smp_i]` directly, and that difference
 * -- one source honouring the knob and one not -- is what made it visible at all.
 *
 * A slot that is a single hit finds one slice, which is the whole hit, and that is right:
 * one chop is still a chop.
 *
 * Called at boot and on every slot change. Re-chopping on a slot change discards a manual
 * EDIT, which is correct: a chop belongs to the material it was cut from, so carrying the
 * old boundaries into new material would play the wrong region of the wrong sample. */
static void reslice(void)
{
    if (smp_count)
        slice_count = (uint8_t)wv_slices_split(&smp[smp_i], 0.02f, 200, slices, WV_SLICE_MAX);
    else
        slice_count = 0;
    slices_from = smp_i;
}

static void ui_apply(void)
{
    dsp.cut = k_cut;
    dsp.res = k_res;
    dsp.shape = k_shape;
    dsp.sh_amount = k_amount;
    dsp.morph = k_morph;
    dsp.grain_size = k_grain;
    dsp.spray = k_spray;
    dsp.eg_a = k_eg_a;
    dsp.eg_d = k_eg_d;
    dsp.eg_s = k_eg_s;
    dsp.eg_r = k_eg_r;
}

/* --------------------------------------------------------------- audio --- */

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    static float mono[BLK_MAX];
    uint32_t i;
    float m = (float)master_q12 * (1.0f / 4096.0f);

    if (n > BLK_MAX)
        n = BLK_MAX;

    /* the buffer arrives with whatever uac_mix left in it, so clear it rather than
     * add to it; the DSP then writes the app's own signal */
    for (i = 0; i < n; i++) {
        mono[i] = 0.0f;
        lr[2 * i] = 0;
        lr[2 * i + 1] = 0;
    }

    wv_render(&dsp, &bank, mono, n, 4096.0f);

    for (i = 0; i < n; i++) {
        float v = mono[i] * m;
        int32_t s = (int32_t)(v * 8388607.0f);
        /* 24-bit and clipped here as well as in the DSP: whichever of the two is
         * edited next, the DMA never sees a wrapped sample */
        if (s > 8388607)
            s = 8388607;
        if (s < -8388608)
            s = -8388608;
        lr[2 * i] = s;
        lr[2 * i + 1] = s;
    }

    /* The scope's ring: one write per sample, in 16-bit units of full scale.
     *
     * It was int8 (-127..127) while the scope normalised every frame to its own peak,
     * and that made the resolution floor the reason for the normalisation rather than
     * a separate decision. It is int16 now because the scope has a FIXED scale and a
     * fixed scale has to be able to see past 0 dBFS: eight voices at full velocity
     * measure +2.4 dBFS (1.32 of full scale), so an int8 ring would have clipped the
     * one case the scale exists to show. 768 samples of int16 is 1536 B of the 98304
     * the platform gives us, against the 768 the int8 ring took.
     *
     * Pre-master on purpose: MASTER is the listening level, and a scope that shrank
     * when the pot was turned down would be measuring the wrong thing. What this
     * shows is what the instrument produces -- velocity, envelope, polyphony, the
     * shaper, the filter. */
    for (i = 0; i < n; i++) {
        float v = mono[i] * 32767.0f;
        int32_t q = (int32_t)v;
        if (q > 32767)
            q = 32767;
        if (q < -32767)
            q = -32767;
        wf_buf[wf_idx] = (int16_t)q;
        wf_idx = (uint16_t)((wf_idx + 1u) % WF_SPAN);
    }
}

/* ---------------------------------------------------------------- input --- */

/* Slice map: the keys are 53..76 (NKEYS), the slices are numbered from SLICE_BASE
 * upward, because a chop is played near the bottom of the keyboard where a drummer
 * puts it and the top keys stay melodic. A slot with fewer slices than keys leaves
 * the rest playing the whole sample at the right pitch, which is what a player
 * expects when only four chops were made. */
#define SLICE_BASE  53

/* The platform reports buttons held, not pressed: plat_buttons() is a level. Every
 * "toggle on the press" has to make its own edge, and forgetting to is how a button
 * becomes a coin flip that lands while you hold it. */
static uint32_t btn_prev;

static int btn_pressed(uint32_t now, int b)
{
    return (now & (1u << b)) && !(btn_prev & (1u << b));
}

/* One place that decides what a key plays. Everything else in the input path is the
 * same for both sources, which is why SOURCE is a switch and not a second engine. */
static void play_note(uint8_t note, uint8_t vel)
{
    int k;

    if (src == SRC_SYNTH || !smp_count) {
        float hz = 440.0f * fm_exp2f((float)((int)note - 69) / 12.0f);
        scope_note_hz = hz;
        scope_note_live = 1;
        wv_note_on(&dsp, note, vel, hz / WAVE_HZ);
        return;
    }

    /* GRAN: the whole slot as a cloud, pitched from its root so a grain voice is in
     * tune with the same slot played as a sample. The chop-key map is SMPL's: a cloud
     * over a single chop is a legitimate sound but a confusing default, so the keys
     * stay melodic in GRAN and the material knob chooses the slot. */
    if (src == SRC_GRAN) {
        /* Always from the start of the material.
         *
         * A grain fades in and out over the same piece of the sample that a one-shot
         * would have fired once, so the sound of a note is decided by the material at
         * the point it starts reading. The playhead used to resume where the last note
         * parked, which makes that point depend on when the previous note was RELEASED:
         * measured on the demo kit, the first 100 ms of a cloud started at 50 % of the
         * slot is 25.7 dB below one started at 0, and at 100 % it is 57.2 dB down. The
         * same key at the same velocity would be near-silent because of history, and that
         * is not an instrument.
         *
         * The cost is one-sided and known: scatter cannot reach backwards past sample 0,
         * so at high SPRAY the cloud is denser at the start of the material than it would
         * be in the middle (measured, 11.7 % of grains in the first 5 % of the slot
         * against 5 % uniform). That is a bias toward the transient, which is the
         * direction a player would accept.
         */
        wv_note_on_grain(&dsp, &smp[smp_i], note, vel, 0u, 0);
        return;
    }

    /* the slice under this key, 1x from its own start: a chop is played, not
     * detuned, so `note` only decides which chop */
    /* The chops are only usable if they were cut from the slot that is selected now.
     * reslice() keeps the two in step, and this check is the belt to that braces: if a
     * future path changes `smp_i` without re-chopping, the keys fall back to the whole
     * slot rather than playing one sample's boundaries inside another sample. Silence in
     * the wrong place would be a bug; the wrong region of the wrong sample is worse
     * because it still sounds like something. */
    k = (int)note - SLICE_BASE;
    if (k >= 0 && k < (int)slice_count && slices_from == smp_i) {
        wv_note_on_slice(&dsp, &smp[smp_i], note, vel,
                         slices[k].start, slices[k].end, 0);
        return;
    }

    /* no chop under this key: the whole selected slot, pitched from its own root, so a
     * sample can still be played as an instrument. This was `smp[0]` -- the boot slot --
     * which made EN_PRESET a display-only control for every key above the chops. */
    wv_note_on_sample(&dsp, &smp[smp_i], note, vel, 0, 0, 0);
}

/* the input state, kept for the lamp pass at the end of the frame: the lights are
 * drawn after the screen, and rebuilding the key mask there would read the matrix a
 * second time in the same frame */
static uint32_t in_btn, in_keys;

static void input(void)
{
    uint32_t i, keys = plat_keys(), btn = plat_buttons();
    int32_t e;

    in_btn = btn;
    in_keys = keys;

    /* touch.h only for the velocity; the keys themselves come from plat_keys so a
     * build without APP_TOUCH still works */
    for (i = 0; i < NKEYS; i++) {
        uint32_t bit = 1u << i;
        uint8_t note = (uint8_t)(i + 53);
        if ((keys & bit) && !(held_prev & bit)) {
            uint8_t vel = touch_vel(i);
            play_note(note, vel ? vel : 100);
            /* the FM-1 as a keyboard as well as an instrument: the keys go out over
             * USB-MIDI and Bluetooth, so a DAW can record what is being played */
            plat_midi_send(0x09u | 0x90u << 8 | (uint32_t)note << 16
                           | (uint32_t)(vel ? vel : 100) << 24, TO_USB | TO_BLE);
        } else if (!(keys & bit) && (held_prev & bit)) {
            /* the last key up: the scope can go back to a neutral window. A single
             * key-up while others are held must not, so the flag is cleared only when
             * no key is left down -- checked after the loop. */
            wv_note_off(&dsp, note);
            plat_midi_send(0x08u | 0x80u << 8 | (uint32_t)note << 16, TO_USB | TO_BLE);
        }
    }
    held_prev = keys;
    if (!keys)
        scope_note_live = 0;

    /* the four live knobs: relative, so the panel's position and the value agree
     * from the first turn rather than jumping to wherever the knob happens to be */
    /* The ENV page owns K1-K4 while it is up. The page is a toggle with a lit button,
     * not a fourth mode, so leaving it puts every knob back where it was — see the
     * note on env_page. */
    if (btn_pressed(btn, B_ENV))
        env_page ^= 1u;

    if ((e = plat_enc(EN_K1)) != 0) {
        if (env_page) k_eg_a = (uint8_t)clamp((int32_t)k_eg_a + e * 4, 0, 127);
        else          k_cut  = (uint8_t)clamp((int32_t)k_cut  + e * 4, 0, 127);
    }
    if ((e = plat_enc(EN_K2)) != 0) {
        if (env_page) k_eg_d = (uint8_t)clamp((int32_t)k_eg_d + e * 4, 0, 127);
        else          k_res  = (uint8_t)clamp((int32_t)k_res  + e * 4, 0, 127);
    }
    if ((e = plat_enc(EN_K3)) != 0) {
        if (env_page) k_eg_s = (uint8_t)clamp((int32_t)k_eg_s + e * 4, 0, 127);
        else          k_shape = (uint8_t)((k_shape + (e > 0 ? 1 : WV_SHAPE_N - 1)) % WV_SHAPE_N);
    }
    if ((e = plat_enc(EN_K4)) != 0) {
        if (env_page) k_eg_r = (uint8_t)clamp((int32_t)k_eg_r + e * 4, 0, 127);
        else          k_amount = (uint8_t)clamp((int32_t)k_amount + e * 4, 0, 127);
    }
    /* EN_ALGO is MORPH in SYNTH, and GRAIN SIZE in GRAN. This is the one place a knob
     * changes meaning with the mode, and it is deliberate: MORPH has nothing to read
     * in a grain voice (wv_table_read is the only consumer, and a cloud never reaches
     * it), while GRAIN SIZE is the most-turned granular parameter and has to be on a
     * knob. GRAN needs seven controls for seven encoders, and this is the seventh.
     * In SMPL it stays MORPH and is dead, as it always was. */
    if ((e = plat_enc(EN_ALGO)) != 0) {
        if (src == SRC_GRAN) k_grain = (uint8_t)clamp((int32_t)k_grain + e * 4, 0, 127);
        else                 k_morph = (uint8_t)clamp((int32_t)k_morph + e * 8, 0, 127);
    }

    /* SOURCE: SYNTH reads the wavetable, SMPL reads the sample pool, GRAN reads it as
     * a cloud. This is the one switch that changes what a key does, so it lives next
     * to the keys.
     *
     * It was on EN_SELECT as a detented stop in the spec, and moved here when the
     * sample path needed a switch and B_ENV's page did not exist yet. Both are now
     * true at once -- the page exists and SOURCE is on B_SEL -- and that is the
     * better arrangement: a three-state switch on a button plus an on-screen
     * indicator uses one button, while a detented encoder for the same thing spends
     * a knob that GRAN needs for SPRAY. The screen, not the button's lamp, says which
     * source is live; a lamp can only say on or off, and there are three. */
    if (btn_pressed(btn, B_SEL))
        src = (uint8_t)((src + 1u) % 3u);

    /* EN_PRESET is a different control depending on SOURCE (see docs/spec.md):
     * SYNTH picks the bank, SMPL picks the sample. One knob, two jobs, because the
     * panel has one knob and both jobs are "which material". */
    /* EN_SELECT picks the material in every source, because it is the knob marked
     * SELECT and that is what SELECT means: which thing is loaded. In SYNTH that is the
     * bank, in SMPL and GRAN it is the sample slot. It sits top-right on the screen
     * because it sits top-right on the panel.
     *
     * The rest follows the same rule -- the screen's corners are the panel's corners:
     *
     *              SELECT   (top right)    material: BANK / SLOT
     *              PRESET   (bottom left)  GRAN only: SPRAY
     *              ALGO     (bottom right) SYNTH: MORPH   GRAN: GRAIN SIZE
     *
     * Both lower knobs are free in every mode except GRAN, where SPRAY and GRAIN SIZE
     * are the two things a granular player reaches for. That is why SPRAY could leave
     * EN_SELECT without costing anything: the knob that used to hold it now holds the
     * material, which is the job it is named after. */
    if ((e = plat_enc(EN_SELECT)) != 0) {
        if (src == SRC_SYNTH) {
            bank_i = (uint8_t)((bank_i + (e > 0 ? 1 : WV_BANK_COUNT - 1)) % WV_BANK_COUNT);
            bank.frames = WV_BANKS[bank_i];
        } else if (smp_count) {
            smp_i = (uint8_t)((smp_i + (e > 0 ? 1 : smp_count - 1)) % smp_count);
            reslice();
        }
    }
    if (src == SRC_GRAN) {
        /* SPRAY on EN_PRESET: bottom-left, next to GRAIN SIZE on bottom-right, because
         * those are the two knobs a granular player turns together. It was on EN_SELECT
         * only because the material had to live somewhere else; now the material is on
         * SELECT and this knob is free. */
        if ((e = plat_enc(EN_PRESET)) != 0)
            k_spray = (uint8_t)clamp((int32_t)k_spray + e * 4, 0, 127);
    } else if (src == SRC_SMPL) {
        if (btn_pressed(btn, B_EDIT) && smp_count) {
            /* EDIT in SMPL re-chops the current slot from its own material: the
             * keys then play the chops. There is no chop editor on this panel, so
             * detection is the editor. */
            reslice();
        }
    }
    btn_prev = btn;

    ui_apply();
}

/* ------------------------------------------------------------------- UI --- */

static const char *const SHAPE_NAME[WV_SHAPE_N] = { "SOFT", "HARD", "FOLD", "SINE" };

static void num(char *b, int32_t v)
{
    char t[8];
    int i = 0, j = 0;
    if (v < 0) { b[j++] = '-'; v = -v; }
    do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) b[j++] = t[--i];
    b[j] = 0;
}

/* num() writes into a buffer the caller owns. knob() needs the value inline, and a
 * single static buffer would let the second call overwrite the first before knob()
 * had drawn it -- the same trap sel_time() documents. Ring of four: the widest screen
 * draws four numeric values at once (CUT RES AMOUNT, plus one mode knob). */
/* "3/8": a position and the count of positions. Static ring like pct(), because the
 * caller draws it immediately and a single buffer would be clobbered by the next call. */
/* Rendered width of a string in FONT_XS, from the font's advance table. Proportional,
 * so this cannot be "7 px times the glyph count". */
static int32_t txt_w(const char *t)
{
    int32_t w = 0;
    while (*t) {
        int32_t c = (uint8_t)*t++;
        if (c >= FONT_XS.first && c <= FONT_XS.last)
            w += FONT_XS.adv[c - FONT_XS.first];
    }
    return w;
}

static char *num2(int32_t a, int32_t b)
{
    static char ring[4][8];
    static uint8_t turn;
    char *o = ring[turn++ % 4];
    char t[8];
    int n = 0, i, k;
    for (k = 0; k < 2; k++) {
        int32_t v = k ? b : a;
        if (v < 0) { o[n++] = '-'; v = -v; }
        i = 0;
        do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (i) o[n++] = t[--i];                 /* t[] is least-significant-first */
        if (k == 0) o[n++] = '/';
    }
    o[n] = 0;
    return o;
}

static char *pct(uint8_t v)
{
    static char ring[4][6];
    static uint8_t turn;
    char *b = ring[turn++ % 4];
    num(b, (int32_t)v * 100 / 127);
    return b;
}

/* A time parameter as text: milliseconds below a second, seconds above it. A
 * percentage is meaningless for a time — "37" on an attack could be 37 ms or 37% of
 * four seconds, and the player has no way to tell which.
 *
 * `which` 0/1/2 are attack/decay/release, 3 is GRAIN SIZE. GRAIN SIZE shares the
 * helper because it is a time with the same ms/s presentation, not because it is an
 * envelope stage.
 *
 * The buffer is a small ring: two of these appear in one screen, and one static
 * buffer would make the second call overwrite the first argument before knob() had
 * used it. */
static char *sel_time(uint8_t v, int which)
{
    static char ring[3][10];
    static uint8_t turn;
    char *b = ring[turn++ % 3];
    float sec;
    int n = 0;

    if (which == 3)
        sec = 0.010f * fm_exp2f((float)v * (1.0f / 127.0f) * 4.321928f);  /* 10 .. 200 ms */
    else
        sec = wv_eg_time(which == 0 ? WV_EG_ATTACK
                       : which == 1 ? WV_EG_DECAY : WV_EG_RELEASE, v);

    if (sec < 1.0f) {
        int32_t ms = (int32_t)(sec * 1000.0f + 0.5f);
        if (ms >= 100)
            b[n++] = (char)('0' + ms / 100);
        if (ms >= 10)
            b[n++] = (char)('0' + (ms / 10) % 10);
        b[n++] = (char)('0' + ms % 10);
        b[n++] = 'm';
        b[n++] = 's';
    } else {
        int32_t cs = (int32_t)(sec * 100.0f + 0.5f);   /* centiseconds */
        if (cs / 100 >= 10)
            b[n++] = (char)('0' + cs / 1000);
        b[n++] = (char)('0' + (cs / 100) % 10);
        b[n++] = '.';
        b[n++] = (char)('0' + (cs / 10) % 10);
        b[n++] = (char)('0' + cs % 10);
        b[n++] = 's';
    }
    b[n] = 0;
    return b;
}

/* ─── the source indicator ────────────────────────────────────────────────── */

/* SOURCE has three states and B_SEL's lamp has two, so the screen says it -- the way
 * a colour-coded mixer does: the whole screen takes the source's colour, text and
 * all, and the source's shape is drawn once at the bottom. Which source is live is
 * then visible from across the room without reading anything.
 *
 * This replaces a tint on the scope band alone. That version was real (three distinct
 * near-blacks on the first canvas) and invisible, because two thirds of the screen --
 * the knob band and the strip -- stayed black and took the eye. Covering every canvas
 * is not a bigger version of that tint, it is a different signal: the panel stops
 * looking neutral and starts looking like the instrument is in one of three modes.
 *
 * WHERE THE COLOURS COME FROM. The palette (gfx.c) is five steps: three neutral greys
 * that carry structure, two that carry the theme's hue. There is no second hue in it
 * to give, and palette_set() runs once at boot from APP_PALETTE, so the theme cannot
 * follow the source. But cv_begin's background and cv_text's colour are raw RGB565,
 * not palette indices, so the app can colour its own screen without the platform
 * owning three palettes. That is the whole trick.
 *
 * The hues are the palette's own highlight steps (gfx.c PALETTES: green 150,255,175;
 * amber 255,184,70; cyan 150,225,255), so SYNTH's curve is the same amber the rest of
 * the instrument uses for a value and stepping through sources does not teach a second
 * colour language. What changes per source is which role the hue plays:
 *
 *   SYNTH  dark ground   + amber ink    the default, and the easiest to read
 *   SMPL   amber ground  + amber ink    the same two colours, roles swapped
 *   GRAN   cyan ground   + cyan ink     the ground itself carries the mode
 *
 * So the screen carries two independent facts -- the ground and the ink -- and either
 * one answers "which source". Nothing is inverted into unreadability: white is the
 * platform's one accent ("what is being touched"), so it is kept for the dark grounds
 * and GRAN gets a near-white cyan instead.
 *
 * Each source keeps a distinct shape, drawn ONCE in the strip. It used to be three
 * icons with two dimmed, which is decoration in the one place the eye rests: only one
 * source is ever live. All three shapes are the same idea -- what a key reads -- and
 * differ only in how continuous that is, which is exactly the musical difference:
 *
 *   SYN   a saw: one cycle, continuous, the table being scanned
 *   SMP   a hit and its decay: a waveform that exists once and then fades
 *   GRN   a sine under scattered dots: the same waveform, broken into pieces
 *
 * The first version drew "three stacked steps" for SYNTH on the reasoning that a table
 * has eight frames. But a table read for one note is one waveform: the frames are what
 * MORPH chooses between, not what SOURCE selects. That icon described the data layout
 * instead of the sound. The saw describes the sound. */
typedef struct {
    uint16_t bg;      /* the screen's ground, every canvas */
    uint16_t track;   /* rules and empty bars -- structure, never content */
    uint16_t ink;     /* names, units: read at a glance */
    uint16_t ink_hi;  /* values and curves */
    uint16_t acc;     /* the accent: what is being touched */
} src_skin_t;

static src_skin_t sk;   /* set once per frame from src_skin(), read by the painters */

static src_skin_t src_skin(void)
{
    src_skin_t k;

    if (src == SRC_SMPL) {
        /* Green: one of the three bench scope colours (amber / green / blue-white).
         * The hue is the platform's own GREEN palette step (gfx.c: 80,220,120 /
         * 150,255,175) so it is the same green a THEME=GREEN screen uses, not a fourth
         * invention. SMPL got this colour because it used to be "the other amber" and
         * was indistinguishable from SYNTH -- see src_skin's SYNTH branch for why the
         * three-source mapping wins over THEME here. */
        k.bg     = RGB(13, 41, 22);
        k.track  = RGB(8, 26, 13);
        k.ink    = RGB(80, 220, 120);
        k.ink_hi = RGB(150, 255, 175);
        k.acc    = C_WHITE;
    } else if (src == SRC_GRAN) {
        /* cyan as the ground, and the ink stays inside the hue: a cyan curve on dim
         * cyan says "grains" before a single label is read. White would fight it. */
        k.bg     = RGB(8, 38, 52);
        k.track  = RGB(4, 22, 30);
        k.ink    = RGB(60, 180, 235);
        k.ink_hi = RGB(150, 225, 255);
        k.acc    = RGB(205, 245, 255);
    } else {
        /* SYNTH: amber, the AMBER palette's own two steps, on a ground pulled down from
         * them.
         *
         * This used to keep the platform palette (black ground, neutral grey labels,
         * amber only on the curve), on the argument that overriding it would fight
         * GLOBAL's THEME. That argument does not hold: the three sources are a fixed
         * amber/green/blue-white mapping by design, so THEME stopped reaching this
         * screen the moment the second colour was added. What it actually produced was
         * an inconsistency -- two sources tinted throughout and the default one black
         * with grey labels -- and a screen where the source colour looked like an
         * afterthought on the source you spend the most time in.
         *
         * Same structure as the other two: ground is a dimmed version of the hue, ink
         * is the hue, ink_hi is its highlight step. White stays the accent for "what
         * is being touched" only, in all three.
         *
         * The cost, stated: APP_PALETTE / THEME now changes nothing on this screen.
         * Three fixed source colours and a user-selectable theme cannot both be true;
         * three colours is what the panel is for. */
        k.bg     = RGB(46, 30, 8);
        k.track  = RGB(30, 20, 5);
        k.ink    = RGB(232, 136, 30);
        k.ink_hi = RGB(255, 184, 70);
        k.acc    = C_WHITE;
    }
    return k;
}

/* The source's shape, in the strip. A polyline rather than rects, because the thing
 * being drawn is a curve and a curve is what the scope above shows: the same visual
 * language, one scale down. */
static void src_icon(const src_skin_t *k, int32_t x0, int32_t y0, int32_t w, int32_t h)
{
    /* Geometry is the caller's: the icon is drawn at 32 px in the material band and was
     * drawn at 56 in the strip, and hardcoding either would have made the other land in
     * the wrong place. */
    int32_t mid = y0 + h / 2, amp = h / 2 - 1;
    int32_t i, px = 0, py = 0;

    for (i = 0; i < w; i++) {
        float t = (float)i / (float)(w - 1);
        float v;
        int32_t y;

        if (src == SRC_SYNTH) {
            /* saw: two cycles, so both the ramp and the drop are visible. The drop is
             * the discontinuity the table is read across, and it is the shape whose
             * MORPH the ear most easily follows. */
            float ph = t * 2.0f;
            v = 2.0f * (ph - fm_floorf(ph)) - 1.0f;
        } else if (src == SRC_SMPL) {
            /* one hit and its decay: a waveform that exists once */
            v = fm_sin_turns(t * 3.0f) * (1.0f - t * 0.9f);
        } else {
            /* the sine GRAN is built from, before the grains break it up */
            v = fm_sin_turns(t * 2.0f);
        }
        y = mid - (int32_t)(v * (float)amp);
        if (i == 0) {
            cv_rect(x0, y, 2, 2, k->ink_hi);
        } else {
            cv_line(px, py, x0 + i, y, k->ink_hi);
        }
        px = x0 + i;
        py = y;
    }

    if (src == SRC_GRAN) {
        /* grains: a fixed scatter over the same span. Deterministic on purpose -- a
         * random icon would shimmer every frame, and shimmer is what a fault looks
         * like. Eleven dots at two heights either side of the curve. */
        for (i = 0; i < 11; i++) {
            int32_t gx = x0 + (i * 11 + 3) % w;
            int32_t gy = mid + (((i * 37) % 7) - 3) * (amp / 3);
            cv_rect(gx, gy, 3, 3, k->ink);
        }
    }
}

/* One knob: its name, its value, and a bar that follows the value. The geometry is
 * a parameter rather than a constant because the panel is two rows now (see the
 * panel layout note below), and a row is wider than a cell in the other row.
 *
 * The bar is 8 px rather than 6: at the old 30 px width a bar was a tick mark, at
 * 54 it is the thing the eye actually tracks while turning. */
/* ─── the source shape ────────────────────────────────────────────────────── */

/* What is being FED to the chain, as opposed to what came out of it. The scope at the
 * top shows the result of shaper -> filter -> VCA, which is what the ear hears; this
 * shows the material. Without it a dull or thin sound is ambiguous: is the table like
 * that, or is CUT/SHAPE/AMOUNT doing it? Two pictures answer that, one does not.
 *
 * SYNTH only for now. The table is 256 points and the strip is 240 columns, so it maps
 * almost one to one and every wiggle survives. A sample would need a min/max envelope
 * (8800 samples into 240 columns: transients visible, individual cycles not) and GRAN
 * would need the grain window and its overlap rather than a waveform -- and that shape
 * is already the source icon, so drawing it twice would say nothing.
 *
 * THE PHASE IS FIXED, and that is the whole design rather than a shortcut. Reading the
 * table at the audio phase would scroll, which is the defect the scope was just cured
 * of. Stepping a local phase 0..1 across the width makes this a still picture of the
 * material: it moves when BANK or MORPH moves, and at no other time. It follows the
 * sound, not the moment, which is the job.
 *
 * MORPH goes through the same wv_table_read() the voices use, so the drawn shape is the
 * shape being played -- a 50% morph is the interpolated frame, not frame 3.5 rounded. */
/* ─── the two sample sources' material pictures ───────────────────────────────
 *
 * SYNTH can draw its table directly: 256 points and the picture is the shape you hear.
 * A sample cannot be drawn that way, and the reason is arithmetic rather than style.
 *
 * DEMO K is 9256 samples and the band is 176 px, so each column would stand for 53
 * samples. Drawing the value at one sample per column gives a line that jumps between
 * whatever those 53 happened to be at that instant -- the picture would change character
 * with the phase and tell you nothing. What survives the reduction is the envelope: for
 * each column, the minimum and the maximum over the samples in it, drawn as a vertical
 * bar. That is the standard waveform display in every editor, and it is lossless in the
 * only sense that matters here: a bar from min to max is a true statement about the
 * material in that column.
 *
 * Both pictures answer the same question the table picture answers -- what is in here --
 * and neither is a level meter. The scope above already shows level.
 */

/* A sample's own shape: min/max per column, which is the only reduction that stays
 * honest across 53 samples per pixel.
 *
 * `from`/`to` in the slot's samples let the same function draw a slice's region (SMPL
 * draws the whole slot: the band shows the material, and the chops are what the keys
 * are for).
 *
 * Note what is NOT done here: no per-column peak normalisation. The table picture
 * normalises per frame because a bank's frames differ in peak by 3.4 dB and MORPH must
 * stay readable. A sample's own shape is the material, and its dynamics are part of
 * that shape -- a drum's decaying bars are the information, not a defect to correct.
 * The only scaling is one common factor to the band's height, chosen once for the whole
 * slot, so a quiet sample is drawn quiet. */
/* The material's envelope: min/max per column, filled, drawn dim.
 *
 * A filled min/max band is what a waveform display wants, and it was removed once on my
 * reading of "the sampling is so dense it comes out as a solid shape" -- the reasoning
 * was that 53 samples per column let a drum hit span 60 of the 78 px, so every column of
 * every hit was filled. That measurement was right and the conclusion was wrong: the
 * player has now seen both and wants the fill back. A filled band is how much energy is
 * at this moment in the material; a peak line is only its outline, and at 1 px on a dark
 * ground the outline of a loud passage is a thin thing that does not read as loud.
 *
 * So: fill, at 2/5 of the ink. The brightness is the part that was actually wrong -- SMPL
 * used to draw this at full ink_hi strength while GRAN drew the same material dim, so the
 * two sources disagreed about how loud their own background was. Both now call this with
 * dim(ink, 2, 5), so they cannot drift apart again.
 */
static uint16_t dim6(uint16_t c, int32_t num, int32_t den);

static void env_line(const wv_sample *s, int32_t x0, int32_t y0, int32_t w, int32_t h,
                     uint32_t from, uint32_t to, uint16_t colour)
{
    int32_t mid = y0 + h / 2, amp = h / 2 - 1;
    int32_t i;
    float pk = 0.0f, sc;

    if (!s || !s->data || !s->len || to <= from || to > s->len) {
        /* nothing to draw: a flat line at zero, which says "no material" rather than
         * leaving the band empty and looking like a fault */
        cv_rect(x0, mid, w, 1, colour);
        return;
    }

    {   /* one scale for the whole slot, from its own peak: the picture shows shape,
         * not absolute level (the scope is where level is read) */
        uint32_t j;
        for (j = from; j < to; j++) {
            float v = (float)s->data[j] * (1.0f / 32768.0f);
            if (v < 0.0f)
                v = -v;
            if (v > pk)
                pk = v;
        }
        if (pk < 0.02f)
            pk = 1.0f;
        sc = (float)amp / pk;
    }

    for (i = 0; i < w; i++) {
        uint32_t a = from + (uint32_t)((uint64_t)(to - from) * (uint32_t)i / (uint32_t)w);
        uint32_t b = from + (uint32_t)((uint64_t)(to - from) * (uint32_t)(i + 1) / (uint32_t)w);
        float lo = 0.0f, hi = 0.0f;
        uint32_t j;
        int32_t yh, yl;

        if (b <= a)
            b = a + 1;
        for (j = a; j < b && j < s->len; j++) {
            float v = (float)s->data[j] * (1.0f / 32768.0f);
            if (v < lo)
                lo = v;
            if (v > hi)
                hi = v;
        }
        yh = mid - (int32_t)(hi * sc);
        yl = mid - (int32_t)(lo * sc);
        if (yh < y0)
            yh = y0;
        if (yl > y0 + h - 1)
            yl = y0 + h - 1;
        if (yl < yh)
            yl = yh;
        cv_rect(x0 + i, yh, 1, yl - yh + 1, colour);
    }
}

/* GRAN's material picture: three layers, one per parameter that has a position.
 *
 *   envelope    dim, the whole slot, min/max per column      what the sample is
 *   grain bar   bright, x from pos, WIDTH = grain length     GRAINSIZE
 *   their spread over the map                                SPRAY
 *
 * The design went through two wrong versions, and the reason matters more than the
 * result:
 *
 *   1. Ticks at the grain positions. Two grains landed 5 px apart and read as one tick,
 *      so the picture could not answer "how many grains". Ticks encoded position and
 *      nothing else.
 *   2. The sin² window drawn as a shape. Measured, at the default GRAINSIZE of 45 ms a
 *      grain is 4.0 px wide on a 2 s slot and 9.5 px on an 0.84 s one -- four pixels
 *      cannot show a shape, and two overlapping four-pixel shapes cannot show a sum.
 *
 * What fixed it was noticing that a grain's length IS a length: at 176 px across the
 * slot, GRAINSIZE 0..127 maps to 2..42 px of material (0.84 s slot) or 1..18 px (2 s
 * slot). So the width of a grain's mark is the grain length, and the spread of the
 * marks is SPRAY -- neither needs a symbol, a legend, or a decoded brightness. The
 * picture is a plan view of what the reader is doing.
 *
 * Width uses `inc`, not 1x: a grain at pitch consumes `length * rate` of material, which
 * is why the same GRAINSIZE covers more of the sample when you play higher. Drawing at
 * 1x would be a picture of a parameter instead of a picture of the read.
 *
 * The bars are stacked at four fixed heights rather than all on the axis, because grains
 * that overlap horizontally are common (SPRAY 0 puts them at the same x by definition)
 * and a merged bar would hide the count. Heights are fixed, not randomised: a mark that
 * moves for no reason looks like a fault.
 */
static uint16_t dim(uint16_t c, int32_t num, int32_t den)
{
    int32_t r = ((c >> 11) & 0x1Fu) * num / den;
    int32_t g = ((c >> 5) & 0x3Fu) * num / den;
    int32_t b = (c & 0x1Fu) * num / den;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void src_grain_map(const src_skin_t *k, const wv_sample *s, int32_t x0, int32_t y0,
                          int32_t w, int32_t h, const wv_grains *gr)
{
    int32_t mid = y0 + h / 2;
    int32_t i;
    /* four heights: the grain slots are four, so four lanes are enough for every grain
     * to have its own row and none can hide another */
    static const int8_t lane[WV_GRAINS] = { -7, 7, -3, 3 };

    if (!s || !s->data || !s->len) {
        cv_rect(x0, mid, w, 1, k->ink);
        return;
    }

    /* ── layer 1: the material, dim. It is the background, and it must be readable
     * enough to be a map and dark enough that a bar on top of it is unambiguous. Same
     * painter and same 2/5 brightness as SMPL: the complaint was that SMPL's envelope was
     * much brighter, which was true -- it was drawn in ink_hi at full strength. */
    env_line(s, x0, y0, w, h, 0, s->len, dim(k->ink, 2, 5));

    /* ── layer 2: the loop range. The playhead wraps at loop_end, so a grain appearing on
     * the left after being on the right is the loop and not a jump; without the marks
     * that reads as a fault. Dotted, in the dim ink: it is a boundary, not material. */
    {
        float ls = gr ? gr->loop_start : 0.0f;
        float le = gr ? gr->loop_end : 0.0f;
        int32_t xa, xb;
        if (!gr || !gr->started) {          /* no cloud yet: the whole slot is the loop */
            ls = 0.0f;
            le = (float)(s->len - 1u);
        }
        if (le <= ls)
            le = (float)(s->len - 1u);
        xa = x0 + (int32_t)((float)w * ls / (float)s->len);
        xb = x0 + (int32_t)((float)w * le / (float)s->len);
        /* the loop is the whole slot until there is a way to set a range, so both marks
         * land on the band's own edges and are indistinguishable from its border. Nudged
         * one pixel in so they are visible at all: a mark you cannot see is a mark that
         * is not there. */
        if (xa <= x0)
            xa = x0 + 1;
        if (xb >= x0 + w - 1)
            xb = x0 + w - 2;
        for (i = 0; i < h; i += 3) {
            cv_rect(xa, y0 + i, 1, 1, dim(k->ink, 3, 5));
            cv_rect(xb, y0 + i, 1, 1, dim(k->ink, 3, 5));
        }
    }

    /* ── layer 3: the grains, and the playhead they are scattered around.
     *
     * Each bar: x is where that grain is reading right now, its width is GRAINSIZE at
     * this voice's pitch, and it slides left to right because the readhead does.
     *
     * Two things were wrong in the first version, and the player saw both before I did:
     *
     * (a) The lane was the DSP's grain SLOT index. Slots rotate (`next = (next+1) %
     *     WV_GRAINS`), so a grain changed lane on every respawn and the bars visibly
     *     jumped up and down -- which reasonably looked like SPRAY but was constant at
     *     SPRAY 0. Measured over eight frames, the upper bar's y went
     *     125, 125, 125, 121, 121, 121, 125, 125. The lane now follows DRAWING order
     *     (lowest position gets the upper lane), so a bar changes lane only when the two
     *     change order, i.e. when they cross.
     *
     * (b) SPRAY was only readable as the distance between the two bars, which at low
     *     settings is 17 px in a 176 px band -- easy to miss, and with nothing to measure
     *     it against it does not read as "scatter" at all, just as "two bars somewhere".
     *
     *     A dim playhead tick was added as a reference for that distance, and then
     *     removed on the player's judgement: a grain mark is a claim about reading, so it
     *     must not be drawn when nothing is read -- and by the same argument the playhead
     *     is not a thing you would look at either, since the interesting fact is always
     *     "where are the grains". SPRAY is read from the two bars' separation, and the
     *     mark that carries it is that SPRAY 0 puts them on top of each other.
     *
     * The width is constant -- `length * rate` -- because that is what GRAINSIZE sets. An
     * earlier attempt drew the *remaining* read range, which shrank as the window closed;
     * that made the width breathe with the grain phase, and the width is the one thing
     * here that must not move.
     */
    if (gr) {
        float per_px = (float)s->len / (float)w;
        int32_t order[WV_GRAINS];
        int32_t n = 0;

        /* drawing order by position: two grains are the normal case, and this keeps the
         * near one below the far one regardless of which slot they occupy */
        for (i = 0; i < (int32_t)WV_GRAINS; i++)
            if (gr->g[i].on) {
                int32_t q = n++;
                while (q > 0 && gr->g[order[q - 1]].pos > gr->g[i].pos) {
                    order[q] = order[q - 1];
                    q--;
                }
                order[q] = i;
            }

        for (i = 0; i < n; i++) {
            const wv_grain *g = &gr->g[order[i]];
            int32_t gx, gw, gy;

            /* the material a grain consumes over its whole life is length * rate; 1x here
             * would draw the knob instead of the read */
            gw = (int32_t)((1.0f / (g->dt > 0.0f ? g->dt : 1.0f)) * g->inc / per_px);
            if (gw < 2)
                gw = 2;                     /* a 1 px bar disappears into the material */

            /* past the edge: slide back at full width. A bar one px short of the edge is
             * still true -- a grain is reading there -- while a missing bar says nothing
             * is reading, which is false. */
            if (gw > w)
                gw = w;
            gx = x0 + (int32_t)(g->pos / per_px);
            if (gx + gw > x0 + w)
                gx = x0 + w - gw;
            if (gx < x0)
                gx = x0;

            gy = mid + lane[i] - 2;
            if (gy < y0)
                gy = y0;
            if (gy + 5 > y0 + h)
                gy = y0 + h - 5;
            cv_rect(gx, gy, gw, 5, k->ink_hi);
        }
    }
}

static void src_shape(const src_skin_t *k, int32_t x0, int32_t y0, int32_t w, int32_t h)
{
    int32_t i, py = 0, mid = y0 + h / 2, amp = h / 2 - 1;
    float mf = (float)k_morph * ((float)(WV_FRAMES - 1u) / 127.0f);
    float pk = 0.0f;

    /* Normalise by the frame's own peak, the same decision the scope's auto-gain makes
     * and for the same reason: the question is "what shape is this", not "how loud".
     * It is not cosmetic here -- measured, the SAW SQR bank's frames fall from 32767 to
     * 19892 across MORPH (the banks are peak-normalised per frame, and a triangle at the
     * same peak has less area than a saw), so drawing at a fixed scale made the last
     * frames look almost flat while they are in fact a clean triangle. Normalising lets
     * every frame fill the band and the shape stays readable end to end. */
    for (i = 0; i < w; i++) {
        float v = wv_table_read(&bank, (float)i / (float)w, mf);
        if (v < 0.0f)
            v = -v;
        if (v > pk)
            pk = v;
    }
    if (pk < 0.05f)
        pk = 1.0f;                      /* an empty table: draw it flat, not amplified */
    else
        pk = 1.0f / pk;

    for (i = 0; i < w; i++) {
        float v = wv_table_read(&bank, (float)i / (float)w, mf) * pk;
        int32_t y = mid - (int32_t)(v * (float)amp);
        if (y < y0)
            y = y0;
        if (y > y0 + h - 1)
            y = y0 + h - 1;
        if (i == 0)
            cv_rect(x0, y, 2, 2, k->ink_hi);
        else
            cv_line(x0 + i - 1, py, x0 + i, y, k->ink_hi);
        py = y;
    }
}

static void knob(int32_t x, int32_t y, int32_t w, const char *name, const char *val,
                 int32_t v127, int lit)
{
    uint16_t c = lit ? sk.ink_hi : sk.ink;
    cv_text(x, y, &FONT_XS, name, c);
    cv_text(x, y + 18, &FONT_XS, val, lit ? sk.acc : sk.ink);
    cv_rect(x, y + 34, w, 8, sk.track);
    if (v127 > 0) {
        int32_t fw = w * v127 / 127;
        if (fw < 1)
            fw = 1;                       /* a bar at "just off zero" is not nothing */
        cv_rect(x, y + 34, fw, 8, lit ? sk.ink_hi : sk.ink);
    }
}


/* ---------------------------------------------------------------- scope --- */

/* The vertical scale is FIXED and LINEAR in amplitude: full scale (0 dBFS) is the
 * edge of the band, and a sample at 0.256 of full scale sits a quarter of the way out.
 *
 * This was a per-frame peak normaliser, and the measurement that condemned it is that
 * velocity 20, 60 and 127 all drew 63 px: a scope that auto-gains carries no level at
 * all. But the replacement cannot be a dB axis either, and that is the trap: the
 * instantaneous value of a waveform crosses zero twice a cycle, and in dB a zero
 * crossing is -infinity. A dB scale on a trace plunges to the floor at every crossing
 * and the picture becomes a filled band -- tried, measured, 47 px of ink in every one
 * of the 240 columns.
 *
 * So: linear, fixed, and referenced to full scale. Then amplitude IS level, the shape
 * survives, and the numbers are the DSP's own -- a single note at velocity 127 peaks
 * at 0.256 of full scale (measured), so it draws 12 px each side of the centre in a
 * 92 px band, and velocity 32 draws 3. The eight-voice headroom is above full scale,
 * so a full chord runs into the edge of the band, which is exactly what a scope does
 * when the signal exceeds the screen.
 *
 * Pre-master on purpose: MASTER is the listening level, and a scope that shrank when
 * the pot was turned down would be measuring the wrong thing. What this answers is
 * what the instrument produces -- velocity, envelope, polyphony, shaper, filter. */
#define SCOPE_FS ((float)SCOPE_HALF)   /* full scale reaches the band edge */

/* the graticule: a scope is not a plot, it has a screen. The horizontal lines are the
 * dB markers of the linear scale (full scale = 0 dBFS, a line every 10 dB to -50),
 * the vertical ones are quarters of the window. They are a neutral grey, not a skin
 * colour, because they must sit under all three skins -- a graticule tinted by the
 * source would be content, and a graticule is the glass. */
/* Two levels, and both had to be raised: the dim one was RGB(36,38,42), only 7 units of
 * luminance above the darkest ground -- measured, effectively invisible, so the vertical
 * lines and five of the six dB markers did not read as a grid at all. A graticule that
 * cannot be seen is not a subtle graticule, it is a missing one.
 *
 * The two levels stay well under the ink (luminance 150-185 depending on the skin) so a
 * bright trace still owns the screen: the grid is glass, and glass that competes with the
 * signal is a defect. GRID marks 0 dBFS, the one reference a full-scale chord crosses. */
#define GRID_DIM  RGB(58, 61, 66)     /* was 36,38,42: dLum 7 -> 29 against every ground */
#define GRID     RGB(96, 99, 104)     /* was 62,64,68: the 0 dBFS line, dLum 67 */

/* amplitude as a fraction of full scale (-1..1) to a distance from the centre line */
static float fs_px(float a)
{
    float f = SCOPE_FS;
    float v = a * f;
    if (v > f)
        v = f;
    if (v < -f)
        v = -f;
    return v;
}

void app_frame(void)
{
    static uint32_t last;
    uint32_t now = plat_ms();

    input();

    sk = src_skin();

    /* the scope redraws every frame: it is the one thing on screen that must not
     * look frozen while a note is held */
    cv_begin(240, SCOPE_H, sk.bg);
    {
        /* Read the ring backwards from where the audio has got to, so the newest
         * sample is at the right edge: a scope that scrolls the other way reads as
         * noise. `end` is sampled once; a block landing mid-read costs at most one
         * stale block, not a torn frame. */
        uint16_t end = wf_idx;
        int32_t x;
        int32_t lo, hi, view, tbase;

        /* ── the pitch-synchronous window ────────────────────────────────────────
         *
         * The window is an integer number of periods of the note being played, so the
         * picture stands still. This is the difference between an oscilloscope and a
         * film of one: 440 Hz against a fixed 8.71 ms window gives 3.83 cycles, so
         * every frame catches a different phase and the trace slides (measured:
         * -14/+14/0/+27/+14/+40 px per frame on a held A440).
         *
         * Three things have to be true for it to stand still, and the third is the one
         * that is usually forgotten:
         *   1. the window length is an integer number of periods;
         *   2. the newest sample is the window's right edge, so the start is always
         *      `end - view` and never a frame-rate-dependent quantity;
         *   3. that length ROUNDS to a whole number of samples. At 440 Hz a period is
         *      100.2 samples -- 0.2 of a sample of drift per cycle, 0.6 per window,
         *      which on a 3-cycle window is under one sample and invisible on a 240 px
         *      screen. It would not be invisible for a window of 100 cycles.
         *
         * WF_VIEW is the fallback when no pitch is known (release tail, a sample path).
         * Keeping it as the default rather than freezing on the last note means the
         * scope never lies about a period it no longer has. */
        view = (int32_t)WF_VIEW;
        if (scope_note_live && scope_note_hz > 20.0f) {
            float per = WAVE_HZ / scope_note_hz;      /* samples per period */
            /* The period count is not constant, because the keyboard is not: three
             * cycles of the lowest key (F3, 174.6 Hz) is 758 samples, and three cycles
             * of the highest (E5) is 201. A fixed count would either overrun the ring
             * or draw 15 cycles of a high note. So the count follows the pitch -- as
             * many whole periods as fit in SCOPE_CYCLES worth of pixels-per-period:
             * low notes get 1 cycle (visible detail at 240 columns), high notes get 4.
             * Both are stable; only the zoom differs, which is exactly how a scope
             * behaves when it auto-ranges instead of sweeping.
             *
             * Clamped to the ring, and the ring is 768 samples with the trigger taking
             * up to one period of it. */
            int32_t cyc, v2;
            /* How many whole periods to show: as many as leave at least
             * SCOPE_COLS_PER_CYCLE columns for each one, capped at SCOPE_CYCLES. The
             * columns one period occupies is 240 * per / view, and view is per * cyc,
             * so columns-per-period is 240/cyc -- it does not depend on the pitch at
             * all. What the pitch decides is how many periods FIT, which is the ring's
             * limit and the clamp below.
             *
             * This started as per/80 rounded, which put 440 Hz (100.2 samples/period)
             * at one cycle and drew it across all 240 columns: technically stable, and
             * a single cycle blown up to fill the screen. The column count is the thing
             * that has to be bounded; the sample count follows from it. */
            cyc = (int32_t)(240.0f / (float)SCOPE_COLS_PER_CYCLE + 0.5f);
            if (cyc < 1) cyc = 1;
            if (cyc > SCOPE_CYCLES) cyc = SCOPE_CYCLES;
            v2 = (int32_t)(per * (float)cyc + 0.5f);
            while (v2 > (int32_t)WF_SPAN - (int32_t)per - 2 && cyc > 1) {
                cyc--;
                v2 = (int32_t)(per * (float)cyc + 0.5f);
            }
            if (v2 >= 32 && v2 <= (int32_t)WF_SPAN - (int32_t)per - 2)
                view = v2;
        }

        /* ── the trigger ─────────────────────────────────────────────────────────
         *
         * An integer number of periods is NOT enough, and this is the part that is
         * easy to get wrong: the window still ended at "the newest sample", so each
         * frame started at a different phase -- measured at 28/14/35/-56/48/-42 px of
         * shift between frames on a held A440 even with a 301-sample (3-period)
         * window. A real scope does not do this: it arms on the signal.
         *
         * So find the newest rising zero crossing and put the RIGHT edge of the window
         * on it. Every frame then draws the same phase, because the anchor is a
         * property of the waveform rather than of when the frame happened to fire.
         * The cost is that the right edge is up to one period stale; that is the
         * trigger's own delay and a scope has exactly the same one.
         *
         * Rising, not falling: on a saw the rising crossing is the ramp's own zero, so
         * the discontinuity lands in the middle of the picture instead of at the edge
         * where it reads as a vertical line on the border.
         *
         * If there is no crossing within one window (silence, a DC-ish sample, the
         * last period of a release tail) there is nothing to trigger on and the window
         * falls back to the newest sample. It will drift while it does that, which is
         * honest: there is no period on screen to be stable about. */
        tbase = 0;
        {
            int32_t b;
            for (b = 0; b + 1 < view; b++) {
                int32_t c0 = wf_buf[(uint16_t)((end + WF_SPAN - 1u - (uint32_t)b) % WF_SPAN)];
                int32_t c1 = wf_buf[(uint16_t)((end + WF_SPAN - 2u - (uint32_t)b) % WF_SPAN)];
                if (c0 >= 0 && c1 < 0) {          /* c1 is one sample older: rising */
                    tbase = b;
                    break;
                }
            }
        }

        /* ── the vertical scale: fixed, in dBFS ──────────────────────────────────
         *
         * This was a per-frame peak normaliser, and the measurement that condemned it
         * is that velocity 20, 60 and 127 all drew 63 px: a scope that auto-gains is
         * answering "what shape is this", and the shape is already answered one band
         * down by the material picture. So the question here is the scope's own --
         * how loud, and when -- and a fixed scale answers it.
         *
         * The scale is linear in dB, from SCOPE_DB_TOP down to SCOPE_DB_BOT, mapped
         * onto the half-height each side of the centre. A linear-in-dB axis is what
         * makes velocity read as a step: the DSP's velocity curve is linear in
         * amplitude (measured: 0.00201 at velocity 1 up to 0.25586 at 127), so on a
         * dB axis those equal amplitude steps arrive as a curve, and on a linear axis
         * the same notes would all crowd the top.
         *
         * +6 dB at the top because eight voices at full velocity measure +2.4 dBFS of
         * full scale (1.316); the headroom bar is drawn against 0 dBFS so that number
         * has somewhere to go. -54 dB at the bottom: quiet enough for the tail of a
         * release, and shallow enough that 60 dB of range still uses the height.
         *
         * The columns keep their min/max: a period can span a whole cycle in fewer
         * columns than it has samples, so a column is a range of the signal and not a
         * point on it. */
        {
            const float mid = (float)SCOPE_MID;

            /* ── the graticule ──────────────────────────────────────────────
             *
             * Drawn before the trace, so the waveform runs over it: a scope's grid is
             * the glass, not the signal.
             *
             * The scale is linear in amplitude, so the dB lines are NOT equally
             * spaced: the distance from 0 to -10 dB is 41 * (1 - 0.316) = 28 px, from
             * -10 to -20 dB is 41 * (0.316 - 0.1) = 8.9 px, and so on. That is the
             * signature of a dB scale and it is the point of drawing one: an eye that
             * has read "half the height" off the screen is being told a dB number
             * instead, and half the height on this band is -4 dBFS, not -30.
             *
             * Labels: "+0" only, at the right edge in the top row. The others would be
             * "-10", "-20"... -- five words of six px each in a 2 px gap, which is not
             * readable and not a scope. The 0 line is the reference and the only one
             * a loud chord visibly crosses; the rest of the scale is read off the
             * spacing. */
            {
                int32_t db, col;
                for (col = 1; col * 20 < 240; col++)
                    cv_line(col * 20, 0, col * 20, SCOPE_H - 1, GRID_DIM);
                for (db = 0; db >= -50; db -= 10) {
                    /* the y of db on this band, each side of the centre: fm_db2lin
                     * is the platform's own dB -> linear, the inverse of the scale */
                    int32_t up = (int32_t)(mid - fs_px(fm_db2lin(db)));
                    cv_rect(0, up, 240, 1, db == 0 ? GRID : GRID_DIM);
                    cv_rect(0, (int32_t)(mid + (mid - up)), 240, 1, db == 0 ? GRID : GRID_DIM);
                }
                {
                    char lab[3] = { '+', '0', 0 };
                    cv_text(240 - 24, 1, &FONT_XS, lab, GRID);
                }
            }

            /* a centre line, so silence reads as a flat line and not as nothing */
            cv_rect(0, (int32_t)mid, 240, 1, sk.track);

            for (x = 0; x < WF_W; x++) {
                uint32_t a = (uint32_t)x * (uint32_t)view / WF_W;
                uint32_t b = (uint32_t)(x + 1) * (uint32_t)view / WF_W;
                int32_t k, yup, ydn;

                lo = 32767;
                hi = -32767;
                if (b <= a)
                    b = a + 1;
                for (k = (int32_t)a; k < (int32_t)b; k++) {
                    uint32_t back = (uint32_t)view - 1u - (uint32_t)k + (uint32_t)tbase;
                    uint16_t at = (uint16_t)((end + WF_SPAN - 1u - back) % WF_SPAN);
                    int32_t v = wf_buf[at];
                    if (v < lo)
                        lo = v;
                    if (v > hi)
                        hi = v;
                }

                /* min/max preserved: a column can span more than one sample, so it is a
                 * range of the signal and both ends of that range are drawn */
                yup = (int32_t)(mid - fs_px((float)hi * (1.0f / 32767.0f)));
                ydn = (int32_t)(mid - fs_px((float)lo * (1.0f / 32767.0f)));

                if (yup == ydn) {
                    /* a quiet or steady column is a dot, not a gap: a scope that drops
                     * columns looks broken */
                    cv_rect(x, yup, 1, 1, sk.ink_hi);
                } else {
                    cv_line(x, yup, x, ydn, sk.ink_hi);
                }
            }
        }
    }
    cv_blit(0, 0);                 /* without this the band is drawn and never shown */

    /* ─── the panel body ──────────────────────────────────────────────────────
     *
     * ONE ROW DID NOT FIT. Seven encoders across 240 px is 34 px each, and the names
     * are 3-8 characters of FONT_XS at 6.2 px plus padding: "AMOUNT" measures 43 px
     * and ran 9 px into the next cell. Measured on the host before this change, by
     * scanning the label rows for ink runs -- CUT/RES/SHAPE/MORPH clean, AMOUNT
     * (112..154) overlapping the knob at 148. Shortening the names fixes that one
     * collision and leaves every label at 4-5 characters, which is not what the
     * panel should force a name down to.
     *
     * So the panel is two rows:
     *
     *   row A  K1..K4     4 cells of 60   CUT RES SHAPE AMOUNT      (or A D S R in ENV)
     *   row B  encoders   3 cells of 80   MORPH / knob | SOURCE | title
     *
     * and each row is one encoder each -- location follows the physical knob, which is
     * the rule this panel has held since the first layout (see panel-map.md). What
     * moves is only how much room each control gets, and the row a control sits in.
     *
     * The row heights differ because the rows do different jobs. Row A is four knobs
     * and nothing else, so it is compact (56 px: label 10, value 13, gap, bar). Row B
     * is where the mode-specific controls live, so it is taller and its value line can
     * carry a bank name instead of a number.
     *
     * The bottom 40 px is then free, and it earns its keep as the mode strip: the
     * source icon, and the two selections that have no encoder of their own to be
     * shown next to -- the wavetable bank and the sample slot.
     *
     * Height budget: 112 scope + 56 row A + 76 row B + 40 strip = 284... which is 44
     * too many. The scope gives them up: 112 -> 68. A scope 68 px tall with the
     * waveform filling it is more readable than 112 px half empty (the auto-gain from
     * the previous commit already fills whatever band it is given), and the scope is
     * the one band whose height is not load-bearing.
     *
     *   0..68    scope
     *   68..124  row A
     *   124..200 row B
     *   200..240 mode strip
     */
    /* ─── the material, and the four knobs ───────────────────────────────────────
     *
     * Layout, top to bottom:
     *
     *   0..68    scope        the result of shaper -> filter -> VCA, what the ear hears
     *   68..176  material     the table's own shape, 108 px tall
     *   176..240 the four     CUT RES SHAPE AMOUNT, or A D S R on the ENV page
     *
     * The table's shape earned the middle of the screen and the knobs moved to the
     * bottom, because the shape is now the more useful of the two: at 28 px in the
     * strip it was legible as "something moves" and not as a waveform, and BANK and
     * MORPH are the two parameters it already draws. A number beside a waveform that
     * shows that number's effect is a worse use of a cell than the waveform at a size
     * you can read. So BANK and MORPH lose their cells and keep their values, printed
     * over the picture they control.
     *
     * The knobs being last is also where the hand is: the bottom row of the screen is
     * nearest the bottom row of keys. K1..K4 sit in the physical order left to right,
     * which is the rule this panel has kept since the first layout. */
    cv_begin(240, MAT_H, sk.bg);
    {
        /* The picture runs the full width, and the three knob values sit on top of it in
         * the corners their knobs occupy:
         *
         *                              SEL n/m       top right
         *          PRESET n            ...           bottom left   (GRAN only)
         *                              ALGO 45ms     bottom right
         *
         * No knob NAME is printed. The four knobs below this band are drawn as CUT, RES,
         * SHAPE, AMOUNT with no encoder numbers, because the position is the name. The
         * same holds up here: the value in the top-right corner belongs to the top-right
         * knob. Printing "SEL" over the SELECT knob's own corner adds nothing.
         *
         * The values are printed over the picture rather than beside it, and the cost is
         * one clear rectangle of waveform per label. The player asked for this after
         * seeing the side-by-side version: a full-width picture with three small labels
         * on it reads better than a narrow picture with a clean margin beside it.
         *
         * Drawn picture first, text second, so a label always wins. No boxes behind the
         * text -- a box is a third colour in a band that only has two. */
        const int32_t ytop = MAT_ROW_TOP;
        const int32_t ybot = MAT_ROW_BOT;

        if (!smp_count && src != SRC_SYNTH) {
            cv_text(MAT_X0 + 2, 46, &FONT_XS, "NO SAMPLES", sk.ink);
        } else if (src == SRC_SYNTH) {
            src_shape(&sk, MAT_X0, MAT_PAD, SYN_W, MAT_WAVE);
        } else if (src == SRC_GRAN) {
            /* the voice the cloud is most likely to be heard from: the newest one. With
             * several keys down the picture shows one voice's cloud, and the newest is
             * the one the player just caused. */
            const wv_grains *gr = 0;
            int32_t vi;
            uint32_t newest = 0;
            for (vi = 0; vi < (int32_t)WV_VOICES; vi++) {
                if (dsp.v[vi].active && dsp.v[vi].age >= newest) {
                    newest = dsp.v[vi].age;
                    gr = &dsp.v[vi].gr;
                }
            }
            src_grain_map(&sk, &smp[smp_i], MAT_X0, MAT_PAD, MAT_W, MAT_WAVE, gr);
        } else {
            env_line(&smp[smp_i], MAT_X0, MAT_PAD, MAT_W, MAT_WAVE, 0, smp[smp_i].len,
                     dim(sk.ink, 2, 5));
        }
        if (src != SRC_SYNTH && smp_count)
            cv_text(MAT_X0 + 2, MAT_PAD + 1, &FONT_XS,
                    smp[smp_i].name[0] ? smp[smp_i].name : "SMPL", dim(sk.ink_hi, 3, 5));

        /* ── the three values, in their knobs' corners ── */

        /* Every value gets its name. An earlier pass printed the numbers alone, on the
         * argument that the corner is the name -- the argument the four knobs below use,
         * where CUT really is at CUT's position. It does not carry up here, because the
         * same corner means a different thing in each source: the top right is BANK in
         * SYNTH but SLOT in SMPL and GRAN, and the bottom right is MORPH in SYNTH but
         * GRAIN in GRAN. "2/2" and "45ms" do not say which. A position is only a name
         * when it is the same name every time, and the player said so within a minute.
         *
         * Name first, then the value, so a label reads as one phrase. Left corner is
         * flush left at the band edge, right corner flush right at it, which keeps the
         * two symmetric about the picture.

         * The width is summed from the font's own advance table rather than assumed:
         * FONT_XS is proportional (a digit is 6 px, '/' is 3), so a fixed 7 px per glyph
         * left the right-hand labels 4-9 px off the edge and drifting with the value's
         * digit count. */
        const char *nm;
        char *vv;
        int32_t w;

        /* The right-hand column's edge. SYNTH keeps the labels out of its picture (§ SYN_W
         * above); SMPL and GRAN run theirs full width and print over the envelope, which
         * the player accepted because that envelope is at 2/5 ink and the bars are 1 px. */
        const int32_t xr = (src == SRC_SYNTH) ? SYN_COL_R : MAT_LAB_R;

        /* SELECT: top right. */
        nm = (src == SRC_SYNTH) ? "BANK" : "SLOT";
        vv = num2(src == SRC_SYNTH ? bank_i + 1 : smp_i + 1,
                  src == SRC_SYNTH ? (int32_t)WV_BANK_COUNT : (int32_t)smp_count);
        if (src == SRC_SYNTH || smp_count) {
            cv_text(xr - txt_w(nm) - 4 - txt_w(vv), ytop, &FONT_XS, nm,
                    dim(sk.ink_hi, 3, 5));
            cv_text(xr - txt_w(vv), ytop, &FONT_XS, vv, sk.ink_hi);
        }

        /* SYNTH's second line: the bank's NAME. "BANK 3/8" says which one you are on and
         * nothing about what it is; the name is the part that tells you to keep turning or
         * stop. WV_BANK_NAME has carried these since the generator was written -- SAW SQR,
         * RESO, HOLLOW, VOX, DIGI, OPEN1..3 -- and the screen had never shown one. */
        if (src == SRC_SYNTH)
            cv_text(xr - txt_w(WV_BANK_NAME[bank_i]), SYN_ROW_2, &FONT_XS,
                    WV_BANK_NAME[bank_i], sk.ink_hi);

        /* PRESET: bottom left, GRAN only -- SPRAY is the only thing on that knob. */
        if (src == SRC_GRAN) {
            cv_text(MAT_LAB_L, ybot, &FONT_XS, "SPRAY", dim(sk.ink_hi, 3, 5));
            cv_text(MAT_LAB_L + txt_w("SPRAY") + 4, ybot, &FONT_XS, pct(k_spray),
                    sk.ink_hi);
        }

        /* ALGO: bottom right. MORPH in SYNTH, GRAIN in GRAN. Not drawn in SMPL: morph
         * indexes a wavetable frame and SMPL reads no wavetable, so the knob is dead
         * there, and a dead value is worse than an empty corner. */
        if (src == SRC_SYNTH || src == SRC_GRAN) {
            nm = (src == SRC_SYNTH) ? "MORPH" : "GRAIN";
            vv = (src == SRC_SYNTH) ? pct(k_morph) : sel_time(k_grain, 3);
            w = txt_w(vv);
            cv_text(xr - txt_w(nm) - 4 - w, ybot, &FONT_XS, nm, dim(sk.ink_hi, 3, 5));
            cv_text(xr - w, ybot, &FONT_XS, vv, sk.ink_hi);
        }

        if (env_page)
            cv_text(MAT_X0 + 2, MAT_PAD + MAT_WAVE - 10, &FONT_XS, "ENV PAGE", sk.ink);
    }
    cv_blit(0, SCOPE_H);

    cv_blit(0, SCOPE_H);

    cv_blit(0, SCOPE_H);

    cv_blit(0, SCOPE_H);

    cv_blit(0, SCOPE_H);

    cv_begin(240, KNOB_H, sk.bg);
    if (env_page) {
        /* The ENV page: the same four bars in the same four positions, so the hand
         * does not move — only what they mean. Time in ms/s as text, because "37" on
         * an envelope is unreadable and "37 ms" is not. */
        knob(4,   KNOB_TOP, 54, "ATTACK",  sel_time(k_eg_a, 0), k_eg_a, 1);
        knob(62,  KNOB_TOP, 54, "DECAY",   sel_time(k_eg_d, 1), k_eg_d, 1);
        knob(120, KNOB_TOP, 54, "SUSTAIN", pct(k_eg_s), k_eg_s, 1);
        knob(178, KNOB_TOP, 54, "RELEASE", sel_time(k_eg_r, 2), k_eg_r, 1);
    } else {
        knob(4,   KNOB_TOP, 54, "CUT",    pct(k_cut), k_cut, 0);
        knob(62,  KNOB_TOP, 54, "RES",    pct(k_res), k_res, 0);
        knob(120, KNOB_TOP, 54, "SHAPE",  SHAPE_NAME[k_shape], k_shape * 42, 0);
        knob(178, KNOB_TOP, 54, "AMOUNT", pct(k_amount), k_amount, 0);
    }
    cv_blit(0, SCOPE_H + MAT_H);

    /* The lights, at 30 Hz rather than every frame: the matrix scan and the audio
     * want different rhythms, and the platform's own comment warns that driving the
     * LEDs from a render starves the scanner.
     *
     * What is lit is a state, not a highlight:
     *   B_ENV   the page is up -- a page has to announce itself on the panel, and
     *           this is the whole reason a page is allowed to take K1-K4
     *   B_SEL   SOURCE is not SYNTH, the power-on default. Three states on a two-state
     *           lamp, so the lamp only says "moved"; the screen says which
     *   B_EDIT  SOURCE is SMPL, where EDIT is the chop button and does nothing else
     * Keys light while held, which is plat_leds's own behaviour and the reason to
     * call it instead of plat_lights.
     *
     * plat_leds, not plat_lights: the latter takes the raw masks and skips PLAY's
     * green LED, which is the one lamp the platform drives for the app. */
    if (now - last >= 33u) {
        uint32_t lit = 0;
        last = now;
        if (env_page)
            lit |= 1u << B_ENV;
        if (src != SRC_SYNTH)
            lit |= 1u << B_SEL;
        if (src == SRC_SMPL)
            lit |= 1u << B_EDIT;
        plat_leds(lit, in_keys);
    }
}

/* ------------------------------------------------------------ host checks --- */

/* Every knob and every page state, for the host's "expect". This is not debug code:
 * docs/panel-map.md claims what each control does in each mode, and a claim that no
 * script can check is a claim that drifts. The names are the panel's own, so a
 * failure reads as the control the player would turn.
 *
 * `cut`/`res`/`shape`/`amount`/`morph`/`grain`/`spray` are the values; `src` and
 * `env` are the two states that decide what those values mean. */
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "cut"))        { *got = k_cut;    return 1; }
    if (!strcmp(what, "res"))        { *got = k_res;    return 1; }
    if (!strcmp(what, "shape"))      { *got = k_shape;  return 1; }
    if (!strcmp(what, "amount"))     { *got = k_amount; return 1; }
    if (!strcmp(what, "morph"))      { *got = k_morph;  return 1; }
    if (!strcmp(what, "grain"))      { *got = k_grain;  return 1; }
    if (!strcmp(what, "spray"))      { *got = k_spray;  return 1; }
    if (!strcmp(what, "eg_a"))       { *got = k_eg_a;   return 1; }
    if (!strcmp(what, "eg_d"))       { *got = k_eg_d;   return 1; }
    if (!strcmp(what, "eg_s"))       { *got = k_eg_s;   return 1; }
    if (!strcmp(what, "eg_r"))       { *got = k_eg_r;   return 1; }
    if (!strcmp(what, "src"))        { *got = src;      return 1; }
    if (!strcmp(what, "env"))        { *got = env_page; return 1; }
    if (!strcmp(what, "bank"))       { *got = bank_i;   return 1; }
    if (!strcmp(what, "slot"))       { *got = smp_i;    return 1; }
    if (!strcmp(what, "active"))     { *got = (int)wv_active(&dsp); return 1; }
    /* How many grains the newest voice has sounding, and its grain length in px of the
     * material band. The screen draws one bar per live grain, so "the picture is
     * missing a bar" and "the DSP has no grain there" are different bugs; this is the
     * only way to tell them apart from a test. */
    if (!strcmp(what, "grains")) {
        int32_t vi;
        uint32_t newest = 0, found = 0;
        *got = 0;
        for (vi = 0; vi < (int32_t)WV_VOICES; vi++)
            if (dsp.v[vi].active && dsp.v[vi].age >= newest) {
                newest = dsp.v[vi].age;
                found = 1;
            }
        if (found)
            for (vi = 0; vi < (int32_t)WV_VOICES; vi++)
                if (dsp.v[vi].active && dsp.v[vi].age == newest)
                    *got = (int)wv_grains_on(&dsp.v[vi].gr);
        return 1;
    }
    if (!strcmp(what, "grainpos")) {
        int32_t vi;
        uint32_t newest = 0, found = 0;
        *got = -1;
        for (vi = 0; vi < (int32_t)WV_VOICES; vi++)
            if (dsp.v[vi].active && dsp.v[vi].age >= newest) {
                newest = dsp.v[vi].age;
                found = 1;
            }
        if (found)
            for (vi = 0; vi < (int32_t)WV_VOICES; vi++)
                if (dsp.v[vi].active && dsp.v[vi].age == newest)
                    for (int32_t q = 0; q < (int32_t)WV_GRAINS; q++)
                        if (dsp.v[vi].gr.g[q].on) {
                            *got = (int)((float)MAT_W * dsp.v[vi].gr.g[q].pos
                                         / (float)smp[smp_i].len);
                            break;
                        }
        return 1;
    }
    if (!strcmp(what, "grainpx")) {
        int32_t vi;
        uint32_t newest = 0, found = 0;
        *got = 0;
        for (vi = 0; vi < (int32_t)WV_VOICES; vi++)
            if (dsp.v[vi].active && dsp.v[vi].age >= newest) {
                newest = dsp.v[vi].age;
                found = 1;
            }
        if (found && smp_count)
            for (vi = 0; vi < (int32_t)WV_VOICES; vi++)
                if (dsp.v[vi].active && dsp.v[vi].age == newest) {
                    const wv_grains *g = &dsp.v[vi].gr;
                    float g1 = g->g[0].on ? g->g[0].dt : g->g[1].dt;
                    if (g1 > 0.0f)
                        *got = (int)((1.0f / g1) * g->rate / ((float)smp[smp_i].len / (float)MAT_W));
                }
        return 1;
    }
    return 0;
}

void app_ui_init(void)
{
    /* the first screen is drawn by app_frame; nothing to restore yet */
}

void app_input_only(void)
{
    input();
}

void app_init(void)
{
    wv_init(&dsp);
    ui_apply();
    bank_i = 0;
    bank.frames = WV_BANKS[0];
    held_prev = 0;
    btn_prev = 0;
    wf_idx = 0;
    memset((void *)wf_buf, 0, sizeof wf_buf);
    src = SRC_SYNTH;

    /* The pool is flash, handed over by the platform with its length; plat_res
     * returns NULL on an app built without a resource region, which is not an error,
     * it just means no samples. A pool whose magic is wrong is treated the same way:
     * absent, never half-read. */
    pool = plat_res(&pool_len);
    smp_count = pool ? (uint8_t)wv_smp_parse(pool, pool_len, smp, SMP_MAX) : 0;
    smp_i = 0;
    slice_count = 0;
    /* Chop the first slot at boot so SMPL is playable the moment SOURCE is flipped.
     * A slot that is a single hit finds one slice, which is the whole hit, which is
     * right: one chop is still a chop. */
    reslice();
}

/* app.h asks this at boot (APP_BLE_AT_BOOT). This build has no BLE switch of its
 * own yet, so Bluetooth is always on: the panel's REC-held toggle comes with the
 * UI pass, and until then the FM-1 connects the way it does out of the box. */
int template_ble_off(void) { return 0; }

void app_panic(void)
{
    wv_init(&dsp);
    ui_apply();
    held_prev = 0;
}

/* Say a short message: to the screen, and to a screen reader when one is
 * listening. This is the platform's announce.c (app_say is in the contract, so an
 * app that has anything to say about a state the screen cannot show — "no samples",
 * "restored" — says it here rather than only on the LCD). */
void app_say(const char *a, const char *b)
{
    char t[48];
    int i = 0, k;

    for (k = 0; a && a[k] && i < 23; k++)
        t[i++] = a[k];
    if (b && b[0]) {
        t[i++] = ' ';
        for (k = 0; b[k] && i < 23; k++)
            t[i++] = b[k];
    }
    t[i] = 0;
    ann_say(t, 0, ANN_NOW);
}

/* ------------------------------------------------------------ backup --- */

/* Nothing streams while a note sounds, the panel has no unsaved state, and a
 * restore only changes settings: the pool arrives by its own path (the installer
 * writes the region, then the app is restarted by the update). */
int  app_backup_playing(void)  { return wv_active(&dsp) != 0; }
void app_backup_flush(void)    { }
void app_backup_hold(int on)   { (void)on; }
void app_backup_restored(void) { }

void app_tick(void)
{
    touch_update();
}
