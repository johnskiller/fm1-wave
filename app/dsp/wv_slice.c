/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* Slice detection. See wv_slice.h for the behaviour it serves.
 *
 * The detector is deliberately simple: a threshold, a minimum gap, and a state
 * machine with three facts (in a slice; where it started; how long we have been
 * quiet). A spectral splitter or an onset detector would find more chops, but they
 * also find "chops" in a sustained note, and a wrong split is worse than a missing
 * one — the panel can add a split by hand, it cannot easily explain why there are
 * eleven where there are eight.
 */
#include "wv_slice.h"

static int32_t mag(int16_t v)
{
    return v < 0 ? -(int32_t)v : (int32_t)v;
}

uint32_t wv_slices_split(const wv_sample *s, float thresh, uint32_t min_gap,
                         wv_slice *out, uint32_t max)
{
    uint32_t i, n = 0;
    int32_t peak = 0, t;
    int in_slice = 0;
    uint32_t slice_start = 0, gap_start = 0, gap_len = 0;

    if (!s || !s->data || s->len < 2u || !out || max == 0)
        return 0;
    if (min_gap == 0)
        min_gap = 1;

    /* the threshold is relative to the slot's own peak, so one setting works on a
     * quiet chop and a loud one */
    for (i = 0; i < s->len; i++) {
        int32_t v = mag(s->data[i]);
        if (v > peak)
            peak = v;
    }
    if (peak == 0)
        return 0;
    t = (int32_t)((float)peak * (thresh > 0.0f ? thresh : 0.02f));
    if (t < 1)
        t = 1;

    for (i = 0; i < s->len; i++) {
        int quiet = mag(s->data[i]) < t;

        if (!in_slice) {
            if (!quiet) {
                in_slice = 1;
                slice_start = i;
                gap_len = 0;
            }
            continue;
        }

        if (!quiet) {
            /* sound again before the gap got long enough: it was not a separator,
             * it was a zero crossing or a beat, so the slice continues */
            gap_len = 0;
            continue;
        }

        if (gap_len == 0)
            gap_start = i;
        gap_len++;
        if (gap_len >= min_gap) {
            /* A long enough gap closes the slice. It closes at the *start of the
             * gap*, not at the last audible sample: a hit's decay crosses any fixed
             * threshold several times on its way down, so cutting at the first
             * crossing amputates the tail. The gap belongs to the slice that
             * precedes it, and the next slice starts at its own first sound. */
            if (n < max && gap_start > slice_start) {
                out[n].start = slice_start;
                out[n].end = gap_start;
                out[n].note = 0;
                out[n].pad = 0;
                n++;
            }
            in_slice = 0;
            gap_len = 0;
            if (n == max)
                break;
        }
    }

    /* the last slice: from its first sound to the last sample that is still
     * audible, so trailing silence is not played but a decaying tail is kept */
    if (in_slice && n < max) {
        uint32_t last = slice_start;
        for (i = slice_start; i < s->len; i++)
            if (mag(s->data[i]) >= t)
                last = i;
        out[n].start = slice_start;
        out[n].end = last + 1u;
        out[n].note = 0;
        out[n].pad = 0;
        n++;
    }

    /* Now give every slice its tail back. A slice ends where the next one begins,
     * so the decay of a hit is heard in full and the silence before the next hit is
     * part of the earlier slice. This is what the first version got wrong: it ended
     * each slice at the first quiet sample, which on a real drum loop left 50 ms of
     * a 180 ms hit — audible as a click, not as a chop.
     *
     * The last slice keeps the rest of the slot, which is the same rule (there is no
     * next slice to bound it) and means a trailing tail is never truncated either. */
    for (i = 0; i + 1u < n; i++)
        out[i].end = out[i + 1u].start;

    /* One slice is not a chop: the caller plays the whole slot instead. And a
     * slice of one sample cannot interpolate. */
    {
        uint32_t k = 0;
        for (i = 0; i < n; i++)
            if (out[i].end > out[i].start + 1u)
                out[k++] = out[i];
        n = k;
    }
    return n < 2 ? 0 : n;
}

int wv_slice_for_note(const wv_slice *sl, uint32_t n, uint8_t note, wv_slice *out)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (sl[i].note == note) {
            if (out)
                *out = sl[i];
            return 1;
        }
    }
    return 0;
}

void wv_slices_layout(wv_slice *sl, uint32_t n, uint8_t base)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        sl[i].note = (uint8_t)(base + i);
}
