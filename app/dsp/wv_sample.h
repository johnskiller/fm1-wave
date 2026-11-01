/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* Samples: the resource's layout, and the reader that turns one into sound.
 *
 * The blob lives in the platform's resource region (fm1.toml resources = FILE ->
 * plat_res), read in place from flash — never copied to RAM, because the region is
 * 276 KB and the pool is 250 KB. Layout, little-endian, no alignment requirement:
 *
 *   off 0   char     magic[4]   "FMWV"
 *   off 4   uint8    version    1
 *   off 5   uint8    count      1..8
 *   off 6   uint16   reserved   0
 *   off 8   entry[count], 20 bytes each:
 *             uint32 offset   from the start of the blob
 *             uint32 len      samples (not bytes)
 *             uint16 rate     Hz
 *             uint8  bits     16 or 8
 *             uint8  root     MIDI note the material was recorded at
 *             int16  gain     Q8: playback gain, 256 = unity (see below)
 *             char   name[6]  ASCII, not necessarily terminated at 6
 *   off 8 + 20*count  the samples, contiguous
 *
 * 8 entries = 176 B of header, so the rest of the region is samples. Rates are
 * per slot and that is the point: a wavetable source wants 11.025 kHz (it is read
 * 20x its own rate at the top of the keyboard) while a slice wants 22.05 or 44.1
 * (it plays at 1x and its bandwidth is the material's).
 *
 * `gain` exists because banks and samples have different crest factors: measured,
 * the eight synth banks differ by 4.3 dB from each other, so samples will differ by
 * more. It is per entry and corrected by ear, not computed.
 */
#pragma once
#include <stdint.h>

#define WV_SMP_MAGIC   "FMWV"
#define WV_SMP_VERSION 1u
#define WV_SMP_MAX     8            /* the same count as the synth banks, so the panel means the same thing */
#define WV_SMP_ENTRY   20u
#define WV_SMP_HEAD    (8u + WV_SMP_ENTRY * WV_SMP_MAX)

/* One slot, as the reader needs it: everything except the samples themselves. */
typedef struct {
    const int16_t *data;           /* the samples (int16 even for 8-bit: the loader widens) */
    uint32_t len;                  /* samples */
    uint32_t rate;                 /* Hz, the slot's own */
    uint8_t  root;                 /* MIDI note of the material */
    uint8_t  bits;
    int16_t  gain;                 /* Q8, 256 = unity */
    char     name[8];
} wv_sample;

/* Parse the blob. Returns the number of slots (0 if the blob is absent, short, or
 * not this format) and fills `out`. Samples are referenced, not copied. */
uint32_t wv_smp_parse(const uint8_t *blob, uint32_t len, wv_sample *out, uint32_t max);

/* The playback increment for one slot at one note, in that slot's own samples.
 * A slot at its root reads 1x; an octave up reads exactly 2x. */
float wv_smp_inc(const wv_sample *s, float note);

/* One slot rendered: linear interpolation, a windowed reader for slices, and a
 * loop for a wavetable-source. `pos` and `dir` are the caller's per-voice state. */
typedef struct {
    float pos;                     /* in samples, fractional */
    float inc;
    int32_t end;                   /* one past the last sample this reader may read; -1: the slot's end */
    int32_t loop_start, loop_end;  /* -1: no loop (a slice plays once) */
    int8_t  dir;                   /* +1 forward; granular reverses grains with -1 */
    uint8_t done;                  /* a one-shot reached the end: it stays silent */
} wv_reader;

void wv_reader_start(wv_reader *r, const wv_sample *s, uint32_t offset, float inc);

/* Limit the reader to [start, end): this is what makes a slice a slice. Separate
 * from wv_reader_loop because a slice that plays once and a slice that is held both
 * end at the same place, and only the second one wraps. Without this a slice runs to
 * the end of the whole slot — which is exactly the bug this function exists to
 * prevent, and it does not sound like a bug, it sounds like the sampler ignoring
 * the chop. */
void wv_reader_range(wv_reader *r, uint32_t start, uint32_t end);

/* Turn the reader into a loop over [start, end) of the slot: how a slice is held
 * (a drum hit does not loop, but holding it loops the slice instead of going
 * silent, which is what a sampler does). A range shorter than 2 samples is
 * ignored: it cannot interpolate. */
void wv_reader_loop(wv_reader *r, uint32_t start, uint32_t end);

/* The next sample, -1..1, with the slot's gain applied. Advances `pos`. A one-shot
 * that reached the end returns 0, sets *done, and returns 0 from then on. */
float wv_reader_next(wv_reader *r, const wv_sample *s, int *done);
