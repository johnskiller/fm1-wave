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
#include "wv_banks.h"
#include "build_id.h"          /* the commit this image is: tools/gen_build_id.py */
/* save.c is INCLUDED, not linked separately: the platform's unity build compiles app.c
 * and nothing else from the app (device/unity.c: #include "app.c"), and platform/ is a
 * submodule this firmware does not modify. So an app's own translation units are all
 * one unit by construction, which is also why there is no prototype/static mismatch to
 * worry about between them. */
#include "save.c"             /* the record that survives a power cycle */

#define WAVE_HZ   44100.0f
#define BLK_MAX   256u
#define SMP_MAX   8

static wv_state dsp;
static uint8_t  cpu2;            /* the second core took the job (app_init's plat_cpu2_set) */
/* SYNTH's EDIT: a phase offset per frame, in points of the 256-point cycle.
 *
 * WHY THIS EXISTS: MORPH reads two frames at the same phase and crossfades them. Two
 * waves at the same phase ADD, so where the frames are alike they reinforce and where
 * they are not they cancel -- a comb. Turning one frame n points against its neighbour
 * moves the cancellation, which is audible as the morph opening up.
 *
 * Doubt it? app/tools/render_foff.sh renders the A/B as a WAV: the same note, same
 * morph, offsets off then on.
 *
 * WHY A TABLE AND NOT A KNOB ON `off[frame]` ALONE: eight frames turned independently is
 * eight numbers, and the setting that matters most is not eight numbers but the SHAPE of
 * the ramp across them. A ramp is one number (points per frame) plus one shape.
 */
static uint8_t foff_tab[WV_BANK_COUNT][WV_FRAMES];

/* The offset in RAM is a ramp, not a per-frame value: SPREAD and SHAPE describe all
 * eight at once, and OFFSET edits the frame the page is pointed at, which desynchronises
 * that one frame from the ramp (that is the point -- it is the frame you are listening
 * to). Turning SPREAD rewrites the whole table, discarding those edits, so the page shows
 * the ramp whenever it is moved. */
enum { FOFF_LIN, FOFF_ALT, FOFF_RAND, FOFF_SHAPE_N };
static uint8_t foff_spread;     /* points per frame, 0..127 */
static uint8_t foff_shape;      /* FOFF_* */

static uint8_t foff_cur(void);
static void foff_ramp(void);
static wv_bank  bank = { WV_BANKS[0], 0 };   /* foff is armed by foff_ramp() */

/* The sample pool, as the platform handed it over. `pool` points into the resource
 * blob, not a copy: the blob is 276 KB of flash and there is no RAM for a copy. */
static const uint8_t *pool;
static void           pool_poll(void);
static void           midi_in(void);      /* (defined below: app_render asks it every block) */
static uint32_t       pool_len;
static wv_sample      smp[SMP_MAX];
static uint8_t        smp_count;
static uint8_t        smp_i;      /* which slot EN_SELECT picked, in SMPL and GRAN */

/* ── the region of a slot that sounds, one per slot ──────────────────────────
 *
 * Until this existed a sample played [0, len) once: the reader could already take a
 * range and loop inside it, but there was no way to set one, so SMPL and GRAN both
 * used the whole slot and a held key on a long sample went silent when the material
 * ran out. These four numbers per slot are what the EDIT page moves.
 *
 * start/end is the region the voice plays; loop_s/loop_e is the part it repeats. The
 * reader owns both pairs separately (wv_reader_range + wv_reader_loop), so the voice
 * plays start..end and, on reaching end, wraps to loop_s -- which is the normal
 * sampler layout: an attack before the loop, and the loop for as long as the key is
 * down. loop_s == start and loop_e == end is the plain "loop the whole region".
 *
 * Per slot, for the same reason the pool's playhead is per slot: a boundary belongs
 * to the material it was found in. Switching slots and back returns to the region
 * that slot was given, and switching slots never needs a re-analysis of the new one.
 *
 * Saved with everything else: B_SAVE writes all eight slots' regions in one record
 * (save.c), and a power cycle without a save reloads this default.
 */
typedef struct {
    uint32_t start, end, loop_s, loop_e;
    uint8_t  on;                  /* loop on/off, the PRESET knob in EDIT */
} smp_region;

static smp_region     region[SMP_MAX];
static uint8_t        edit_page;
static uint8_t        edit_zoom;      /* 0..7: the visible fraction is 1/2^edit_zoom */
static uint8_t        edit_live;      /* which marker the last turn moved: keeps the window there */
/* Where each slot's cloud resumes. The picture is meant to be readable while you play,
 * and at 210 px/s on the demo kit a bar crosses the band in 0.84 s -- faster than the
 * eye resolves, so what you see is a blur and then nothing when the note stops.
 *
 * So the playhead position is kept per slot and the next granular note starts there
 * instead of at the loop start. That has two effects and the second is the point: the
 * picture is legible when still, and consecutive notes continue the material instead of
 * jumping back to 0 -- which is also how a granular instrument should behave, since
 * retriggering a texture usually means "read on", not "restart the sample".
 *
 * Per slot, because the position belongs to the material: switching slots and back
 * returns to where that sample was left. */

/* SOURCE: which thing a key plays. SYNTH reads the wavetable, SMPL reads the pool as
 * a sampler, GRAN reads the same pool as a grain cloud. Three sources, one switch:
 * GRAN is the pool with a different reader, not a different instrument, so it shares
 * the knob set and the key map. */
enum { SRC_SYNTH, SRC_SMPL, SRC_GRAN };
static uint8_t src = SRC_SYNTH;

/* The record's two directions (defined under app_init, used by input and app_frame):
 * app.c's statics out, and back in. Declared here because input() and app_frame() use
 * them and both come before app_init in this file. */
static void save_apply(const save_rec *r);
static void save_snapshot(save_rec *r);

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
static int32_t scope_pk_q;   /* observation: pre-master peak, 1/4096 units */
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
/* The LFO page's two controls. RATE 0 is off, which is the default: a patch made
 * before the LFO existed must sound the same after it. */
static uint8_t k_lfo_rate = 0, k_lfo_depth = 0;
static uint8_t lfo_page;
/* The FX page's two controls: the filter's tap and key tracking.
 *
 * TYPE starts at LP and KEY TRK at 0, which is exactly what the voice did before either
 * existed -- so a patch saved by an older build, and the instrument's own default, both
 * sound the same after this page appeared. That is the same rule the LFO page followed. */
static uint8_t k_ftype = WV_FTYPE_LP;
static uint8_t k_ktrack = 0;          /* 0, 25, 50, 75, 100 */
static uint8_t fx_page;
/* SYNTH's EDIT page: which of the eight frames the offset knobs act on. K4 picks it.
 * The page owns this rather than K1 because K1-K3 are the value's three digits -- 0..255
 * does not fit one knob's detents usefully, and a frame picker has to be visible while
 * the digits are being turned. */
static uint8_t edit_frame;
/* Which source opened the page currently up, so a source switch closes it instead of
 * handing SMPL's editor to SYNTH. */
static uint8_t edit_src;
/* key tracking's five settings, in the panel's order. One list, so the panel's detents
 * and the values save_load accepts can never drift apart. */
static const uint8_t KT_VALUES[5] = { 0, 25, 50, 75, 100 };
#define KT_STEPS 5
static uint8_t kt_index(uint8_t pct); /* the detent for a setting, defined with the table */
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

/* --- the arpeggiator ------------------------------------------------------
 *
 * B_ARP toggles a page the way B_ENV does, and K1-K4 become RATE / TYPE / OCT /
 * GATE while it is up. Three things about it are decisions, not defaults:
 *
 * 1. RATE 0 is OFF. The knob is the switch, like the LFO's, so a patch made
 *    before the arp existed sounds the same after it and there is no separate
 *    on/off to forget.
 *
 * 2. It is a clock, not a free Hz knob. An arpeggiator that will not sit on a
 *    beat is not an arpeggiator. The steps are note divisions at a fixed 120 BPM,
 *    because the platform receives MIDI clock but does not track tempo yet; when
 *    it does, the divisor stays and the BPM becomes the input.
 *
 * 3. The panel's keys are armed too. ARP is a state of the instrument, not a
 *    property of USB. A chord from the keys and the same chord from a DAW have to
 *    arpeggiate the same way, which is the lesson from MIDI in: one engine, one
 *    set of rules.
 *
 * The held notes are kept here rather than read from the key matrix, because a
 * note can arrive from MIDI where there is no key to read -- arming the arp must
 * not care which link the note came in on.
 */
static uint8_t arp_page;
static uint8_t k_arp_rate = 0;            /* 0 = off; 1..127 = a clock division */
static uint8_t k_arp_type = 0;            /* ARP_UP / DOWN / UPDN / RND */
static uint8_t k_arp_oct  = 1;            /* 1..4 octaves */
static uint8_t k_arp_gate = 64;           /* 1..127: the note length as a fraction of the step */

enum { ARP_UP, ARP_DOWN, ARP_UPDN, ARP_RND, ARP_TYPE_N };
static const char *const ARP_NAME[ARP_TYPE_N] = { "UP", "DOWN", "UP-DN", "RND" };

/* The held notes, as MIDI numbers. 8 is the voice count: a ninth note cannot
 * sound even without the arp, so holding more would only change the pattern and
 * not the sound. Sorted before it is walked, because an arp's order is by pitch
 * and never by when the fingers landed.
 *
 * `age` orders them: the oldest is the one that gives way when a ninth arrives,
 * the same rule the voices use. */
#define ARP_HOLD 8
static uint8_t arp_hold[ARP_HOLD];
static uint8_t arp_vel[ARP_HOLD];
static uint8_t arp_held;                  /* how many entries are in use */
static uint32_t arp_pos;                  /* the position in the walk, notes x octaves */
static uint8_t arp_up;                    /* unused by the bounce (see arp_advance), kept for ARP_RND */
static uint32_t arp_lcg = 12345u;         /* ARP_RND's state */
static uint8_t arp_note = 0xFFu;          /* the note the arp is sounding, 0xFF none */
static uint32_t arp_t;                    /* ms until the next step */
static uint32_t arp_ms;                   /* the step length, from k_arp_rate */
static uint32_t arp_off_at;               /* ms into the step at which the note ends */

/* Is the arp taking the keys? RATE 0 is off, and so is an empty page behind it --
 * the knob is the switch, so there is nothing else to consult. */
static int arp_on(void) { return k_arp_rate != 0; }
static uint8_t arp_gated;                 /* the note of this step has already ended */

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
/* ─── the three bands ─────────────────────────────────────────────────────────
 *
 * Every height here is derived from what the band has to draw. The first version had
 * the numbers as literals and got this wrong in a way that does not look like a bug:
 * the material band was cut from 108 to 84 while its contents stayed where they were,
 * so src_shape drew to y=103 inside an 84 px canvas and its bottom 20 px were clipped,
 * and the "ENV PAGE" line at y=92 vanished entirely. gfx clips at the canvas edge
 * (cv_rect: `if (y1 > cv_y1) y1 = cv_y1;`) with no warning, so an overrun reads as a
 * missing element rather than as an error.
 *
 * So: content first, height second, and the sum is checked by an assertion below.
 */
/* The name starts at the band's own first row. It used to start 4 px down, which was
 * affordable while the value had two rows under it; with the ring taking 47 of the 64 px,
 * those 4 px are exactly what pushed the ring's bottom edge past the band (measured:
 * cy + 23 = 67 against a 64 px band, so the last four rows of every ring were clipped). */
#define KNOB_TOP   0                                  /* knob(): where its ink starts */
#define KNOB_INK   (KNOB_TOP + 34 + 8)                /* label, value, bar = 46 px */
#define KNOB_H     64                                 /* the 46 px of text plus a 26 px arc
                                                       * box: the bar is gone, see knob_arc() */

/* The save line: two rows of MAT_PAD, in the air below the scope's baseline.
 *
 * LENGTH covers SAVE_LINE_FULL_CHANGES groups; beyond that the line is simply full, because
 * saying "a lot" is all a 232 px bar can say about a record with 28 changeable groups. 8 was
 * chosen against how the instrument is actually used -- a hand turns two or three knobs, not
 * twenty -- so that those two or three show clearly. COLOUR reaches full ink at
 * SAVE_LINE_FULL_MS. Both numbers are first guesses and both are in this one place;
 * SAVE_LINE_ROW is where the line sits. */
#define SAVE_LINE_FULL_CHANGES 8
#define SAVE_LINE_FULL_MS      300000u                /* 5 minutes */

/* Which rows of MAT_PAD the line occupies, and why not the top two.
 *
 * It started at MAT_PAD + 0..1, which is the row directly under the scope's 0 dBFS
 * graticule -- one row below a full-width 240 px grey line, with no air between them.
 * Measured: that graticule is (98,97,106) and a full-brightness line is (238,137,24); two
 * full-width bars stacked on consecutive rows read as one crowded block, and the player
 * said so ("离scope的下线太近"). Down here the gap is three rows, and the wave's first ink
 * is y 91, so there is still a row of air below the line at its heaviest. */
#define SAVE_LINE_ROW          3                      /* 0..2 stay empty under the scope */

#define MAT_PAD    6                                  /* above and below the picture */
#define MAT_WAVE   80                                 /* the picture's own height */
#define MAT_H      (MAT_PAD + MAT_WAVE + MAT_PAD)     /* 92 */

/* The picture runs the full band, and the three knob values sit ON it, in the corners
 * the knobs themselves occupy:
 *
 *                              n/m          top right    (bank or slot)
 *          SPRAY n             ...          bottom left  (GRAN only)
 *                              45ms         bottom right  (morph or grain size)
 *
 * No knob NAME is printed. The panel has three labelled knobs around this screen's top
 * edge and four below it, and the four below are drawn as CUT/RES/SHAPE/AMOUNT without
 * their encoder numbers -- the position is the name. The same argument applies here:
 * the top-right value is the top-right knob. Printing "SEL" over a corner that IS the
 * SELECT knob's corner says nothing the position has not already said.
 *
 * The values are printed over the picture instead of beside it, which costs a clear
 * rectangle of waveform in one corner of each label. That is the trade the player asked
 * for after seeing the side-by-side version: a wider picture with three small labels on
 * top of it reads better than a narrow picture with a clean margin beside it.
 *
 * The picture is drawn first and the text over it, so a label always wins; each one gets
 * its own backdrop-free position rather than a box, because a box is a third colour on
 * screen and this band only has two. */
#define MAT_X0     4
#define MAT_W      232                                /* the picture, x 4..235 */
#define MAT_LAB_L  (MAT_X0 + 2)                       /* the left corners' edge */
#define MAT_LAB_R  (MAT_X0 + MAT_W - 2)               /* the right corners' edge */

/* SYNTH is the one source that needs a clean margin, and the reason is measurable: its
 * picture is drawn at full ink (a wavetable frame IS the picture), while SMPL and GRAN
 * draw their envelope at 2/5 and put 1 px bars on top of it. Overlaying bright text on a
 * bright trace is what the player objected to -- "和标签冲突的厉害". So SYNTH gives up the
 * right-hand 60 px instead, and the labels get a background of their own.
 *
 * 60 px is not a round number picked for looks; it is the widest label plus slack,
 * measured with the font's own advance table rather than estimated:
 *
 *     BANK 1/8    46 px        SAW SQR    42 px   (the widest bank name)
 *     MORPH 0     42 px        MORPH 100  54 px   <- the worst case
 *
 * 180 + 60 is the whole screen with no margins at all, so the picture is 172 and the
 * 60 px column sits at x 176..235 with a 4 px gap after the picture and 4 px at the
 * screen edge. The widest string is 54 px, leaving 6 px of slack. */
#define SYN_W      172                                /* SYNTH's picture, x 4..175 */

/* The stack's shear, in px per frame, chosen by eye off six rendered variants.
 *
 * Eight frames on one 172 x 80 picture: each is pushed STK_DX right and STK_DY up from
 * the one behind it, so the eight read as one object with depth rather than eight traces
 * on top of each other. No perspective: a frame's width does not shrink with depth,
 * because a narrower far frame would say the far frame holds less TIME, and every frame
 * is 256 points.
 *
 * STK_DY >= STK_DX is the part that matters. Equal steps (5/5) packed the frames as
 * tightly vertically as horizontally and the eight curves tangled. Vertically apart is
 * what lets the eye separate them, because a frame's own height is only 2 * amp.
 *
 * THE BUDGET, which is the whole argument here. The picture is 80 px tall and it has to
 * hold seven steps, the lift, and two amplitudes:
 *
 *     2 * amp + 7 * STK_DY + RACK_LIFT <= 80 - 2      =>  amp <= (78 - 7*DY - LIFT)/2
 *
 *     DY 8, lift 8  ->  amp  7 px   (the first version; the player said "振幅太小")
 *     DY 6, lift 8  ->  amp 14 px   <- this one, twice the height, 3 px between planes
 *     DY 5, lift 8  ->  amp 16 px   2 px between planes, the zero lines start to merge
 *
 * 14 px is where a frame's own waveform is readable at all: the SAW SQR bank's frames
 * span peak to peak over that height, and at 7 px the curve was a nearly flat line with
 * the shape squeezed out of it. The frames are allowed to touch -- they are planes of one
 * object, and the depth comes from brightness, not from air between them.
 *
 * Width is untouched: frame width = 172 - 7 * 6 = 130 px.
 *
 * The far frames are DIMMED rather than outlined. This band has two colours by design
 * (ground and ink); a card border would be a third, and eight borders on 16-px-tall
 * frames would be more ink than the curves. Brightness is the depth cue instead: f0 at
 * half ink, f7 at full. */
#define STK_DX     6              /* px right per frame */
#define STK_DY     3              /* px up per frame */
#define RACK_LIFT  6              /* how far the edited frame rises */
#define SYN_COL_W  60                                 /* the labels' column */
#define SYN_COL_R  (240 - 4)                          /* its right edge: x 235 */
#define SYN_COL_X  (SYN_COL_R - SYN_COL_W)            /* 175 */
#define SYN_ROW_2  (MAT_ROW_TOP + 12)                 /* the bank name's own line */
#define MAT_ROW_TOP 8                                 /* screen y 92: SELECT's corner */
#define MAT_ROW_BOT 62                                /* screen y 146: the lower corners */

#define SCOPE_H    (240 - MAT_H - KNOB_H)             /* 84 */
#define SCOPE_MID  (SCOPE_H / 2)                      /* 42 */
#define SCOPE_HALF ((SCOPE_H - 2) / 2)                /* 41: 0 dBFS stays inside the band */

#if (MAT_H + KNOB_H + SCOPE_H) != 240
#error "the three bands must fill the 240 px screen"
#endif

#define WF_W   240
#define WF_SPAN 768                      /* samples held: 3 windows' worth, so the reader never wraps mid-frame */
#define WF_VIEW 384                      /* samples on screen: 8.7 ms, 1.6 columns per sample */
static volatile int16_t wf_buf[WF_SPAN];
static volatile uint16_t wf_idx;

/* The region a slot starts with: all of it, looping all of it, loop off.
 *
 * Loop off by default because that is what SMPL did before the page existed: a key
 * plays the material once. Turning LOOP on is the change a player makes on purpose.
 * The loop range defaults to the whole region so that turning LOOP on with the range
 * untouched loops what is already sounding, instead of looping somewhere else. */
static void region_reset(uint8_t i)
{
    if (i >= SMP_MAX)
        return;
    region[i].start  = 0;
    region[i].end    = smp[i].len;
    region[i].loop_s = 0;
    region[i].loop_e = smp[i].len;
    region[i].on     = 0;
}

static void region_reset_all(void)
{
    uint8_t i;

    for (i = 0; i < SMP_MAX; i++) {
        region_reset(i);
        if (i >= smp_count)
            break;                          /* the rest are zero-length anyway */
    }
}

/* Keep one slot's region inside its own material, after a turn or a slot change.
 *
 * The four numbers are edited independently, so they can be told to cross: START can
 * be turned past END, LOOP END past END. Instead of refusing the turn (the knob then
 * feels stuck, and which of the four is at fault is invisible on a 240 px screen), the
 * numbers are pushed apart by the minimum that makes them valid. A one-sample region
 * cannot interpolate, which is the reason for the 2 rather than the 1. */
static void region_fix(uint8_t i)
{
    const uint32_t len = smp[i].len;

    if (!len) {
        region[i].start = region[i].end = region[i].loop_s = region[i].loop_e = 0;
        return;
    }
    if (region[i].end > len)
        region[i].end = len;
    if (region[i].end < 2)
        region[i].end = 2;
    if (region[i].start > region[i].end - 2)
        region[i].start = region[i].end - 2;

    if (region[i].loop_e > region[i].end)
        region[i].loop_e = region[i].end;
    if (region[i].loop_e < region[i].start + 2)
        region[i].loop_e = region[i].start + 2;
    if (region[i].loop_s < region[i].start)
        region[i].loop_s = region[i].start;
    if (region[i].loop_s > region[i].loop_e - 2)
        region[i].loop_s = region[i].loop_e - 2;
}

/* Turn `*v` by `d` steps of `step` samples, and report how far it actually moved, so
 * a caller can stop nudging when the region refuses to give any more. */
static int32_t region_step(uint32_t *v, int32_t d, uint32_t step, uint32_t lo, uint32_t hi)
{
    int64_t n = (int64_t)*v + (int64_t)d * (int64_t)step;
    int64_t was = (int64_t)*v;

    if (n < (int64_t)lo)
        n = (int64_t)lo;
    if (n > (int64_t)hi)
        n = (int64_t)hi;
    *v = (uint32_t)n;
    return (int32_t)(n - was);
}

/* The step a turn moves a marker by, in samples: ONE PIXEL OF THE WINDOW.
 *
 * A turn has to move the marker by what the eye sees, not by a fixed slice of the
 * slot. The first version used len/1000 >> zoom, which made the whole slot 1000
 * detents: at 1x on a 3 s slot a full pass was ~160 turns of the wrist, and that is
 * why moving a boundary a long way was miserable while the same turn at 128x moved
 * one sample. Pixels are the unit that matches the picture at every zoom, so the
 * window's span (len >> zoom) divided by its width in pixels is the step, and a turn
 * moves the marker exactly as far as it looks like it should.
 *
 * The window is 232 px wide, not 240: the material band starts at x=4 and ends at
 * x=235, so a turn that moved 240 samples' worth would drift against the picture. */
static uint32_t region_step_samples(const wv_sample *s)
{
    uint32_t step = (s->len >> edit_zoom) / MAT_W;

    /* Never zero. A slot shorter than 232 samples at high zoom divides to 0, and a
     * zero step makes all four boundary knobs dead -- the page looks alive and the
     * markers do not move. Measured earlier with the other formula: at 128x
     * `turn K1 900` left region_start at 0. */
    return step ? step : 1u;
}


static void ui_apply(void)
{
    dsp.cut = k_cut;
    dsp.res = k_res;
    dsp.shape = k_shape;
    dsp.sh_amount = k_amount;
    dsp.lfo_rate = k_lfo_rate;
    dsp.lfo_depth = k_lfo_depth;
    dsp.ftype = k_ftype;
    dsp.ktrack = k_ktrack;
    dsp.morph = k_morph;
    dsp.grain_size = k_grain;
    dsp.spray = k_spray;
    dsp.eg_a = k_eg_a;
    dsp.eg_d = k_eg_d;
    dsp.eg_s = k_eg_s;
    dsp.eg_r = k_eg_r;
}

/* --------------------------------------------------------------- audio --- */

/* ═══ is the second core doing the work, or is the platform covering for it ═══
 *
 * This exists because the load percentage cannot answer that question and neither can
 * the audio. plat_cpu2_wait() returns -1 when the core has been given up ("the job
 * dropped: do it yourself"), the caller then renders that half on CPU0, and the output
 * is complete and correct -- the ears cannot hear which core did the arithmetic.
 * Measured on the device, four notes read 51% and eight also read 51%, which is
 * consistent both with "core 2 works" and with "core 2 is dead, CPU0 does everything,
 * and the percentage is just the total". So it gets measured, not inferred.
 *
 * The load percentage does not help: cpu_q12 is wall time in the audio ISR, and the
 * wait for core 2 is inside it. Offloading makes the interval shorter, and a dead
 * core re-rendering the half makes it longer, but the same figure could be either
 * until the split moves -- so the split needs to be seen from both sides.
 *
 * What is measured here, and what each number can and cannot say:
 *
 *   the count of handovers -- CPU0 offered the jobs. Says nothing about them being done.
 *   the interval run..wait -- CPU0's own half plus any wait at the end. It reads about
 *       half a block whether the core rendered its half or refused and let the caller
 *       re-render it (that refusal returns at once, so the short wait says nothing).
 *   the stack watermark -- the deepest the second core's OWN stack has gone, by the
 *       mark it leaves. Only that core writes there, so a nonzero reading is proof it
 *       executed instructions; it cannot prove it did the audio.
 *
 * The number that decides it is the load percentage, compared against the same notes
 * with the split off: a core doing half the voices makes the block about half as long
 * as one core doing all of them, and a core that died and let the platform cover for
 * it leaves the total exactly where one core was. Before this change four GRAN notes
 * read 51%; if the second core works they now read about 26%, and if the fallback is
 * covering for a dead core they still read 51%. That difference is the proof. */
