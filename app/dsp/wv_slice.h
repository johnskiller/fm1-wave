/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* Slices: cutting one sample into segments and triggering them from the keys.
 *
 * This is the reason the sample rate is per slot (spec §8): a slice plays at 1x
 * and its bandwidth is the material's own, so a slice wants 22.05 or 44.1 kHz
 * while a wavetable source wants 11.025 kHz (it is read ~20x its own rate at the
 * top of the keyboard). One global rate cannot serve both.
 *
 * How it behaves, and each of these is a decision:
 *
 * - **A slice plays once.** The key's release does not stop it; a drum hit cut
 *   short by a finger is not a drum hit. (Holding a key can loop it — see
 *   wv_slice_mode.)
 * - **Slices are laid out by key from a base note.** Slice n plays from note
 *   base+n, so a chop of eight slices is playable across eight white keys. That is
 *   how an MPC's slice mode behaves and how the FM-1's 27 keys want to be used.
 * - **Splits are found, not given.** The tool detects the quiet gaps; the panel's
 *   material page can then add or remove one by hand. Doing it in the tool means
 *   the device never needs the sample in RAM to analyse it.
 * - **A slice does not transpose.** It plays at its own rate. A chop that
 *   transposed with the key would be a wavetable source, which is the other mode.
 */
#pragma once
#include <stdint.h>

#include "wv_sample.h"

#define WV_SLICE_MAX 16    /* a chop across 16 keys: two octaves of the FM-1's 27 */

typedef struct {
    uint32_t start;        /* sample index of the first sample */
    uint32_t end;          /* one past the last: end - start is the length */
    uint8_t  note;         /* the key that fires it */
    uint8_t  pad;
} wv_slice;

/* Find up to `max` slices in a slot by quiet gaps.
 *
 * `thresh` is a fraction of the slot's peak (0.02 = -34 dB): a run of samples
 * below it, longer than `min_gap` samples, separates two slices. Everything before
 * the first gap is slice 0, so a sample that starts immediately is not lost.
 * Returns the count (0 if the slot holds fewer than two slices, which is not a
 * chop: the caller should play the whole thing). */
uint32_t wv_slices_split(const wv_sample *s, float thresh, uint32_t min_gap,
                         wv_slice *out, uint32_t max);

/* Which slice a key fires, or 0 if the key is outside the layout. Fills *out. */
int wv_slice_for_note(const wv_slice *sl, uint32_t n, uint8_t note, wv_slice *out);

/* Lay the slices out by key from `base`: slice n gets note base+n. Separate from
 * the split because detection and layout are independent decisions — a chop found
 * left to right does not have to be played left to right. */
void wv_slices_layout(wv_slice *sl, uint32_t n, uint8_t base);

/* The key the chop starts on: chosen so the slices fit on the keyboard from
 * F3 (key 0, note 53) upward. Called by the app, not baked in, so a 3-slice chop
 * is not stuck at the bottom of the keyboard. */
#define WV_SLICE_BASE_NOTE 53u
