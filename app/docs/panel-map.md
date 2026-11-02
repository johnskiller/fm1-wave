# Panel map

The FM-1 has **7 encoders and 14 buttons**. This file is the authority on what
each one does in fm1-wave **right now**. `docs/spec.md` §3 says what the panel is
*meant* to be; this file says what is wired.

Read this before assigning a control. Do not re-derive it from `app.c` — the point
of the file is that the answer is in one place. **And it is checked**:
`app/tests/panel.txt` asserts every row of the two tables below against the running
app, so a change that contradicts this file fails a build rather than drifting.

Three sources own a control, and the difference matters:

- **app** — fm1-wave's own code (`app/app.c`). Changeable here.
- **platform** — the platform reads it before or beside the app
  (`platform/device/*.c`). Taking it from the app means breaking a platform
  feature, so it is listed as unavailable rather than free.
- **free** — nothing reads it anywhere in this build.

---

## Encoders

| Encoder | Printed | SYNTH | SMPL | GRAN | Owner | app.c |
|---|---|---|---|---|---|---|
| `EN_SELECT` | SELECT | — | — | **SPRAY** 0–127 | app | 47 |
| `EN_ALGO` | ALGORITHM | **MORPH** 0–127 | **MORPH** *(no effect)* | **GRAIN SIZE** 10–200 ms | app | 47 |
| `EN_PRESET` | PRESETS | **BANK** 0–7, detented | **SLOT** | **SLOT** | app | 47 |
| `EN_K1` | KNOB 1 | **CUT** | CUT | CUT | app | 47 |
| `EN_K2` | KNOB 2 | **RES** | RES | RES | app | 47 |
| `EN_K3` | KNOB 3 | **SHAPE** 0–3 detented | SHAPE | SHAPE | app | 47 |
| `EN_K4` | KNOB 4 | **AMOUNT** | AMOUNT | AMOUNT | app | 47 |

**K1–K4 never move.** They are CUT / RES / SHAPE / AMOUNT in every source, because
a grain voice runs through the *same* shaper → SVF → VCA as a table voice
(`voice_block`): the source is one branch, everything after it is shared. Taking the
filter off GRAN would remove it from the source that needs it most, since grain
material is bright and broken up by construction.

**`EN_ALGO` is the one encoder that changes meaning with the source**, and only in
GRAN. The arithmetic forced it: GRAN needs seven controls (SPRAY, GRAIN SIZE, SLOT,
CUT, RES, SHAPE, AMOUNT) and the panel has seven encoders. MORPH is the only one of
the seven that has nothing to read in a grain voice — `wv_table_read` is its sole
consumer and a cloud never reaches it — so it is the one that yields. In SYNTH and
SMPL it is MORPH and never moves.

**All knobs are relative.** They accumulate detents from wherever they are; there is
no absolute position to jump to. So a value survives a source change, and a knob
does not have to be swept back.

### Encoder detail

- **SELECT** is free in SYNTH and SMPL, SPRAY in GRAN.
- **ALGORITHM** moves 8 per detent for MORPH (16 detents across 8 frames) and 4 per
  detent for GRAIN SIZE.
- **PRESETS** is detented in SYNTH (8 banks, wrap-around). In SMPL and GRAN it is a
  material slot picker: a detent loads the next slot rather than sweeping a range.
- **CUT / RES / SHAPE / AMOUNT** are live in every source.

---

## Pages

A **page** is not a mode, and the distinction is what makes the layout legal:

- a page has a **button that is lit while it is up**
- it owns a **fixed set of knobs**, and leaving it always returns every one of them
  to the parameter it had
- **no value is lost** — the page edits its own parameters, not the knobs'

That is why the envelope is allowed to take K1–K4 while GRAIN SIZE is not: there is
a button that says "you are editing the envelope", and there is no button that says
"you are editing the grain". GRAIN SIZE is a permanent property of one source, so it
needed a permanent knob, and `EN_ALGO` was it.

### `B_ENV` — the envelope page

| | K1 | K2 | K3 | K4 | SELECT | ALGO | PRESET |
|---|---|---|---|---|---|---|---|
| page up | **ATTACK** 1 ms–4 s | **DECAY** 2 ms–8 s | **SUSTAIN** 0–127 | **RELEASE** 2 ms–8 s | unchanged | unchanged | unchanged |

