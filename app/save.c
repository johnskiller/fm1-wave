/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* The saved state: what survives a power cycle.
 *
 * Everything a player turns lives in one store object (OBJ_SETTINGS), because the object
 * is the unit the platform gives an app (plat.h: 3840 B per object, A/B sectors, CRC,
 * torn-write safe). This record is ~190 B of the 3840, so the limit is not a design
 * constraint -- which is why the record is a plain struct with a version and no
 * cleverness.
 *
 * ── written only when SAVE is pressed, and why ───────────────────────────────────
 *
 * A save is neither free nor small. storage.c's st_save erases a 4 KiB sector, programs
 * the payload 256 B at a time, and then READS THE WHOLE THING BACK to prove it (a
 * write-protected part must not report SAVED). On the device that is tens of ms of flash
 * work, and this app executes from the same flash (XIP). Scheduling that behind a knob
 * being turned is not something to trade a parameter for.
 *
 * So there is one writer: B_SAVE. It writes whenever it is pressed, including while notes
 * are held, because that is exactly what the player asked for -- a save that waits for
 * silence is a save that does not happen when the chord is the thing you want to keep.
 *
 * The cost of that choice is that an edit lives in RAM until it is saved, so the panel
 * has to say so: save_dirty() is what the screen draws its "not saved" mark from. A
 * parameter that turns and never persists is the failure this mark exists to prevent, and
 * it is a failure you cannot hear.
 *
 * The dirty flag is not set by hand at each knob. app.c hands the whole record over once a
 * frame and it is compared against the last one written. Setting a flag at each writer is
 * one forgotten line away from that silent bug -- a parameter that turns, sounds right,
 * and never persists -- and there are ~40 writers among the knobs, the keys, the pages and
 * MIDI. Comparing 190 B at 60 Hz is cheaper than that risk.
 *
 * ── what is NOT in the record ───────────────────────────────────────────────────
 *
 * Positions and player actions are not settings: the grain playhead's position, the
 * arp's held set, which page is open, edit_zoom/edit_live (how you were looking at the
 * material, not what it is), and every voice's runtime state. A power cycle returns the
 * instrument to a sound, not to a moment.
 */
#include <stddef.h>                /* offsetof -- the bare-metal libc has this */
#include "app.h"
#include "save.h"

/* memcmp/memset: the device toolchain has no <string.h>, so these come from the
 * platform's own declarations (app.c already uses memset without including it). */
static save_rec  rec;                       /* the live record, as last scanned */
static save_rec  rec_written;               /* what is in flash: the dirty baseline */
static uint8_t   loaded;                    /* 1: flash had a good record */
static uint8_t   dirty;                     /* rec differs from rec_written */
static int8_t    last_rc;                   /* the last write's result, for the screen */
static uint32_t  saves;                     /* writes since boot, for the tests */

/* Sized against the platform's limit at compile time: if a field is ever added that
 * pushes this past PLAT_STORE_MAX, the build says so instead of the save failing at
 * runtime on a device. */
typedef char save_rec_fits[(sizeof(save_rec) <= PLAT_STORE_MAX) ? 1 : -1];

/* Compare the record's BODY, not its header.
 *
 * The header (magic/version/bytes) belongs to this module, and app.c's save_snapshot
 * cannot set it -- it does not know those constants and should not. So the live copy
 * app.c hands to save_scan has a zero header by construction, while the baseline is a
 * written record with a real one. Comparing whole structs made every scan after a save
 * look dirty forever (measured: turn, tap SAVE, `expect save_dirty` still read 1). The
 * body is also the only part that describes the sound, which is what "is this saved" is
 * actually asking. */
static int same_body(const save_rec *a, const save_rec *b)
{
    return memcmp(&a->cut, &b->cut, sizeof *a - offsetof(save_rec, cut)) == 0;
}

int save_loaded(void)      { return loaded; }
int save_last_result(void) { return last_rc; }
uint32_t save_count(void)  { return saves; }

/* Is there a change in RAM that flash does not have? The screen's "not saved" mark. */
int save_dirty(void) { return dirty; }

/* The record as loaded (or as last written), for app.c to copy into its statics. */
const save_rec *save_loaded_rec(void) { return &rec; }

/* Boot: the record from flash, or the defaults. Beyond the platform's own CRC check,
 * what is validated here is a record written by a DIFFERENT build: same magic, same
 * version, and a field this one would read as a mode it does not have. */
int save_load(void)
{
    int n;

    loaded = 0;
    n = plat_store_load(OBJ_SETTINGS, &rec, (uint32_t)sizeof rec);
    if (n == (int)sizeof rec && rec.magic == SAVE_MAGIC && rec.version == (uint16_t)SAVE_VERSION
        && rec.bytes == (uint16_t)sizeof rec
        && rec.src < 3u && rec.shape < WV_SHAPE_N && rec.bank_i < WV_BANK_COUNT
        && rec.arp_type < 4u && rec.arp_oct >= 1u && rec.arp_oct <= 4u) {
        loaded = 1;
    } else {
        memset(&rec, 0, sizeof rec);
    }
    rec_written = rec;
    dirty = 0;
    last_rc = 0;
    return loaded;
}

/* app.c -> us, once a frame (app_frame, after input). Compares the whole record: no
 * writer has to remember to raise a flag. */
void save_scan(const save_rec *live)
{
    if (same_body(live, &rec_written)) {
        dirty = 0;
        return;
    }
    dirty = 1;
    rec = *live;
}

/* B_SAVE. Takes the record to write rather than a copy of app.c's statics, so there is
 * one place (app.c's save_snapshot) that knows how the statics map into it. Returns the
 * platform's code: 0 = written, negative = it refused (a full pool, a failing part). */
int save_now(const save_rec *live)
{
    int rc;

    rec = *live;
    rec.magic = SAVE_MAGIC;
    rec.version = (uint16_t)SAVE_VERSION;
    rec.bytes = (uint16_t)sizeof rec;

    rc = plat_store_save(OBJ_SETTINGS, &rec, (uint32_t)sizeof rec);
    last_rc = (int8_t)(rc == 0 ? 0 : -1);
    if (rc == 0) {
        rec_written = rec;
        dirty = 0;
        saves++;
    }
    return rc;
}