#ifdef DBG_ON
static uint32_t c2_t0_us;           /* when the outstanding handover started, us */
static uint32_t c2_jobs;            /* jobs handed over since boot */
static uint32_t c2_handoff_us;      /* total inside run..wait, us */
static uint32_t c2_slowest_us;      /* the longest single wait */
#endif

static int cpu2_run_timed(void (*fn)(uint32_t), uint32_t arg)
{
#ifdef DBG_ON
    int r = plat_cpu2_run(fn, arg);

    if (r) {
        c2_t0_us = plat_ticks24() / PLAT_TICKS_PER_US;
        c2_jobs++;
    }
    return r;
#else
    return plat_cpu2_run(fn, arg);
#endif
}

static int cpu2_wait_timed(void)
{
#ifdef DBG_ON
    uint32_t t0 = c2_t0_us;
    int r = plat_cpu2_wait();
    uint32_t dt = plat_ticks24() / PLAT_TICKS_PER_US - t0;

    if (dt > c2_slowest_us)
        c2_slowest_us = dt;
    c2_handoff_us += dt;
    return r;
#else
    return plat_cpu2_wait();
#endif
}

void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    static float mono[BLK_MAX];
    static float scope[BLK_MAX];
    uint32_t i;

    if (n > BLK_MAX)
        n = BLK_MAX;

    /* MIDI first, so a note that arrived since the last block starts on this one
     * rather than a block later (5.8 ms at the current block size) */
    midi_in();

    /* the buffer arrives with whatever uac_mix left in it, so clear it rather than
     * add to it; the DSP then writes the app's own signal */
    for (i = 0; i < n; i++) {
        mono[i] = 0.0f;
        lr[2 * i] = 0;
        lr[2 * i + 1] = 0;
    }

    /* Split across both cores when the second one is up. wv_render is the same call
     * with the split off, so a one-core unit is not a different code path. */
    /* MASTER goes into the DSP now, before the bus limiter: the limiter is the only
     * non-linear stage on the bus, and it used to see the full-scale sum whatever
     * the pot said. `scope` is the same sum before master and the limiter. */
    pool_poll();          /* before the render: a pool that arrived mid-note is playable now */

    wv_render_split(&dsp, &bank, mono, n, (float)master_q12, cpu2, cpu2_run_timed,
                    cpu2_wait_timed, plat_cpu2_sync, scope);

    /* Observation only: the peak of the pre-master sum, in units of full scale.
     * The scope's vertical scale is pinned to this number, so a test that says
     * "the picture did not grow" has to be able to say what it should have grown
     * from. app_expect("tap_pk") reports it as per-mille. */
    {
        float pk = 0.0f;
        for (i = 0; i < n; i++) {
            float a = scope[i] < 0.0f ? -scope[i] : scope[i];
            if (a > pk)
                pk = a;
        }
        scope_pk_q = (int32_t)(pk * 4096.0f);
    }

    for (i = 0; i < n; i++) {
        float v = mono[i];
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

    /* The scope's ring: one write per sample, in 16-bit units of full scale.
     *
     * It was int8 (-127..127) while the scope normalised every frame to its own peak,
     * and that made the resolution floor the reason for the normalisation rather than
     * a separate decision. It is int16 now because the scope has a FIXED scale and a
     * fixed scale has to be able to see past 0 dBFS: eight voices at full velocity
     * measure +2.4 dBFS (1.32 of full scale), so an int8 ring would have clipped the
     * one case the scale exists to show. 768 samples of int16 is 1536 B of the 98304
     * the platform gives us, against the 768 the int8 ring took.
     *
     * Pre-master on purpose: MASTER is the listening level, and a scope that shrank
     * when the pot was turned down would be measuring the wrong thing. What this
     * shows is what the instrument produces -- velocity, envelope, polyphony, the
     * shaper, the filter. */
    for (i = 0; i < n; i++) {
        float v = scope[i] * 32767.0f;
        int32_t q = (int32_t)v;
        if (q > 32767)
            q = 32767;
        if (q < -32767)
            q = -32767;
        wf_buf[wf_idx] = (int16_t)q;
        wf_idx = (uint16_t)((wf_idx + 1u) % WF_SPAN);
    }
}

/* ---------------------------------------------------------------- input --- */

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
        /* Always from the start of the material.
         *
         * A grain fades in and out over the same piece of the sample that a one-shot
         * would have fired once, so the sound of a note is decided by the material at
         * the point it starts reading. The playhead used to resume where the last note
         * parked, which makes that point depend on when the previous note was RELEASED:
         * measured on the demo kit, the first 100 ms of a cloud started at 50 % of the
         * slot is 25.7 dB below one started at 0, and at 100 % it is 57.2 dB down. The
         * same key at the same velocity would be near-silent because of history, and that
         * is not an instrument.
         *
         * The cost is one-sided and known: scatter cannot reach backwards past sample 0,
         * so at high SPRAY the cloud is denser at the start of the material than it would
         * be in the middle (measured, 11.7 % of grains in the first 5 % of the slot
         * against 5 % uniform). That is a bias toward the transient, which is the
         * direction a player would accept.
         */
        wv_note_on_grain(&dsp, &smp[smp_i], note, vel, 0u, 0);
        return;
    }

    /* The slot's own region, pitched from its root, so a sample is played as an
     * instrument. This was `smp[0]` -- the boot slot -- which made EN_PRESET a
     * display-only control, then a chop lookup, now the region the EDIT page sets:
     * a sample is one thing with boundaries, not a kit of chops (the chop pass was
     * removed when the region arrived; a kit is a different instrument).
     *
     * `region[i].on` is the LOOP switch. With it off the voice plays start..end once
     * and goes quiet, which is what every SMPL key did before this existed. With it on
     * it wraps inside loop_s..loop_e for as long as the key is held, so a held key on a
     * long sample keeps sounding instead of running out of material.
     *
     * The loop pair is passed separately. It was not, and the consequence was visible
     * and audible at once: the two loop lines sat where the page put them, and the
     * playhead crossed the whole start..end region between them, because the engine had
     * only ever been told "loop, from start to end". */
    wv_note_on_sample_loop(&dsp, &smp[smp_i], note, vel,
                           region[smp_i].start, region[smp_i].end,
                           region[smp_i].loop_s, region[smp_i].loop_e,
                           region[smp_i].on);
}

/* --- the arpeggiator's clock and its walk ------------------------------------
 *
 * A step division at 120 BPM: the note values a player thinks in, and the set every
 * DAW offers, so "1/16" here and "1/16" there are the same length. A triplet is 2/3
 * of the plain value, which is why the table is not a power-of-two halving.
 *
 * The knob maps 1..127 linearly onto the eight entries. Coarse on purpose: an arp
 * rate is chosen by ear, and a fine control would only make a division slow to find.
 */
/* At 120 BPM a beat is 500 ms, so 1/4 is 500 and each plain division halves it: 250,
 * 125, 62. A triplet is two thirds of the plain value it sits between, which is where
 * 167 and 83 come from. The table used to hold { 1000, 667, 500, ... }, which is not a
 * note division at any tempo -- 667 ms is not 1/8 (250) and not 1/8 at half speed
 * (375) either -- so RATE could not print a name it was actually playing. */
static const uint16_t ARP_STEP_MS[8] = { 500, 250, 167, 125, 83, 62, 42, 31 };

/* The names, in the table's order. Kept next to it so the two cannot drift: the
 * readout and the clock index the same array. */
static const char *const ARP_STEP_NAME[8] = { "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32", "1/32T", "1/64" };

static uint32_t arp_step_ms(uint8_t v)
{
    if (!v)
        return 0;
    return ARP_STEP_MS[(uint32_t)(v - 1u) * 8u / 127u];
}

/* Keep a note in the held set. A ninth gives way to the oldest, the same rule the
 * voices use -- so holding more keys than the instrument can sound still does
 * something predictable rather than something different. */
static void arp_hold_add(uint8_t note, uint8_t vel)
{
    uint8_t i;

    for (i = 0; i < arp_held; i++)
        if (arp_hold[i] == note) {
            arp_vel[i] = vel;
            return;
        }
    if (arp_held < ARP_HOLD) {
        arp_hold[arp_held] = note;
        arp_vel[arp_held] = vel;
        arp_held++;
        return;
    }
    for (i = 1; i < ARP_HOLD; i++) {
        arp_hold[i - 1] = arp_hold[i];
        arp_vel[i - 1] = arp_vel[i];
    }
    arp_hold[ARP_HOLD - 1] = note;
    arp_vel[ARP_HOLD - 1] = vel;
}

static void arp_hold_del(uint8_t note)
{
    uint8_t i;

    for (i = 0; i < arp_held; i++)
        if (arp_hold[i] == note) {
            for (; i + 1 < arp_held; i++) {
                arp_hold[i] = arp_hold[i + 1];
                arp_vel[i] = arp_vel[i + 1];
            }
            arp_held--;
            return;
        }
}

/* The notes this step walks, sorted by pitch. Order is by pitch and never by when
 * the fingers landed, which is what makes an arp sound like one. */
static uint8_t arp_sort(void)
{
    uint8_t i, j, t, v;

    for (i = 1; i < arp_held; i++) {
        t = arp_hold[i];
        v = arp_vel[i];
        for (j = i; j > 0 && arp_hold[j - 1] > t; j--) {
            arp_hold[j] = arp_hold[j - 1];
            arp_vel[j] = arp_vel[j - 1];
        }
        arp_hold[j] = t;
        arp_vel[j] = v;
    }
    return arp_held;
}

static void arp_silence(void)
{
    if (arp_note != 0xFFu) {
        wv_note_off(&dsp, arp_note);
        arp_note = 0xFFu;
    }
}

/* One step. `arp_pos` walks the whole span (notes x octaves) and never wraps: it
 * is reset when it runs out, so UP-DN can bounce inside it without a second
 * counter, and RND can ignore it entirely. */
static void arp_advance(void)
{
    uint8_t n = arp_sort(), idx, note, vel;
    uint32_t span;

    if (!n) {
        arp_silence();
        arp_pos = 0;
        arp_up = 1;
        return;
    }

    span = (uint32_t)n * (uint32_t)k_arp_oct;

    switch (k_arp_type) {
    case ARP_DOWN:
        idx = (uint8_t)(span - 1u - arp_pos);
        break;
    case ARP_UPDN:
        /* 0 1 2 1 0 1 2 ... for three notes: the ends are not played twice, so the
         * span the bounce covers is 2 * span - 2 long. */
        if (span < 2u) {
            idx = 0;
        } else {
            uint32_t period = 2u * span - 2u, at = arp_pos % period;
            idx = (uint8_t)(at < span ? at : period - at);
        }
        break;
    case ARP_RND:
        arp_lcg = arp_lcg * 1103515245u + 12345u;
        idx = (uint8_t)((arp_lcg >> 16) % span);
        break;
    default:
        idx = (uint8_t)arp_pos;
        break;
    }

    note = (uint8_t)(arp_hold[idx % n] + 12u * (uint8_t)(idx / n));
    if (note > 127u)
        note = arp_hold[idx % n];
    vel = arp_vel[idx % n];

    arp_silence();
    play_note(note, vel ? vel : 100);
    arp_note = note;

    /* UP-DN needs a longer cycle than the span, or its bounce never happens: the
     * walk was clamped at `span`, so `arp_pos % (2*span - 2)` only ever saw 0..span-1
     * and UP-DN played UP. The cycle length belongs to the TYPE, so it is computed
     * here the same way arp_advance computes the index. */
    arp_pos++;
    {
        uint32_t cycle = span;
        if (k_arp_type == ARP_UPDN && span >= 2u)
            cycle = 2u * span - 2u;
        if (arp_pos >= cycle)
            arp_pos = 0;
    }
}

/* 1 kHz. The step ends, the note ends `gate` of the way through it, and a held set
 * that empties stops the note instead of waiting for the step to run out. */
static void arp_tick(void)
{
    uint32_t gate;

    if (!k_arp_rate) {
        if (arp_note != 0xFFu)
            arp_silence();
        return;
    }
    arp_ms = arp_step_ms(k_arp_rate);

    if (!arp_t) {
        arp_advance();
        arp_t = arp_ms;
        gate = (arp_ms * k_arp_gate) / 127u;
        arp_off_at = gate ? arp_ms - gate : 0u;    /* ms into the step the note ends */
    }
    if (!arp_gated && arp_t <= arp_off_at) {
        arp_silence();
        arp_gated = 1;
    }
    if (!arp_held) {
        arp_silence();
        arp_gated = 1;
    }
    if (arp_t)
        arp_t--;
}

/* the input state, kept for the lamp pass at the end of the frame: the lights are
 * drawn after the screen, and rebuilding the key mask there would read the matrix a
 * second time in the same frame */
static uint32_t in_btn, in_keys;

/* MIDI in: the packets the platform has collected, taken in the audio interrupt the
 * way the template does it (plat_midi_in drains a ring the USB/BLE/TRS handlers fill;
 * the panel's own input() runs in app_frame, which is too slow for a note that arrives
 * between frames).
 *
 * An instrument, not a master keyboard: what comes in plays exactly what the keys play
 * -- the same play_note, the same wv_note_off -- so a note from USB and the same note
 * from the panel are not two different voices with two different rules. Sending stays
 * as it was (the panel's keys out), because that is a convenience nobody depends on.
 *
 * USB, BLE and the TRS jack all arrive here: the platform merges them into one ring
 * and says which link through the port argument, which this does not need yet -- the
 * ARP and the velocity would be the only reasons to care. */