Any source. `B_ENV` toggles. `SELECT`, `ALGO` and `PRESET` keep doing what the
current source says, because a page only takes the knobs it needs.

Times are exponential (like CUT and GRAIN SIZE) and are shown as **ms below a
second, seconds above it** — a percentage is meaningless for a time, since "37" on
an attack could be 37 ms or 37% of four seconds.

Defaults are `A=0, D=64, S=127, R=32`: a ~1 ms attack, full sustain (so decay has
nowhere to go) and a ~30 ms release. That reproduces the hardcoded VCA this replaced,
so **a patch made before the envelope existed sounds the same after it**.

---

## Buttons

| Button | Printed | Current function | Owner | Where |
|---|---|---|---|---|
| `B_SEL` | SEL | **SOURCE** cycles SYNTH → SMPL → GRAN | app | app.c:47 |
| `B_ENV` | ENV | **ENV page** toggle (A/D/S/R on K1–K4) | app | app.c:47 |
| `B_EDIT` | EDIT | **re-chop** the current slot; dead in SYNTH and GRAN | app | app.c:47 |
| `B_FX` | FX | nothing | **free** | — |
| `B_LFO` | LFO | nothing | **free** | — |
| `B_GLO` | GLO | nothing | **free** | — |
| `B_SAVE` | SAVE | nothing | **free** | — |
| `B_ARP` | ARP | nothing (not read anywhere in the platform) | **free** | — |
| `B_SEQ` | SEQ | nothing (not read anywhere in the platform) | **free** | — |
| `B_PLAY` | PLAY | **safe mode only**: a second failed boot, then PLAY = "try again" | platform | main_fm1.c:818 |
| `B_REC` | REC | **power-on only**: held at boot = UPDATE MODE | platform | main_fm1.c:968 |
| `B_HOME` | HOME | **RESTORE prompt only**: "no" | platform | unity.c:340 |
| `B_OCT-` | OCT- | **power-on only**: with OCT+ = hardware calibration | platform | main_fm1.c:966 |
| `B_OCT+` | OCT+ | **power-on only**: same | platform | main_fm1.c:966 |

### How much the platform ones really cost

Not all "platform" is equal, and it decides whether a button is usable:

- **`B_REC` and the OCT pair are read only at power-on.** Using them while the
  instrument runs breaks nothing.
- **`B_HOME` is read only inside the RESTORE prompt**, which appears only during a
  restore. Otherwise completely idle.
- **`B_PLAY` is not just a lamp** — the earlier note in this file said it was, and
  that was wrong. `safe_main()` reads it: after two failed boots the firmware parks
  in safe mode where PLAY means "start the app again". That mode is never entered in
  normal use, but a PANIC assignment must not go here: when the instrument will not
  start, that is the button you need.

So: **only `B_REC` and the OCT pair are genuinely owned**, and only for a moment at
power-on.

---

## Screen layout

240 x 240, drawn as three canvases (`cv_begin` / `cv_blit`, one pair per band, because
each canvas is a fixed-size pool and the bands are different heights):

```
  0.. 83   scope      84 px   the live waveform, fixed linear scale, 0 dBFS = band edge
 84..175   material   92 px   the table's own shape (SYNTH); icon + material name otherwise
176..239   the four   64 px   K1..K4: CUT RES SHAPE AMOUNT, or A D S R on the ENV page
```

**These are derived, not filled in.** `app/app.c` computes them from what each band has
to draw and refuses to compile if they do not sum to 240:

```c
#define MAT_PAD    6
#define MAT_WAVE   80
#define MAT_H      (MAT_PAD + MAT_WAVE + MAT_PAD)     /* 92 */
#define SCOPE_H    (240 - MAT_H - KNOB_H)             /* 84 */
#define SCOPE_MID  (SCOPE_H / 2)                      /* 42 */
#define SCOPE_HALF ((SCOPE_H - 2) / 2)                /* 41 */
#define KNOB_TOP   4
#define KNOB_INK   (KNOB_TOP + 34 + 8)                /* 46 */
#define KNOB_H     64
#if (MAT_H + KNOB_H + SCOPE_H) != 240
#error "the three bands must fill the 240 px screen"
#endif
```

### The clipping bug this replaced (and why it looked like overlap)

The heights started as literals. When the material band was cut from 108 to 84 px, only
`cv_begin` and `cv_blit` were changed -- every coordinate inside stayed at its 108-era
value:

