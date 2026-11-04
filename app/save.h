/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* save.h -- the record that survives a power cycle.
 *
 * app.c owns the statics; this module owns the record's layout, its validation and the
 * one write. The dependency runs one way: app.c fills a save_rec in, save.c writes and
 * reads it. Save.c never reaches back into app.c.
 */
#pragma once
#include <stdint.h>
#include "plat.h"

#define SAVE_MAGIC    0x57415645u          /* "WAVE", the same word APP_BACKUP_MAGIC uses */
/* 2: the record gained `morph_mod` (spec 5c3), so a version-1 record's bytes would be read
 * one field out of register and every knob after it would land on the wrong parameter.
 * The version gate is what makes that a refused load with the defaults booting, instead of
 * an instrument that comes back sounding wrong for no visible reason. */
#define SAVE_VERSION  3u
#define SAVE_SLOTS    8                    /* == SMP_MAX in app.c */
#define SAVE_BANKS    8                    /* == WV_BANK_COUNT */
/* The LFO's two dimensions. Plain numbers rather than taken from wv_dsp.h because the
 * persistence layer does not otherwise know the DSP exists; app.c asserts at compile time
 * that they still match WV_LFOS / WV_DESTS. */
#define SAVE_LFOS     2                    /* == WV_LFOS */
#define SAVE_DESTS    7                    /* == WV_DESTS */

typedef struct {
    uint32_t start, end, loop_s, loop_e;
    uint8_t  on;
} save_region;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;                 /* sizeof this record: a shorter one is rejected */

    /* the knobs, in the panel's own units (0..127 unless the field says otherwise) */
    uint8_t  cut, res, shape, sh_amount;
    uint8_t  morph;
    uint8_t  grain, spray;
    uint8_t  eg_a, eg_d, eg_s, eg_r;
    /* The two LFOs: a rate each and one amount per destination each. The amounts are a
     * full row per LFO rather than the two named ones the version-2 record carried, because
     * the route table can point any destination anywhere and a record that saved only CUT's
     * and MORPH's would forget the other five on reload. 0 means the route is off, so a
     * shorter record cannot be padded with zeroes and still mean the same thing -- which is
     * why this is version 3 and not version 2 plus slack.
     *
     * lfo_edit is saved too: it is which LFO the page shows, and a patch that comes back
     * with the other one on screen is not wrong, only confusing. */
    uint8_t  lfo_rate[SAVE_LFOS];
    uint8_t  lfo_amt[SAVE_LFOS][SAVE_DESTS];
    uint8_t  lfo_shape[SAVE_LFOS];
    uint8_t  lfo_sync[SAVE_LFOS];
    uint8_t  lfo_trig[SAVE_LFOS];
    uint8_t  lfo_edit;
    /* the filter's tap and how far its corner follows the note. Both live in the
     * single-byte parameter run, so save_changes() counts each as one group without
     * being told. */
    uint8_t  ftype, ktrack;
    uint8_t  arp_rate, arp_type, arp_oct, arp_gate;

    /* the selectors: which bank, which slot, and which source a key plays */
    uint8_t  bank_i;
    uint8_t  smp_i;
    uint8_t  src;

    /* SYNTH's EDIT page: the phase offsets, 8 banks x 8 frames, plus the ramp's own two
     * settings. The table is saved rather than rebuilt from SPREAD and SHAPE because K2
     * can pull one frame off the ramp, and that edit is a value like any other. SPREAD and
     * SHAPE are saved too, so the page opens showing the numbers that produced the table
     * instead of showing zero over a table that has offsets in it. */
    uint8_t  foff[SAVE_BANKS][8];
    uint8_t  foff_spread;
    uint8_t  foff_shape;

    save_region region[SAVE_SLOTS];
} save_rec;

int             save_load(void);              /* boot: 1 if flash had a good record */
const save_rec *save_loaded_rec(void);        /* the record to copy into app.c's statics */
void            save_scan(const save_rec *live);   /* once a frame: keeps save_dirty honest */
void            save_adopt(const save_rec *base);  /* boot with no record: this IS saved */
int             save_now(const save_rec *live);    /* B_SAVE: 0 written, < 0 refused */
int             save_loaded(void);
int             save_dirty(void);             /* RAM has a change flash does not: the mark */
int             save_changes(const save_rec *live); /* groups differing from flash */
int             save_change_count(void);      /* the last scan's count, for the screen */
int             save_last_result(void);       /* the last write: 0 ok, -1 failed */
uint32_t        save_count(void);             /* writes since boot, for the tests */
