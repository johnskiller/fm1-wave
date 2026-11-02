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
#define WF_W   240
#define WF_SPAN 768                      /* samples held: 3 windows' worth, so the reader never wraps mid-frame */
#define WF_VIEW 384                      /* samples on screen: 8.7 ms, 1.6 columns per sample */
static volatile int8_t  wf_buf[WF_SPAN];
static volatile uint16_t wf_idx;

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

    /* The scope's ring: one write per sample, -127..127. Clamped rather than
     * scaled, because the DSP already bounds its output; a sample that needed
     * clamping is a defect to see, not a level to hide. */
    for (i = 0; i < n; i++) {
        float v = mono[i] * 127.0f;
        int32_t q = (int32_t)v;
        if (q > 127)
            q = 127;
        if (q < -127)
            q = -127;
        wf_buf[wf_idx] = (int8_t)q;
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
        wv_note_on(&dsp, note, vel, hz / WAVE_HZ);
        return;
    }

    /* GRAN: the whole slot as a cloud, pitched from its root so a grain voice is in
     * tune with the same slot played as a sample. The chop-key map is SMPL's: a cloud
     * over a single chop is a legitimate sound but a confusing default, so the keys
     * stay melodic in GRAN and the material knob chooses the slot. */
    if (src == SRC_GRAN) {
        wv_note_on_grain(&dsp, &smp[smp_i], note, vel, 0, 0);
        return;
    }

    /* the slice under this key, 1x from its own start: a chop is played, not
     * detuned, so `note` only decides which chop */
    k = (int)note - SLICE_BASE;
    if (k >= 0 && k < (int)slice_count) {
        wv_note_on_slice(&dsp, &smp[slices_from], note, vel,
                         slices[k].start, slices[k].end, 0);
        return;
    }

    /* no chop under this key: the first slot, pitched from its own root, so a
     * sample can still be played as an instrument */
    wv_note_on_sample(&dsp, &smp[0], note, vel, 0, 0, 0);
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
            wv_note_off(&dsp, note);
            plat_midi_send(0x08u | 0x80u << 8 | (uint32_t)note << 16, TO_USB | TO_BLE);
        }
    }
    held_prev = keys;

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
    if (src == SRC_SYNTH) {
        if ((e = plat_enc(EN_PRESET)) != 0) {
            bank_i = (uint8_t)((bank_i + (e > 0 ? 1 : WV_BANK_COUNT - 1)) % WV_BANK_COUNT);
            bank.frames = WV_BANKS[bank_i];
        }
    } else if (src == SRC_GRAN) {
        /* SPRAY moves to EN_SELECT, so MORPH keeps its own knob in every mode.
         *
         * The first version put SPRAY on EN_PRESET and the material slot on EN_ALGO,
         * which read well on paper ("which material" is one job in both sample modes)
         * and was wrong in the hand: MORPH then had no knob at all in GRAN, and a
         * control that moves between modes is worse than a control that is off in one
         * of them. EN_SELECT is free while a source is selected (SOURCE is B_SEL), so
         * it takes SPRAY and nothing else changes position between modes.
         *
         * SPRAY on EN_SELECT is a 32-turn knob for a 0..127 range; EN_PRESET keeps the
         * material slot (as in SMPL) and MORPH keeps EN_ALGO (as in SYNTH). */
        if ((e = plat_enc(EN_SELECT)) != 0)
            k_spray = (uint8_t)clamp((int32_t)k_spray + e * 4, 0, 127);
        if ((e = plat_enc(EN_PRESET)) != 0 && smp_count)
            smp_i = (uint8_t)((smp_i + (e > 0 ? 1 : smp_count - 1)) % smp_count);
    } else {
        if ((e = plat_enc(EN_PRESET)) != 0 && smp_count)
            smp_i = (uint8_t)((smp_i + (e > 0 ? 1 : smp_count - 1)) % smp_count);
        if (btn_pressed(btn, B_EDIT) && smp_count) {
            /* EDIT in SMPL re-chops the current slot from its own material: the
             * keys then play the chops. There is no chop editor on this panel, so
             * detection is the editor. */
            slice_count = (uint8_t)wv_slices_split(&smp[smp_i], 0.02f, 200,
                                                  slices, WV_SLICE_MAX);
            slices_from = smp_i;
        }
    }
    btn_prev = btn;

    ui_apply();
}

/* ------------------------------------------------------------------- UI --- */

static const char *const SHAPE_NAME[WV_SHAPE_N] = { "SOFT", "HARD", "FOLD", "SINE" };

/* The three sources, named the way the screen names them. The order is SRC_*, so a
 * detented readout and an icon strip can both index it directly. */
static const char *const SRC_NAME[3] = { "SYN", "SMP", "GRN" };

static void num(char *b, int32_t v)
{
    char t[8];
    int i = 0, j = 0;
    if (v < 0) { b[j++] = '-'; v = -v; }
    do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) b[j++] = t[--i];
    b[j] = 0;
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

