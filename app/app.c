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

/* num() writes into a buffer the caller owns. knob() needs the value inline, and a
 * single static buffer would let the second call overwrite the first before knob()
 * had drawn it -- the same trap sel_time() documents. Ring of four: the widest screen
 * draws four numeric values at once (CUT RES AMOUNT, plus one mode knob). */
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
        /* inverted: the two colours SYNTH uses, roles swapped. The ground is the amber
         * step pulled down until white sits on it -- the palette step itself is meant
         * as ink on black, and as a ground it would swallow the labels. */
        k.bg     = RGB(52, 34, 6);
        k.track  = RGB(30, 20, 4);
        k.ink    = RGB(232, 136, 30);
        k.ink_hi = RGB(255, 184, 70);
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
        /* SYNTH keeps the platform's own palette: it is the default, and the theme is
         * the wearer's choice -- overriding it here would fight GLOBAL's THEME. */
        k.bg     = C_BLACK;
        k.track  = C_LINE;
        k.ink    = C_GRAY;
        k.ink_hi = C_HI;
        k.acc    = C_WHITE;
    }
    return k;
}

/* The source's shape, in the strip. A polyline rather than rects, because the thing
 * being drawn is a curve and a curve is what the scope above shows: the same visual
 * language, one scale down. */
static void src_icon(const src_skin_t *k)
{
    /* The strip's own canvas is 240x40. The icon sits in the left corner and is 56 px
     * wide rather than centred: the strip's right side carries BANK and SLOT, and a
     * centred icon measured 78..161, overlapping the BANK text at 112. A corner icon
     * is also the thing the user asked for -- the bottom band is now free for the
     * information that has no encoder to sit next to. */
    int32_t x0 = 6, y0 = 8, w = 56, h = 24;
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

void app_frame(void)
{
    static uint32_t last;
    uint32_t now = plat_ms();

    input();

    sk = src_skin();

    /* the scope redraws every frame: it is the one thing on screen that must not
     * look frozen while a note is held */
    cv_begin(240, 68, sk.bg);
    {
        /* Read the ring backwards from where the audio has got to, so the newest
         * sample is at the right edge: a scope that scrolls the other way reads as
         * noise. `end` is sampled once; a block landing mid-read costs at most one
         * stale block, not a torn frame. */
        uint16_t end = wf_idx;
        int32_t x;
        int32_t lo, hi, peak, view, tbase;

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

        /* a centre line, so silence reads as a flat line and not as nothing */
        cv_rect(0, 34, 240, 1, sk.track);

        /* the frame's own peak, for the vertical gain: one pass, before drawing */
        peak = 0;
        for (x = 0; x < view; x++) {
            uint16_t at = (uint16_t)((end + WF_SPAN - 1u - (uint32_t)(x + tbase)) % WF_SPAN);
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
            uint32_t a = (uint32_t)x * (uint32_t)view / WF_W;
            uint32_t b = (uint32_t)(x + 1) * (uint32_t)view / WF_W;
            int32_t k;
            lo = 127;
            hi = -127;
            if (b <= a)
                b = a + 1;
            for (k = (int32_t)a; k < (int32_t)b; k++) {
                /* `end` is newest-first going backwards, wrapped into the ring */
                uint32_t back = (uint32_t)view - 1u - (uint32_t)k + (uint32_t)tbase;
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
            lo = lo * 31 / peak;
            hi = hi * 31 / peak;
            if (hi - lo < 1) {
                /* a quiet or steady column is a dot, not a gap: a scope that drops
                 * columns looks broken */
                cv_rect(x, 34 - hi, 1, 1, sk.ink_hi);
            } else {
                cv_line(x, 34 - hi, x, 34 - lo, sk.ink_hi);
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
    cv_begin(240, 56, sk.bg);
    if (env_page) {
        /* The ENV page: the same four bars in the same four positions, so the hand
         * does not move — only what they mean. Time in ms/s as text, because "37" on
         * an envelope is unreadable and "37 ms" is not. */
        knob(4,   6, 52, "ATTACK",  sel_time(k_eg_a, 0), k_eg_a, 1);
        knob(64,  6, 52, "DECAY",   sel_time(k_eg_d, 1), k_eg_d, 1);
        knob(124, 6, 52, "SUSTAIN", pct(k_eg_s), k_eg_s, 1);
        knob(184, 6, 52, "RELEASE", sel_time(k_eg_r, 2), k_eg_r, 1);
    } else {
        knob(4,   6, 52, "CUT",    pct(k_cut), k_cut, 0);
        knob(64,  6, 52, "RES",    pct(k_res), k_res, 0);
        knob(124, 6, 52, "SHAPE",  SHAPE_NAME[k_shape], k_shape * 42, 0);
        knob(184, 6, 52, "AMOUNT", pct(k_amount), k_amount, 0);
    }
    cv_blit(0, 68);

    cv_begin(240, 76, sk.bg);
    {
        /* row B, cell 1: the knob whose meaning follows the source. GRAIN SIZE in
         * GRAN, MORPH otherwise -- MORPH is read only by the table path (see
         * wv_dsp.c), so in GRAN it is the one encoder that does nothing audible and
         * the one that can be lent out. Its label carries the source's name so the
         * lending is visible rather than something to remember. */
        int32_t k;
        if (src == SRC_GRAN) {
            knob(4, 6, 72, "GRAIN SIZE", sel_time(k_grain, 3), k_grain, 1);
        } else {
            knob(4, 6, 72, "MORPH", pct(k_morph), k_morph, 0);
        }

        /* row B, cell 2: SOURCE. Not an encoder -- B_SEL owns it (three states on a
         * two-state lamp, which is why the screen carries it) -- so this cell is
         * drawn as a selector: the three names, the live one bright and underlined.
         * That is the one place on this panel where the screen is the control's only
         * readout, and drawing it as text rather than as a bar says so. */
        cv_text(84, 6, &FONT_XS, "SOURCE", sk.ink);
        {
            int32_t x = 84;
            for (k = 0; k < 3; k++) {
                int32_t on = (k == (int32_t)src);
                int32_t w = 3 * 6 + (k == 1 ? 2 : 0);
                cv_text(x, 30, &FONT_XS, SRC_NAME[k], on ? sk.ink_hi : sk.track);
                if (on)
                    cv_rect(x, 41, w, 2, sk.ink_hi);
                x += 24;
            }
        }

        /* row B, cell 3: the title -- what this box currently is, in the one string
         * that answers "what am I playing". Bank name, slot name, or that the samples
         * are missing, which is the only failure this build can be in at boot. */
        /* Named, not just printed: a bare "SAW SQR" sitting one cell right of SOURCE
         * reads as SOURCE's value. The label is the source's own word for it -- BANK
         * for a table, SLOT for a sample -- and the strip below carries the index
         * ("BANK 3/8"), so the two are the same fact at two precisions. */
        cv_text(166, 6, &FONT_XS, env_page ? "ENV PAGE" : (src == SRC_SYNTH ? "BANK" : "SLOT"),
                sk.ink);
        if (src == SRC_SYNTH)
            cv_text(166, 30, &FONT_XS, WV_BANK_NAME[bank_i], sk.ink_hi);
        else if (!smp_count)
            cv_text(166, 30, &FONT_XS, "NONE", sk.track);
        else
            cv_text(166, 30, &FONT_XS, smp[smp_i].name[0] ? smp[smp_i].name : "SMPL", sk.ink_hi);
    }
    cv_blit(0, 124);

    /* the mode strip: the source's shape in the corner, and the positions that have
     * no encoder to be drawn next to. B_ENV's page and B_SEL's source both live here
     * because the panel has no spare lamps for either. */
    cv_begin(240, 40, sk.bg);
    src_icon(&sk);
    {
        char b[12];
        /* bank, right-aligned: "BANK 3/8" rather than a bar, because a bar cannot say
         * which of eight and the number is the thing worth knowing */
        int32_t bl = 0, k;
        b[bl++] = 'B'; b[bl++] = 'A'; b[bl++] = 'N'; b[bl++] = 'K'; b[bl++] = ' ';
        if (WV_BANK_COUNT > 9u) { b[bl++] = (char)('0' + (bank_i + 1) / 10 % 10); }
        b[bl++] = (char)('0' + (bank_i + 1) % 10);
        b[bl++] = '/';
        b[bl++] = (char)('0' + (WV_BANK_COUNT % 10));
        b[bl] = 0;
        cv_text(70, 14, &FONT_XS, b, sk.ink);

        if (src != SRC_SYNTH && smp_count) {
            /* slot index, the same shape of information for the sample sources */
            bl = 0;
            b[bl++] = 'S'; b[bl++] = 'L'; b[bl++] = 'O'; b[bl++] = 'T'; b[bl++] = ' ';
            b[bl++] = (char)('0' + (smp_i + 1) % 10);
            b[bl++] = '/';
            b[bl++] = (char)('0' + (smp_count % 10));
            b[bl] = 0;
            cv_text(150, 14, &FONT_XS, b, sk.ink);
        }
        (void)k;
    }
    cv_blit(0, 200);

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