```
src_shape(&sk, 4, 4, 176, 100);   // draws to y = 103, inside an 84 px canvas
cv_text(4, 92, ... "ENV PAGE");   // entirely outside an 84 px canvas
```

`gfx.c` clips at the canvas edge (`cv_rect`: `if (y1 > cv_y1) y1 = cv_y1;`) with no
warning, so this is not a crash and not a drawing artefact: the bottom 20 px of the
picture and the whole "ENV PAGE" marker simply did not appear. On a 240 px screen that
reads as "the knobs are covering it", which is how it was reported.

`app/tests/test_screen.py` now checks by pixel that each band stays inside its own rows,
that the material band clears the knobs band, and that the table picture uses its full
height. Verified against the bug by injecting it: with `MAT_H` forced to 75 the material
ink ran to row 175 (the knobs boundary) and 5 checks failed.

Measured, with the derivation in place:

```
band       rows        ink extent
scope      0..83       33..50    (the trace; centre line at 42)
material   84..175     91..169   SYNTH (79 px of the 80 px picture), 91..171 ENV/GRAN, 94..160 SMPL
knobs      176..239    183..221  (39 px of ink, 18 px spare for the round knob art)
```

### How these heights got here

They are not arbitrary, and they are not computed from content either -- they are
whatever each band was squeezed to. The history, from `git show <rev>:app/app.c`:

```
920761e  112 + 68              = 180  (60 px unused; no bottom band yet)
fecd1a3  112 + 68 + 60         = 240  (the strip appears)
9743f27   68 + 56 + 76 + 40    = 240  (scope cut 112 -> 68 to fit two knob rows)
bb55fd3   68 + 108 + 64        = 240  (material takes the middle, knobs to the bottom)
cea807b   92 + 84 + 64         = 240  (scope takes 24 px back from material -- CLIPPED)
this      84 + 92 + 64         = 240  (derived; the material's contents fit again)
```

The 92/84 split is the first one chosen from a measurement rather than from "make it
fit": the scope gained 24 px because a fixed scale needs the height (see below), and the
material kept 84 px because at 84 the table still uses 76 px of vertical extent, which
was the thing the 28 px strip failed to do at 13.

### Why the scope has a fixed scale, and why not dB

It had a per-frame peak normaliser, and the measurement that condemned it is that
velocity 20, 60 and 127 all drew **63 px**: an auto-gained scope carries no level at all.

A dB axis is the obvious replacement and it is wrong for a *trace*: the instantaneous
value of a waveform crosses zero twice per cycle, and in dB a zero crossing is
-infinity, so the trace plunges to the floor at every crossing. Tried and measured:
**47 px of ink in each of the 240 columns** -- a filled band, the same defect the
original `fabsf` peak code had.

So the scale is **linear in amplitude and fixed**: full scale is the band edge, 46 px
each side of the centre line, pre-master (MASTER is the listening level; a scope that
shrank when the pot turned down would be measuring the wrong thing). The DSP's own
numbers then read directly:

```
1 voice  v127   0.256 of full scale   -> 11.8 px/side   (measured 20 px span)
2 voices v127   0.512                 -> 23.5           (measured 34)
3 voices v127   0.744                 -> 34.2           (measured 35)
8 voices v127   1.316                 -> clipped to edge (measured 41)
```

Amplitude now IS level: velocity, envelope, polyphony and the shaper all move it. The
8-voice case exceeds the band, which is correct for a scope -- the signal is above 0
dBFS and the drawing says so.

**Velocity cannot be tested on the host**: `touch_vel()` is the app's only velocity path
and `APP_KEY_SENSE` is not enabled, so `key <n> down <vel>` changes nothing. Polyphony is
the stand-in, and the DSP-level sweep (velocity 1 -> 0.00201, 127 -> 0.25586, linear in
amplitude) is measured directly in a throwaway harness instead.

The ring is `int16` (units of full scale) rather than `int8`: a fixed scale has to see
past 0 dBFS, and eight voices measure +2.4 dBFS. 768 samples of int16 is 1536 B of the
98304 available.

### The scope graticule

The band is a scope screen, not a plot, so it has a grid: five vertical lines (every
20 px) and the horizontal dB markers of the linear scale. The dB lines are NOT equally
spaced, and that is the point, not a bug: the scale is linear in amplitude, so 0 dBFS
is the band edge and the lines fall at