/* SOURCE has three states and B_SEL's lamp has two, so the screen says it. Two
 * channels, both cheap and both readable from playing distance:
 *
 *   1. the whole screen's background carries the source's tint. The scope band is a
 *      canvas, and a canvas is its background: changing it costs one argument, and
 *      it is the only signal here visible without looking directly at the screen.
 *   2. three icons at the bottom, one lit. Twenty-one pixels tall, in the band below
 *      the knobs that nothing else uses.
 *
 * The tints are the palette's own two brightest steps, not invented greys: a colour
 * on this screen has to keep meaning "value, part, alarm" (gfx.c), so the tint is
 * dim (it is a background, not a value) and the icon is bright.
 *
 * What each icon is: the shape of what a key will read that source — a table's
 * stacked frames, one sample's transient and decay, grains scattered over a slot.
 * Not three letters, for a reason that survives contact with this panel: the knob
 * band is full of text and none of it is read at a glance, it is read when the hand
 * is already on a knob. A source indicator has to answer "what am I about to play"
 * from across the room, and a shape does that where three letters do not. */
static uint16_t src_bg(void)
{
    /* A tint per source: neutral, cool, warm. The palette's steps are greys plus a
     * hue, so the "neutral" case is the untinted line colour and the other two are
     * the dim step at two strengths -- different enough to see at a glance, close
     * enough that no reading is needed. */
    if (src == SRC_SMPL) return C_LINE;      /* the darkest tint */
    if (src == SRC_GRAN) return C_DIM;       /* one step up */
    return C_BLACK;                          /* SYNTH: no tint at all */
}

static void src_icons(void)
{
    int32_t i;
    for (i = 0; i < 3; i++) {
        int32_t x = 40 + i * 60, y = 10;      /* in the strip's own canvas: y 180 on screen */
        uint16_t c = (i == (int32_t)src) ? C_HI : C_LINE;
        if (i == 0) {
            /* SYN: three stacked steps, a table of frames */
            int32_t k;
            for (k = 0; k < 3; k++)
                cv_rect(x + k * 5, y + 14 - k * 6, 22, 4, c);
        } else if (i == 1) {
            /* SMP: one hit and its decay */
            int32_t k;
            cv_rect(x, y, 4, 21, c);
            for (k = 0; k < 8; k++) {
                int32_t h = 18 - k * 2;
                if (h > 0)
                    cv_rect(x + 4 + k * 3, y + 21 - h, 2, h, c);
            }
        } else {
            /* GRN: grains scattered over a span */
            int32_t k;
            cv_rect(x, y + 18, 32, 2, c);
            for (k = 0; k < 5; k++)
                cv_rect(x + k * 7, y + (k & 1 ? 2 : 8), 4, 7, c);
        }
    }
}

/* one knob column: its name, its value, and a bar that follows the value */
static void knob(int32_t x, const char *name, const char *val, int32_t v127, int lit)
{
    uint16_t c = lit ? C_HI : C_GRAY;
    cv_text(x, 6, &FONT_XS, name, c);
    cv_text(x, 26, &FONT_XS, val, lit ? C_WHITE : C_DIM);
    cv_rect(x, 48, 30, 6, C_LINE);
    if (v127 > 0)
        cv_rect(x, 48, 30 * v127 / 127, 6, lit ? C_HI : C_GRAY);
}

