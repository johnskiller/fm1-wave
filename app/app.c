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
#include "wv_banks.h"

#define WAVE_HZ   44100.0f
#define BLK_MAX   256u

static wv_state dsp;
static wv_bank  bank = { WV_BANKS[0] };

/* Held notes as the UI sees them, and the velocity each arrived with. The DSP gets
 * note on/off from here; nothing in the audio path reads plat_keys, because the
 * interrupt must not consult the matrix scanner. */
static uint32_t held_prev;
static uint8_t  bank_i;

/* the panel's four live knobs, as 0..127 */
static uint8_t k_cut = 127, k_res = 0, k_shape = WV_SHAPE_SOFT, k_amount = 0;
static uint8_t k_morph = 0;

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

static void input(void)
{
    uint32_t i, keys = plat_keys(), btn = plat_buttons();
    int32_t e;

    /* touch.h only for the velocity; the keys themselves come from plat_keys so a
     * build without APP_TOUCH still works */
    for (i = 0; i < NKEYS; i++) {
        uint32_t bit = 1u << i;
        if ((keys & bit) && !(held_prev & bit)) {
            uint8_t vel = touch_vel(i);
            float hz = 440.0f * fm_exp2f((float)((int)i + 53 - 69) / 12.0f);
            wv_note_on(&dsp, (uint8_t)(i + 53), vel ? vel : 100, hz / WAVE_HZ);
            /* the FM-1 as a keyboard as well as an instrument: the keys go out over
             * USB-MIDI and Bluetooth, so a DAW can record what is being played */
            plat_midi_send(0x09u | 0x90u << 8 | (uint32_t)(i + 53) << 16
                           | (uint32_t)(vel ? vel : 100) << 24, TO_USB | TO_BLE);
        } else if (!(keys & bit) && (held_prev & bit)) {
            wv_note_off(&dsp, (uint8_t)(i + 53));
            plat_midi_send(0x08u | 0x80u << 8 | (uint32_t)(i + 53) << 16, TO_USB | TO_BLE);
        }
    }
    held_prev = keys;

    /* the four live knobs: relative, so the panel's position and the value agree
     * from the first turn rather than jumping to wherever the knob happens to be */
    if ((e = plat_enc(EN_K1)) != 0) k_cut    = (uint8_t)clamp((int32_t)k_cut + e * 4, 0, 127);
    if ((e = plat_enc(EN_K2)) != 0) k_res    = (uint8_t)clamp((int32_t)k_res + e * 4, 0, 127);
    if ((e = plat_enc(EN_K3)) != 0) k_shape  = (uint8_t)((k_shape + (e > 0 ? 1 : WV_SHAPE_N - 1)) % WV_SHAPE_N);
    if ((e = plat_enc(EN_K4)) != 0) k_amount = (uint8_t)clamp((int32_t)k_amount + e * 4, 0, 127);
    if ((e = plat_enc(EN_ALGO)) != 0) k_morph = (uint8_t)clamp((int32_t)k_morph + e * 8, 0, 127);

    /* EN_PRESET picks the bank; the bank's first frame is re-read next render */
    if ((e = plat_enc(EN_PRESET)) != 0) {
        bank_i = (uint8_t)((bank_i + (e > 0 ? 1 : WV_BANK_COUNT - 1)) % WV_BANK_COUNT);
        bank.frames = WV_BANKS[bank_i];
    }
    (void)btn;

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
    knob(184, "BANK",   WV_BANK_NAME[bank_i], (int32_t)bank_i * 127 / (WV_BANK_COUNT - 1), 0);
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
    wf_fill = 0;
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

void app_tick(void)
{
    touch_update();
}
