/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 johnskiller */
/* The sample reader. See wv_sample.h for the blob's layout and why the rate is
 * per slot.
 *
 * No platform here either: the blob arrives as a pointer, so the parser and the
 * reader are testable under plain cc, and the firmware's only job is to hand over
 * what plat_res gives it.
 */
#include "wv_sample.h"
#include "fastmath.h"

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

uint32_t wv_smp_parse(const uint8_t *blob, uint32_t len, wv_sample *out, uint32_t max)
{
    uint32_t i, count;

    if (!blob || len < 8u)
        return 0;
    if (blob[0] != 'F' || blob[1] != 'M' || blob[2] != 'W' || blob[3] != 'V')
        return 0;
    if (blob[4] != WV_SMP_VERSION)
        return 0;
    count = blob[5];
    if (count == 0 || count > WV_SMP_MAX || count > max)
        return 0;
    if (len < 8u + count * WV_SMP_ENTRY)
        return 0;

    /* Validate every entry before handing any of them out, so a half-written blob
     * yields 0 slots rather than slots whose samples point past the buffer. The
     * resource region is CRC-checked before this is called, but a blob from
     * anywhere else (a test, a future USB load) is not. */
    for (i = 0; i < count; i++) {
        const uint8_t *e = blob + 8u + i * WV_SMP_ENTRY;
        uint32_t off = rd32(e), slen = rd32(e + 4), rate = rd16(e + 8);
        uint8_t bits = e[10];
        if (bits != 16u)               /* 8-bit is in the format, not in the reader: see wv_sample.h */
            return 0;
        if (rate < 4000u || rate > 48000u)
            return 0;
        if (slen == 0)
            return 0;
        if (off < 8u + count * WV_SMP_ENTRY || off > len)
            return 0;
        if (slen > (len - off) / 2u)
            return 0;
    }

    for (i = 0; i < count; i++) {
        const uint8_t *e = blob + 8u + i * WV_SMP_ENTRY;
        wv_sample *s = &out[i];
        uint32_t j;
        s->data = (const int16_t *)(const void *)(blob + rd32(e));
        s->len = rd32(e + 4);
        s->rate = rd16(e + 8);
        s->bits = e[10];
        s->root = e[11];
        s->gain = (int16_t)rd16(e + 12);
        for (j = 0; j < 6u; j++) {
            char c = (char)e[14 + j];
            /* an unset or padded name reads as empty rather than as whatever the
             * file happened to hold there */
            s->name[j] = ((unsigned char)c < 0x20 || c == 0x7F) ? 0 : c;
        }
        s->name[6] = 0;
        s->name[7] = 0;
    }
    return count;
}

float wv_smp_inc(const wv_sample *s, float note)
{
    /* Two factors, and they are independent:
     *   rate/44100     the slot's own rate against the engine's
     *   2^((note-root)/12)   the transposition from where the material sits
     * A slot at its root reads 1x of its own rate; an octave up reads exactly 2x,
     * which for an 11.025 kHz slot is a clean 4:1 against 44.1 — the reason that
     * rate was chosen (spec §8). */
    return ((float)s->rate * (1.0f / 44100.0f))
           * fm_exp2f(((float)note - (float)s->root) * (1.0f / 12.0f));
}

void wv_reader_start(wv_reader *r, const wv_sample *s, uint32_t offset, float inc)
{
    r->pos = (float)(offset < s->len ? offset : 0u);
    r->inc = inc;
    r->dir = 1;
    r->end = -1;
    r->loop_start = -1;
    r->loop_end = -1;
    r->done = 0;
}

void wv_reader_range(wv_reader *r, uint32_t start, uint32_t end)
{
    /* end is exclusive and clamped to the slot: a slice that claimed to run past its
     * own sample would read the next slot's data */
    if (end > start + 1u)
        r->end = (int32_t)end;
    if (r->pos < (float)start)
        r->pos = (float)start;
}

void wv_reader_loop(wv_reader *r, uint32_t start, uint32_t end)
{
    /* A loop that is shorter than two samples cannot interpolate and is almost
     * certainly a mistake in the material: leave the reader one-shot instead. */
    if (end > start + 1u) {
        r->loop_start = (int32_t)start;
        r->loop_end = (int32_t)end;
    }
}

float wv_reader_next(wv_reader *r, const wv_sample *s, int *done)
{
    uint32_t i0, i1;
    float frac, a, b, v;

    if (r->done) {
        if (done)
            *done = 1;
        return 0.0f;
    }

    if (r->dir > 0) {
        if (r->loop_end > 0) {
            /* wrapped into [loop_start, loop_end): the subtraction form, not a
             * modulo, because the overshoot is normally a fraction of a sample */
            if (r->pos >= (float)r->loop_end) {
                r->pos -= (float)(r->loop_end - r->loop_start);
                if (r->pos >= (float)r->loop_end)
                    r->pos = (float)r->loop_start;
            }
        } else if (r->pos >= (float)((r->end > 0 ? (uint32_t)r->end : s->len) - 1u)) {
            r->done = 1;
            if (done)
                *done = 1;
            return 0.0f;
        }
    } else {
        if (r->loop_start >= 0) {
            if (r->pos < (float)r->loop_start)
                r->pos += (float)(r->loop_end - r->loop_start);
        } else if (r->pos < 0.0f) {
            /* The boundary is 0, and sample 0 is still valid: stopping at < 0
             * instead of <= 0 emitted one extra sample at position 0 and then went
             * negative, so a backward one-shot played its first sample twice. */
            r->done = 1;
            if (done)
                *done = 1;
            return 0.0f;
        }
    }

    i0 = (uint32_t)r->pos;
    {
        uint32_t lim = r->end > 0 ? (uint32_t)r->end : s->len;
        i1 = (i0 + 1u < lim) ? i0 + 1u : i0;   /* the last sample holds, it does not wrap */
    }
    frac = r->pos - (float)i0;
    a = (float)s->data[i0] * (1.0f / 32768.0f);
    b = (float)s->data[i1] * (1.0f / 32768.0f);
    v = a + (b - a) * frac;

    r->pos += r->inc * (float)r->dir;

    /* Stop *after* emitting the boundary sample, not before it. A backward
     * one-shot that stopped on pos < 0 played sample 0 twice; one that stops here
     * plays it once and reports done on the following call. Same for forward: the
     * verdict is taken on the position the next call would read. */
    if (r->dir > 0 && r->loop_end < 0
        && r->pos > (float)((r->end > 0 ? (uint32_t)r->end : s->len) - 1u)) {
        r->done = 1;
        if (done)
            *done = 1;
    } else if (r->dir < 0 && r->loop_start < 0 && r->pos < 0.0f) {
        r->done = 1;
        if (done)
            *done = 1;
    }

    return v * (float)s->gain * (1.0f / 256.0f);
}