void app_frame(void)
{
    static uint32_t last;
    char t[8];
    uint32_t now = plat_ms();

    input();

    /* the scope redraws every frame: it is the one thing on screen that must not
     * look frozen while a note is held */
    cv_begin(240, 112, src_bg());
    {
        /* Read the ring backwards from where the audio has got to, so the newest
         * sample is at the right edge: a scope that scrolls the other way reads as
         * noise. `end` is sampled once; a block landing mid-read costs at most one
         * stale block, not a torn frame. */
        uint16_t end = wf_idx;
        int32_t x;
        int32_t lo, hi, peak;

        /* a centre line, so silence reads as a flat line and not as nothing */
        cv_rect(0, 56, 240, 1, C_LINE);

        /* the frame's own peak, for the vertical gain: one pass, before drawing */
        peak = 0;
        for (x = 0; x < (int32_t)WF_VIEW; x++) {
            uint16_t at = (uint16_t)((end + WF_SPAN - 1u - (uint32_t)x) % WF_SPAN);
            int32_t v = wf_buf[at];
            if (v < 0)
                v = -v;
            if (v > peak)
                peak = v;
        }
        if (peak < 24)
            peak = 24;

        for (x = 0; x < WF_W; x++) {
            /* column x is VIEW*(x) .. VIEW*(x+1) back from the newest sample, divided
             * by 240. Both are fixed-point in samples-per-column: 384/240 = 1.6, so
             * a column spans one or two samples. */
            uint32_t a = (uint32_t)x * WF_VIEW / WF_W;
            uint32_t b = (uint32_t)(x + 1) * WF_VIEW / WF_W;
            int32_t k;
            lo = 127;
            hi = -127;
            if (b <= a)
                b = a + 1;
            for (k = (int32_t)a; k < (int32_t)b; k++) {
                /* `end` is newest-first going backwards, wrapped into the ring */
                uint32_t back = WF_VIEW - 1u - (uint32_t)k;
                uint16_t at = (uint16_t)((end + WF_SPAN - 1u - back) % WF_SPAN);
                int32_t v = wf_buf[at];
                if (v < lo)
                    lo = v;
                if (v > hi)
                    hi = v;
            }
            /* Vertical gain. A fixed 127->50 px mapping is the obvious choice and it
             * is what made this unreadable: one voice at full velocity is 0.21 of the
             * DSP's full scale (eight voices share the headroom, see WV_VOICE_GAIN),
             * so a single note drew 27 counts = 10 px in a 112 px band. Measuring the
             * frame's own peak and filling the band is what a scope with an auto-gain
             * does, and it is the right thing here because the question is "what shape
             * is this", not "what level is this" -- MASTER and the peak are elsewhere.
             *
             * Floor of 24 counts (~ -14 dBFS) so silence does not amplify its own
             * dither into a full-height block, and a ceiling so a loud eight-voice
             * chord cannot clip the drawing into a square wave. */
            lo = lo * 50 / peak;
            hi = hi * 50 / peak;
            if (hi - lo < 1) {
                /* a quiet or steady column is a dot, not a gap: a scope that drops
                 * columns looks broken */
                cv_rect(x, 56 - hi, 1, 1, C_HI);
            } else {
                cv_line(x, 56 - hi, x, 56 - lo, C_HI);
            }
        }
    }
    cv_blit(0, 0);                 /* without this the band is drawn and never shown */

    /* A canvas drawn and never blitted is a canvas drawn for nothing: each band
     * ends in a blit (gfx.c), so the knob band needs its own cv_begin and blit. */
    cv_begin(240, 68, C_BLACK);
    if (env_page) {
        /* The ENV page: the same four bars in the same four positions, so the hand
         * does not move — only what they mean. Time in ms/s as text, because "37" on
         * an envelope is unreadable and "37 ms" is not. */
        knob(4,   "ATTACK",  sel_time(k_eg_a, 0), k_eg_a, 1);
        knob(40,  "DECAY",   sel_time(k_eg_d, 1), k_eg_d, 1);
        knob(76,  "SUSTAIN", (num(t, k_eg_s * 100 / 127), t), k_eg_s, 1);
        knob(112, "RELEASE", sel_time(k_eg_r, 2), k_eg_r, 1);
        knob(148, "ENV",     "ON", 127, 1);
        knob(184, "SOURCE",  SRC_NAME[src], (int32_t)src * 127 / 2, 1);
    } else {
    knob(4,   "CUT",    (num(t, k_cut * 100 / 127), t), k_cut, 0);
    knob(40,  "RES",    (num(t, k_res * 100 / 127), t), k_res, 0);
    knob(76,  "SHAPE",  SHAPE_NAME[k_shape], k_shape * 42, 0);
    knob(112, "AMOUNT", (num(t, k_amount * 100 / 127), t), k_amount, 0);
    if (src == SRC_GRAN)
        knob(148, "GRAINSIZE", sel_time(k_grain, 3), k_grain, 1);
    else
        knob(148, "MORPH",  (num(t, k_morph * 100 / 127), t), k_morph, 0);
    if (src == SRC_SYNTH) {
        knob(184, "SYNTH", WV_BANK_NAME[bank_i], (int32_t)bank_i * 127 / (WV_BANK_COUNT - 1), 1);
    } else if (!smp_count) {
        knob(184, "SMPL", "NONE", 0, 0);
    } else if (src == SRC_GRAN) {
        /* The last band is SPRAY, because that is the knob GRAN got and this is the
         * band that names the mode-specific control. It used to fall through to the
         * SMPL branch and read "DEMO K  4" — a slot name and a slice count, neither of
         * which GRAN uses, sitting where SPRAY lives. The slice count is specifically
         * wrong here: GRAN plays no chops, so the number described nothing. */
        knob(184, "SPRAY", (num(t, k_spray * 100 / 127), t), k_spray, 1);
    } else {
        /* the slot's name and how many chops it produced: the one number that says
         * whether the keys below 53 are going to do anything interesting */
        knob(184, smp[smp_i].name[0] ? smp[smp_i].name : "SMPL",
             (num(t, slice_count), t), (int32_t)smp_i * 127 / (smp_count ? smp_count : 1), 1);
    }
    }
    cv_blit(0, 112);

    /* the source strip, in the band below the knobs that nothing else uses */
    cv_begin(240, 60, C_BLACK);
    src_icons();
    cv_blit(0, 180);

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
    slices_from = 0;
    if (smp_count && pool)
        slice_count = (uint8_t)wv_slices_split(&smp[0], 0.02f, 200,
                                               slices, WV_SLICE_MAX);
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
