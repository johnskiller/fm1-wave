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

240 x 240, drawn as four canvases (`cv_begin` / `cv_blit`, one pair per band, because
each canvas is a fixed-size pool and the bands are different heights):

```
  0.. 68   scope        the live waveform, auto-gained to the band
 68..124   row A        K1..K4      four cells of 60 px
124..200   row B        encoders    three cells of 80 px
200..240   strip        source icon (left) + BANK/SLOT (right)
```

**Row A** is `CUT RES SHAPE AMOUNT`, or `ATTACK DECAY SUSTAIN RELEASE` on the ENV
page. **Row B** is the three encoder positions, each cell where its knob is:

```
  cell 1  EN_PRESET   BANK (SYNTH) / SLOT (SMPL, GRAN)   + index bar
  cell 2  EN_SELECT   SPRAY in GRAN; empty otherwise
  cell 3  EN_ALGO     MORPH (SYNTH, SMPL) / GRAIN SIZE (GRAN)
```

The physical knob order is the screen's order, which is the rule this panel has kept
since the first layout:

```
MASTER     SELECT        (upper pair, screen's left)
PRESETS    ALGORITHM     (lower pair)
```

`MASTER` is a pot, not an encoder -- `plat_master()` returns 0..4096 and it reaches the
render as the gain, so no cell is spent on it. BANK and MORPH were the other way round
in the first two-row layout, which put BANK on the side ALGORITHM is not; the code
cannot tell (the platform records matrix ids, ALGORITHM 1 and PRESETS 6, not
left/right), so the order comes from the panel in front of the player.

Cell 2 used to hold a SOURCE selector. That was the strip's icon written out in three
more words -- the icon names the source, and the ground and ink say it twice more --
so it cost a cell that GRAN could use. SPRAY moved there from the SOURCE cell. In
SYNTH and SMPL the cell is empty, including on the ENV page: the strip already says
`ENV PAGE`, and saying it in both places drew the identical string twice.

**The strip** (200..240) is the source icon in the left corner and, on the right, only
what the panel cannot show any other way: `ENV PAGE` while the page is up, or
`NO SAMPLES` if the sample sources have no material.

Why two rows: seven encoders across 240 px is 34 px each, and `AMOUNT` at FONT_XS
measures 43 px — measured on the host by scanning the label rows for ink runs, it ran
9 px into the next cell. Shortening the names fixes that one collision and leaves
every label at four or five characters; two rows keeps the full names.

Why the scope gave up height (112 -> 68): 112 + 56 + 76 + 40 = 284, which is 44 more
than the screen has. The scope is the one band whose height is not load-bearing —
the auto-gain fills whatever band it is given — so it paid.

Measured after the change, per source, ink in the strip's three thirds:
`icon(0-64) / BANK(64-146) / SLOT(146-240)` = 98/293/0 (SYN), 84/293/228 (SMP),
189/293/228 (GRN). No overlap, and SYNTH carries no SLOT — that number is a sample
fact and the table path has no slots.

## Source indication

SOURCE has **three** states and `B_SEL`'s lamp has **two**, so the screen says it. The
whole panel is coloured by source and one icon is drawn:

| source | ground | ink | icon |
|---|---|---|---|
| SYNTH | black | the palette's own amber (theme untouched) | a saw, two cycles |
| SMPL | `RGB(52,34,6)` | amber `RGB(232,136,30)` / `255,184,70` | a sine with a decay |
| GRAN | `RGB(8,38,52)` | cyan `RGB(60,180,235)` / `150,225,255` | a sine under scattered dots |

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