static uint32_t midi_seen;          /* packets taken, for app_expect */
static void midi_in(void)
{
    uint32_t pkt;
    int port;

    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24;

        /* clock and active sensing: a device that sends them fills the ring with
         * messages that mean nothing here, and they arrive 24 times a beat */
        if (st == 0xF8u || st == 0xFEu)
            continue;
        midi_seen++;

        if ((st & 0xF0u) == 0x90u && d2) {
            if (arp_on())
                arp_hold_add((uint8_t)d1, (uint8_t)d2);
            else
                play_note((uint8_t)d1, (uint8_t)d2);
        } else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u) {
            /* both, for the reason the panel's key-up gives: a note-on and a note-off
             * can land on different sides of an ARP page change, and a note-off that
             * only knows about one engine hangs the other. */
            wv_note_off(&dsp, (uint8_t)d1);
            arp_hold_del((uint8_t)d1);
        }
        else if (st == 0xB0u && (d1 == 123u || d1 == 120u)) {
            /* all notes off / all sound off: CC123 releases what is sounding,
             * CC120 stops it. The engine has one way to end a voice (release to
             * the floor), and 8 voices is a cheap loop -- but only the notes that
             * are actually held, not 128 calls to wv_note_off.
             *
             * The arp's set goes too: a panic that leaves it holding notes would
             * start playing again on the next tick, which is the opposite of what
             * a panic is for. */
            uint32_t v;
            arp_held = 0;
            arp_silence();
            for (v = 0; v < WV_VOICES; v++)
                if (dsp.v[v].active)
                    wv_note_off(&dsp, dsp.v[v].note);
        }
    }
}

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
            /* With the arp running a key adds to the held set and the clock plays
             * it; without it the key plays now, which is why both paths end at
             * play_note and there is no second engine. */
            if (arp_on())
                arp_hold_add(note, vel ? vel : 100);
            else
                play_note(note, vel ? vel : 100);
            /* the FM-1 as a keyboard as well as an instrument: the keys go out over
             * USB-MIDI and Bluetooth, so a DAW can record what is being played */
            plat_midi_send(0x09u | 0x90u << 8 | (uint32_t)note << 16
                           | (uint32_t)(vel ? vel : 100) << 24, TO_USB | TO_BLE);
        } else if (!(keys & bit) && (held_prev & bit)) {
            /* the last key up: the scope can go back to a neutral window. A single
             * key-up while others are held must not, so the flag is cleared only when
             * no key is left down -- checked after the loop. */
            /* BOTH, and not a choice between them.
             *
             * A note-on goes to the arp's held set when the arp is running and to a
             * voice when it is not, so a key that is held across a page change has
             * been started by one engine and is being let go by the other. Choosing
             * by the arp's state at the moment of the KEY-UP therefore strands the
             * note: hold a chord, open the ARP page, turn RATE up, release -- the
             * three voices are never released and sound until the unit is powered
             * off (measured: `expect active 0` read 2 afterwards). The other way
             * round leaves a note in the held set that plays again the next time
             * RATE leaves zero.
             *
             * Releasing both is what makes the two paths agree whatever the order
             * was: wv_note_off on a note with no voice is already a no-op (it walks
             * the eight voices and matches none), and arp_hold_del on a note that is
             * not in the set is too. The cost is one eight-iteration scan per key-up,
             * which is not a cost. */
            wv_note_off(&dsp, note);
            arp_hold_del(note);
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
    if (btn_pressed(btn, B_ENV)) {
        env_page ^= 1u;
        if (env_page)
            lfo_page = 0, arp_page = 0, edit_page = 0, fx_page = 0;   /* two pages cannot be up at once */
    }
    /* B_FX: the filter's own page, and the second of the two buttons the panel had
     * spare (panel-map.md listed it as free, with "filter type" already pencilled in).
     *
     * It exists because the filter gained two settings that are not knobs a player turns
     * while playing: which tap the VCA reads, and how far the corner follows the note.
     * Both are set once for a patch, which is what a page is for -- and the page is the
     * only place a Hz figure can honestly be shown, because with key tracking on, the
     * corner is a different frequency for every note held.
     *
     * CUT and RES stay on K1 and K2: the page is about the filter, and a filter page
     * that moved the two knobs the filter is made of would be the one thing the panel
     * must not do (the same rule the LFO page follows). */
    if (btn_pressed(btn, B_FX)) {
        fx_page ^= 1u;
        if (fx_page)
            env_page = 0, lfo_page = 0, arp_page = 0, edit_page = 0;
    }
    if (btn_pressed(btn, B_LFO)) {
        lfo_page ^= 1u;
        if (lfo_page)
            env_page = 0, arp_page = 0, edit_page = 0, fx_page = 0;
    }
    /* B_ARP: the same page mechanism. The panel has had the button since the
     * beginning (plat.h's B_ARP) and nothing used it. */
    if (btn_pressed(btn, B_ARP)) {
        arp_page ^= 1u;
        if (arp_page)
            env_page = 0, lfo_page = 0, edit_page = 0, fx_page = 0;
    }
    /* B_EDIT: edit THE CURRENT SOURCE's structure -- SMPL's region, SYNTH's frame rack,
     * and GRAN's grain controls when they exist. It replaced a one-press re-chop: a
     * button that re-analysed the slot and gave no way to see or change what it found
     * was a button that either worked or did not, and the panel has four knobs going
     * spare in every source.
     *
     * The rule is "a source's own structure", which is why this button never has to
     * compete with the two oscillators of one patch: choosing a VCO is a property of the
     * voice, not of the material, and that is planned for B_HOME instead (spec.md §5d).
     * So adding GRAN's page here takes nothing from SMPL's or SYNTH's. */
    if (btn_pressed(btn, B_EDIT)) {
        /* Today that is SMPL and SYNTH. GRAN has no page yet, and the button does
         * nothing there rather than guessing one -- see the GRAN order in spec.md §5d
         * (cloud region first, it is already a call site).
         *
         * It used to switch the source to SMPL from SYNTH and GRAN, on the argument
         * that a button which does nothing is a broken button. That was wrong in use:
         * EDIT sits next to the pages a player actually presses, and reaching for one
         * of them landed on EDIT, which silently replaced the source and the whole
         * screen's colour. A button that does nothing where it applies is quieter than
         * one that changes what the instrument is. */
        if (src == SRC_SMPL || src == SRC_SYNTH) {
            edit_page ^= 1u;
            if (edit_page) {
                env_page = 0, lfo_page = 0, arp_page = 0, fx_page = 0;
                edit_src = src;
            }
        }
    }

    /* The LFO page takes K3 and K4, not K1 and K2.
     *
     * CUT and RES stay where they are because they are the parameters the LFO acts
     * on: with the page up you want to see the filter you are sweeping, and CUT
     * sitting on K1 in every mode and on every page is the one thing the panel must
     * not move. SHAPE and AMOUNT are what give way -- they are the pair a filter LFO
     * does not change, and they are one button away. */
    /* The ARP page keeps CUT on K1 for the same reason the LFO page does, and for a
     * stronger one: RATE doubles as the on/off switch, so the page needs a parameter
     * that stays useful while the arp is off -- a page where every knob does nothing
     * until one of them has been turned is a page you cannot see the state of. */
    if ((e = plat_enc(EN_K1)) != 0) {
        if (edit_page && src == SRC_SYNTH) {
            /* The frame the offset knobs act on. It wraps: a frame is a place in a
             * cycle, not a value with ends. */
            int32_t f = ((int32_t)edit_frame + (int32_t)e) % (int32_t)WV_FRAMES;
            edit_frame = (uint8_t)(f < 0 ? f + (int32_t)WV_FRAMES : f);
        }
        else if (edit_page) {
            int32_t d;
            edit_live = 0;
            d = region_step(&region[smp_i].start, e, region_step_samples(&smp[smp_i]),
                            0, region[smp_i].end - 2u);
            if (d) region_fix(smp_i);
        }
        else if (env_page)      k_eg_a = (uint8_t)clamp((int32_t)k_eg_a + e * 4, 0, 127);
        else if (arp_page) k_arp_rate = (uint8_t)clamp((int32_t)k_arp_rate + e * 4, 0, 127);
        else               k_cut  = (uint8_t)clamp((int32_t)k_cut  + e * 4, 0, 127);
    }
    if ((e = plat_enc(EN_K2)) != 0) {
        if (edit_page && src == SRC_SYNTH) {
            /* One detent is one point of 256 -- the resolution of the table itself, and
             * the finest step the waveform above the knob can draw. */
            int32_t v = (int32_t)foff_cur() + (int32_t)e;
            foff_tab[bank_i][edit_frame] = (uint8_t)(((v % 256) + 256) % 256);
        }
        else if (edit_page) {
            int32_t d;
            edit_live = 2;
            d = region_step(&region[smp_i].loop_s, e, region_step_samples(&smp[smp_i]),
                            region[smp_i].start, region[smp_i].loop_e - 2u);
            if (d) region_fix(smp_i);
        }
        else if (env_page)      k_eg_d = (uint8_t)clamp((int32_t)k_eg_d + e * 4, 0, 127);
        else if (arp_page) k_arp_type = (uint8_t)((k_arp_type + (e > 0 ? 1 : ARP_TYPE_N - 1)) % ARP_TYPE_N);
        else               k_res  = (uint8_t)clamp((int32_t)k_res  + e * 4, 0, 127);
    }
    if ((e = plat_enc(EN_K3)) != 0) {
        if (edit_page && src == SRC_SYNTH) {
            /* The whole ramp at once: this is the knob to reach for first, because one
             * turn fans all eight frames out and that is what the A/B is about. */
            foff_spread = (uint8_t)clamp((int32_t)foff_spread + (int32_t)e, 0, 127);
            foff_ramp();
        }
        else if (edit_page) {
            int32_t d;
            edit_live = 3;
            d = region_step(&region[smp_i].loop_e, e, region_step_samples(&smp[smp_i]),
                            region[smp_i].loop_s + 2u, region[smp_i].end);
            if (d) region_fix(smp_i);
        }
        else if (env_page)      k_eg_s = (uint8_t)clamp((int32_t)k_eg_s + e * 4, 0, 127);
        else if (lfo_page) k_lfo_rate = (uint8_t)clamp((int32_t)k_lfo_rate + e * 4, 0, 127);
        else if (fx_page) k_ftype = (uint8_t)((k_ftype + (e > 0 ? 1 : WV_FTYPE_N - 1)) % WV_FTYPE_N);
        else if (arp_page) k_arp_oct = (uint8_t)clamp((int32_t)k_arp_oct + (e > 0 ? 1 : -1), 1, 4);
        else               k_shape = (uint8_t)((k_shape + (e > 0 ? 1 : WV_SHAPE_N - 1)) % WV_SHAPE_N);
    }
    if ((e = plat_enc(EN_K4)) != 0) {
        if (edit_page && src == SRC_SYNTH) {
            foff_shape = (uint8_t)((uint32_t)(foff_shape + (e > 0 ? 1u : FOFF_SHAPE_N - 1u)) % FOFF_SHAPE_N);
            foff_ramp();
        }
        else if (edit_page) {
            int32_t d;
            edit_live = 1;
            d = region_step(&region[smp_i].end, e, region_step_samples(&smp[smp_i]),
                            region[smp_i].start + 2u, smp[smp_i].len);
            if (d) region_fix(smp_i);
        }
        else if (env_page)      k_eg_r = (uint8_t)clamp((int32_t)k_eg_r + e * 4, 0, 127);
        else if (lfo_page) k_lfo_depth = (uint8_t)clamp((int32_t)k_lfo_depth + e * 4, 0, 127);
        /* KEY TRK: five detents, wrapping, not a 0..127 sweep. The setting is a choice
         * out of five and the DSP test asserts the five values; a continuous knob here
         * would let the panel produce a 37 % that save_load then refuses to load. */
        else if (fx_page) {
            uint8_t i = kt_index(k_ktrack);
            i = (uint8_t)((i + (e > 0 ? 1 : KT_STEPS - 1)) % KT_STEPS);
            k_ktrack = KT_VALUES[i];
        }
        else if (arp_page) k_arp_gate = (uint8_t)clamp((int32_t)k_arp_gate + e * 4, 1, 127);
        else               k_amount = (uint8_t)clamp((int32_t)k_amount + e * 4, 0, 127);
    }
    /* EN_ALGO is MORPH in SYNTH, and GRAIN SIZE in GRAN. This is the one place a knob
     * changes meaning with the mode, and it is deliberate: MORPH has nothing to read
     * in a grain voice (wv_table_read is the only consumer, and a cloud never reaches
     * it), while GRAIN SIZE is the most-turned granular parameter and has to be on a
     * knob. GRAN needs seven controls for seven encoders, and this is the seventh.
     * In SMPL it stays MORPH and is dead, as it always was. */
    if ((e = plat_enc(EN_ALGO)) != 0) {
        if (edit_page && src != SRC_SYNTH) {
            /* ZOOM: left is out, right is in, one step per detent, clamped at both
             * ends. The window is a power of two of the slot, 1/2^zoom, so eight
             * steps reach one sample in 256 -- enough to see the waveform's shape
             * without a scroll control, because the window follows the last marker
             * turned (see the picture painter). */
            int32_t z = (int32_t)edit_zoom + (e > 0 ? 1 : -1);
            edit_zoom = (uint8_t)(z < 0 ? 0 : (z > 7 ? 7 : z));
        }
        else if (src == SRC_GRAN) k_grain = (uint8_t)clamp((int32_t)k_grain + e * 4, 0, 127);
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
    if (btn_pressed(btn, B_SEL)) {
        src = (uint8_t)((src + 1u) % 3u);
        /* Leaving SMPL leaves EDIT. THE PAGE BELONGS TO THE SOURCE, not to the panel.
         *
         * Without this, opening EDIT in SMPL and then switching SOURCE left `edit_page`
         * set on a source that has no EDIT page at all, and the flag is read all over the
         * screen: on GRAN the bottom-right corner printed ZOOM instead of GRAIN, the four
         * knobs below became START / LOOP S / LOOP E / END instead of CUT / RES / SHAPE /
         * AMOUNT, and the material band drew SMPL's region boundaries over a granular
         * cloud that has none. On SYNTH it was the same with LOOP and MORPH replaced.
         *
         * Measured on the host: SMPL + EDIT, then `tap SEL` twice, `expect edit` stayed 1
         * through GRAN and back to SYNTH.
         *
         * B_EDIT itself already refuses to act outside SMPL (see the comment there: a
         * button that does nothing where it does not apply is quieter than one that
         * changes the instrument). This is the other half of that rule -- the page cannot
         * outlive the source that owns it. */
        if (src != SRC_SMPL && src != SRC_SYNTH)
            edit_page = 0;
        /* ...and a page is one source's, so switching from SMPL to SYNTH closes it too
         * rather than reinterpreting the same flag as the other source's page. The flag
         * means "this source's material editor", and the two editors edit different
         * things with different knobs. */
        if (edit_src != src)
            edit_page = 0;
    }

    /* B_SAVE: write the whole panel state to flash, now.
     *
     * It writes while notes are held, on purpose. The alternative -- wait for silence --
     * is a save that does not happen when the chord is the thing you want to keep, and
     * the player pressing SAVE with a chord down is not an edge case, it is the normal
     * way to keep a sound you have just found. The cost is a 4 KiB erase (storage.c) with
     * voices running; that is the player's own decision to make, not the firmware's.
     *
     * The answer is said either way. A save that fails silently is worse than no save:
     * the player believes the sound is kept and finds out at power-off. */
    if (btn_pressed(btn, B_SAVE)) {
        save_rec r;
        save_snapshot(&r);
        if (save_now(&r) == 0)
            app_say("SAVED", "settings");
        else
            app_say("SAVE FAILED", "flash");
    }

    /* EN_PRESET is a different control depending on SOURCE (see docs/spec.md):
     * SYNTH picks the bank, SMPL picks the sample. One knob, two jobs, because the
     * panel has one knob and both jobs are "which material". */
    /* EN_SELECT picks the material in every source, because it is the knob marked
     * SELECT and that is what SELECT means: which thing is loaded. In SYNTH that is the
     * bank, in SMPL and GRAN it is the sample slot. It sits top-right on the screen
     * because it sits top-right on the panel.
     *
     * The rest follows the same rule -- the screen's corners are the panel's corners:
     *
     *              SELECT   (top right)    material: BANK / SLOT
     *              PRESET   (bottom left)  GRAN only: SPRAY
     *              ALGO     (bottom right) SYNTH: MORPH   GRAN: GRAIN SIZE
     *
     * Both lower knobs are free in every mode except GRAN, where SPRAY and GRAIN SIZE
     * are the two things a granular player reaches for. That is why SPRAY could leave
     * EN_SELECT without costing anything: the knob that used to hold it now holds the
     * material, which is the job it is named after. */
    if ((e = plat_enc(EN_SELECT)) != 0) {
        if (src == SRC_SYNTH) {
            bank_i = (uint8_t)((bank_i + (e > 0 ? 1 : WV_BANK_COUNT - 1)) % WV_BANK_COUNT);
            bank.frames = WV_BANKS[bank_i];
            bank.foff = foff_tab[bank_i];
        } else if (smp_count) {
            smp_i = (uint8_t)((smp_i + (e > 0 ? 1 : smp_count - 1)) % smp_count);
            region_fix(smp_i);   /* the region lives per slot, so nothing to redo */
        }
    }
    if (src == SRC_GRAN) {
        /* SPRAY on EN_PRESET: bottom-left, next to GRAIN SIZE on bottom-right, because
         * those are the two knobs a granular player turns together. It was on EN_SELECT
         * only because the material had to live somewhere else; now the material is on
         * SELECT and this knob is free. */
        if ((e = plat_enc(EN_PRESET)) != 0)
            k_spray = (uint8_t)clamp((int32_t)k_spray + e * 4, 0, 127);
    } else if (edit_page && src == SRC_SYNTH) {
        /* ZERO on EN_PRESET: turn it either way and the whole ramp goes back to nothing.
         *
         * A one-way setting with no way back is one people stop touching, and unlike a
         * knob this value has no middle to return to by ear. The knob is free in SYNTH.
         * All eight frames, not just the selected one: what it undoes is the ramp, and
         * leaving seven offsets behind while the page says "zero" would be a lie. */
        if ((e = plat_enc(EN_PRESET)) != 0) {
            foff_spread = 0;
            foff_ramp();
        }
    } else if (edit_page) {
        /* LOOP on EN_PRESET: two states, right is on and left is off, so the knob can
         * be turned without looking at the screen -- the same two-state rule the GLOBAL
         * page's rows use. */
        if ((e = plat_enc(EN_PRESET)) != 0)
            region[smp_i].on = (e > 0);
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

/* num() writes into a buffer the caller owns. knob() needs the value inline, and a
 * single static buffer would let the second call overwrite the first before knob()
 * had drawn it -- the same trap sel_time() documents. Ring of four: the widest screen
 * draws four numeric values at once (CUT RES AMOUNT, plus one mode knob). */
/* "3/8": a position and the count of positions. Static ring like pct(), because the
 * caller draws it immediately and a single buffer would be clobbered by the next call. */
/* Rendered width of a string in FONT_XS, from the font's advance table. Proportional,
 * so this cannot be "7 px times the glyph count". */
static int32_t txt_w(const char *t)
{
    int32_t w = 0;
    while (*t) {
        int32_t c = (uint8_t)*t++;
        if (c >= FONT_XS.first && c <= FONT_XS.last)
            w += FONT_XS.adv[c - FONT_XS.first];
    }
    return w;
}

static char *num2(int32_t a, int32_t b)
{
    static char ring[4][8];
    static uint8_t turn;
    char *o = ring[turn++ % 4];
    char t[8];
    int n = 0, i, k;
    for (k = 0; k < 2; k++) {
        int32_t v = k ? b : a;
        if (v < 0) { o[n++] = '-'; v = -v; }
        i = 0;
        do { t[i++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (i) o[n++] = t[--i];                 /* t[] is least-significant-first */
        if (k == 0) o[n++] = '/';
    }
    o[n] = 0;
    return o;
}

static char *pct(uint8_t v)
{
    static char ring[4][6];
    static uint8_t turn;
    char *b = ring[turn++ % 4];
    num(b, (int32_t)v * 100 / 127);
    return b;
}

/* A position in the material as text: milliseconds of the slot. A boundary is a place
 * in the material, and ms is where a sampler puts it; a sample index would be a number
 * nothing else on the panel speaks in. Two of these appear on one screen, hence the
 * ring. */
static char *pos_text(uint8_t slot, uint32_t at)
{
    static char ring[4][10];
    static uint8_t turn;
    char *b = ring[turn++ % 4];
    uint32_t rate = smp[slot].rate ? smp[slot].rate : 44100u;
    /* SECONDS with three decimals, not milliseconds.
     *
     * A boundary is a place in the material, and this is that place in the unit a sampler
     * puts it in. It was `52369ms`, five digits plus the unit, which measured 50 px and no
     * longer fits anywhere: the value moved INSIDE the knob's ring, and the hole is 37 px
     * wide at the text's own height (a chord at +-7.5 of a 20 px inner radius, not the
     * inner diameter -- that is the arithmetic this had to be checked against). Seconds
     * are 33 px at the widest the slots can be, and read as a time rather than a count.
     *
     * The divisor is the slot's own rate, so 22.05 kHz material and 44.1 kHz material both
     * print the wall-clock length you hear.
     *
     * Longest case: 12.810 s (a 44.1 kHz slot at the 3.2 s limit is 3.200, and 12.810 is
     * the 11.025 kHz one). The three decimals are kept because at 128x zoom the window is
     * 8 samples and a two-decimal millisecond figure would show the same number for
     * different positions -- a boundary control whose number does not move is broken. */
    {
        uint64_t us = (uint64_t)at * 1000000u / rate;
        uint32_t ip = (uint32_t)(us / 1000000u);
        uint32_t fp = (uint32_t)(us % 1000000u) / 1000u;   /* milliseconds 0..999 */
        int n = 0, i2 = 0;
        char t[10];

        do { t[i2++] = (char)('0' + ip % 10u); ip /= 10u; } while (ip);
        while (i2) b[n++] = t[--i2];
        b[n++] = '.';
        b[n++] = (char)('0' + fp / 100u);
        b[n++] = (char)('0' + (fp / 10u) % 10u);
        b[n++] = (char)('0' + fp % 10u);
        b[n++] = 's';
        b[n] = 0;
    }
    return b;
}

/* The same position as the 0..127 a knob's bar takes. A bar is not a value here, it is
 * the marker's place in the slot, which is the one thing the four knobs have in common:
 * at a glance the four bars show the four boundaries' order without reading a number. */
static int32_t region_mark(uint8_t slot, uint8_t which)
{
    uint32_t len = smp[slot].len;
    uint32_t at;

    if (!len)
        return 0;
    at = (which == 0) ? region[slot].start : (which == 1) ? region[slot].end
       : (which == 2) ? region[slot].loop_s : region[slot].loop_e;
    if (at > len)
        at = len;
    return (int32_t)((uint64_t)at * 127u / len);
}

/* ZOOM as the multiplier it is: "1x", "2x" ... "128x". A percentage would say
 * nothing -- zoom is a power of two and 43 % of the way in is not a span anyone can
 * picture. The window is the slot divided by this number. */
static char *zoom_text(uint8_t z)
{
    static char ring[2][8];
    static uint8_t turn;
    char *b = ring[turn++ % 2];
    int32_t n = 0, m = (int32_t)1 << (z > 7 ? 7 : z);
    int i = 0;
    char t[8];

    do { t[i++] = (char)('0' + m % 10); m /= 10; } while (m);
    while (i) b[n++] = t[--i];
    b[n++] = 'x';
    b[n] = 0;
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
/* The LFO page's two readouts. RATE in Hz, because a rate in percent is not a rate;
 * DEPTH in octaves, because CUT is exponential and "half way" means nothing. Both use
 * a small ring of buffers for the same reason sel_time does: knob() takes its text and
 * a box count computed from it, and one static buffer would let the second call
 * overwrite the first before knob() had used it. */
static char *lfo_rate_text(uint8_t v)
{
    static char ring[2][8];
    static uint8_t turn;
    char *b = ring[turn++ % 2];
    float hz = wv_lfo_hz(v);
    int n = 0;

    if (hz == 0.0f) {
        b[0] = 'O'; b[1] = 'F'; b[2] = 'F'; b[3] = 0;
        return b;
    }
    if (hz < 10.0f) {                       /* one decimal: 0.02 .. 9.9 */
        int32_t t = (int32_t)(hz * 10.0f + 0.5f);
        b[n++] = (char)('0' + t / 10);
        b[n++] = '.';
        b[n++] = (char)('0' + t % 10);
    } else {                                /* 10 .. 20, no room for a decimal */
        int32_t t = (int32_t)(hz + 0.5f);
        if (t >= 10)
            b[n++] = (char)('0' + (t / 10) % 10);
        b[n++] = (char)('0' + t % 10);
    }
    b[n++] = 'H';
    b[n++] = 'z';
    b[n] = 0;
    return b;
}

/* The ARP page's two readouts. RATE prints the note division, because that is what
 * the player is choosing and "1/16" is a length while "43" is not; OFF at zero says
 * the knob is also the switch. OCT prints "2x", a height rather than a percentage. */
static char *arp_rate_text(uint8_t v)
{
    static char ring[2][6];
    static uint8_t turn;
    char *b = ring[turn++ % 2];
    const char *s;
    int n = 0;

    if (!v) {
        b[0] = 'O'; b[1] = 'F'; b[2] = 'F'; b[3] = 0;
        return b;
    }
    s = ARP_STEP_NAME[(uint32_t)(v - 1u) * 8u / 127u];
    while (*s && n < 5)
        b[n++] = *s++;
    b[n] = 0;
    return b;
}

static char *arp_oct_text(uint8_t v)
{
    static char ring[2][4];
    static uint8_t turn;
    char *b = ring[turn++ % 2];

    b[0] = (char)('0' + (v < 1u ? 1u : v > 4u ? 4u : v));
    b[1] = 'x';
    b[2] = 0;
    return b;
}

/* The FX page's two identifiers.
 *
 * KEY TRK is stored as 0/25/50/75/100 rather than an index, so the record reads as the
 * setting a player asked for and save_load can reject anything else. The table is what
 * turns a detent into the setting, and kt_index() is the way back -- one list, so the
 * five can never drift apart from the five the DSP validates. */
static uint8_t kt_index(uint8_t pct)
{
    uint8_t i;
    for (i = 0; i < KT_STEPS; i++)
        if (KT_VALUES[i] == pct)
            return i;
    return 0;                          /* an impossible value reads as off */
}

static const char *const FTYPE_NAME[WV_FTYPE_N] = { "LP", "HP", "BP" };

/* KEY TRK as the setting itself. Three digits and a sign is the widest string this page
 * can print ("100%"), which measures inside the ring's 34.9 px chord. */
/* ─── SYNTH's EDIT page: the phase ramp ────────────────────────────────────
 *
 * K1 picks a frame, K2 turns that frame, K3 rewrites all eight as a ramp, K4 picks the
 * ramp's shape. The value is 0..255 points of the 256-point cycle, printed as a fraction
 * of the cycle because "46 pts" means nothing and "1/4 cycle" does.
 */
static uint8_t foff_cur(void)
{
    return foff_tab[bank_i][edit_frame];
}

static void foff_ramp(void)
{
    uint32_t f, v = 0, step = (uint32_t)foff_spread * 2u;   /* 0..254 points per frame */

    for (f = 0; f < WV_FRAMES; f++) {
        switch (foff_shape) {
        case FOFF_ALT:  v = (f & 1u) ? step : 0u; break;
        case FOFF_RAND: v = (f * 73u + 19u) & 0xFFu; v = v * step / 255u; break;
        default:        v = f * step; break;
        }
        foff_tab[bank_i][f] = (uint8_t)v;
    }
    bank.foff = foff_tab[bank_i];
}

/* "1/4" -- the offset as a fraction of the cycle, because 0..255 is what the ring is for
 * and the text should say what the number DOES. Quarters come out exact; anything else
 * prints as a percentage. */
static char *foff_text(void)
{
    static char ring[2][8];
    static uint8_t turn;
    char *b = ring[turn++ % 2];
    uint32_t v = foff_cur();

    if (!v) {
        b[0] = '0'; b[1] = 0; b[2] = 0;
    } else if (v % 64u == 0u) {
        b[0] = (char)('0' + v / 64u); b[1] = '/'; b[2] = '4'; b[3] = 0;
    } else {
        uint32_t p = v * 100u / 256u;
        if (p < 10u) {
            b[0] = (char)('0' + p); b[1] = '%'; b[2] = 0;
        } else if (p < 100u) {
            b[0] = (char)('0' + p / 10u); b[1] = (char)('0' + p % 10u); b[2] = '%'; b[3] = 0;
        } else {
            b[0] = '1'; b[1] = '0'; b[2] = '0'; b[3] = '%'; b[4] = 0;
        }
    }
    return b;
}

/* The ring shows the offset as a position in the cycle: one full turn of the ring is one
 * full cycle of phase, which is the only mapping where the ring means the same thing as
 * the waveform above it. */
static int32_t foff_mark(void)
{
    return (int32_t)((uint32_t)foff_cur() * 127u / 256u);
}

static const char *const FOFF_SHAPE_NAME[FOFF_SHAPE_N] = { "LIN", "ALT", "RAND" };

static char *frame_text(void)
{
    static char b[6];

    b[0] = (char)('0' + (int)edit_frame);
    b[1] = '/';
    b[2] = (char)('0' + (int)(WV_FRAMES - 1u));
    b[3] = 0;
    return b;
}

static char *spread_text(void)
{
    static char ring[2][8];
    static uint8_t turn;
    char *b = ring[turn++ % 2];
    uint32_t v = (uint32_t)foff_spread * 2u;    /* points per frame */

    b[0] = (char)('0' + v / 100u);
    b[1] = (char)('0' + v / 10u % 10u);
    b[2] = (char)('0' + v % 10u);
    b[3] = 0;
    if (v < 100u) { b[0] = (char)('0' + v / 10u); b[1] = (char)('0' + v % 10u); b[2] = 0; }
    if (v < 10u) { b[0] = (char)('0' + v); b[1] = 0; }
    return b;
}

static char *ktrack_text(uint8_t pct)
{
    static char ring[2][6];
    static uint8_t turn;
    char *b = ring[turn++ % 2];
    if (pct >= 100u) {
        b[0] = '1'; b[1] = '0'; b[2] = '0'; b[3] = '%'; b[4] = 0;
    } else if (pct >= 10u) {
        b[0] = (char)('0' + pct / 10u); b[1] = (char)('0' + pct % 10u); b[2] = '%'; b[3] = 0;
    } else {
        b[0] = '0'; b[1] = '%'; b[2] = 0;
    }
    return b;
}

static char *lfo_depth_text(uint8_t v)
{
    static char ring[2][8];
    static uint8_t turn;
    char *b = ring[turn++ % 2];
    /* 0..4 octaves, one decimal */
    int32_t t = (int32_t)(4.0f * ((float)v / 127.0f) * 10.0f + 0.5f);
    b[0] = '+';
    b[1] = (char)('0' + t / 10);
    b[2] = '.';
    b[3] = (char)('0' + t % 10);
    b[4] = 0;
    return b;
}

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

/* How long the current unsaved run has been open, for the save line.
 *
 * The line under the scope is the only place on screen that says "this is not saved", and it
 * says two things at once: LENGTH is how many groups differ (save_change_count), COLOUR is
 * how long they have been that way. Time is the second axis because the two questions are
 * different -- "did I change much?" and "have I been sitting on it?" -- and a single bar
 * cannot answer both.
 *
 * The clock starts when the count first rises from zero, not at boot, so a fresh unit with
 * nothing changed shows no elapsed time. It keeps running while the player keeps editing:
 * the point is how long THIS batch of edits has been unsaved, not how long ago the first
 * one happened.
 *
 * `unsaved_at` is plat_ms() and wraps after 49.7 days. `now - unsaved_at` is unsigned, so
 * the subtraction is correct across the wrap -- no special case needed. */
static uint32_t unsaved_at;     /* plat_ms() when the count rose from zero; 0 = not dirty */
static int32_t  save_line_len;  /* what the save line drew last frame, px */
static int32_t  save_line_step; /* its brightness step, 0..3 (dim(x, step+2, 5)) */
static uint32_t save_line_age;  /* ms since the first unsaved edit */
static int32_t  save_led;       /* B_SAVE's lamp: 0 saved, 1 mid, 2 lit */
/* How many band transfers the last frame sent. Three is the whole screen: scope,
 * material, knobs. The count is here because a transfer is not free -- lcd_data()
 * waits for the previous one, so an extra blit is an extra 8 ms of the frame spent
 * sending pixels that are already on the glass. A probe, not a setting. */
static int32_t  frame_blits;
static int      unsaved_live;   /* unsaved_at holds a start time */

static src_skin_t src_skin(void)
{
    src_skin_t k;

    if (src == SRC_SMPL) {
        /* Green: one of the three bench scope colours (amber / green / blue-white).
         * The hue is the platform's own GREEN palette step (gfx.c: 80,220,120 /
         * 150,255,175) so it is the same green a THEME=GREEN screen uses, not a fourth
         * invention. SMPL got this colour because it used to be "the other amber" and
         * was indistinguishable from SYNTH -- see src_skin's SYNTH branch for why the
         * three-source mapping wins over THEME here. */
        k.bg     = RGB(13, 41, 22);
        k.track  = RGB(8, 26, 13);
        k.ink    = RGB(80, 220, 120);
        k.ink_hi = RGB(150, 255, 175);
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
        /* SYNTH: amber, the AMBER palette's own two steps, on a ground pulled down from
         * them.
         *
         * This used to keep the platform palette (black ground, neutral grey labels,
         * amber only on the curve), on the argument that overriding it would fight
         * GLOBAL's THEME. That argument does not hold: the three sources are a fixed
         * amber/green/blue-white mapping by design, so THEME stopped reaching this
         * screen the moment the second colour was added. What it actually produced was
         * an inconsistency -- two sources tinted throughout and the default one black
         * with grey labels -- and a screen where the source colour looked like an
         * afterthought on the source you spend the most time in.
         *
         * Same structure as the other two: ground is a dimmed version of the hue, ink
         * is the hue, ink_hi is its highlight step. White stays the accent for "what
         * is being touched" only, in all three.
         *
         * The cost, stated: APP_PALETTE / THEME now changes nothing on this screen.
         * Three fixed source colours and a user-selectable theme cannot both be true;
         * three colours is what the panel is for. */
        k.bg     = RGB(46, 30, 8);
        k.track  = RGB(30, 20, 5);
        k.ink    = RGB(232, 136, 30);
        k.ink_hi = RGB(255, 184, 70);
        /* White, pulled two thirds of the way toward this skin's own hue. Pure white on
         * an amber ground reads as a different material -- a paper cut-out laid on the
         * picture -- and the player asked for it to sit in the family. (246,232,214) is
         * white by any measure (all three channels high, no saturated cast) and it still
         * carries a warm bias a few points off the ink's hue, so K1's frame reads as
         * "lit up" rather than "cut out". */
        k.acc    = RGB(246, 232, 214);
    }
    return k;
}

/* The source's shape, in the strip. A polyline rather than rects, because the thing
 * being drawn is a curve and a curve is what the scope above shows: the same visual
 * language, one scale down. */
static void src_icon(const src_skin_t *k, int32_t x0, int32_t y0, int32_t w, int32_t h)
{
    /* Geometry is the caller's: the icon is drawn at 32 px in the material band and was
     * drawn at 56 in the strip, and hardcoding either would have made the other land in
     * the wrong place. */
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
/* ─── the source shape ────────────────────────────────────────────────────── */

/* What is being FED to the chain, as opposed to what came out of it. The scope at the
 * top shows the result of shaper -> filter -> VCA, which is what the ear hears; this
 * shows the material. Without it a dull or thin sound is ambiguous: is the table like
 * that, or is CUT/SHAPE/AMOUNT doing it? Two pictures answer that, one does not.
 *
 * SYNTH only for now. The table is 256 points and the strip is 240 columns, so it maps
 * almost one to one and every wiggle survives. A sample would need a min/max envelope
 * (8800 samples into 240 columns: transients visible, individual cycles not) and GRAN
 * would need the grain window and its overlap rather than a waveform -- and that shape
 * is already the source icon, so drawing it twice would say nothing.
 *
 * THE PHASE IS FIXED, and that is the whole design rather than a shortcut. Reading the
 * table at the audio phase would scroll, which is the defect the scope was just cured
 * of. Stepping a local phase 0..1 across the width makes this a still picture of the
 * material: it moves when BANK or MORPH moves, and at no other time. It follows the
 * sound, not the moment, which is the job.
 *
 * MORPH goes through the same wv_table_read() the voices use, so the drawn shape is the
 * shape being played -- a 50% morph is the interpolated frame, not frame 3.5 rounded. */
/* ─── the two sample sources' material pictures ───────────────────────────────
 *
 * SYNTH can draw its table directly: 256 points and the picture is the shape you hear.
 * A sample cannot be drawn that way, and the reason is arithmetic rather than style.
 *
 * DEMO K is 9256 samples and the band is 176 px, so each column would stand for 53
 * samples. Drawing the value at one sample per column gives a line that jumps between
 * whatever those 53 happened to be at that instant -- the picture would change character
 * with the phase and tell you nothing. What survives the reduction is the envelope: for
 * each column, the minimum and the maximum over the samples in it, drawn as a vertical
 * bar. That is the standard waveform display in every editor, and it is lossless in the
 * only sense that matters here: a bar from min to max is a true statement about the
 * material in that column.
 *
 * Both pictures answer the same question the table picture answers -- what is in here --
 * and neither is a level meter. The scope above already shows level.
 */

/* A sample's own shape: min/max per column, which is the only reduction that stays
 * honest across 53 samples per pixel.
 *
 * `from`/`to` in the slot's samples let the same function draw a slice's region (SMPL
 * draws the whole slot: the band shows the material, and the chops are what the keys
 * are for).
 *
 * Note what is NOT done here: no per-column peak normalisation. The table picture
 * normalises per frame because a bank's frames differ in peak by 3.4 dB and MORPH must
 * stay readable. A sample's own shape is the material, and its dynamics are part of
 * that shape -- a drum's decaying bars are the information, not a defect to correct.
 * The only scaling is one common factor to the band's height, chosen once for the whole
 * slot, so a quiet sample is drawn quiet. */
/* The material's envelope: min/max per column, filled, drawn dim.
 *
 * A filled min/max band is what a waveform display wants, and it was removed once on my
 * reading of "the sampling is so dense it comes out as a solid shape" -- the reasoning
 * was that 53 samples per column let a drum hit span 60 of the 78 px, so every column of
 * every hit was filled. That measurement was right and the conclusion was wrong: the
 * player has now seen both and wants the fill back. A filled band is how much energy is
 * at this moment in the material; a peak line is only its outline, and at 1 px on a dark
 * ground the outline of a loud passage is a thin thing that does not read as loud.
 *
 * So: fill, at 2/5 of the ink. The brightness is the part that was actually wrong -- SMPL
 * used to draw this at full ink_hi strength while GRAN drew the same material dim, so the
 * two sources disagreed about how loud their own background was. Both now call this with
 * dim(ink, 2, 5), so they cannot drift apart again.
 */
static uint16_t dim(uint16_t c, int32_t num, int32_t den);

static void env_line(const wv_sample *s, int32_t x0, int32_t y0, int32_t w, int32_t h,
                     uint32_t from, uint32_t to, uint16_t colour)
{
    int32_t mid = y0 + h / 2, amp = h / 2 - 1;
    int32_t i;
    float pk = 0.0f, sc;

    if (!s || !s->data || !s->len || to <= from || to > s->len) {
        /* nothing to draw: a flat line at zero, which says "no material" rather than
         * leaving the band empty and looking like a fault */
        cv_rect(x0, mid, w, 1, colour);
        return;
    }

    {   /* one scale for the whole slot, from its own peak: the picture shows shape,
         * not absolute level (the scope is where level is read) */
        uint32_t j;
        for (j = from; j < to; j++) {
            float v = (float)s->data[j] * (1.0f / 32768.0f);
            if (v < 0.0f)
                v = -v;
            if (v > pk)
                pk = v;
        }
        if (pk < 0.02f)
            pk = 1.0f;
        sc = (float)amp / pk;
    }

    for (i = 0; i < w; i++) {
        uint32_t a = from + (uint32_t)((uint64_t)(to - from) * (uint32_t)i / (uint32_t)w);
        uint32_t b = from + (uint32_t)((uint64_t)(to - from) * (uint32_t)(i + 1) / (uint32_t)w);
        float lo = 0.0f, hi = 0.0f;
        uint32_t j;
        int32_t yh, yl;

        if (b <= a)
            b = a + 1;
        for (j = a; j < b && j < s->len; j++) {
            float v = (float)s->data[j] * (1.0f / 32768.0f);
            if (v < lo)
                lo = v;
            if (v > hi)
                hi = v;
        }
        yh = mid - (int32_t)(hi * sc);
        yl = mid - (int32_t)(lo * sc);
        if (yh < y0)
            yh = y0;
        if (yl > y0 + h - 1)
            yl = y0 + h - 1;
        if (yl < yh)
            yl = yh;
        cv_rect(x0 + i, yh, 1, yl - yh + 1, colour);
    }
}

/* GRAN's material picture: three layers, one per parameter that has a position.
 *
 *   envelope    dim, the whole slot, min/max per column      what the sample is
 *   grain bar   bright, x from pos, WIDTH = grain length     GRAINSIZE
 *   their spread over the map                                SPRAY
 *
 * The design went through two wrong versions, and the reason matters more than the
 * result:
 *
 *   1. Ticks at the grain positions. Two grains landed 5 px apart and read as one tick,
 *      so the picture could not answer "how many grains". Ticks encoded position and
 *      nothing else.
 *   2. The sin² window drawn as a shape. Measured, at the default GRAINSIZE of 45 ms a
 *      grain is 4.0 px wide on a 2 s slot and 9.5 px on an 0.84 s one -- four pixels
 *      cannot show a shape, and two overlapping four-pixel shapes cannot show a sum.
 *
 * What fixed it was noticing that a grain's length IS a length: at 176 px across the
 * slot, GRAINSIZE 0..127 maps to 2..42 px of material (0.84 s slot) or 1..18 px (2 s
 * slot). So the width of a grain's mark is the grain length, and the spread of the
 * marks is SPRAY -- neither needs a symbol, a legend, or a decoded brightness. The
 * picture is a plan view of what the reader is doing.
 *
 * Width uses `inc`, not 1x: a grain at pitch consumes `length * rate` of material, which
 * is why the same GRAINSIZE covers more of the sample when you play higher. Drawing at
 * 1x would be a picture of a parameter instead of a picture of the read.
 *
 * The bars are stacked at four fixed heights rather than all on the axis, because grains
 * that overlap horizontally are common (SPRAY 0 puts them at the same x by definition)
 * and a merged bar would hide the count. Heights are fixed, not randomised: a mark that
 * moves for no reason looks like a fault.
 */
static uint16_t dim(uint16_t c, int32_t num, int32_t den)
{
    int32_t r = ((c >> 11) & 0x1Fu) * num / den;
    int32_t g = ((c >> 5) & 0x3Fu) * num / den;
    int32_t b = (c & 0x1Fu) * num / den;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void src_grain_map(const src_skin_t *k, const wv_sample *s, int32_t x0, int32_t y0,
                          int32_t w, int32_t h, const wv_grains *gr)
{
    int32_t mid = y0 + h / 2;
    int32_t i;
    /* four heights: the grain slots are four, so four lanes are enough for every grain
     * to have its own row and none can hide another */
    static const int8_t lane[WV_GRAINS] = { -7, 7, -3, 3 };

    if (!s || !s->data || !s->len) {
        cv_rect(x0, mid, w, 1, k->ink);
        return;
    }

    /* ── layer 1: the material, dim. It is the background, and it must be readable
     * enough to be a map and dark enough that a bar on top of it is unambiguous. Same
     * painter and same 2/5 brightness as SMPL: the complaint was that SMPL's envelope was
     * much brighter, which was true -- it was drawn in ink_hi at full strength. */
    env_line(s, x0, y0, w, h, 0, s->len, dim(k->ink, 2, 5));

    /* ── layer 2: the loop range. A grain appearing on the left after being on the
     * right is the loop and not a jump; without the marks that reads as a fault.
     * Dotted, in the dim ink: it is a boundary, not material.
     *
     * loop_end_k, not loop_end: the playhead actually wraps one grain window before
     * the configured end (see wv_grains_next), so that a grain spawned there still has
     * material to read. The marks show the configured range, because the material
     * between the wrap point and the end IS part of the loop -- grain tails read it --
     * and because a boundary that slid back and forth as GRAIN SIZE turned would be
     * showing an implementation detail, not a fact about the sample. */
    {
        float ls = gr ? gr->loop_start : 0.0f;
        float le = gr ? gr->loop_end_k : 0.0f;
        int32_t xa, xb;
        if (!gr || !gr->started) {          /* no cloud yet: the whole slot is the loop */
            ls = 0.0f;
            le = (float)(s->len - 1u);
        }
        if (le <= ls)
            le = (float)(s->len - 1u);
        xa = x0 + (int32_t)((float)w * ls / (float)s->len);
        xb = x0 + (int32_t)((float)w * le / (float)s->len);
        /* the loop is the whole slot until there is a way to set a range, so both marks
         * land on the band's own edges and are indistinguishable from its border. Nudged
         * one pixel in so they are visible at all: a mark you cannot see is a mark that
         * is not there. */
        if (xa <= x0)
            xa = x0 + 1;
        if (xb >= x0 + w - 1)
            xb = x0 + w - 2;
        for (i = 0; i < h; i += 3) {
            cv_rect(xa, y0 + i, 1, 1, dim(k->ink, 3, 5));
            cv_rect(xb, y0 + i, 1, 1, dim(k->ink, 3, 5));
        }
    }

    /* ── layer 3: the grains, and the playhead they are scattered around.
     *
     * Each bar: x is where that grain is reading right now, its width is GRAINSIZE at
     * this voice's pitch, and it slides left to right because the readhead does.
     *
     * Two things were wrong in the first version, and the player saw both before I did:
     *
     * (a) The lane was the DSP's grain SLOT index. Slots rotate (`next = (next+1) %
     *     WV_GRAINS`), so a grain changed lane on every respawn and the bars visibly
     *     jumped up and down -- which reasonably looked like SPRAY but was constant at
     *     SPRAY 0. Measured over eight frames, the upper bar's y went
     *     125, 125, 125, 121, 121, 121, 125, 125. The lane now follows DRAWING order
     *     (lowest position gets the upper lane), so a bar changes lane only when the two
     *     change order, i.e. when they cross.
     *
     * (b) SPRAY was only readable as the distance between the two bars, which at low
     *     settings is 17 px in a 176 px band -- easy to miss, and with nothing to measure
     *     it against it does not read as "scatter" at all, just as "two bars somewhere".
     *
     *     A dim playhead tick was added as a reference for that distance, and then
     *     removed on the player's judgement: a grain mark is a claim about reading, so it
     *     must not be drawn when nothing is read -- and by the same argument the playhead
     *     is not a thing you would look at either, since the interesting fact is always
     *     "where are the grains". SPRAY is read from the two bars' separation, and the
     *     mark that carries it is that SPRAY 0 puts them on top of each other.
     *
     * The width is constant -- `length * rate` -- because that is what GRAINSIZE sets. An
     * earlier attempt drew the *remaining* read range, which shrank as the window closed;
     * that made the width breathe with the grain phase, and the width is the one thing
     * here that must not move.
     */
    if (gr) {
        float per_px = (float)s->len / (float)w;
        int32_t order[WV_GRAINS];
        int32_t n = 0;

        /* drawing order by position: two grains are the normal case, and this keeps the
         * near one below the far one regardless of which slot they occupy */
        for (i = 0; i < (int32_t)WV_GRAINS; i++)
            if (gr->g[i].on) {
                int32_t q = n++;
                while (q > 0 && gr->g[order[q - 1]].pos > gr->g[i].pos) {
                    order[q] = order[q - 1];
                    q--;
                }
                order[q] = i;
            }

        for (i = 0; i < n; i++) {
            const wv_grain *g = &gr->g[order[i]];
            int32_t gx, gw, gy;

            /* the material a grain consumes over its whole life is length * rate; 1x here
             * would draw the knob instead of the read */
            gw = (int32_t)((1.0f / (g->dt > 0.0f ? g->dt : 1.0f)) * g->inc / per_px);
            if (gw < 2)
                gw = 2;                     /* a 1 px bar disappears into the material */

            /* past the edge: slide back at full width. A bar one px short of the edge is
             * still true -- a grain is reading there -- while a missing bar says nothing
             * is reading, which is false. */
            if (gw > w)
                gw = w;
            gx = x0 + (int32_t)(g->pos / per_px);
            if (gx + gw > x0 + w)
                gx = x0 + w - gw;
            if (gx < x0)
                gx = x0;

            gy = mid + lane[i] - 2;
            if (gy < y0)
                gy = y0;
            if (gy + 5 > y0 + h)
                gy = y0 + h - 5;
            cv_rect(gx, gy, gw, 5, k->ink_hi);
        }
    }
}

/* ── the four boundary colours ──────────────────────────────────────────────
 *
 * The EDIT page is the one screen where the source's single ink is not enough. Four
 * boundaries are four different values a hand has to tell apart at a glance, and the
 * SMPL theme's one green made them four identical lines -- the page worked and could
 * not be read. So this page alone breaks the theme: START / END / LOOP S / LOOP E each
 * get their own hue, and the knob that moves a boundary is printed in the same hue as
 * the line it moves. Colour is the only thing on this panel that can say "this knob and
 * that line are the same object" without words, and here that sentence is the whole
 * page.
 *
 * The hues are chosen to survive a 240 px panel in the green ground: a red that is not
 * the SHAPE red, a yellow that is not SYNTH's amber, a cyan and a violet. Each is
 * desaturated enough to sit beside the green without vibrating, and the live one is
 * brightened rather than recoloured, so the identity of a marker never changes when the
 * hand moves.
 */
#define C_ED_START   RGB(255, 96, 96)     /* START   -- red */
#define C_ED_END     RGB(255, 208, 96)    /* END     -- yellow */
#define C_ED_LOOPS   RGB(96, 224, 255)    /* LOOP S  -- cyan */
#define C_ED_LOOPE   RGB(198, 140, 255)   /* LOOP E  -- violet */

/* the same four, dimmed to 2/5 for a boundary that is not being turned */
static uint16_t ed_hue(int32_t which)
{
    switch (which) {
    case 0:  return C_ED_START;
    case 1:  return C_ED_END;
    case 2:  return C_ED_LOOPS;
    default: return C_ED_LOOPE;
    }
}

/* ── SMPL's EDIT picture: the region, the loop, and where the window is ───────
 *
 * The band has one job here that it does not have in the other two sources: it has
 * to show FOUR positions (start, end, loop start, loop end) and say which of them is
 * the one being turned. So it is drawn as a map with a visible window:
 *
 *   window      the part of the slot on screen, from `view` for `span` samples
 *   material    min/max per column, inside the window only
 *   outside     everything outside [start, end) is drawn at 1/5 ink: material that
 *               will not sound is not deleted, it is dimmed, because it is where the
 *               boundaries can be moved back to
 *   markers     four vertical lines: start/end solid, loop pair dotted
 *   live marker the one the last turn moved is full ink_hi; the others are 3/5
 *
 * The window follows the live marker, which is the answer to "zoom in and then I
 * cannot see where else to go": at 8x the visible span is 1/256 of the slot, and any
 * marker that is not the one you are turning would be off screen. Following the turn
 * means the marker you are moving is always on screen, and the others come back into
 * view the moment you turn zoom out or turn one of them.
 *
 * Why a window and not a scroll knob: the panel has seven encoders and the EDIT page
 * already spends six. Following the last turn costs nothing and needs no seventh.
 */
#define ED_INK_OUT  1                       /* the dim/5 of material that will not sound */
#define ED_INK_MARK 3

static void src_edit_map(const src_skin_t *k, const wv_sample *s,
                         int32_t x0, int32_t y0, int32_t w, int32_t h,
                         const smp_region *rg, uint8_t zoom, uint8_t live)
{
    int32_t mid = y0 + h / 2;
    uint32_t span, view, i;
    float    head = 0.0f;               /* the sounding voice's position, in samples */
    uint8_t  head_live = 0;             /* 0: nothing on this slot is sounding */
    int32_t  head_x = -1;               /* the column it lands in, -1: not in the window */
    int32_t  head_y = mid;

    if (!s || !s->data || !s->len) {
        cv_rect(x0, mid, w, 1, k->ink);
        return;
    }

    span = s->len >> zoom;
    if (span < 8u)
        span = 8u;                          /* fewer than 8 samples cannot be a picture */
    if (span > s->len)
        span = s->len;

    /* the window's left edge: centred on the live marker, clamped inside the slot */
    {
        const uint32_t centre = (live == 0) ? rg->start : (live == 1) ? rg->end
                              : (live == 2) ? rg->loop_s : (live == 3) ? rg->loop_e
                              : 0u;         /* 4 = no marker yet: the slot's start */
        view = centre > span / 2u ? centre - span / 2u : 0u;
        if (view + span > s->len)
            view = s->len > span ? s->len - span : 0u;
    }

    /* Where the voice is reading, if a sample voice on this slot is sounding. A
     * playhead is the only thing on this page that shows the loop as a fact rather
     * than a pair of marks: two dots say "a loop lives here", the playhead bouncing
     * between them says it is running.
     *
     * The newest active voice playing THIS slot, read straight out of the audio state
     * the way the GRAN picture reads its grains. No lock: the audio thread advances
     * `pos` and the UI reads it, and a float read is atomic on this core pair, so the
     * worst case is one frame drawn from a position that moved. */
    {
        uint32_t newest = 0;
        uint8_t  found = 0;
        for (i = 0; i < WV_VOICES; i++) {
            /* SAMPLE or SLICE, not just SAMPLE: the engine names a one-shot a SLICE
             * and a held note a SAMPLE (`v->src = loop ? WV_SRC_SAMPLE : WV_SRC_SLICE`),
             * so matching only SAMPLE draws no playhead until the loop is switched on
             * -- which is exactly the state a player is in when they are looking for
             * one. Both are "a voice walking this material". */
            if (dsp.v[i].active && dsp.v[i].smp == s
                && (dsp.v[i].src == WV_SRC_SAMPLE || dsp.v[i].src == WV_SRC_SLICE)
                && dsp.v[i].age >= newest) {
                newest = dsp.v[i].age;
                head = dsp.v[i].rd.pos;
                found = 1;
            }
        }
        head_live = found;
    }

    /* the material, per column, from the window only */
    for (i = 0; i < (uint32_t)w; i++) {
        uint32_t a = view + (uint32_t)((uint64_t)span * i / (uint32_t)w);
        uint32_t b = view + (uint32_t)((uint64_t)span * (i + 1u) / (uint32_t)w);
        float lo = 0.0f, hi = 0.0f, pk = 0.0f, sc;
        uint32_t j;
        int32_t yh, yl;
        uint8_t inside;

        if (b <= a)
            b = a + 1u;
        for (j = a; j < b && j < s->len; j++) {
            float v = (float)s->data[j] * (1.0f / 32768.0f);
            if (v < lo) lo = v;
            if (v > hi) hi = v;
            if (v < 0.0f) v = -v;
            if (v > pk) pk = v;
        }
        /* One scale for the window, from its own peak: zooming in must not make the
         * material look louder. Clamped so a nearly silent window does not fill the
         * band with its own noise. */
        if (pk < 0.02f)
            pk = 1.0f;
        sc = (float)(h / 2 - 1) / pk;
        yh = mid - (int32_t)(hi * sc);
        yl = mid - (int32_t)(lo * sc);
        if (yh < y0) yh = y0;
        if (yl > y0 + h - 1) yl = y0 + h - 1;
        if (yl < yh) yl = yh;

        /* Inside the region: 2/5 ink, the same weight SMPL and GRAN use for material.
         * Outside it: 1/5. The two weights are not decoration -- with the region at its
         * default (the whole slot) the first version drew everything at 2/5 and the two
         * states were indistinguishable, which was measured as 14,117 px at 2/5 against
         * 169 px at 1/5 in the whole band. Turning START now has to be visible as the
         * material losing weight, so the difference has to be there to see. A column is
         * "inside" as soon as any of it is. */
        inside = !(b <= rg->start || a >= rg->end);
        cv_rect(x0 + (int32_t)i, yh, 1, yl - yh + 1,
                inside ? dim(k->ink, 2, 5) : dim(k->ink, ED_INK_OUT, 5));

        /* The playhead's column, caught from the same span this column was drawn
         * with. Deriving the column from `a` instead would quantise it to a column
         * edge and make the head jump in steps of one pixel; the fractional part
         * below puts it inside the column it is really in. */
        if (head_live && head >= (float)a && head < (float)b) {
            float f = (float)(head - (float)a) / (float)(b - a);
            head_x = x0 + (int32_t)i;
            /* its height inside the column, from the same min/max the column drew */
            head_y = mid - (int32_t)((hi + (lo - hi) * f) * sc);
            if (head_y < y0) head_y = y0;
            if (head_y > y0 + h - 1) head_y = y0 + h - 1;
        }
    }

    /* the four markers, in the window's coordinates. A marker outside the window is
     * clamped to the edge it left by, at 1/5 ink, so an edit never becomes invisible
     * evidence that it happened. */
    {
        struct { uint32_t at; uint8_t dotted; uint8_t live_id; } m[4];
        int32_t n;
        m[0].at = rg->start;  m[0].dotted = 0; m[0].live_id = 0;
        m[1].at = rg->end;    m[1].dotted = 0; m[1].live_id = 1;
        m[2].at = rg->loop_s; m[2].dotted = 1; m[2].live_id = 2;
        m[3].at = rg->loop_e; m[3].dotted = 1; m[3].live_id = 3;
        /* the loop pair is only a fact when the loop is on: drawn at 1/5 otherwise,
         * because an off loop does not describe the sound */
        for (n = 0; n < 4; n++) {
            uint16_t c;
            int32_t x;
            int32_t i2;

            if (m[n].at <= view)
                x = x0;
            else if (m[n].at >= view + span)
                x = x0 + w - 1;
            else
                x = x0 + (int32_t)((uint64_t)(m[n].at - view) * (uint64_t)w / (uint64_t)span);

            /* Each boundary keeps its own hue whether or not it is the live one; only
             * its brightness changes. Recolouring the live marker instead would mean a
             * line changes identity as the hand moves, and the page's whole job is
             * "which line am I moving". */
            if (m[n].dotted && !rg->on) {
                /* an off loop is not a fact about the sound, so it drops to 1/5 of its
                 * own hue rather than to the theme's ink */
                c = dim(ed_hue((int32_t)n), ED_INK_OUT, 5);
            } else if (m[n].live_id == live) {
                c = ed_hue((int32_t)n);
            } else {
                c = dim(ed_hue((int32_t)n), ED_INK_MARK, 5);
            }

            /* Two pixels wide and, for the loop pair, a dot every two rows. A 1 px
             * dashed line over an 80 px band was measured at nine visible pixels for
             * the whole loop range: at 240 px on a 1x window the markers were there
             * and could not be seen, which is the same as not having them. Two pixels
             * is still a boundary rather than a feature, and a dot every second row
             * reads as a dashed line at arm's length. */
            for (i2 = 0; i2 < h; i2 += (m[n].dotted ? 2 : 1)) {
                cv_rect(x, y0 + i2, 2, 1, c);
                if (x + 2 < x0 + w)
                    cv_rect(x + 1, y0 + i2, 1, 1, c);
            }
        }
    }

    /* The playhead last, on top of both the material and the markers, because it is
     * the thing that moves and the one thing on this page that is not an edit.
     *
     * It must not read as a fifth boundary: the four markers are full-height 2 px
     * lines, so the playhead is a 5 px tab at the top of the band plus a 3x3 dot at
     * the material's own height. A tab and a dot are a cursor; a line is an edge.
     * `ink_hi` is used by the live marker too (the one being turned), and that is
     * deliberate -- both are "where you are", and they are told apart by shape.
     *
     * Nothing is drawn when no sample voice on this slot is sounding: a playhead
     * parked at 0 would claim the material is playing from the start, which is the
     * same mistake the GRAN picture avoids by not drawing a playhead at all when the
     * cloud is silent. */
    if (head_live && head_x >= x0 && head_x < x0 + w) {
        /* White, and the only white on the screen: the four hues belong to the four
         * boundaries, so the one thing that is not an edit cannot borrow any of them. */
        cv_rect(head_x, y0, 2, 5, C_WHITE);
        if (head_x + 2 <= x0 + w)
            cv_rect(head_x + 1, y0, 1, 5, C_WHITE);
        cv_rect(head_x, head_y, 2, 2, C_WHITE);
        if (head_x + 2 <= x0 + w)
            cv_rect(head_x + 1, head_y, 1, 2, C_WHITE);
    }
}


static void src_shape(const src_skin_t *k, int32_t x0, int32_t y0, int32_t w, int32_t h)
{
    int32_t i, py = 0, mid = y0 + h / 2, amp = h / 2 - 1;
    float mf = (float)k_morph * ((float)(WV_FRAMES - 1u) / 127.0f);
    float pk = 0.0f;

    /* Normalise by the frame's own peak, the same decision the scope's auto-gain makes
     * and for the same reason: the question is "what shape is this", not "how loud".
     * It is not cosmetic here -- measured, the SAW SQR bank's frames fall from 32767 to
     * 19892 across MORPH (the banks are peak-normalised per frame, and a triangle at the
     * same peak has less area than a saw), so drawing at a fixed scale made the last
     * frames look almost flat while they are in fact a clean triangle. Normalising lets
     * every frame fill the band and the shape stays readable end to end. */
    for (i = 0; i < w; i++) {
        float v = wv_table_read(&bank, (float)i / (float)w, mf);
        if (v < 0.0f)
            v = -v;
        if (v > pk)
            pk = v;
    }
    if (pk < 0.05f)
        pk = 1.0f;                      /* an empty table: draw it flat, not amplified */
    else
        pk = 1.0f / pk;

    for (i = 0; i < w; i++) {
        float v = wv_table_read(&bank, (float)i / (float)w, mf) * pk;
        int32_t y = mid - (int32_t)(v * (float)amp);
        if (y < y0)
            y = y0;
        if (y > y0 + h - 1)
            y = y0 + h - 1;
        if (i == 0)
            cv_rect(x0, y, 2, 2, k->ink_hi);
        else
            cv_line(x0 + i - 1, py, x0 + i, y, k->ink_hi);
        py = y;
    }
}

static void src_rack(const src_skin_t *k, int32_t x0, int32_t y0, int32_t w, int32_t h);

/* ── the eight frames, stacked ───────────────────────────────────────────────
 *
 * The SYNTH picture has always drawn the one frame MORPH is on. That answers "what does
 * this sound like now" and nothing about the bank: the eight frames are the material the
 * MORPH knob travels through, and this page's whole job is turning them against each
 * other -- which is invisible if only the current one is drawn.
 *
 * So on this page the picture becomes the RACK: all eight frames on a sheared axis.
 *
 * TWO FRAMES LIGHT, NOT ONE. MORPH does not choose a frame, it crossfades two: at
 * mf = 3.4 the voice adds frame 3 and frame 4 at 60/40. So the rack lights exactly the
 * pair that is sounding, at brightnesses in that ratio -- a single bright frame would be
 * a lie about what the reader is doing, and the pair is also the answer to the question
 * this page exists for ("which two waves am I turning against each other").
 *
 * It is a STATIC highlight: morph is one number shared by all voices (wv_dsp.h:81, and
 * app.c is its only writer), not a modulation target, so the lit pair changes when the
 * MORPH knob moves and at no other time. Drawing it per voice would draw the same pair
 * every frame. When morph becomes modulatable this is where a per-voice marker goes.
 *
 * The frame being EDITED (edit_frame) is a separate mark and must stay legible next to
 * the pair: it keeps the lift plus a bright index, and if it is one of the two sounding
 * frames it simply gets the louder of the two brightnesses.
 *
 * COST, stated: 8 polylines instead of 1 -- about 6x the drawing on this band, at 60 fps.
 * It only runs on this page, which is the page you are on to look rather than to play.
 */
static void src_rack(const src_skin_t *k, int32_t x0, int32_t y0, int32_t w, int32_t h)
{
    const int32_t fw   = w - 7 * STK_DX;                          /* the near frame is widest */
    const int32_t amp  = (h - 2 - 7 * STK_DY - RACK_LIFT) / 2;    /* 14 px at 172 x 80 */
    const int32_t base0 = y0 + h - 1 - amp;                       /* f0's zero line */

    /* the pair the voices are mixing right now -- the same arithmetic wv_frames_at
     * does, so the picture and the reader cannot disagree */
    float mf = (float)k_morph * ((float)(WV_FRAMES - 1u) / 127.0f);
    int32_t m0, m1;
    if (mf < 0.0f) mf = 0.0f;
    if (mf > (float)(WV_FRAMES - 1u)) mf = (float)(WV_FRAMES - 1u);
    m0 = (int32_t)mf;
    m1 = (m0 + 1 < (int32_t)WV_FRAMES) ? m0 + 1 : m0;

    int32_t pass, px, py;

    /* Far frame first, so nearer ones overlap it -- that is the order. The edited frame is
     * drawn LAST whatever its position, or a nearer frame paints over the lift and the
     * highlight becomes a broken rectangle. (Measured before the fix: 116 px of a 664 px
     * border, i.e. the bottom two thirds of the raised card were covered.) */
    for (pass = 0; pass <= (int32_t)WV_FRAMES; pass++) {
        const int32_t sel = (pass == (int32_t)WV_FRAMES);
        const int32_t f   = sel ? (int32_t)edit_frame : pass;
        int32_t x, base, lit;
        uint16_t ink;

        if (!sel && pass == (int32_t)edit_frame)
            continue;                                  /* it gets the last pass */

        x    = x0 + f * STK_DX;
        base = base0 - f * STK_DY - (sel ? RACK_LIFT : 0);

        /* Is this frame one of the two the voices are mixing? `lit` is 0 for "not
         * sounding", else 1 or 2 for the near/far member of the pair. */
        /* 2 = the louder member of the pair (which is m0 while a crossfade is in
         * progress, and both of them when MORPH sits exactly on a frame), 0 = silent. */
        lit = (f == m0) ? 2 : ((f == m1) ? 1 : 0);

        /* ── TWO DIFFERENT QUESTIONS, TWO DIFFERENT MARKS ────────────────────────
         *
         * MORPH's pair answers "what am I hearing" and lives in the amber family: the
         * louder member at ink_hi, the other at 8/12. K1's frame answers "what am I
         * pointing at" and is WHITE (k->acc), with the lift on top of it.
         *
         * Colour is the right axis for this and brightness is not. Brightness is already
         * spoken for twice -- it carries depth ((4+f)/16, far frames darker) and it
         * carries "this one is sounding" -- so a third meaning in the same channel would
         * be three levels of the same colour and the player would have to count them.
         * White is the platform's own accent for exactly this: "what is being touched /
         * where we are" (gfx.c:29), and it is already the colour the panel uses for a
         * knob under the hand. Nothing else in this band is white.
         *
         * The two can coincide: when K1's frame is one of the pair, the frame is white
         * and lifted, and its own index label carries the state.
         */
        if (lit == 2)
            ink = k->ink_hi;
        else if (lit == 1)
            ink = dim(k->ink_hi, 8, 12);
        else
            ink = dim(k->ink, 4 + f, 16);

        if (sel)
            ink = k->acc;                   /* K1's frame: white, whatever MORPH says */

        py = 0;
        for (px = 0; px < fw; px++) {
            /* through the same reader the voices use, at morph = this frame, so what is
             * drawn IS what sounds, offsets and all */
            float v = wv_table_read(&bank, (float)px / (float)fw, (float)f);
            int32_t y = base - (int32_t)(v * (float)amp);
            if (y < y0) y = y0;
            if (y > y0 + h - 1) y = y0 + h - 1;
            if (px == 0)
                cv_rect(x, y, 2, 2, ink);
            else
                cv_line(x + px - 1, py, x + px, y, ink);
            py = y;
        }

    }
}

/* A knob in the theme's own ink, which is what every page but EDIT wants. */
static void knob(int32_t x, int32_t y, int32_t w, const char *name, const char *val,
                 int32_t v127, int lit);

/* The same, in a chosen colour. Only the EDIT page uses it: there the four boundary
 * knobs are the four lines in the picture, and a knob printed in its line's hue is how
 * the panel says which line it moves. `hue` 0 means "the theme's ink". */
/* -------------------------------------------------------------- knob art --- */

/* A knob is an ARC and a pointer, not a bar, and the reason is the panel's own problem:
 * four knobs under a screen are read from a metre away, and a bar answers "how much" but
 * not "how far round". An arc with a pointer reads the way the encoder under the hand
 * turns -- the pointer's angle IS the value -- so a glance at the row says where each
 * knob sits in its range without reading a single number.
 *
 * Geometry, all of it fixed so the four knobs are one instrument and not four widgets:
 *
 *   the arc spans 270 degrees, from 135 deg below-left round the top to 135 deg
 *   below-right, so the gap sits at the bottom where the value's text is. A full circle
 *   was the first attempt: it hides WHERE zero is, and the bottom gap is what makes the
 *   ends of the range readable as ends.
 *
 *   the sweep starts at the lower left and runs CLOCKWISE, which is the direction the
 *   hardware encoder turns for more (measured: turn K1 +1 moves a value up). A knob that
 *   fills anticlockwise would be a second convention in the same panel.
 *
 *   the track is drawn at 1/5 ink and the filled part at full ink or the page's hue, the
 *   same two brightnesses the bar used, so "off" still reads as a scale and not as empty
 *   space.
 *
 * Radius 11 inside a 26 px box: the box is what the four columns can afford (54 px wide,
 * 18 px tall under the value) and 11 leaves a 3 px margin either side so two adjacent
 * arcs never touch. The knob is 22 px across and the pointer is 3 px, which is the
 * smallest pointer that survives one pixel of RGB565 rounding at both ends.
 *
 * Drawn with cv_line segments rather than a real arc primitive (platform/ has none, and
 * this file may not add one): 32 segments over 270 deg is 8.4 deg each, and at r=11 that
 * is a 1.6 px step, so the arc reads smooth at this size. */
/* The band was 64 px tall for a reason, and the small arc was not using it. The value goes
 * INSIDE the ring now, which frees the two text rows under the name and lets the ring have
 * the rest of the band: R = 23 instead of 11, four times the area.
 *
 *      name                     y + 0    (FONT_XS, 15 px tall)
 *      ring                     y + 17 .. y + 63
 *      value, centred inside    the ring's middle row
 *
 * R = 23 is the measured maximum, not a round choice: the band is 64, the name takes 15,
 * two pixels of air make 17, and 64 - 17 = 47 gives (47 - 1) / 2 = 23 so the ring's stroke
 * lands on rows 17..63 exactly. A 23 px radius inside a 54 px column leaves 4 px either
 * side, so two adjacent rings never touch.
 *
 * The value has to fit the hole, and the hole is not the inner diameter: FONT_XS is 15 px
 * tall, so text is a chord at +-7.5 from the middle, where the ring is narrower. With
 * R - 3 inner radius that chord is 39 px, against the widest value string in the app at
 * 32 px ("0.001s"). Measured, not eyeballed, because "-Dw" is the one thing this layout
 * cannot do. */
#define KNOB_ARC_R     23
#define KNOB_ARC_BOX   (2 * KNOB_ARC_R + 1)       /* 47: ring plus its own stroke */
#define KNOB_ARC_TOP   (KNOB_TOP + 17)            /* 21 on the band, 197 on screen */
#define KNOB_ARC_SEGS  32
/* FIVE pixels of stroke. It was two, which at R=23 read as a thin hairline: the ring is
 * 145 px of circumference and 2 px of it is 1.4 % of the visible arc, so the value was
 * carried almost entirely by the tick. Three was tried on paper and five was asked for.
 *
 * The cost is the hole, and the hole is measured against the widest value string rather
 * than guessed: stroke 5 puts the innermost lit radius at 19, the hole's clear radius at
 * 18, and FONT_XS's glyphs occupy 9 rows (measured: ink on y = -4..+4 around the middle,
 * not the font's full 15 px cell). The chord at +-4.5 is 34.9 px, against 33 px for the
 * widest string the app can print ("12.810s"). Four pixels of margin, and thickness 6
 * would be 32.8 -- one pixel short of the text. This is the thickest stroke that fits. */
#define KNOB_ARC_THICK 5                          /* px: radii 23, 22, 21, 20, 19 */
#define KNOB_ARC_SPAN  (0.75f * FM_TWO_PI)        /* 270 deg: three quarters of a turn */

/* ── the ring's trigonometry is a table ──────────────────────────────────────
 *
 * The arc's angles are compile-time constants: 32 segments of a fixed 270 degrees at a
 * fixed radius, drawn every frame on a page that redraws at the UI rate. Computing that
 * with fm_sinf/fm_cosf cost 260 calls a frame for four knobs (32 segments x 2 radii x 2
 * ends x 2 functions, plus the pointer), which is 4 x 13 multiply-adds of a degree-13
 * minimax polynomial to produce the same coordinates forever.
 *
 * 129 entries of int8 sin and cos, quarter of a pixel at R = 23 (measured worst error
 * 0.086 px), 258 B of flash. The table is indexed by SEGMENT, not by angle: segment k
 * starts at table[k] and ends at table[k+1], so the compiler can fold the two adjacent
 * segments' shared point into one lookup and no interpolation is needed anywhere.
 *
 * fm_tanf is unaffected -- it is sin(x)/sin(pi/2-|x|), not a series, and it is used by
 * the filter, where a table would have to interpolate to stay quiet. */
#define KNOB_TRIG_N    128
static const int8_t knob_sin[KNOB_TRIG_N + 1] = {
     -90,  -93,  -96,  -99, -102, -105, -107, -110, -112, -114, -116, -118, -120, -121, -122, -124,
    -125, -125, -126, -127, -127, -127, -127, -127, -126, -126, -125, -124, -123, -122, -121, -119,
    -117, -115, -113, -111, -109, -106, -104, -101,  -98,  -95,  -92,  -89,  -85,  -82,  -78,  -74,
     -71,  -67,  -63,  -58,  -54,  -50,  -46,  -41,  -37,  -32,  -28,  -23,  -19,  -14,   -9,   -5,
       0,    5,    9,   14,   19,   23,   28,   32,   37,   41,   46,   50,   54,   58,   63,   67,
      71,   74,   78,   82,   85,   89,   92,   95,   98,  101,  104,  106,  109,  111,  113,  115,
     117,  119,  121,  122,  123,  124,  125,  126,  126,  127,  127,  127,  127,  127,  126,  125,
     125,  124,  122,  121,  120,  118,  116,  114,  112,  110,  107,  105,  102,   99,   96,   93,
      90,
};

static const int8_t knob_cos[KNOB_TRIG_N + 1] = {
     -90,  -86,  -83,  -79,  -76,  -72,  -68,  -64,  -60,  -56,  -51,  -47,  -43,  -38,  -34,  -29,
     -25,  -20,  -16,  -11,   -6,   -2,    3,    8,   12,   17,   22,   26,   31,   35,   40,   44,
      49,   53,   57,   61,   65,   69,   73,   77,   81,   84,   88,   91,   94,   97,  100,  103,
     106,  108,  111,  113,  115,  117,  118,  120,  122,  123,  124,  125,  126,  126,  127,  127,
     127,  127,  127,  126,  126,  125,  124,  123,  122,  120,  118,  117,  115,  113,  111,  108,
     106,  103,  100,   97,   94,   91,   88,   84,   81,   77,   73,   69,   65,   61,   57,   53,
      49,   44,   40,   35,   31,   26,   22,   17,   12,    8,    3,   -2,   -6,  -11,  -16,  -20,
     -25,  -29,  -34,  -38,  -43,  -47,  -51,  -56,  -60,  -64,  -68,  -72,  -76,  -79,  -83,  -86,
     -90,
};
static int32_t knob_cx(int32_t v, int32_t k) { return (int32_t)knob_cos[k] * v / 127; }
static int32_t knob_sy(int32_t v, int32_t k) { return (int32_t)knob_sin[k] * v / 127; }

/* x where the edge (x0,y0)-(x1,y1) crosses row y, rounded to nearest. The caller skips
 * horizontal edges, so y1 != y0 always. */
static int32_t knob_edge_x(int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t y)
{
    int32_t num = (y - y0) * (x1 - x0), den = y1 - y0;
    if (den < 0) { den = -den; num = -num; }
    if (den == 0) return x0;                 /* horizontal: either end will do */
    return x0 + (num >= 0 ? (num + den / 2) / den : (num - den / 2) / den);
}

/* One segment of the ring, FILLED between the inner and the outer radius.
 *
 * The first version traced KNOB_ARC_THICK separate 1 px Bresenham chains, one per radius,
 * and that is what left the pinholes: measured on the host, a finished ring enclosed 6
 * background pixels at r = 19.8 .. 21.9 -- between two of those chains, not in the bottom
 * gap and not at a segment join. Chains 1 px apart do not tile a 5 px band, because each
 * one rounds its own way and two neighbours can straddle a pixel. Two chains (the original
 * 2 px stroke) almost always end up touching, which is why no hole was visible until the
 * stroke got thick. It is not the trigonometry table: the table is exact at the 128
 * segment ends, and the old sine version would have holed the same way at this width.
 *
 * So the stroke is covered the way a rasteriser covers a shape: the segment is the quad
 * between (inner[k], outer[k]) and (inner[k+1], outer[k+1]), and that quad is filled row
 * by row. Consecutive segments share an edge, so there is no seam left to hole. */
static void knob_arc_seg(int32_t cx, int32_t cy, int32_t k0, int32_t k1, uint16_t c)
{
    const int32_t ro = KNOB_ARC_R, ri = KNOB_ARC_R - KNOB_ARC_THICK + 1;
    const int32_t ax = cx + knob_sy(ri, k0), ay = cy - knob_cx(ri, k0);   /* inner, k0 */
    const int32_t bx = cx + knob_sy(ro, k0), by = cy - knob_cx(ro, k0);   /* outer, k0 */
    const int32_t dx = cx + knob_sy(ro, k1), dy = cy - knob_cx(ro, k1);   /* outer, k1 */
    const int32_t ex = cx + knob_sy(ri, k1), ey = cy - knob_cx(ri, k1);   /* inner, k1 */
    int32_t y, y0 = ay, y1 = ay;

    if (by < y0) y0 = by;
    if (dy < y0) y0 = dy;
    if (ey < y0) y0 = ey;
    if (by > y1) y1 = by;
    if (dy > y1) y1 = dy;
    if (ey > y1) y1 = ey;

    for (y = y0; y <= y1; y++) {
        int32_t lo = 0x7fffffff, hi = -0x7fffffff, x;
        /* the four edges, each one only where it actually spans this row */
        /* CLOSED at both ends. The first version used the usual half-open interval
         * (min <= y < max), which is right for a general polygon but wrong here: at the
         * 12 o'clock vertex every edge of the quad is horizontal or ends exactly on that
         * row, so no edge claimed it and the top of the ring kept a 1 px background dot
         * at every value (measured: `FFFFFFFFFF.FFFFFFFFFF` on row 197 of the host shot).
         * Painting a row twice costs one extra compare; painting it never is a hole. */
        if (ay <= y && y <= ey) { x = knob_edge_x(ax, ay, ex, ey, y); if (x < lo) lo = x; if (x > hi) hi = x; }
        if (ey <= y && y <= dy) { x = knob_edge_x(ex, ey, dx, dy, y); if (x < lo) lo = x; if (x > hi) hi = x; }
        if (dy <= y && y <= by) { x = knob_edge_x(dx, dy, bx, by, y); if (x < lo) lo = x; if (x > hi) hi = x; }
        if (by <= y && y <= ay) { x = knob_edge_x(bx, by, ax, ay, y); if (x < lo) lo = x; if (x > hi) hi = x; }
        /* and the reversed windings, for the segments either side of 0 degrees */
        if (ey <= y && y <= ay) { x = knob_edge_x(ex, ey, ax, ay, y); if (x < lo) lo = x; if (x > hi) hi = x; }
        if (dy <= y && y <= ey) { x = knob_edge_x(dx, dy, ex, ey, y); if (x < lo) lo = x; if (x > hi) hi = x; }
        if (by <= y && y <= dy) { x = knob_edge_x(bx, by, dx, dy, y); if (x < lo) lo = x; if (x > hi) hi = x; }
        if (ay <= y && y <= by) { x = knob_edge_x(ax, ay, bx, by, y); if (x < lo) lo = x; if (x > hi) hi = x; }
        if (lo <= hi)
            cv_rect(lo, y, hi - lo + 1, 1, c);
    }
}

static void knob_arc(int32_t cx, int32_t cy, int32_t v, uint16_t c, uint16_t track)
{
    /* Which segment the fill ends on, in table units: 0..KNOB_TRIG_N. Integer arithmetic
     * all the way, because the answer only ever needs to pick a segment and a float
     * comparison against a segment boundary is how the fill ended up 82, 83, 83, 83 in an
     * earlier draft. */
    const int32_t fill_end = (v < 0 ? 0 : v > 127 ? 127 : v) * KNOB_TRIG_N / 127;
    int32_t k;

    for (k = 0; k < KNOB_TRIG_N; k++) {
        knob_arc_seg(cx, cy, k, k + 1, track);
        if (k < fill_end)
            knob_arc_seg(cx, cy, k, k + 1, c);
    }

    /* The pointer: a short tick ACROSS the arc at the value's angle, not a hand from the
     * centre. A hand was the first version and the measurement that condemned it is that
     * it runs through the 90-degree gap at the bottom, so at v=0 it fills part of the gap
     * and the control reads as a nearly closed ring -- the one place the "where does the
     * range end" information lives. A tick outward from the arc keeps the gap empty at
     * every value, and it also survives a much smaller radius than a hand does.
     *
     * It straddles the stroke rather than sitting just outside it: at R = 23 a tick that
     * only stuck out would be four pixels of a 145-pixel ring, and the pointer's job is to
     * be findable before it is to be tidy. */
    {
        int32_t r0 = KNOB_ARC_R - KNOB_ARC_THICK;
        int32_t r1 = KNOB_ARC_R + 2;
        cv_line(cx + knob_sy(r0, fill_end), cy - knob_cx(r0, fill_end),
                cx + knob_sy(r1, fill_end), cy - knob_cx(r1, fill_end), c);
    }
}

static void knob_c(int32_t x, int32_t y, int32_t w, const char *name, const char *val,
                   int32_t v127, int lit, uint16_t hue)
{
    uint16_t c = hue ? hue : (lit ? sk.ink_hi : sk.ink);
    uint16_t cv = hue ? hue : (lit ? sk.acc : sk.ink);
    int32_t cx = x + w / 2;
    int32_t cy = KNOB_ARC_TOP + KNOB_ARC_R;   /* 21 + 23 = 44 on the band */

    /* the name on top, then the ring, then the value INSIDE the ring.
     *
     * The value used to sit under the name and above a bar, then under the name above a
     * small arc. Inside the ring is the layout the 64 px band was always tall enough for:
     * the ring gets 47 of the 64 px and the value needs no row of its own, so the ring is
     * four times the area it had. It is centred in the hole, which is where a knob's number
     * belongs -- at the knob's middle, not at its left edge, or four values of different
     * lengths read as four left-aligned strings instead of four objects. */
    cv_text(x, y, &FONT_XS, name, c);
    knob_arc(cx, cy, v127, hue ? hue : (lit ? sk.ink_hi : sk.ink), dim(sk.ink, 1, 5));
    cv_text(cx - txt_w(val) / 2, cy - FONT_XS.h / 2, &FONT_XS, val, cv);
}

static void knob(int32_t x, int32_t y, int32_t w, const char *name, const char *val,
                 int32_t v127, int lit)
{
    knob_c(x, y, w, name, val, v127, lit, 0);
}


/* ---------------------------------------------------------------- scope --- */

/* The vertical scale is FIXED and LINEAR in amplitude: full scale (0 dBFS) is the
 * edge of the band, and a sample at 0.256 of full scale sits a quarter of the way out.
 *
 * This was a per-frame peak normaliser, and the measurement that condemned it is that
 * velocity 20, 60 and 127 all drew 63 px: a scope that auto-gains carries no level at
 * all. But the replacement cannot be a dB axis either, and that is the trap: the
 * instantaneous value of a waveform crosses zero twice a cycle, and in dB a zero
 * crossing is -infinity. A dB scale on a trace plunges to the floor at every crossing
 * and the picture becomes a filled band -- tried, measured, 47 px of ink in every one
 * of the 240 columns.
 *
 * So: linear, fixed, and referenced to full scale. Then amplitude IS level, the shape
 * survives, and the numbers are the DSP's own -- a single note at velocity 127 peaks
 * at 0.256 of full scale (measured), so it draws 12 px each side of the centre in a
 * 92 px band, and velocity 32 draws 3. The eight-voice headroom is above full scale,
 * so a full chord runs into the edge of the band, which is exactly what a scope does
 * when the signal exceeds the screen.
 *
 * Pre-master on purpose: MASTER is the listening level, and a scope that shrank when
 * the pot was turned down would be measuring the wrong thing. What this answers is
 * what the instrument produces -- velocity, envelope, polyphony, shaper, filter. */
/* SCOPE_FS is what reaches the band edge, and it is NOT 1.0 any more.
 *
 * It was 1.0 (full scale = 0 dBFS at the edge), which is the honest mapping and which
 * drew a single note 2 px from the centre: the voice sum is scaled by WV_VOICE_GAIN
 * (1/sqrt 8, the headroom eight voices need) and taken BEFORE MASTER, so one note
 * peaks at 0.21 of full scale. The user's report was "the scope amplitude is too
 * small, you can barely see the waveform", and a 2 px trace is that report.
 *
 * Raising it to a constant is the wrong fix (the trace would clip on a chord); the
 * reference has to be the loudest thing the instrument actually produces. Measured on
 * the device path, pre-master peak by polyphony at velocity 127: 1 voice 0.210,
 * 2  0.232, 4 0.444, 8 0.487. So 0.5 is "eight voices, wide open" and it is what the
 * edge means now: the 0 dBFS line stays the line a full chord reaches, and a single
 * note draws 17 px each side instead of 2.
 *
 * Consequence, and it is deliberate: the graticule's dB labels are now relative to
 * that reference, so -6 dB on screen is a chord at half the eight-voice peak. Nothing
 * plays past this, so nothing clips except a 9th voice or a louder master, and MASTER
 * still does not move the trace (it is taken before). */
#define SCOPE_REF  0.5f
#define SCOPE_FS   ((float)SCOPE_HALF / SCOPE_REF)   /* the reference reaches the band edge */

/* the graticule: a scope is not a plot, it has a screen. The horizontal lines mark
 * fixed dB values on an amplitude-linear axis (0 dBFS at the band edge, then every
 * 10 dB down to -50), the vertical ones are quarters of the window. They are a neutral
 * grey, not a skin colour, because they must sit under all three skins -- a graticule
 * tinted by the source would be content, and a graticule is the glass. */
/* Two levels, and both had to be raised: the dim one was RGB(36,38,42), only 7 units of
 * luminance above the darkest ground -- measured, effectively invisible, so the vertical
 * lines and five of the six dB markers did not read as a grid at all. A graticule that
 * cannot be seen is not a subtle graticule, it is a missing one.
 *
 * The two levels stay well under the ink (luminance 150-185 depending on the skin) so a
 * bright trace still owns the screen: the grid is glass, and glass that competes with the
 * signal is a defect. GRID marks 0 dBFS, the one reference a full-scale chord crosses. */
#define GRID_DIM  RGB(58, 61, 66)     /* was 36,38,42: dLum 7 -> 29 against every ground */
#define GRID     RGB(96, 99, 104)     /* was 62,64,68: the 0 dBFS line, dLum 67 */

/* amplitude as a fraction of full scale (-1..1) to a distance from the centre line */
static float fs_px(float a)
{
    float f = SCOPE_FS;
    float v = a * f;
    if (v > f)
        v = f;
    if (v < -f)
        v = -f;
    return v;
}

/* ---------------------------------------------------------------- debug --- */

/* The debug corner: what the instrument is doing, three short rows at the scope's top
 * left. Compiled in only with DBG_ON (fm1.toml dbg = true -> -DDBG_ON), because a
 * release's screen should be the instrument, not its instrumentation.
 *
 * Row 1: the audio render's load, in % of its budget, and the xruns since boot. This is
 * plat_cpu_pct(), the platform's own reading, smoothed over ~16 blocks: it times the
 * interrupt, which is the only place a late block can come from.
 *
 * Row 2: the second core. "c2 --" one core (never started, or retired and the fallback
 * took over), "c2 ok" on and idle, "c2 wt" on with a job outstanding at this instant.
 * The platform does not report the second core's load, so what is shown is the thing
 * that actually costs the render: a handover still in flight when the first core has
 * finished its half means the first core waits, and a render can miss its deadline
 * without ever looking loaded. "w!" marks a run that saw at least one.
 *
 * Row 3: the core clock in MHz, measured rather than read. The platform has no clock
 * query -- fm1_clock.h's registers are behind the MMIO ban that keeps this app out of
 * a register -- so it is timed against the 24 MHz crystal with a fixed count of
 * dependent float multiply-adds. Dependent, because then the FPU's latency is timed and
 * not its width: throughput would measure the machine's shape, which is the same at
 * both clocks. A compiler that unrolls the loop changes the count and the number goes
 * wrong by exactly that factor, so the result is only accepted when it lands on one of
 * the two clocks this chip is found at. */
#ifdef DBG_ON

/* The clocks an FM-1 runs at (fm1_clock.h): 360 MHz under M-VAVE's bootloader, 320
 * under m-bootloader 1.2. A reading within 30 MHz of one is rounded to it: the loop's
 * cycle count is fixed at -O2, so a percent or two of error is expected, and "358 MHz"
 * would be a worse answer than "360". */
static const uint16_t MPU_CLKS[] = { 240, 320, 360, 480 };

static uint32_t dbg_mhz;            /* the reading, MHz: 0 = not measured */
static uint8_t  dbg_seen_wait;      /* a handover was seen in flight at least once */

/* The clock measurement: the core's own cycle counter against the 24 MHz crystal.
 *
 * The first version of this counted a float loop and divided by an assumed 8 cycles an
 * iteration; on the device it read 129 MHz, which is 1555 / 12 -- the loop is ~22
 * cycles, not 8, because the FPU's dependent-operation latency dominates. Every
 * constant in that version was a guess, so it is gone.
 *
 * fm1_perf.h exposes the hardware counter (C0_TL_CKCNT, the AC79's corex2 block, which
 * JieLi's own debug.c reads to compute CPU efficiency): cycles on one side, the 24 MHz
 * crystal's ticks on the other, no assumption in between. It is marked "not verified on
 * hardware" upstream, so it is checked first -- a counter that does not move falls back
 * to nothing rather than to a wrong number.
 *
 * On the host there is no such counter and the host's own cycles are not the FM-1's, so
 * there is no reading at all: the row stays blank. A fabricated figure is worse than an
 * empty one. */
#if defined(PLAT_HOST)
#define DBG_NO_CYCLES 1
#else
#include "fm1_perf.h"
#endif

static uint32_t dbg_mhz;            /* 0 = not measured */
static uint8_t  dbg_mhz_ok = 1;     /* 0 = the reading is not a known clock */
/* Why row 3 is blank, when it is: the measurement has four ways to end and they mean
 * different things on the device. Shown as a short tag so a single flash says which. */
enum { DBG_ST_NONE, DBG_ST_OK, DBG_ST_NOMOVE, DBG_ST_NOTICK, DBG_ST_ODD };
static uint8_t  dbg_state;
static uint8_t  dbg_seen_wait;      /* a handover was seen in flight at least once */

#define DBG_SPAN_US 2000u           /* long enough that one cycle of error is 0.05% */

static void dbg_measure_clock(void)
{
#if !defined(DBG_NO_CYCLES)
    /* This does not measure the clock any more, because there is no way to.
     *
     * fm1_perf.h reads C0_TL_CKCNT and says "not verified on hardware". It is not: with
     * DBG_CON bits 0-2 set -- the write JieLi's own cpu_effic_init does, and the only
     * write to that register anywhere in the SDK -- the register still reads the same
     * value 100 us later, which the device reports as "ckNo". Those bits gate the three
     * stall counters, not this one; corex2(0) is at 0x1EEE000 so the address is right;
     * and nothing else in the SDK touches it. So it is not a free-running core clock on
     * this part however it is named.
     *
     * The first version of this measured instead of reading: a loop, divided by an
     * assumed cost per iteration. It read 129 MHz and the assumption was the error (the
     * loop is ~22 cycles, not 8, because dependent multiplies wait on the FPU). Guessing
     * a constant and printing the result is how that happened, and doing it again with a
     * different constant would be the same mistake twice.
     *
     * What is left is the configuration, which the platform reads out of the PLL
     * registers at the start (fm1_clock.h: PLL1 = 24/12 * 270 = 540 MHz, the system
     * clock 2/3 of it = 360, which is what the FM-1 runs under either bootloader). That
     * is a fact about a register the firmware set, not a measurement -- and the app
     * cannot see it, since register access belongs to the BSP (tools/build.py
     * mmio_check). So row 3 says why there is no reading, and stops. */
    fm1_perf_stalls_enable(1);
    dbg_state = fm1_perf_cycles_run() ? DBG_ST_OK : DBG_ST_NOMOVE;
#endif
}

/* The corner. Drawn after the trace, on purpose: an overlay the waveform can hide is an
 * overlay that lies about what it is not covering. */
static void dbg_draw(const src_skin_t *k)
{
    char r[20], t[8];
#if defined(PLAT_HOST) && defined(DBG_ON)
    char dbg_row1_p[20] = "", dbg_row2_p[20] = "", dbg_row3_p[20] = "";
#endif
    uint32_t pct = plat_cpu_pct();
    int32_t c2 = plat_cpu2_on();
    int32_t busy = plat_cpu2_busy();
    char *q;

    if (busy)
        dbg_seen_wait = 1;

    /* A backing box, because the graticule runs straight through these rows and a
     * scope trace runs through them too: text over a grid at this size is unreadable,
     * and a debug readout that cannot be read is not a debug readout. 62 px covers the
     * widest row ("c2 wt!", "360M?"). */
    cv_rect(0, 3, 84, 36, k->bg);

    /* row 1: "12% xr0" -- the load and the xruns, the two numbers a render that is
     * running out of time shows first */
    q = r;
    num(t, (int32_t)pct);
    for (const char *p = t; *p; )
        *q++ = *p++;
    *q++ = '%';
    *q++ = ' ';
    *q++ = 'x';
    *q++ = 'r';
    num(t, (int32_t)plat_xruns());
    for (const char *p = t; *p; )
        *q++ = *p++;
    /* The build's commit, on the row that is already there.
     *
     * `build.py` names every package `wave.fwsc` and the image carried no mark, so
     * "which firmware is on this unit?" had no answer -- and a comparison between the
     * device and an emulator built from a different commit looks exactly like a bug
     * that only the device has. It is worth the twelve pixels: a claim about the
     * sources should be readable on the thing that runs them. See tools/gen_build_id.py
     * for why the hash is the tree's and not the clock's. */
    *q++ = ' ';
    for (const char *p = APP_BUILD_ID; *p; )
        *q++ = *p++;
    *q = 0;
    cv_text(1, 4, &FONT_T, r, k->ink_hi);
#if defined(PLAT_HOST) && defined(DBG_ON)
    { int i = 0; while (r[i] && i < 19) { dbg_row1_p[i] = r[i]; i++; } dbg_row1_p[i] = 0; }
#endif

    /* row 2: what the second core actually did, not whether it was switched on.
     *
     *   "c2 --"     it was never started, or it was given up: one core
     *   "c2 0u"     it is on but no job was ever handed over -- it is idle, and saying
     *               "ok" there would have been a lie
     *   "c2 12u"    twelve jobs handed over, each taking on average one microsecond:
     *               the handover happened and cost nothing, which means the wait did
     *               not wait
     *   "c2 12u184" twelve jobs, 184 us each: CPU0 spent that long inside run..wait
     *               while the core worked -- the number that distinguishes work from a
     *               core that died and let the platform re-render its half
     *
     * The unit is us per job because that is the quantity with meaning: a block is
     * 5805 us, so a handover of ~2400 us is the second core doing about half of it. */
    q = r;
    *q++ = 'c';
    *q++ = '2';
    *q++ = ' ';
    if (c2 <= 0) {
        *q++ = '-';
        *q++ = '-';
    } else {
        num(t, (int32_t)c2_jobs);
        for (const char *p = t; *p; )
            *q++ = *p++;
        *q++ = 'u';
        if (c2_jobs) {
            num(t, (int32_t)(c2_handoff_us / c2_jobs));
            for (const char *p = t; *p; )
                *q++ = *p++;
        }
    }
    if (c2 > 0 && busy) *q++ = 'w';
    /* What the far half actually put into the sum, as a peak in 1/4096. A core that
     * returns every job on time with p0 has rendered silence: the voices on its side
     * of the cut are not being heard, and no amount of timing tells you that. */
    if (c2 > 0 && c2_jobs) {
        *q++ = ' ';
        *q++ = 'p';
        num(t, (int32_t)(wv_c2_peak() * 4096.0f));
        for (const char *p = t; *p; )
            *q++ = *p++;
    }
    *q = 0;
    cv_text(1, 16, &FONT_T, r, k->ink_hi);
#if defined(PLAT_HOST) && defined(DBG_ON)
    { int i = 0; while (r[i] && i < 19) { dbg_row2_p[i] = r[i]; i++; } dbg_row2_p[i] = 0; }
#endif

    /* row 3: the clock, or why there is none. "ck?" would be a mystery; "ckX" with the
     * tag is a diagnosis, and one flash of the firmware should be enough to settle it.
     *   (nothing)  the probe did not run at all
     *   ckNo       C0_TL_CKCNT does not move here: the register is not a usable core
     *              clock on this part, with or without DBG_CON's stall-enable bits.
     *              This is the answer to the question, not a failure to get one. */
    q = r;
    if (dbg_mhz) {
        num(t, (int32_t)dbg_mhz);
        for (const char *p = t; *p; )
            *q++ = *p++;
        *q++ = 'M';
        if (!dbg_mhz_ok)
            *q++ = '?';
    } else if (dbg_state == DBG_ST_NOMOVE) {
        *q++ = 'c'; *q++ = 'k'; *q++ = 'N'; *q++ = 'o';
    } else if (dbg_state == DBG_ST_NOTICK) {
        *q++ = 'c'; *q++ = 'k'; *q++ = 'N'; *q++ = 't';
    }
    *q = 0;     /* r is reused: an empty row must be an empty string, not the last one */
    /* Then the second core's stack watermark, when it has one: "360M s412".
     *
     * This is the one reading that cannot be produced by CPU0 doing the work. The
     * second core's stack is marked at its start and only that core writes there, so a
     * nonzero depth means it executed its job -- measured, not inferred from a
     * percentage that the fallback path leaves unchanged. What it does NOT prove is
     * that the audio it made was kept; only the load comparison does that. */
    if (q != r && plat_cpu2_stack()) {
        *q++ = ' ';
        *q++ = 's';
        num(t, (int32_t)plat_cpu2_stack());
        for (const char *p = t; *p; )
            *q++ = *p++;
        *q = 0;
    }
    if (q != r) {
        *q = 0;
        cv_text(1, 28, &FONT_T, r, k->ink_hi);
    }
#if defined(PLAT_HOST) && defined(DBG_ON)
    { int i = 0; while (r[i] && i < 19) { dbg_row3_p[i] = r[i]; i++; } dbg_row3_p[i] = 0; }
#endif
#if defined(PLAT_HOST) && defined(DBG_ON) && !defined(PLAT_WEB)
    /* The host cannot read its own glyphs back, so the rows are printed where they are
     * built: the tests assert the format of what the device would show, and the bitmap
     * checks still cover that it drew.
     *
     * Not on the web build: emu_web.c compiles with PLAT_HOST too, and the emulator's
     * worklet reports zero bytes written from fd_write. Emscripten's stdio retries until
     * a write reports progress, so one printf per frame there is an endless loop: the
     * audio thread never returns from its first block, and the screen stays black while
     * the page's buttons keep working. */
    {
        extern int printf(const char *, ...);
        printf("dbg row1=%s| row2=%s| row3=%s\n", dbg_row1_p, dbg_row2_p, dbg_row3_p);
    }
#endif
}
#else
#define dbg_draw(k)         ((void)0)
#define dbg_measure_clock() ((void)0)
#endif

void app_frame(void)
{
    static uint32_t last;
    uint32_t now = plat_ms();

    /* touch_poll drains the scan's key events into touch_keys/touch_vel -- it is the
     * ONLY writer of touch_v[], and the template calls it every frame. This app called
     * it from app_tick, which the platform never calls (app.h has no APP_TICK), so it
     * never ran: the event ring filled up and every press past the 16th was dropped,
     * while touch_vel answered its 100 fallback for everything. */
    touch_poll();
    input();

    /* Keep save_dirty() honest: the whole record is compared against what flash holds,
     * once a frame. This is what the save line under the scope reads. */
    {
        save_rec r;
        save_snapshot(&r);
        /* On the FIRST frame with no record in flash, what boot set up IS the baseline.
         *
         * The defaults are not all zero: region_reset_all() gives every slot its whole self
         * (region_end = the slot's length, 52369 for the demo PAD) while save_load() left
         * rec_written zeroed, so the first scan compared a real record against an empty one
         * and reported unsaved from frame one. Measured: `expect save_dirty 0` passed
         * before any frame ran and read 1 after `wait 100`.
         *
         * This is not new -- it was true before the save line existed. It was invisible
         * because the old mark was two words nobody had asked the meaning of at boot; a bar
         * that measures the distance to flash cannot be built on top of it.
         *
         * Done here rather than at the end of app_init because app_init has not finished
         * when save_load() returns: ui_apply(), the bank's frame count and region_fix() all
         * still move the state afterwards, and a baseline taken at that moment marks those
         * corrections as edits. By the first frame everything that runs at boot has run. */
        save_scan(&r);
    }

    sk = src_skin();

    /* the scope redraws every frame: it is the one thing on screen that must not
     * look frozen while a note is held */
    cv_begin(240, SCOPE_H, sk.bg);
    {
        /* Read the ring backwards from where the audio has got to, so the newest
         * sample is at the right edge: a scope that scrolls the other way reads as
         * noise. `end` is sampled once; a block landing mid-read costs at most one
         * stale block, not a torn frame. */
        uint16_t end = wf_idx;
        int32_t x;
        int32_t lo, hi, view, tbase;

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
         * ── the search is bounded by the ring, and that is not a detail ──────────
         *
         * The window is [tbase, tbase + view) samples back from the newest one, so the
         * furthest sample it reads is view - 1 + tbase back. WF_SPAN is 768 and a
         * three-period window at 175 Hz is 758 samples on its own: the ring does NOT
         * hold "3 windows' worth" (the comment there said so and was wrong), and a
         * trigger found near the far end of the search ran off it. Past WF_SPAN the
         * modulo does not read history, it reads the cell the next block overwrote, so
         * the picture was two instants spliced together -- per note, because it
         * depended on both the window length and where the crossing happened to land.
         * Measured against the same window read straight out of the signal, the trace
         * correlated 0.04-0.46 at notes 53/58/60/65 and 0.997+ at 64/69/76.
         *
         * So the search stops where the window still fits: at most WF_SPAN - view
         * samples back. A note whose window does not fit at all (view > WF_SPAN/2)
         * keeps tbase = 0 and simply does not trigger, which is what it already did
         * when it found no crossing.
         *
         * If there is no crossing in that range (silence, a DC-ish sample, the last
         * period of a release tail) there is nothing to trigger on and the window
         * falls back to the newest sample. It will drift while it does that, which is
         * honest: there is no period on screen to be stable about. */
        tbase = 0;
        {
            int32_t b;
            int32_t bmax = (int32_t)WF_SPAN - view;      /* window still inside the ring */
            if (bmax > view - 1)
                bmax = view - 1;
            for (b = 0; b < bmax; b++) {
                int32_t c0 = wf_buf[(uint16_t)((end + WF_SPAN - 1u - (uint32_t)b) % WF_SPAN)];
                int32_t c1 = wf_buf[(uint16_t)((end + WF_SPAN - 2u - (uint32_t)b) % WF_SPAN)];
                if (c0 >= 0 && c1 < 0) {          /* c1 is one sample older: rising */
                    tbase = b;
                    break;
                }
            }
        }

        /* ── the vertical scale: fixed, LINEAR IN AMPLITUDE ─────────────────────
         *
         * This was a per-frame peak normaliser, and the measurement that condemned it
         * is that velocity 20, 60 and 127 all drew 63 px: a scope that auto-gains is
         * answering "what shape is this", and the shape is already answered one band
         * down by the material picture. So the question here is the scope's own --
         * how loud, and when -- and a fixed scale answers it.
         *
         * Fixed does not mean a dB axis, and this is the trap that the comment here
         * used to describe: the trace's instantaneous value crosses zero twice a cycle,
         * and in dB a zero crossing is -infinity, so a dB-mapped trace plunges to the
         * floor at every crossing and the picture becomes a filled band.
         *
         * So the mapping is amplitude-proportional: fs_px() scales by SCOPE_FS, which
         * is SCOPE_HALF, so full scale lands on the band edge exactly (y 1 and y 83
         * around the centre at 42) and there is NO headroom above 0 dBFS -- a signal
         * past full scale clamps to the edge. The graticule's horizontal lines ARE at
         * dB values, but they are placed by the same amplitude mapping, which is why
         * they crowd towards the centre: -10 dB sits 13 px in, -30 dB 2 px in, and
         * -40 and -50 both round to the same row.
         *
         * Measured on the host, pre-master, every voice at velocity 127: one note
         * peaks at 0.210 of full scale, four at 0.444, eight at 0.487. SCOPE_REF is
         * 0.5, so eight voices reach the edge and one note draws 17 px each side of
         * the centre. That is the change that made the scope readable; before it the
         * reference was digital full scale and a single note drew 2 px, which was
         * reported as "you can barely see the waveform".
         *
         * The columns keep their min/max: a period can span a whole cycle in fewer
         * columns than it has samples, so a column is a range of the signal and not a
         * point on it. */
        {
            const float mid = (float)SCOPE_MID;

            /* ── the graticule ──────────────────────────────────────────────
             *
             * Drawn before the trace, so the waveform runs over it: a scope's grid is
             * the glass, not the signal.
             *
             * The scale is linear in amplitude, so the dB lines are NOT equally
             * spaced: the distance from 0 to -10 dB is 41 * (1 - 0.316) = 28 px, from
             * -10 to -20 dB is 41 * (0.316 - 0.1) = 8.9 px, and so on. That is the
             * signature of a dB scale and it is the point of drawing one: an eye that
             * has read "half the height" off the screen is being told a dB number
             * instead, and half the height on this band is -4 dBFS, not -30.
             *
             * Labels: "+0" only, at the right edge in the top row. The others would be
             * "-10", "-20"... -- five words of six px each in a 2 px gap, which is not
             * readable and not a scope. The 0 line is the reference and the only one
             * a loud chord visibly crosses; the rest of the scale is read off the
             * spacing. */
            {
                int32_t db, col;
                for (col = 1; col * 20 < 240; col++)
                    cv_line(col * 20, 0, col * 20, SCOPE_H - 1, GRID_DIM);
                /* Down to -30 dBFS, not -50. The scale is linear in amplitude, so the
                 * lines bunch up geometrically toward the centre: measured on this band,
                 * -30 lands at y 40/44, -40 at 41/43, and -50 at 41/43 as well -- the
                 * last two are the SAME ROW, so -50 drew nothing and -40 drew a line
                 * 1 px from the centre rule with the trace's own back-and-forth running
                 * through it. Three rules inside 4 px of the centre is what made a
                 * near-vertical trace look like it was fighting the graticule.
                 *
                 * Stopping at -30 keeps every line at least 2 px from the centre rule
                 * (-30 is the innermost, at 40/44) and leaves the spacing that reads as
                 * a scale: 28 px from 0 to -10, 8 to -20, 3 to -30. Below -30 the
                 * remaining space is 2 px, which cannot carry a rule that says anything.
                 *
                 * What is lost, stated: nothing below -30 dBFS is marked. A trace down
                 * there is 0.032 of full scale, i.e. a 2 px line, and it already had no
                 * grid to read against. */
                for (db = 0; db >= -30; db -= 10) {
                    /* The y of db on this band, each side of the centre. fm_db2lin is
                     * the platform's own dB -> linear, and it is scaled by SCOPE_REF:
                     * the lines are dB relative to the eight-voice peak that reaches
                     * the edge (SCOPE_FS above), not to digital full scale. Without
                     * this factor the +0 line would still be drawn at the band edge
                     * while the trace reached it at 0.5, and the graticule would be
                     * lying about where a chord sits. */
                    int32_t up = (int32_t)(mid - fs_px(fm_db2lin(db) * SCOPE_REF));
                    cv_rect(0, up, 240, 1, db == 0 ? GRID : GRID_DIM);
                    cv_rect(0, (int32_t)(mid + (mid - up)), 240, 1, db == 0 ? GRID : GRID_DIM);
                }
                {
                    char lab[3] = { '+', '0', 0 };
                    cv_text(240 - 24, 1, &FONT_XS, lab, GRID);
                }
            }

            /* a centre line, so silence reads as a flat line and not as nothing */
            cv_rect(0, (int32_t)mid, 240, 1, sk.track);

            for (x = 0; x < WF_W; x++) {
                uint32_t a = (uint32_t)x * (uint32_t)view / WF_W;
                uint32_t b = (uint32_t)(x + 1) * (uint32_t)view / WF_W;
                int32_t k, yup, ydn;

                lo = 32767;
                hi = -32767;
                if (b <= a)
                    b = a + 1;
                for (k = (int32_t)a; k < (int32_t)b; k++) {
                    uint32_t back = (uint32_t)view - 1u - (uint32_t)k + (uint32_t)tbase;
                    uint16_t at = (uint16_t)((end + WF_SPAN - 1u - back) % WF_SPAN);
                    int32_t v = wf_buf[at];
                    if (v < lo)
                        lo = v;
                    if (v > hi)
                        hi = v;
                }

                /* min/max preserved: a column can span more than one sample, so it is a
                 * range of the signal and both ends of that range are drawn */
                yup = (int32_t)(mid - fs_px((float)hi * (1.0f / 32767.0f)));
                ydn = (int32_t)(mid - fs_px((float)lo * (1.0f / 32767.0f)));

                if (yup == ydn) {
                    /* a quiet or steady column is a dot, not a gap: a scope that drops
                     * columns looks broken */
                    cv_rect(x, yup, 1, 1, sk.ink_hi);
                } else {
                    cv_line(x, yup, x, ydn, sk.ink_hi);
                }
            }
        }
    }
    dbg_draw(&sk);
    frame_blits = 0;
    cv_blit(0, 0);                 /* without this the band is drawn and never shown */
    frame_blits++;

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
    /* ─── the material, and the four knobs ───────────────────────────────────────
     *
     * Layout, top to bottom:
     *
     *   0..68    scope        the result of shaper -> filter -> VCA, what the ear hears
     *   68..176  material     the table's own shape, 108 px tall
     *   176..240 the four     CUT RES SHAPE AMOUNT, or A D S R on the ENV page
     *
     * The table's shape earned the middle of the screen and the knobs moved to the
     * bottom, because the shape is now the more useful of the two: at 28 px in the
     * strip it was legible as "something moves" and not as a waveform, and BANK and
     * MORPH are the two parameters it already draws. A number beside a waveform that
     * shows that number's effect is a worse use of a cell than the waveform at a size
     * you can read. So BANK and MORPH lose their cells and keep their values, printed
     * over the picture they control.
     *
     * The knobs being last is also where the hand is: the bottom row of the screen is
     * nearest the bottom row of keys. K1..K4 sit in the physical order left to right,
     * which is the rule this panel has kept since the first layout. */
    cv_begin(240, MAT_H, sk.bg);
    {
        /* ── the save line ──────────────────────────────────────────────────────
         *
         * Two pixels under the scope, in the padding the band already had.
         *
         * It used to be the words "NOT SAVED" at the top left of the picture, and the
         * measurement that condemned that is that the words sat ON the waveform: in the
         * label's own 13 x 62 box, 443 px of SYNTH's waveform and 403 px of SMPL's and
         * GRAN's were erased to make room, in the brightest part of the picture. Moving it
         * into MAT_PAD's top 2 rows costs the picture nothing -- the wave reaches no higher
         * than y = 90 and the line is at y = 84, so four rows of air stay between them.
         *
         * LENGTH is the number of groups that differ from flash (save_change_count: 17
         * parameters, 3 selectors, 8 regions -- a region counts once, because dragging a
         * loop boundary is one edit). COLOUR is how long they have been unsaved: dim at
         * first, `ink` at SAVE_LINE_FULL_MS, never ink_hi, because ink_hi is what a knob
         * arc fills with and the bottom of this band is not a fifth control.
         *
         * Nothing is drawn when the record matches flash. A permanent empty track would
         * be two rows of grey that mean "saved", which is the absence of news, and this
         * screen has no room for news that says nothing.
         *
         * The divisions are integer: the count is 0..28 and the width is 232 px, so the
         * unit is 240/SAVE_LINE_FULL_CHANGES, the full screen width. Rounded up, so one
         * change is never zero pixels -- a line that stays empty for the first edit reads
         * as broken. */
        if (save_dirty()) {
            int32_t n = save_change_count();
            /* Full 240, not the picture's 232. The line is a readout of the whole panel, not
             * a picture of the material, so it spans the screen edge to edge -- starting at
             * x 4 left a 4 px gap that reads as a mistake rather than as a margin. One unit
             * is therefore 240/8 = 30 px. */
            int32_t w = n * 240 / SAVE_LINE_FULL_CHANGES;
            uint32_t age;

            if (w > 240)
                w = 240;
            if (w < 1)
                w = 1;
            save_line_len = w;
            if (!unsaved_live) {
                unsaved_live = 1;
                unsaved_at = plat_ms();
            }
            age = plat_ms() - unsaved_at;
            /* 3/5 -> 5/5 of ink across SAVE_LINE_FULL_MS. The brightness is the second
             * axis: it starts bright enough to be read at a glance and still has room to
             * say "this has been sitting here too long". */
            {
                /* 3/5 -> 5/5, not 2/5 -> 5/5. Measured against each skin's own ground the
                 * old first step sat only dLum +26 (SYNTH) / +34 (SMPL) / +26 (GRAN) from
                 * the background, which is not a line, it is a rumour of one; the player
                 * reported exactly that ("一开始的红色不够醒目"). At 3/5 the same numbers
                 * are +56 / +68 / +58, still well under ink_hi (+125 / +135 / +119 + the
                 * highlight step), so the ramp keeps its meaning and its ceiling.
                 *
                 * Three steps instead of four is the cost, and it is paid where it matters
                 * least: this line has one job on its first frame -- to be seen -- and
                 * four steps it cannot see are worth less than three it can. */
                int32_t step = (int32_t)(age * 3u / SAVE_LINE_FULL_MS);
                if (step > 3)
                    step = 3;
                save_line_step = step;
                save_line_age = age;
                cv_rect(0, SAVE_LINE_ROW, w, 2, dim(sk.ink, step + 2, 5));
            }
        } else {
            unsaved_live = 0;
            save_line_len = 0;
            save_line_step = 0;
        }

        /* The picture runs the full width, and the three knob values sit on top of it in
         * the corners their knobs occupy:
         *
         *                              SEL n/m       top right
         *          PRESET n            ...           bottom left   (GRAN only)
         *                              ALGO 45ms     bottom right
         *
         * No knob NAME is printed. The four knobs below this band are drawn as CUT, RES,
         * SHAPE, AMOUNT with no encoder numbers, because the position is the name. The
         * same holds up here: the value in the top-right corner belongs to the top-right
         * knob. Printing "SEL" over the SELECT knob's own corner adds nothing.
         *
         * The values are printed over the picture rather than beside it, and the cost is
         * one clear rectangle of waveform per label. The player asked for this after
         * seeing the side-by-side version: a full-width picture with three small labels
         * on it reads better than a narrow picture with a clean margin beside it.
         *
         * Drawn picture first, text second, so a label always wins. No boxes behind the
         * text -- a box is a third colour in a band that only has two. */
        const int32_t ytop = MAT_ROW_TOP;
        const int32_t ybot = MAT_ROW_BOT;

        if (!smp_count && src != SRC_SYNTH) {
            cv_text(MAT_X0 + 2, 46, &FONT_XS, "NO SAMPLES", sk.ink);
        } else if (src == SRC_SYNTH) {
            /* The rack on this page, the single frame everywhere else: this page edits the
             * eight against each other, and one curve cannot show the thing being edited. */
            if (edit_page)
                src_rack(&sk, MAT_X0, MAT_PAD, SYN_W, MAT_WAVE);
            else
                src_shape(&sk, MAT_X0, MAT_PAD, SYN_W, MAT_WAVE);
        } else if (src == SRC_GRAN) {
            /* the voice the cloud is most likely to be heard from: the newest one. With
             * several keys down the picture shows one voice's cloud, and the newest is
             * the one the player just caused. */
            const wv_grains *gr = 0;
            int32_t vi;
            uint32_t newest = 0;
            for (vi = 0; vi < (int32_t)WV_VOICES; vi++) {
                if (dsp.v[vi].active && dsp.v[vi].age >= newest) {
                    newest = dsp.v[vi].age;
                    gr = &dsp.v[vi].gr;
                }
            }
            src_grain_map(&sk, &smp[smp_i], MAT_X0, MAT_PAD, MAT_W, MAT_WAVE, gr);
        } else {
            if (edit_page)
                src_edit_map(&sk, &smp[smp_i], MAT_X0, MAT_PAD, MAT_W, MAT_WAVE,
                             &region[smp_i], edit_zoom, edit_live);
            else
                env_line(&smp[smp_i], MAT_X0, MAT_PAD, MAT_W, MAT_WAVE,
                         region[smp_i].start, region[smp_i].end, dim(sk.ink, 2, 5));
        }
        if (src != SRC_SYNTH && smp_count)
            cv_text(MAT_X0 + 2, MAT_PAD + 1, &FONT_XS,
                    smp[smp_i].name[0] ? smp[smp_i].name : "SMPL", dim(sk.ink_hi, 3, 5));

        /* ── the three values, in their knobs' corners ── */

        /* Every value gets its name. An earlier pass printed the numbers alone, on the
         * argument that the corner is the name -- the argument the four knobs below use,
         * where CUT really is at CUT's position. It does not carry up here, because the
         * same corner means a different thing in each source: the top right is BANK in
         * SYNTH but SLOT in SMPL and GRAN, and the bottom right is MORPH in SYNTH but
         * GRAIN in GRAN. "2/2" and "45ms" do not say which. A position is only a name
         * when it is the same name every time, and the player said so within a minute.
         *
         * Name first, then the value, so a label reads as one phrase. Left corner is
         * flush left at the band edge, right corner flush right at it, which keeps the
         * two symmetric about the picture.

         * The width is summed from the font's own advance table rather than assumed:
         * FONT_XS is proportional (a digit is 6 px, '/' is 3), so a fixed 7 px per glyph
         * left the right-hand labels 4-9 px off the edge and drifting with the value's
         * digit count. */
        const char *nm;
        char *vv;
        int32_t w;

        /* The right-hand column's edge. SYNTH keeps the labels out of its picture (§ SYN_W
         * above); SMPL and GRAN run theirs full width and print over the envelope, which
         * the player accepted because that envelope is at 2/5 ink and the bars are 1 px. */
        const int32_t xr = (src == SRC_SYNTH) ? SYN_COL_R : MAT_LAB_R;

        /* SELECT: top right. */
        nm = (src == SRC_SYNTH) ? "BANK" : "SLOT";
        vv = num2(src == SRC_SYNTH ? bank_i + 1 : smp_i + 1,
                  src == SRC_SYNTH ? (int32_t)WV_BANK_COUNT : (int32_t)smp_count);
        if (src == SRC_SYNTH || smp_count) {
            cv_text(xr - txt_w(nm) - 4 - txt_w(vv), ytop, &FONT_XS, nm,
                    dim(sk.ink_hi, 3, 5));
            cv_text(xr - txt_w(vv), ytop, &FONT_XS, vv, sk.ink_hi);
        }

        /* SYNTH's second line: the bank's NAME. "BANK 3/8" says which one you are on and
         * nothing about what it is; the name is the part that tells you to keep turning or
         * stop. WV_BANK_NAME has carried these since the generator was written -- SAW SQR,
         * RESO, HOLLOW, VOX, DIGI, OPEN1..3 -- and the screen had never shown one. */
        if (src == SRC_SYNTH)
            cv_text(xr - txt_w(WV_BANK_NAME[bank_i]), SYN_ROW_2, &FONT_XS,
                    WV_BANK_NAME[bank_i], sk.ink_hi);

        /* PRESET: bottom left. SPRAY in GRAN, LOOP in the EDIT page, nothing
         * otherwise -- one knob, and each source has one thing for it. */
        if (src == SRC_GRAN) {
            cv_text(MAT_LAB_L, ybot, &FONT_XS, "SPRAY", dim(sk.ink_hi, 3, 5));
            cv_text(MAT_LAB_L + txt_w("SPRAY") + 4, ybot, &FONT_XS, pct(k_spray),
                    sk.ink_hi);
        } else if (edit_page && src != SRC_SYNTH) {
            nm = "LOOP";
            vv = region[smp_i].on ? "ON" : "OFF";
            w = txt_w(vv);
            cv_text(MAT_LAB_L, ybot, &FONT_XS, nm, dim(sk.ink_hi, 3, 5));
            cv_text(MAT_LAB_L + txt_w(nm) + 4, ybot, &FONT_XS, vv,
                    region[smp_i].on ? sk.ink_hi : dim(sk.ink_hi, 3, 5));
        }

        /* ALGO: bottom right. MORPH in SYNTH, GRAIN in GRAN, ZOOM in the EDIT page.
         * Not drawn in SMPL outside EDIT: morph indexes a wavetable frame and SMPL
         * reads no wavetable, so the knob is dead there, and a dead value is worse
         * than an empty corner. */
        if (src == SRC_SYNTH || src == SRC_GRAN || edit_page) {
            if (edit_page && src != SRC_SYNTH) {
                nm = "ZOOM";
                vv = zoom_text(edit_zoom);
            } else if (src == SRC_SYNTH) {
                nm = "MORPH";
                vv = pct(k_morph);
            } else {
                nm = "GRAIN";
                vv = sel_time(k_grain, 3);
            }
            w = txt_w(vv);
            cv_text(xr - txt_w(nm) - 4 - w, ybot, &FONT_XS, nm, dim(sk.ink_hi, 3, 5));
            cv_text(xr - w, ybot, &FONT_XS, vv, sk.ink_hi);
        }

        {
            const int32_t ypage = MAT_PAD + MAT_WAVE - 10;

            if (env_page)
                cv_text(MAT_X0 + 2, ypage, &FONT_XS, "ENV PAGE", sk.ink);
            else if (lfo_page)
                cv_text(MAT_X0 + 2, ypage, &FONT_XS, "LFO PAGE", sk.ink);
            else if (arp_page)
                cv_text(MAT_X0 + 2, ypage, &FONT_XS, "ARP PAGE", sk.ink);
            else if (edit_page && src != SRC_SYNTH)
                cv_text(MAT_X0 + 2, ypage, &FONT_XS, "EDIT PAGE", sk.ink);
            /* SYNTH's edit page prints no caption: the rack IS the page (all eight frames
             * at once is not something the other sources ever do), and a caption on its
             * last row sat on top of the near frame's curve. The knob labels in the
             * column say FRAME / OFFSET / SPREAD / SHAPE, which is more than "EDIT PAGE"
             * ever said. */

            /* ── "not saved" ────────────────────────────────────────────────────
             *
             * B_SAVE is the only writer, so an edit lives in RAM until it is pressed.
             * That is the arrangement the player chose over an automatic write (a save
             * is a 4 KiB sector erase plus a read-back, and it has no business
             * happening behind a knob), and its one real cost is exactly this: you
             * cannot hear whether the sound you are listening to is the one that will
             * come back after power-off.
             *
             * So the mark is not decoration, it is the whole reason the manual model is
             * safe to use. Right-hand edge, the mirror of the page name on the left: a
             * filled dot and two words. It sits at 3/5 ink rather than ink_hi so it reads
             * as a note beside the picture and not as a value; a warning that competes
             * with the parameters is a warning that gets ignored.
             *
             * Nothing is drawn once it is saved: a permanent "saved" light would be one
             * more thing on the screen that means nothing. */
        }
    }
    /* One transfer for this band. There used to be five in a row -- and the row sits
     * after cv_begin(240, MAT_H, ...), so all five were sending the MATERIAL band to
     * the screen, four of them redundantly. lcd_data() syncs (waits out the previous
     * DMA) before it starts, so those four were four full 240x92 transfers of pixels
     * already on the glass, ~8 ms each at the measured ~44 MHz. The last one wins and
     * it is the same buffer every time, so nothing on screen depended on the rest. */
    cv_blit(0, SCOPE_H);
    frame_blits++;

    cv_begin(240, KNOB_H, sk.bg);
    if (env_page) {
        /* The ENV page: the same four bars in the same four positions, so the hand
         * does not move — only what they mean. Time in ms/s as text, because "37" on
         * an envelope is unreadable and "37 ms" is not. */
        knob(4,   KNOB_TOP, 54, "ATTACK",  sel_time(k_eg_a, 0), k_eg_a, 1);
        knob(62,  KNOB_TOP, 54, "DECAY",   sel_time(k_eg_d, 1), k_eg_d, 1);
        knob(120, KNOB_TOP, 54, "SUSTAIN", pct(k_eg_s), k_eg_s, 1);
        knob(178, KNOB_TOP, 54, "RELEASE", sel_time(k_eg_r, 2), k_eg_r, 1);
    } else if (arp_page) {
        /* The ARP page: RATE doubles as the switch, so it reads OFF at zero and a
         * note division otherwise. OCT prints "2x" because the useful question is how
         * far it climbs, not a percentage. */
        knob(4,   KNOB_TOP, 54, "RATE",  arp_rate_text(k_arp_rate), k_arp_rate, 1);
        knob(62,  KNOB_TOP, 54, "TYPE",  ARP_NAME[k_arp_type], k_arp_type * 42, 1);
        knob(120, KNOB_TOP, 54, "OCT",   arp_oct_text(k_arp_oct), k_arp_oct, 1);
        knob(178, KNOB_TOP, 54, "GATE",  pct(k_arp_gate), k_arp_gate, 1);
    } else if (edit_page && src == SRC_SYNTH) {
        /* SYNTH's EDIT page: the phase ramp.
         *
         * K1 picks a frame, K2 turns that frame, K3 fans all eight out, K4 picks the
         * shape of the fan. The page cannot show the wave being edited -- 256 points
         * across 240 px with no cursor is a guessing game, and the offset is a value you
         * hear rather than see. The frame prints as a number because frames have no
         * names and "3" is what the MORPH knob's position corresponds to. */
        knob(4,   KNOB_TOP, 54, "FRAME",  frame_text(),  edit_frame * 127 / (WV_FRAMES - 1), 1);
        knob(62,  KNOB_TOP, 54, "OFFSET", foff_text(),   foff_mark(),  1);
        knob(120, KNOB_TOP, 54, "SPREAD", spread_text(), (int32_t)foff_spread, 1);
        knob(178, KNOB_TOP, 54, "SHAPE",  (char *)FOFF_SHAPE_NAME[foff_shape],
             (int32_t)foff_shape * 127 / (FOFF_SHAPE_N - 1), 1);
    } else if (edit_page) {
        /* The EDIT page: the four boundaries, in the order they appear on the picture,
         * so K1 is the leftmost marker and K4 the rightmost. START and END are the
         * region; LOOP S and LOOP E are the repeated part inside it. The unit is a
         * position in the material -- ms of the slot, which is what a sampler shows and
         * what a boundary means.
         *
         * The live marker's knob is lit and the other three are not, because at 8x zoom
         * only one of the four is inside the window and the lit one is the knob the
         * picture is following. */
        /* Each knob in its boundary's hue, so the four lines in the picture and the
         * four knobs under it are one set of four objects, not two.
         *
         * The order is the picture's order, left to right: start, then the loop pair,
         * then end. The first version had END on K2 because the four values are listed
         * that way everywhere else (start, end, loop start, loop end), and that put a
         * hand's second knob at the far right end of the sample while the third and
         * fourth were inside it -- the knobs read left to right and the material does
         * not, so a small move of the loop was a reach across the whole row. */
        knob_c(4,   KNOB_TOP, 54, "START",  pos_text(smp_i, region[smp_i].start),  region_mark(smp_i, 0), edit_live == 0, C_ED_START);
        knob_c(62,  KNOB_TOP, 54, "LOOP S", pos_text(smp_i, region[smp_i].loop_s), region_mark(smp_i, 2), edit_live == 2, C_ED_LOOPS);
        knob_c(120, KNOB_TOP, 54, "LOOP E", pos_text(smp_i, region[smp_i].loop_e), region_mark(smp_i, 3), edit_live == 3, C_ED_LOOPE);
        knob_c(178, KNOB_TOP, 54, "END",    pos_text(smp_i, region[smp_i].end),    region_mark(smp_i, 1), edit_live == 1, C_ED_END);
    } else if (lfo_page) {
        /* The LFO page takes K3-K4 and leaves CUT and RES alone: see the note where
         * the page's knobs are read. RATE prints Hz, because a rate in percent is not
         * a rate; DEPTH prints octaves, because CUT is exponential. */
        knob(4,   KNOB_TOP, 54, "CUT",   pct(k_cut), k_cut, 0);
        knob(62,  KNOB_TOP, 54, "RES",   pct(k_res), k_res, 0);
        knob(120, KNOB_TOP, 54, "RATE",  lfo_rate_text(k_lfo_rate), k_lfo_rate, 1);
        knob(178, KNOB_TOP, 54, "DEPTH", lfo_depth_text(k_lfo_depth), k_lfo_depth, 1);
    } else if (fx_page) {
        /* The FX page: the filter's own. CUT and RES stay on K1 and K2 because they ARE
         * the filter, exactly as on the LFO page -- and because a page that moved them
         * would make the two pages disagree about where the filter lives.
         *
         * TYPE's ring fills 1/3 then 2/3 then 3/3, which is the only thing a three-state
         * ring can say; the name in the hole says which. KEY TRK's ring shows the actual
         * percentage, so 25 % is a quarter of the way round and not a detent count.
         *
         * Neither value is a Hz figure, and that is deliberate: with key tracking on, the
         * corner is a different frequency per note, so a single number here would be a
         * number that is true for one note and false for the chord. The ring is the knob
         * position, which is the thing the knob actually controls. */
        knob(4,   KNOB_TOP, 54, "CUT",    pct(k_cut), k_cut, 0);
        knob(62,  KNOB_TOP, 54, "RES",    pct(k_res), k_res, 0);
        knob(120, KNOB_TOP, 54, "TYPE",   FTYPE_NAME[k_ftype],
             (int32_t)k_ftype * 127 / (WV_FTYPE_N - 1), 1);
        /* The ring's argument is 0..127, and k_ktrack is PERCENT -- passing the percent
         * straight in drew 100 % as 100/127 of the arc, a ring that never filled. The
         * conversion is the page's, not the DSP's, because the DSP's unit is percent and
         * the ring's unit is a knob position; the two scales meeting in one place is how
         * they were allowed to disagree. */
        knob(178, KNOB_TOP, 54, "KEY TRK", ktrack_text(k_ktrack),
             (int32_t)k_ktrack * 127 / 100, 1);
    } else {
        knob(4,   KNOB_TOP, 54, "CUT",    pct(k_cut), k_cut, 0);
        knob(62,  KNOB_TOP, 54, "RES",    pct(k_res), k_res, 0);
        knob(120, KNOB_TOP, 54, "SHAPE",  SHAPE_NAME[k_shape], k_shape * 42, 0);
        knob(178, KNOB_TOP, 54, "AMOUNT", pct(k_amount), k_amount, 0);
    }
    cv_blit(0, SCOPE_H + MAT_H);
    frame_blits++;

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
        /* The LFO lamp has two readings, because the page and the modulation are two
         * different things and the panel showed only the first.
         *
         *   page up            solid: the knobs are RATE and DEPTH right now
         *   page down, LFO on  blinking: the LFO is still running on a parameter you
         *                      cannot see, which is how "LFO can be heard but the page
         *                      is shut" stopped being a thing you have to remember
         *
         * A page is a view, not a switch: closing ENV does not zero the ADSR, and
         * closing LFO must not zero RATE/DEPTH either. What it must do is stop the
         * instrument from hiding a modulator that is audibly working. */
        if (lfo_page)
            lit |= 1u << B_LFO;
        else if (k_lfo_rate != 0 && k_lfo_depth != 0 && ((now >> 8) & 1u))
            lit |= 1u << B_LFO;
        if (arp_page)
            lit |= 1u << B_ARP;
        if (src != SRC_SYNTH)
            lit |= 1u << B_SEL;
        if (src == SRC_SMPL)
            lit |= 1u << B_EDIT;

        /* B_SAVE says the same thing the line under the scope says, on the button you
         * would press about it. The two read the same state and change on the same frame.
         *
         * Three levels, because that is what the matrix can show: lit, mid (felucca-bsp's
         * steady ~12 % level) and the glow every unlit button already has.
         *
         *   saved              glow, like every button at rest -- no news
         *   unsaved            mid
         *   unsaved 5 minutes  lit, the same instant the line reaches full ink
         *
         * Tying "fully lit" to SAVE_LINE_FULL_MS rather than inventing a second threshold
         * is the point: one event, said in two places. A lamp with its own clock would be
         * a second thing to keep in step, and it would be the one that drifts.
         *
         * plat_lights_mid, not plat_lights: the mid level is a separate call in the
         * platform's contract ("call it after plat_lights each time the lights change").
         * plat_leds is still the right base because it owns PLAY's green LED and the glow. */
        {
            uint32_t mid = 0;

            save_led = 0;
            if (save_dirty()) {
                save_led = save_line_step >= 3 ? 2 : 1;
                if (save_led == 2)
                    lit |= 1u << B_SAVE;
                else
                    mid |= 1u << B_SAVE;
            }
            plat_leds(lit, in_keys);
            if (mid)
                plat_lights_mid(mid, 0);
        }
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
    if (!strcmp(what, "lfo"))        { *got = lfo_page; return 1; }
    if (!strcmp(what, "arp"))        { *got = arp_page; return 1; }
    /* the fourth page: EDIT is only meaningful in SMPL, so its state is a separate
     * fact from the source and a test has to be able to read it alone */
    if (!strcmp(what, "edit"))       { *got = edit_page; return 1; }
    if (!strcmp(what, "arp_on"))     { *got = arp_on(); return 1; }
    /* The save story, as three facts a test can tell apart: flash had a record at boot,
     * RAM has a change flash does not, and how many writes have happened. The count is
     * what proves B_SAVE wrote ONCE and not once per frame. */
    if (!strcmp(what, "saved"))       { *got = save_loaded(); return 1; }
    if (!strcmp(what, "save_dirty"))  { *got = save_dirty(); return 1; }
    if (!strcmp(what, "frame_blits")) { *got = frame_blits;    return 1; }
    if (!strcmp(what, "save_line"))   { *got = save_line_len;  return 1; }
    if (!strcmp(what, "save_ink"))    { *got = save_line_step; return 1; }
    if (!strcmp(what, "save_age"))    { *got = (int)save_line_age; return 1; }
    if (!strcmp(what, "save_led"))    { *got = (int)save_led; return 1; }
    if (!strcmp(what, "save_count"))  { *got = (int)save_count(); return 1; }
    if (!strcmp(what, "save_last"))   { *got = save_last_result(); return 1; }
    if (!strcmp(what, "arp_rate"))   { *got = k_arp_rate; return 1; }
    if (!strcmp(what, "arp_type"))   { *got = k_arp_type; return 1; }
    if (!strcmp(what, "arp_oct"))    { *got = k_arp_oct; return 1; }
    if (!strcmp(what, "arp_gate"))   { *got = k_arp_gate; return 1; }
    if (!strcmp(what, "arp_held"))   { *got = arp_held; return 1; }
    if (!strcmp(what, "arp_note"))   { *got = (arp_note == 0xFFu) ? -1 : arp_note; return 1; }
    if (!strcmp(what, "arp_step"))   { *got = (int)arp_step_ms(k_arp_rate); return 1; }
    if (!strcmp(what, "tap_pk"))      { *got = scope_pk_q; return 1; }
    /* How many slots the pool gave: 0 means the app is playing without material, which
     * is a state the screen shows (NO SAMPLES) and a test has to be able to assert
     * without reading pixels. */
    if (!strcmp(what, "smp_count"))   { *got = smp_count; return 1; }
    if (!strcmp(what, "midi_seen"))   { *got = (int)midi_seen; return 1; }
    /* the velocity each sounding voice landed at. The polyphony bug this exists for:
     * four keys down and only the first is audible, which is either four voices at
     * four velocities or one voice and three near-silent ones -- and the two look the
     * same from the output level alone. vel_min is the quietest sounding voice. */
    if (!strcmp(what, "vel_min")) {
        uint32_t v; int32_t lo = 1000; int any = 0;
        for (v = 0; v < WV_VOICES; v++)
            if (dsp.v[v].active) { any = 1; if ((int32_t)dsp.v[v].vel < lo) lo = dsp.v[v].vel; }
        *got = any ? lo : 0;
        return 1;
    }
    if (!strcmp(what, "vel_max")) {
        uint32_t v; int32_t hi = 0;
        for (v = 0; v < WV_VOICES; v++)
            if (dsp.v[v].active && (int32_t)dsp.v[v].vel > hi) hi = dsp.v[v].vel;
        *got = hi;
        return 1;
    }
    if (!strcmp(what, "c2_jobs"))     { *got = (int)wv_c2_job_count(); return 1; }
    if (!strcmp(what, "c2_fails"))    { *got = (int)wv_c2_fail_count(); return 1; }
    if (!strcmp(what, "c2_peak"))     { *got = (int)(wv_c2_peak() * 4096.0f); return 1; }
    if (!strcmp(what, "split"))       { *got = (int)wv_last_split(); return 1; }
    if (!strcmp(what, "voices")) {
        uint32_t v, n = 0;
        for (v = 0; v < WV_VOICES; v++) n += dsp.v[v].active ? 1u : 0u;
        *got = (int)n;
        return 1;
    }
    if (!strcmp(what, "ftype"))       { *got = k_ftype;  return 1; }
    if (!strcmp(what, "ktrack"))      { *got = k_ktrack; return 1; }
    if (!strcmp(what, "fx_page"))     { *got = fx_page;  return 1; }
    /* SYNTH's EDIT page. `foff` is the offset of the frame K4 has selected -- the value
     * the three digit knobs are turning -- and `edit_frame` is which one that is. A test
     * that only checked one of them could not tell "the knob did nothing" from "the knob
     * edited the wrong frame". */
    if (!strcmp(what, "foff"))        { *got = foff_tab[bank_i][edit_frame]; return 1; }
    /* The pair the rack lights. Computed the same way src_rack does it, from k_morph, so a
     * test can assert WHICH frames the picture lights without reading pixels -- the curves
     * of neighbouring frames overlap in an 80 px band and a pixel probe kept counting the
     * frame above (measured: every low-MORPH reading came back "f0 f1 f2 HI"). */
    if (!strcmp(what, "rack_pair")) {
        float mf = (float)k_morph * ((float)(WV_FRAMES - 1u) / 127.0f);
        int32_t m0;
        if (mf < 0.0f) mf = 0.0f;
        if (mf > (float)(WV_FRAMES - 1u)) mf = (float)(WV_FRAMES - 1u);
        m0 = (int32_t)mf;
        *got = m0 * 16 + ((m0 + 1 < (int32_t)WV_FRAMES) ? m0 + 1 : m0);   /* m0 << 4 | m1 */
        return 1;
    }
    if (!strcmp(what, "foff_spread")) { *got = foff_spread; return 1; }
    if (!strcmp(what, "foff_shape"))  { *got = foff_shape; return 1; }
    if (!strcmp(what, "foff_sum"))    {                       /* the whole table, summed */
        int32_t i, j, t = 0;
        for (i = 0; i < WV_BANK_COUNT; i++)
            for (j = 0; j < WV_FRAMES; j++)
                t += (int32_t)foff_tab[i][j];
        *got = t; return 1;
    }
    if (!strcmp(what, "edit_frame"))  { *got = edit_frame; return 1; }
    if (!strcmp(what, "edit_src"))    { *got = edit_src; return 1; }
    /* The whole table, as a sum: 8 banks x 8 frames packed one byte each would overflow
     * the int, so this is the sum of products with a position weight. A single byte
     * moving changes it, and a byte moving to the WRONG frame changes it differently --
     * which is the injection the per-frame probe could not see, because a probe that
     * reads the frame the page says it is on agrees with a page that always writes
     * frame 0 whenever it is looking at frame 0. */
    if (!strcmp(what, "foff_hash")) {
        uint8_t b, f; int32_t h = 0;
        for (b = 0; b < WV_BANK_COUNT; b++)
            for (f = 0; f < WV_FRAMES; f++)
                h += (int32_t)foff_tab[b][f] * (1 + (int32_t)f);
        *got = h;
        return 1;
    }
    if (!strcmp(what, "foff_at"))     { *got = foff_tab[0][0]; return 1; }
    if (!strcmp(what, "foff_max")) {
        uint8_t b, f; int32_t mx = 0;
        for (b = 0; b < WV_BANK_COUNT; b++)
            for (f = 0; f < WV_FRAMES; f++)
                if ((int32_t)foff_tab[b][f] > mx) mx = foff_tab[b][f];
        *got = mx;
        return 1;
    }
    if (!strcmp(what, "lfo_rate"))   { *got = k_lfo_rate; return 1; }
    if (!strcmp(what, "lfo_depth"))  { *got = k_lfo_depth; return 1; }
    if (!strcmp(what, "bank"))       { *got = bank_i;   return 1; }
    if (!strcmp(what, "slot"))       { *got = smp_i;    return 1; }
    /* The EDIT page's own numbers. A picture can be right about a region the engine
     * never sees (test_edit.py's pixel checks prove what is drawn); these prove what the
     * voice is told, which is the half that decides whether the sound changes. Four
     * names rather than one index, because app_expect takes (what, got) only. */
    if (!strcmp(what, "region_start"))  { *got = (int)region[smp_i].start;  return 1; }
    if (!strcmp(what, "region_end"))    { *got = (int)region[smp_i].end;    return 1; }
    if (!strcmp(what, "region_loops"))  { *got = (int)region[smp_i].loop_s; return 1; }
    if (!strcmp(what, "region_loope"))  { *got = (int)region[smp_i].loop_e; return 1; }
    if (!strcmp(what, "region_on"))     { *got = region[smp_i].on; return 1; }
    if (!strcmp(what, "region_len"))    { *got = (int)smp[smp_i].len; return 1; }
    if (!strcmp(what, "zoom"))          { *got = edit_zoom; return 1; }
    if (!strcmp(what, "edit_page"))     { *got = edit_page; return 1; }
    if (!strcmp(what, "active"))     { *got = (int)wv_active(&dsp); return 1; }
    /* How many grains the newest voice has sounding, and its grain length in px of the
     * material band. The screen draws one bar per live grain, so "the picture is
     * missing a bar" and "the DSP has no grain there" are different bugs; this is the
     * only way to tell them apart from a test. */
    if (!strcmp(what, "grains")) {
        int32_t vi;
        uint32_t newest = 0, found = 0;
        *got = 0;
        for (vi = 0; vi < (int32_t)WV_VOICES; vi++)
            if (dsp.v[vi].active && dsp.v[vi].age >= newest) {
                newest = dsp.v[vi].age;
                found = 1;
            }
        if (found)
            for (vi = 0; vi < (int32_t)WV_VOICES; vi++)
                if (dsp.v[vi].active && dsp.v[vi].age == newest)
                    *got = (int)wv_grains_on(&dsp.v[vi].gr);
        return 1;
    }
    if (!strcmp(what, "grainpos")) {
        int32_t vi;
        uint32_t newest = 0, found = 0;
        *got = -1;
        for (vi = 0; vi < (int32_t)WV_VOICES; vi++)
            if (dsp.v[vi].active && dsp.v[vi].age >= newest) {
                newest = dsp.v[vi].age;
                found = 1;
            }
        if (found)
            for (vi = 0; vi < (int32_t)WV_VOICES; vi++)
                if (dsp.v[vi].active && dsp.v[vi].age == newest)
                    for (int32_t q = 0; q < (int32_t)WV_GRAINS; q++)
                        if (dsp.v[vi].gr.g[q].on) {
                            *got = (int)((float)MAT_W * dsp.v[vi].gr.g[q].pos
                                         / (float)smp[smp_i].len);
                            break;
                        }
        return 1;
    }
    if (!strcmp(what, "grainpx")) {
        int32_t vi;
        uint32_t newest = 0, found = 0;
        *got = 0;
        for (vi = 0; vi < (int32_t)WV_VOICES; vi++)
            if (dsp.v[vi].active && dsp.v[vi].age >= newest) {
                newest = dsp.v[vi].age;
                found = 1;
            }
        if (found && smp_count)
            for (vi = 0; vi < (int32_t)WV_VOICES; vi++)
                if (dsp.v[vi].active && dsp.v[vi].age == newest) {
                    const wv_grains *g = &dsp.v[vi].gr;
                    float g1 = g->g[0].on ? g->g[0].dt : g->g[1].dt;
                    if (g1 > 0.0f)
                        *got = (int)((1.0f / g1) * g->rate / ((float)smp[smp_i].len / (float)MAT_W));
                }
        return 1;
    }
    return 0;
}

#ifdef PLAT_HOST
#include <stdio.h>
/* `show WHAT`: print an observation instead of asserting it.
 *
 * `expect` is the right tool when the value is known, and the wrong one when it is
 * not: test_arp.py has to read the note the arp walked, because a correct walk is a
 * sequence and any single value it asserted would fail on a correct run half the
 * time. This prints the reading and lets the script be the thing that decides. */
int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d)
{
    (void)b; (void)c; (void)d;
    /* `plant VER` -- write a record with this many bytes and a chosen version straight
     * into the store, bypassing the app. It exists for exactly one question, the one the
     * platform's CRC cannot answer: what does this build do with an INTACT record written
     * by a different one? A restore from an older firmware is the case, and it cannot be
     * produced by the app's own SAVE. Host builds only (APP_HOST_CMD). */
    if (!strcmp(cmd, "plant") && a[0]) {
        save_rec r;
        int ver = atoi(a);
        int n = b[0] ? atoi(b) : (int)sizeof r;

        memset(&r, 0, sizeof r);
        r.magic = SAVE_MAGIC;
        r.version = (uint16_t)(ver < 0 ? 0 : ver);
        r.bytes = (uint16_t)n;
        r.cut = 3;                       /* a value no default has, so a load is visible */
        r.res = 5;
        r.arp_oct = 2;
        if (plat_store_save(OBJ_SETTINGS, &r, (uint32_t)(sizeof r < (size_t)n ? sizeof r : (size_t)n)) < 0) {
            printf("plant failed\n");
            fflush(stdout);
            return 1;
        }
        printf("planted version %d\n", ver);
        fflush(stdout);
        return 1;
    }
    if (strcmp(cmd, "show") || !a[0])
        return 0;
    {
        int got = 0;
        if (app_expect(a, &got))
            printf("show %s %d\n", a, got);
        else
            printf("show %s ?\n", a);
    }
    fflush(stdout);
    return 1;
}
#endif

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
    dbg_measure_clock();

    /* The second core, started here because app_init is the only time it may start
     * (plat.h, cpu2.c: at power-on, before the audio and the timers). Its result is
     * kept rather than checked: a unit where it does not come up renders on one core,
     * which is slower and not different -- the fallback in wv_render_split covers the
     * rest. fm1.toml has had cpu2 = true since the DSP was split in half; what was
     * missing was this call, so the core was started at boot and then never given a
     * job: the split existed and ran as one core. */
    cpu2 = plat_cpu2_set(1) == 0;
    wv_init(&dsp);
    ui_apply();
    bank_i = 0;
    bank.frames = WV_BANKS[0];
    bank.foff = foff_tab[0];
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
    /* Every slot starts as its whole self, loop off: SMPL is playable the moment
     * SOURCE is flipped, and a key plays the material once exactly as it did before
     * the EDIT page existed. */
    region_reset_all();
    edit_zoom = 0;
    edit_live = 4;                      /* 4 = no marker yet: the window is the slot */

    /* The saved state, last, because it overwrites what everything above just set. The
     * three things that must happen in this order and are easy to get wrong:
     *
     *   1. the pool is parsed first (above), so a saved region can be clamped against the
     *      slot's real length -- a record from a build whose pool had a longer slot must
     *      not leave a boundary past the end of the material;
     *   2. ui_apply() runs after the knobs are restored, or the DSP keeps the defaults
     *      while the screen shows the saved values;
     *   3. bank.frames follows bank_i, which bank_i's own restore does not do. */
    if (save_load()) {
        save_apply(save_loaded_rec());
        ui_apply();
        if (bank_i >= WV_BANK_COUNT)
            bank_i = 0;
        bank.frames = WV_BANKS[bank_i];
        if (smp_count && smp_i >= smp_count)
            smp_i = 0;
        /* A boundary past the material is the one way a record can be wrong without
         * being corrupt: the pool is a resource the installer writes, so it can change
         * under a saved record. region_fix pushes the four numbers back inside. */
        {
            uint8_t i;
            for (i = 0; i < SMP_MAX; i++)
                region_fix(i);
        }
    } else {
        /* No record in flash: the state boot just settled on IS "saved".
         *
         * Nothing has been edited yet, so there is nothing to save, and the player should
         * not be told to save a sound they never touched.
         *
         * This is not cosmetic. The boot defaults are not all zero -- region_reset_all()
         * gives every slot its whole self (region_end is the slot's length, 52369 for the
         * demo PAD) while save_load() leaves its baseline zeroed. So the first scan
         * compared a real record against an empty one and reported "unsaved" from the
         * first frame. Measured: `expect save_dirty 0` passed before any frame ran and
         * read 1 after `wait 100`.
         *
         * That was already true before the save line existed, and it was invisible because
         * the old mark was two words nobody had asked the meaning of at boot. A bar whose
         * length is the distance to flash cannot stand on top of it.
         *
         * Placed last in app_init, not at the end of the save_load() block above and not at
         * the first frame: everything that settles the boot state has to have run, and on
         * the host a script's first command runs after app_init but before the first
         * frame -- adopting at the first frame swallowed that command as part of the
         * baseline (measured: `spin K1 -34` then `expect save_dirty 1` read 0). */
        save_rec r;
        save_snapshot(&r);
        save_adopt(&r);
    }
}

/* app.c's statics -> the record. The one place that knows the mapping: writing it out
 * twice (once for the save, once for the dirty scan) is how the two drift apart. */
static void save_snapshot(save_rec *r)
{
    uint8_t i;

    memset(r, 0, sizeof *r);
    r->cut = k_cut;
    r->res = k_res;
    r->shape = k_shape;
    r->sh_amount = k_amount;
    r->morph = k_morph;
    r->grain = k_grain;
    r->spray = k_spray;
    r->eg_a = k_eg_a;
    r->eg_d = k_eg_d;
    r->eg_s = k_eg_s;
    r->eg_r = k_eg_r;
    r->lfo_rate = k_lfo_rate;
    r->lfo_depth = k_lfo_depth;
    r->ftype = k_ftype;
    r->ktrack = k_ktrack;
    r->arp_rate = k_arp_rate;
    r->arp_type = k_arp_type;
    r->arp_oct = k_arp_oct;
    r->arp_gate = k_arp_gate;
    r->bank_i = bank_i;
    r->smp_i = smp_i;
    r->src = src;
    for (i = 0; i < SAVE_SLOTS; i++) {
        r->region[i].start = region[i].start;
        r->region[i].end = region[i].end;
        r->region[i].loop_s = region[i].loop_s;
        r->region[i].loop_e = region[i].loop_e;
        r->region[i].on = region[i].on;
    }
    {
        uint8_t b, f;
        for (b = 0; b < SAVE_BANKS; b++)
            for (f = 0; f < 8; f++)
                r->foff[b][f] = (b < WV_BANK_COUNT && f < WV_FRAMES)
                                ? foff_tab[b][f] : 0u;
        r->foff_spread = foff_spread;
        r->foff_shape = foff_shape;
    }
}

/* the record -> app.c's statics. The inverse of save_snapshot, and it has to stay one:
 * a field added to one and not the other is a parameter that saves and does not come
 * back. */
static void save_apply(const save_rec *r)
{
    uint8_t i;

    k_cut = r->cut;
    k_res = r->res;
    k_shape = r->shape;
    k_amount = r->sh_amount;
    k_morph = r->morph;
    k_grain = r->grain;
    k_spray = r->spray;
    k_eg_a = r->eg_a;
    k_eg_d = r->eg_d;
    k_eg_s = r->eg_s;
    k_eg_r = r->eg_r;
    k_lfo_rate = r->lfo_rate;
    k_lfo_depth = r->lfo_depth;
    k_ftype = r->ftype;
    k_ktrack = r->ktrack;
    k_arp_rate = r->arp_rate;
    k_arp_type = r->arp_type;
    k_arp_oct = r->arp_oct;
    k_arp_gate = r->arp_gate;
    bank_i = r->bank_i;
    smp_i = r->smp_i;
    src = r->src;
    for (i = 0; i < SAVE_SLOTS; i++) {
        region[i].start = r->region[i].start;
        region[i].end = r->region[i].end;
        region[i].loop_s = r->region[i].loop_s;
        region[i].loop_e = r->region[i].loop_e;
        region[i].on = r->region[i].on;
    }
    {
        uint8_t b, f;
        for (b = 0; b < WV_BANK_COUNT; b++)
            for (f = 0; f < WV_FRAMES; f++)
                foff_tab[b][f] = (b < SAVE_BANKS && f < 8) ? r->foff[b][f] : 0u;
        foff_spread = r->foff_spread > 127u ? 0u : r->foff_spread;
        foff_shape = r->foff_shape < FOFF_SHAPE_N ? r->foff_shape : 0u;
    }
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

/* The pool is read once at boot, and until now that was the whole story: resources
 * arrive over USB from the host, and the platform reloads the app after it writes
 * them (rs_begin calls app_panic, then app_ui_init), but this app's own copy of
 * `smp_count` was taken in app_init and never taken again. Install a pool into a
 * running FM-1 and it plays NO SAMPLES until the next power cycle -- the transfer
 * reports success, the region checks out, and the instrument denies having any
 * material.
 *
 * So: while there is no material, ask again. plat_res re-reads the region's commit
 * record every call (res.c: res_ok is refreshed by res_check after a write), so this
 * costs a flash header read per tick and nothing at all once a pool is there. It
 * cannot resurrect a pool that went bad -- that needs a boot -- but a pool that
 * arrived cannot be missed. */
static void pool_poll(void)
{
    if (smp_count)
        return;
    pool = plat_res(&pool_len);
    smp_count = pool ? (uint8_t)wv_smp_parse(pool, pool_len, smp, SMP_MAX) : 0;
    if (smp_count) {
        smp_i = 0;
        region_reset_all();
    }
}

void app_tick(void)
{
    /* the arpeggiator's clock. touch_update lives here too (pressure, every 5 ms);
     * touch_poll and pool_poll do NOT -- they need to run whether or not the tick
     * does, and the tick is the one hook a header can forget to enable. */
    arp_tick();
    touch_update();
}