```
0 dB    row 1   (the band edge)
-10 dB  row 29  (28 px in)
-20 dB  row 37
-30 dB  row 40
-40 dB  row 41
-50 dB  row 41  (coincides with -40: 41.6 and 41.9 both round to 41)
```

Denser near the centre, sparser at the top -- the signature of a dB scale. The bottom
two markers fall on the same row and draw as one, which is honest: on a 41 px half-band
a dB scale resolves six markers down to five visible lines, and a scope that cannot
distinguish -40 from -50 at this depth should not pretend to. The measured rows (1, 29,
37, 40, 41 above the centre at 42) match this exactly. The only
label is "+0" at the right edge, in the top row: the 0 dBFS line is the reference a
loud chord visibly crosses, and the rest is read off the spacing, because five words
of six px in a 2 px gap would not read as a scale but as noise.

The grid is a fixed neutral grey (`RGB(96,99,104)` for the 0 dBFS line, `RGB(58,61,66)`
for the rest), not a skin colour: it must sit under all three skins, and a graticule
tinted by the source would be content, not the glass.

Both were raised after being measured and found invisible: the dim one was
`RGB(36,38,42)`, **7 units of luminance above the darkest ground**, so the vertical lines
and five of the six dB markers did not read as a grid at all. Now `dLum 29-31` for the
dim lines and `67-69` for the 0 dBFS line, against ink at `150-185` -- so the grid is
visible without competing with the trace. A graticule that cannot be seen is not a subtle
graticule, it is a missing one.

The trace runs over it (drawn after), which is how a real scope looks: the grid is
behind the signal.

### The material band

The table's own shape, 176 x 100 in SYNTH, with BANK and MORPH printed in the right
column because they are exactly the two parameters it draws. Those two lost their cells
in `bb55fd3` for that reason: a number beside a waveform that shows that number's effect
is a worse use of a cell than the waveform at a readable size.

At 28 px (the first version) the wave used 13 px of height and the MORPH steps measured
0.59 px apart. At 100 px they measure 2.90 / 6.74 / 12.00 / 19.44 px.

SMPL and GRAN have no picture yet: the band carries the source icon and the material's
name. A sample needs a min/max envelope (8800 samples into 176 columns: transients
visible, individual cycles not) and GRAN needs the grain window and its overlap. Neither
has a design, and the waveform shape GRAN would get is already its icon.

## Source indication

SOURCE has **three** states and `B_SEL`'s lamp has **two**, so the screen says it. The
whole panel is coloured by source and one icon is drawn:

| source | ground | ink | icon |
|---|---|---|---|
| SYNTH | `RGB(46,30,8)` | amber `RGB(232,136,30)` / `255,184,70` | a saw, two cycles |
| SMPL | `RGB(13,41,22)` | green `RGB(80,220,120)` / `150,255,175` | a sine with a decay |
| GRAN | `RGB(8,38,52)` | cyan `RGB(60,180,235)` / `150,225,255` | a sine under scattered dots |

All three follow one structure: the ground is a dimmed version of the hue, `ink` is the
hue, `ink_hi` is its highlight step, and white is the accent for "what is being touched"
only. SYNTH used to be the exception -- black ground, neutral grey labels, amber only on
the curve -- on the argument that overriding the palette would fight `GLOBAL`'s THEME.
It does not: three fixed source colours and a user-selectable theme cannot both be true,
and the three colours are what the panel is for, so THEME no longer reaches this screen.
Half-theme/half-fixed was the worst of both: the source colour looked like an
afterthought on the source you spend the most time in.

Two earlier versions were wrong and are recorded here because the reason matters more
than the result:

1. **Only the scope band was tinted.** The frame is three canvases, so two thirds of
   the screen stayed black and took the eye: three distinct near-blacks, measured,
   and invisible in use.
2. **Three icons were drawn with two dimmed.** Only one source is ever live, so the
   dim ones were decoration in the one place the eye rests.

Where the colours come from: `palette_set()` runs once at boot from `APP_PALETTE`, so
the theme cannot follow the source — but `cv_begin`'s background and `cv_text`'s
colour are raw RGB565, not palette indices, so the app colours its own screen without
the platform owning three palettes. The hues are the palette's own highlight steps
(`gfx.c` PALETTES), so no second colour language is learned. What changes per source
is the role the hue plays: SYNTH keeps the theme's dark ground, SMPL swaps the same
two colours (amber ground, amber ink), GRAN puts the hue in the ground.

