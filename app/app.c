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
/* granular: grain length and scatter. Same "pick the material" knob as SMPL uses;
 * SPRAY takes EN_PRESET in GRAN, exactly as the spec assigns it. */
static uint8_t k_grain = 64;      /* 0..127: 10..200 ms */
static uint8_t k_spray = 0;       /* 0..127: 0 makes a grain voice a sample player */

/* the waveform view: one peak per bucket, written by the audio interrupt */
#define WF_W 240
static volatile uint8_t  wf_peak[WF_W];
static volatile uint16_t wf_fill;

static void ui_apply(void)
{
    dsp.cut = k_cut;
    dsp.res = k_res;
    dsp.shape = k_shape;
    dsp.sh_amount = k_amount;
    dsp.morph = k_morph;
    dsp.grain_size = k_grain;
    dsp.spray = k_spray;
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

    /* The scope, and the arithmetic is the whole thing: 256 frames in a block,
     * 240 columns on screen. One block cannot fill the screen, so the position is
     * carried between blocks (wf_fill) and only wraps after 240 blocks — 1.4 s of
     * 44.1 kHz, which is ~5 px per cycle of a 220 Hz note: one cycle of two to
     * three columns. That is enough to see the shape, which is all a scope here is
     * for. (The first version divided the *absolute* position by 184, so a block
     * landed in one or two columns and the screen showed five vertical lines.) */
    for (i = 0; i < n; i++) {
        uint32_t pos = (uint32_t)wf_fill + i;
        uint16_t b = (uint16_t)(pos % WF_W);
        uint8_t p = (uint8_t)(fm_fabsf(mono[i]) * 127.0f);
        if (p > 127)
            p = 127;
        if (p > wf_peak[b])
            wf_peak[b] = p;
    }
    wf_fill = (uint16_t)((wf_fill + n) % WF_W);
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

static void input(void)
{
    uint32_t i, keys = plat_keys(), btn = plat_buttons();
    int32_t e;

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
    if ((e = plat_enc(EN_K1)) != 0) k_cut    = (uint8_t)clamp((int32_t)k_cut + e * 4, 0, 127);
    if ((e = plat_enc(EN_K2)) != 0) k_res    = (uint8_t)clamp((int32_t)k_res + e * 4, 0, 127);
    if ((e = plat_enc(EN_K3)) != 0) k_shape  = (uint8_t)((k_shape + (e > 0 ? 1 : WV_SHAPE_N - 1)) % WV_SHAPE_N);
    if ((e = plat_enc(EN_K4)) != 0) k_amount = (uint8_t)clamp((int32_t)k_amount + e * 4, 0, 127);
    if (src != SRC_GRAN && (e = plat_enc(EN_ALGO)) != 0)
        k_morph = (uint8_t)clamp((int32_t)k_morph + e * 8, 0, 127);

    /* SOURCE: SYNTH reads the wavetable, SMPL reads the sample pool. This is the
     * one switch that changes what a key does, so it lives next to the keys.
     * B_ENV is the free button on this panel; the ENV page it was meant for is not
     * built yet, so SOURCE gets it first and the page moves to a long press. */
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
        int32_t dig;
        /* EN_PRESET is SPRAY here (the spec's assignment) and the material picks the
         * slot on EN_ALGO, which is MORPH's knob in SYNTH. One read per knob: reading
         * EN_ALGO twice would spend the detent on the first call and always see 0 on
         * the second, so MORPH would never move in GRAN. */
        if ((e = plat_enc(EN_PRESET)) != 0)
            k_spray = (uint8_t)clamp((int32_t)k_spray + e * 4, 0, 127);
        if ((dig = plat_enc(EN_ALGO)) != 0) {
            if (smp_count)
                smp_i = (uint8_t)((smp_i + (dig > 0 ? 1 : smp_count - 1)) % smp_count);
        }
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

static void num(char *b, int32_t v)
{
    char t[8];
    int i = 0, j = 0;
    if (v < 0) { b[j++] = '-'; v = -v; }
    do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (i) b[j++] = t[--i];
    b[j] = 0;
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
    cv_begin(240, 112, C_BLACK);
    {
        int32_t x;
        for (x = 0; x < WF_W; x++) {
            int32_t p = (int32_t)wf_peak[x];
            int32_t h = p * 52 / 127;
            if (h > 0)
                cv_line(x, 56 - h, x, 56 + h, C_HI);
            else
                cv_rect(x, 56, 1, 1, C_LINE);
            wf_peak[x] = 0;
        }
    }
    cv_blit(0, 0);                 /* without this the band is drawn and never shown */

    /* A canvas drawn and never blitted is a canvas drawn for nothing: each band
     * ends in a blit (gfx.c), so the knob band needs its own cv_begin and blit. */
    cv_begin(240, 68, C_BLACK);
    knob(4,   "CUT",    (num(t, k_cut * 100 / 127), t), k_cut, 0);
    knob(40,  "RES",    (num(t, k_res * 100 / 127), t), k_res, 0);
    knob(76,  "SHAPE",  SHAPE_NAME[k_shape], k_shape * 42, 0);
    knob(112, "AMOUNT", (num(t, k_amount * 100 / 127), t), k_amount, 0);
    knob(148, "MORPH",  (num(t, k_morph * 100 / 127), t), k_morph, 0);
    if (src == SRC_SYNTH) {
        knob(184, "SYNTH", WV_BANK_NAME[bank_i], (int32_t)bank_i * 127 / (WV_BANK_COUNT - 1), 1);
    } else if (!smp_count) {
        knob(184, "SMPL", "NONE", 0, 0);
    } else {
        /* the slot's name and how many chops it produced: the one number that says
         * whether the keys below 53 are going to do anything interesting */
        knob(184, smp[smp_i].name[0] ? smp[smp_i].name : "SMPL",
             (num(t, slice_count), t), (int32_t)smp_i * 127 / (smp_count ? smp_count : 1), 1);
    }
    cv_blit(0, 112);

    /* the lights: the knob band has nothing to light, so this is a placeholder
     * until the ENV toggle and the bank buttons exist */
    if (now - last >= 33u) {
        last = now;
        plat_lights(0, 0, 0, 0, 0);
    }
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
    wf_fill = 0;
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