The icons share one idea — what a key reads — and differ in continuity, which is the
musical difference: a saw is continuous (the table being scanned), a sine with a decay
exists once and fades (a sample), a sine under a scatter is the same waveform broken
up (grains). The first SYNTH icon was three stacked steps, arguing from the data
layout: a table has eight frames. But a table read for one note is one waveform — the
frames are what MORPH chooses between, not what SOURCE selects — so it described
storage, not sound.

Measured, as the dominant colour of each of the three bands: SYNTH `(0,0,0)` on all
three, SMPL `(49,32,0)` on all three, GRAN `(8,36,49)` on all three. Knob-band ink
follows: `(148,149,156)` / `(238,137,24)` / `(57,182,238)`.

---

## Lights

`app.c` used to call `plat_lights(0, 0, 0, 0, 0)` every 33 ms — a template
placeholder that turned everything off, including PLAY's green LED that `plat_leds`
would have kept on. **Correction: as of this version it still does.** No button
lights up when pressed, and that has to change before any assignment relies on an
LED saying "this control is active" — including `B_ENV`, whose whole justification
as a page is that its button is lit while the page is up.

---

## What is not assigned

| Function | State | Why unassigned |
|---|---|---|
| **PANIC** / all-notes-off | not implemented | `B_HOME` is the natural home (idle except during restore) |
| Material page | not implemented | no button of its own; spec's `BTN_BANK` names a control this panel does not have |
| Octave shift | not implemented | `B_OCT-` / `B_OCT+`, power-on conflict only |
| Glide / portamento | not in the DSP | `B_GLO` is free |
| LFO | not in the DSP | `B_LFO` is free |
| Filter type switch | not in the DSP (SVF low-pass only) | — |
| Panic on MIDI | — | — |

`B_BANK` **does not exist** on this panel: the platform's button enum is
`B_FX, B_SEL, B_ENV, B_LFO, B_EDIT, B_GLO, B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY,
B_REC, B_OCTDN, B_OCTUP`. `B_ENV` is the envelope page; the material page has no
button.

---

## Free buttons, and what each could carry

Six buttons are entirely unread. The suggestions are grouped by **cost**, because
there are more free buttons than pending functions.

| Button | Could carry | Cost / caveat |
|---|---|---|
| `B_HOME` | **PANIC** — all notes off | cheapest useful thing on the panel; idle except during restore |
| `B_GLO` | **glide / portamento** on or off | needs a per-voice pitch slew, ~20 lines |
| `B_SAVE` | save the current knob set as the boot state | `plat_store_save` exists; the settings object is already there |
| `B_FX` | a second waveshaper stage, or a filter-type switch | no DSP behind it yet |
| `B_LFO` | LFO rate/depth page | no LFO in the DSP |
| `B_ARP` | arpeggiator | needs a clock and a note queue; not small |
| `B_SEQ` | step sequencer | larger still; that is Saadet's territory |

**If only one is done**: `B_HOME` → PANIC. It costs a dozen lines and fixes the one
failure mode that makes an instrument unusable in front of people.

---

## Keys

27 keys, F3–G5 (`plat.h: NKEYS 27`). In SYNTH and GRAN every key is a note. In
SMPL the mapping is two-part:

- A key at or above `SLICE_BASE` (note 53, the lowest) **and** below
  `SLICE_BASE + slice_count` plays that chop at 1×, from the chop's own start.
- Every other key — including all of them when nothing has been chopped — falls
  back to `wv_note_on_sample`.

**Known defect in that fallback**: it always plays `smp[0]`, not the slot
`EN_PRESET` selected. The chop path is correct (it uses `slices_from`); only the
fallback hardcodes index 0.

---

## How this file is checked

```
python3 platform/tools/build.py app --host
app/build/host/wave_host app/tests/panel.txt app/build/host
```

`app/tests/panel.txt` asserts the two tables above against the app's own state
through `app_expect` (`app/app.h`). It expects **exact** values, not bounds: every
encoder is relative and one detent is known (K1–K4 = 4, ALGO = 8 for MORPH and 4
for GRAIN SIZE, SELECT = 4), so a knob that moves by the wrong amount fails.

Verified that the script can fail: replacing GRAIN SIZE's branch on `EN_ALGO` with
MORPH's produces three failures and names `grain`.
