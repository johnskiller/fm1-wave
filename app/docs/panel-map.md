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
| `EN_SELECT` | SELECT | **BANK** 0–7, detented | **SLOT** | **SLOT** | app | 47 |
| `EN_ALGO` | ALGORITHM | **MORPH** 0–127 | **MORPH** *(no effect)* | **GRAIN SIZE** 10–200 ms | app | 47 |
| `EN_PRESET` | PRESETS | — | — | **SPRAY** 0–127 | app | 47 |
| `EN_K1` | KNOB 1 | **CUT** | CUT | CUT | app | 47 |
| `EN_K2` | KNOB 2 | **RES** | RES | RES | app | 47 |
| `EN_K3` | KNOB 3 | **SHAPE** 0–3 detented | SHAPE | SHAPE | app | 47 |
| `EN_K4` | KNOB 4 | **AMOUNT** | AMOUNT | AMOUNT | app | 47 |

**Why SELECT holds the material.** SELECT is the knob whose name says "which thing is
loaded", so it holds the material in every source: BANK in SYNTH, SLOT in SMPL and GRAN.
It used to hold SPRAY in GRAN only, because the material had to live on PRESETS -- which
meant the knob marked SELECT did nothing in two of three modes while the knob marked
PRESETS picked the bank. Both names were wrong for what they did.

With the material on SELECT, SPRAY moves to PRESETS and GRAIN SIZE stays on ALGORITHM.
That is also the better hand arrangement: PRESETS is bottom-left and ALGORITHM is
bottom-right, so GRAN's two most-turned knobs are the two lower knobs.

**The screen is laid out as the panel is.** Each value is printed in the corner its own
knob occupies on the panel -- SELECT top-right, PRESETS bottom-left, ALGORITHM
bottom-right -- so a label is found by where it is, not by reading it. Every label carries
its parameter's NAME as well as the value (`BANK 1/8`, `SPRAY 0`, `GRAIN 45ms`): the same
corner means different things in different sources, so the position alone is not a name.

SMPL and GRAN run the picture the full 232 px with the labels printed over it, which the
player accepted because their envelope is at 2/5 ink. SYNTH gives up the right 60 px
instead -- its frame is drawn at full ink and bright text on it fights. See the material
band section below for the measurement behind 60.

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

- **SELECT** picks the material: BANK in SYNTH (detented, 8 banks, wrap-around), SLOT in
  SMPL and GRAN (a detent loads the next slot rather than sweeping a range).
- **ALGORITHM** moves 8 per detent for MORPH (16 detents across 8 frames) and 4 per
  detent for GRAIN SIZE. Dead in SMPL.
- **PRESETS** is SPRAY in GRAN, LOOP ON/OFF in the SMPL EDIT page, and does nothing in
  SYNTH and in SMPL outside that page.
- **ALGORITHM** is ZOOM in the SMPL EDIT page: left zooms out, right zooms in, 8 steps
  from the whole slot to 128x.
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

### `B_LFO` — the LFO page

| | K1 | K2 | K3 | K4 | SELECT | ALGO | PRESET |
|---|---|---|---|---|---|---|---|
| page up | CUT | RES | **RATE** 0.02–20 Hz | **DEPTH** 0–4 oct | unchanged | unchanged | unchanged |

Any source. `B_LFO` toggles, and the two pages are mutually exclusive: opening one
closes the other, so the lit lamp is never ambiguous and which parameter a knob edits
never depends on the order the buttons were pressed.

**The page takes K3–K4 and leaves CUT and RES alone**, which is the opposite of what
the ENV page does. The reason is that CUT is what the LFO sweeps: with the page up you
want to watch the filter you are modulating, and CUT sitting on K1 on every page is the
one thing the panel must not move. SHAPE and AMOUNT are what give way — they are the
pair a filter LFO does not change, and they are one button away.

`RATE` prints **Hz**, because a rate in percent is not a rate. `DEPTH` prints
**octaves**, because CUT is exponential over nine octaves and a depth in Hz would mean
different things at different CUT settings.

The LFO is **one oscillator for the whole instrument**, not one per voice: an LFO is
not a voice property, and eight copies of the same sine is eight times the cost for one
sine. Every voice is swept together, which is also what a player expects from a filter
LFO. Its phase advances **once per block, in `smooth_step`** — `voice_block` runs twice
a block (once per core), so advancing it there would run the LFO at double speed
whenever the second core was up.

The waveform is a sine and the target is CUT; there is no destination matrix and no
tempo sync. This panel has seven encoders and four permanent parameters, and a matrix
that cannot show DEST and AMOUNT for each route on a 240×240 screen would spend the
parameters it has.

Defaults are `RATE 0, DEPTH 0` — **off** — so a patch made before the LFO existed
sounds the same after it.

### `B_ARP` — the arpeggiator page

| | K1 | K2 | K3 | K4 | SELECT | ALGO | PRESET |
|---|---|---|---|---|---|---|---|
| page up | **RATE** off / 1/4 … 1/64 | **TYPE** UP·DOWN·UP-DN·RND | **OCT** 1–4 | **GATE** 1–100% | unchanged | unchanged | unchanged |

Any source. `B_ARP` toggles, and it is the third of the mutually exclusive pages: ENV,
LFO and ARP each close the other two.

**`RATE` doubles as the switch.** Zero is off, which is also the default, so an
arpeggiator that has never been turned on cannot change how an existing patch plays.
The divisions are at **120 BPM** — a beat is 500 ms, so 1/4 is 500 and each plain
division halves it (250, 125, 62); a triplet is two thirds of the plain value it sits
between (167, 83). The readout prints the division, because a division is what the
player is choosing and a length in milliseconds is not.

**This is a clock, not a free-Hz knob.** An arpeggiator that will not sit on a beat is
not an arpeggiator. The platform receives MIDI clock but does not track tempo yet, so
the tempo is fixed at 120 and following a clock is a second stage.

What the arp outputs is **only the held notes, transposed by whole octaves**. It never
invents a pitch: a note outside the chord is a sequencer's job, and `B_SEQ` is still
the empty button on this panel.

`TYPE` walks the held set **sorted by pitch**, not by the order the fingers landed:

| TYPE | walk for a three-note chord |
|---|---|
| UP | 0 1 2 0 1 2 … |
| DOWN | 2 1 0 2 1 0 … |
| UP-DN | 0 1 2 1 0 1 2 … (the ends are not played twice, so the cycle is `2n-2`) |
| RND | any of the held notes, chosen by an LCG |

`OCT` repeats the whole set 1–4 octaves up before wrapping, and is clamped there: a
fifth octave would leave the keyboard range.

`GATE` is the note's length as a fraction of the step, 1–100%. It is on a knob rather
than fixed because it is what makes an arp sound staccato or legato, and it is the
parameter a player reaches for first.

**The panel's keys and MIDI share one set and one engine.** With the arp on, a key does
not play a voice — it enters the held set, and the clock plays it. A note from USB, BLE
or the TRS jack enters the same set. This is the lesson from the MIDI-in work: one
engine, one sound, or the instrument has two personalities.

The held set is **eight notes**, and a ninth gives way to the oldest — the same rule the
voices use, so holding more keys than the instrument can sound still does something
predictable rather than something different.

The arp's clock is the platform's 1 kHz `app_tick`. That hook needs `APP_TICK` in
`app/app.h`; without the macro the platform never calls it, which is the same trap that
made `touch_poll` dead code.

---

**`B_EDIT` — the SMPL edit page**

**EDIT means edit the voice.** A knob is a performance control: one hand, while the other
plays, and its meaning is a number. A page is for structure — a boundary, a frame, a grain's
direction — where the value is not something a hand sweeps but a decision about what the
material is.

EDIT is only implemented for SMPL, and the button does nothing in SYNTH and GRAN. What
those two could carry is specified in `docs/spec.md` §5d with the measurements behind it
(flash and store limits for a bank, the per-sample cost of a frame phase, the four grain
axes and which one is a call site).

**What this page does today** — the whole of it:

| | |
|---|---|
| region | `start` … `end` sounds, `loop_s` … `loop_e` repeats |
| loop | one switch, on the PRESETS encoder |
| release | leaves the loop and rings on to `end` |
| zoom | 0–7 → 1×…128×, window centres on the last boundary turned |
| step | one detent = one pixel of the window |
| picture | four coloured boundaries, dimmed material outside the region, a white playhead |
| stored | nothing — a power cycle restores the whole slot |

**What it does not do, and is not planned to**: pan after zoom (the boundaries are the
navigation), per-slot edit persistence until `B_SAVE` exists, slicing.

EDIT does nothing outside SMPL. It used to switch the source to SMPL from SYNTH and
GRAN, on the argument that a button which does nothing is a broken button — but EDIT sits
beside the pages a player actually presses, and a stray press replaced the source and
repainted the whole screen. A button that does nothing where it does not apply is quieter
than one that changes what the instrument is.

| | K1 | K2 | K3 | K4 | SELECT | ALGO | PRESET |
|---|---|---|---|---|---|---|---|
| page up | **START** | **LOOP S** | **LOOP E** | **END** | SLOT | **ZOOM** | **LOOP** on/off |

**The knobs are in the picture's order, left to right.** START, the loop pair, then END.
The first version had END on K2 because every list of these four values is written
start / end / loop start / loop end — but that put a hand's second knob at the far right
of the sample while the third and fourth were inside it, so nudging the loop was a reach
across the whole row while END, the boundary a player sets once, sat in the middle of the
work.

SMPL only — `B_EDIT` does nothing in SYNTH or GRAN, where there is no material region to
edit. It is the fourth of the mutually exclusive pages.

**Path A, one region and one loop.** The slot is not chopped into pieces. There is one
`start`–`end` span that sounds, and inside it one `loop_s`–`loop_e` span that repeats
when LOOP is on.

**Release leaves the loop.** A loop is a sustain: while the key is held the reader wraps
inside LOOP S..LOOP E forever. On note-off the loop retires and the reader walks on to
END, so the release fades the tail that was recorded after the loop instead of a fragment
of it over and over. A short release ends the note inside a few milliseconds and this is
invisible; a long one is not, and neither is the picture — the head crosses LOOP E and
keeps going. Measured from a loop at 59..152 px with a 1.2 s release: the head reads 132,
142, 161, 174 at 20 / 120 / 320 / 450 ms after the key came up.

**The region and the loop are two pairs, and the engine takes them separately.** A held
note walks in from START and then repeats inside the loop — the intro-plus-sustain that
is the ordinary use of a sampler. This was wrong for one round: the page drew LOOP S and
LOOP E where they belonged, but `sample_voice` passed `start, end` to `wv_reader_loop`,
so the engine looped the whole region whatever the marks said. Nothing in the picture
could catch it — the marks were right and the sound was wrong — so it is now guarded by
following the playhead over time: it must sweep in from START, then stay inside
LOOP S..LOOP E and wrap there. Measured with a loop at 59..152 px of 232: the head reads
22, 51, 90, 129, 76, 115, 154 — in, then 129 → 76 is the wrap.

Chopping was the earlier plan and was dropped: the automatic chop was already good enough
to *play*, and what a sample instrument actually lacked was a loop and a way to trim it.

**Why a loop at all.** Before this page all three `wv_note_on_sample` call sites passed
`loop = 0`, so a held key played the material once and stopped. That is a drum hit, not
an instrument: the README's first sentence is "sample-as-wavetable" and nothing could
hold a note. Turning LOOP on is what makes a held key sustain.

**The four values are ordered and clamped as a chain**, `0 <= start < loop_s < loop_e <=
end <= len`, recalculated on every turn: a boundary cannot be dragged past its neighbour,
and the region can never invert. The loop pair is clamped inside the region, not the
slot, because a loop outside the region would never be reached.

**LOOP on PRESETS is a two-state knob, not a button.** Turning it right turns the loop
on, left turns it off; there is no press. It is the only free encoder in SMPL (see the
encoder table), and a knob that reports its state back is what the corner label is for.

**ZOOM has no pan, and does not need one.** ALGORITHM is the zoom level, 8 steps, 0 =
the whole slot and 15 = 128x. The window follows **whichever boundary was turned last**,
centred on it — so a zoomed window is never lost off to the side; turning START brings
START to the middle. K1–K4 are the navigation. Measured at 128x: turning START leaves the
live marker at column 115–116 of 232, the centre.

**The step is one pixel of the window.** A turn moves a boundary by `(len >> zoom) / 232`
samples — the window's own span divided by its width — so a detent moves the marker
exactly as far as it looks like it should, at every zoom. Measured against the picture:
ratio 0.997 at 1x, 0.992 at 4x, 0.992 at 16x. It is floored at 1 sample, because a slot
shorter than 232 samples at high zoom divides to zero and a zero step makes all four
knobs dead while the page still looks alive.

This replaced `len / 1000 >> zoom`, which made the whole slot 1000 detents: on a 3 s slot
a full pass was ~160 turns of the wrist at 1x while the same turn at 128x moved one
sample. A turn has to move the marker by what the eye sees, and pixels are the only unit
that is true at every zoom. The first version also reached zero at 64x — `turn K1 900`
left `region_start` at 0 — which the same floor fixes.

**This page breaks the source's single colour, on purpose.** Four boundaries are four
values a hand has to tell apart, and SMPL's one green made them four identical lines — the
page worked and could not be read. So START / END / LOOP S / LOOP E each get a hue, and
the knob that moves a boundary is printed in the hue of the line it moves:

| | hue | live | not live |
|---|---|---|---|
| START | red `255,96,96` | full | 3/5 |
| END | yellow `255,208,96` | full | 3/5 |
| LOOP S | cyan `96,224,255` | full | 3/5 |
| LOOP E | violet `198,140,255` | full | 3/5 |

**Only the brightness changes, never the hue.** Recolouring the live marker instead would
mean a line changes identity as the hand moves, which is exactly the question the page
exists to answer. An off loop drops to 1/5 of its own hue rather than to the theme's ink,
because an off loop is not a fact about the sound but it is still LOOP S.

This is the one place on the panel where a colour is not the source's colour, and it is
justified by the page being the only one that shows four of the same kind of thing at
once. SYNTH and GRAN keep one ink.

**The playhead is white, and the only white on the screen.** It is drawn only while a
voice is walking this slot — a head parked at 0 would claim the material is playing from
the start, the same mistake the GRAN picture avoids by not drawing one at all when the
cloud is silent. Its shape is a 5 px tab at the top of the band plus a dot at the
material's own height: the four markers are full-height 2 px lines, so a tab and a dot
read as a cursor and a line reads as an edge. Measured while a key is held: column 4, then
24, then 58 at 60 / 260 / 620 ms.

**The picture shows four positions, so it needed a different drawing.** In SYNTH and GRAN
the band shows material. Here it shows a *state*: material inside the region at 2/5 ink,
material outside at 1/5, the live marker in `ink_hi` at 2 px, the other three in 3/5 ink,
and the loop pair dotted. Two defects were measured and fixed on the way in:

- at the default (region = whole slot) everything was 2/5 and the two states were
  indistinguishable — 14,117 px at 2/5 against 169 px at 1/5 in the whole band;
- 1 px dotted markers over an 80 px band came to nine visible pixels for the whole loop
  range, so the markers existed and could not be seen. They are 2 px and dot every 2 rows.

**The edits live in RAM only.** Nothing persists them: `B_SAVE` does nothing, and the
region resets to the whole slot on boot. That is the current state of the panel's save
story, not a property of this page.

## Buttons

| Button | Printed | Current function | Owner | Where |
|---|---|---|---|---|
| `B_SEL` | SEL | **SOURCE** cycles SYNTH → SMPL → GRAN | app | app.c:47 |
| `B_ENV` | ENV | **ENV page** toggle (A/D/S/R on K1–K4) | app | app.c:47 |
| `B_EDIT` | EDIT | **SMPL EDIT page** toggle (START/END/LOOP S/LOOP E on K1–K4, LOOP on PRESETS, ZOOM on ALGORITHM); dead in SYNTH and GRAN | app | app.c |
| `B_FX` | FX | nothing | **free** | — |
| `B_LFO` | LFO | **LFO page** toggle (RATE/DEPTH on K3–K4) | app | app.c:47 |
| `B_GLO` | GLO | nothing | **free** | — |
| `B_SAVE` | SAVE | nothing | **free** | — |
| `B_ARP` | ARP | **ARP page** toggle (RATE/TYPE/OCT/GATE on K1–K4) | app | app.c |
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
 84..175   material   92 px   SYNTH: the table's shape. SMPL: the sample's envelope.
                              GRAN: that envelope dimmed, with the live grains on top
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

### The three material pictures

Each source draws what it actually is. SYNTH uses 172 px with a 60 px label column;
SMPL and GRAN use the full 232 px, because the picture is the whole point of those two
pages.

| source | picture | what carries which parameter |
|---|---|---|
| SYNTH | the wavetable's shape at the current MORPH, fixed phase | shape = the table; MORPH = which frame, printed at the right |
| SMPL | min/max envelope of the whole slot, one column per 53 samples on the demo kit | the slot's own dynamics; SLOT printed over it |
| GRAN | the same envelope at 2/5 brightness, with one bar per live grain | bar WIDTH = GRAINSIZE, their SPREAD = SPRAY |

**GRAN's cloud is the whole slot, and that is a gap, not a decision.** `app.c` calls
`wv_note_on_grain(&dsp, &smp[smp_i], note, vel, 0u, 0)` — always the full slot — while
`wv_grains_start()` already takes `[start, end)` and already wraps inside it. So a grain
voice cannot be aimed at a part of the material, which is the one control a granular
instrument is built around. It is one call site away, and it is the same class of defect
as the loop that was wired to `start, end` instead of the loop's own marks: the engine
takes the arguments and the call site does not pass them. Specified in `spec.md` §5d as
the first GRAN candidate precisely because it is a call site and not a feature.

**Why min/max and not the waveform.** DEMO K is 9256 samples across 176 px: 53 samples
per column. Drawing one sample per column gives a line that changes character with the
phase and says nothing about the material. A bar from the column's minimum to its maximum
is a true statement about those 53 samples, and it is what every editor draws.

**Why SMPL is not normalised per column.** SYNTH normalises each frame by its own peak
because MORPH has to stay readable across frames that differ by 3.4 dB. A sample's
dynamics ARE its shape: the drum's decaying bars are the information. One scale is chosen
for the whole slot, so a quiet sample is drawn quiet.

**The playhead is gone, and why that is the right answer.** Three versions, and the
last one is the one the player reasoned to:

1. A dim playhead tick drawn while a cloud ran, as a reference for SPRAY.
2. Parked where the note stopped, drawn always, resuming from there.
3. Removed. A grain mark is a claim that something is being read, so it must not be
   drawn when nothing is read -- and by the same argument the playhead is not worth
   drawing either, because the interesting fact is always "where are the grains".
   With no playhead there is nothing to resume, so GRAN starts every note from 0.

The reason is not aesthetic. A grain fades in and out over the piece of the sample that a
one-shot would have fired once, so the sound of a note is decided by where it starts
reading. Resuming makes that point depend on when the previous note was RELEASED.
Measured on the demo kit, the first 100 ms of a cloud started at 50 % of the slot is
25.7 dB below one started at 0, and at 100 % it is 57.2 dB down. The same key at the same
velocity would be near-silent because of history -- and a note's attack must be a
property of the note.

The cost is one-sided and known: scatter cannot reach backwards past sample 0, so at high
SPRAY the cloud is denser at the start of the material than it would be in the middle
(measured, 11.7 % of grains in the first 5 % of the slot against 5 % uniform). That is a
bias toward the transient, which is the direction a player would accept.

The loop marks stay. They sit at the band's edges nudged in one pixel, because the loop
is the whole slot until there is a way to set a range, and a mark on the border is
indistinguishable from the border itself.

**The envelope: filled min/max, drawn dim, shared by SMPL and GRAN.**

*The brightness was the real defect.* SMPL drew its envelope in `ink_hi` at full strength
while GRAN drew the same material at 2/5 of `ink`, so the two sources disagreed about
their own background. Both now call `env_line()` at `dim(ink, 2, 5)`.

*The fill went away and came back.* It was changed to a 1 px peak line on my reading of
"the sampling is so dense it comes out as a solid shape", and restored to a filled
min/max band on the player's judgement after seeing both. The measurements that motivated
the removal are still true -- 53 samples per column, a drum hit spanning 60 of the 78 px
inside a single column -- but they describe why the band is dense, not why dense is wrong:
a filled band is how much energy is at this moment, and a 1 px outline of a loud passage
does not read as loud at all.

What the column count does, for the record: **fewer** columns make it denser, because a
wider window means a wider min/max range.

```
DEMO K   176 cols: 15.6 px tall   88 cols: 17.7   44 cols: 21.0   22 cols: 25.1
```

Measured coverage of the material picture, filled:

```
SMPL DEMO K  24.7 %      GRAN  25.0 %
```

DEMO T is a sustained tone, so its band is full height end to end. That is the material,
not the drawing: there is no envelope to show because the sample has none.

**GRAN: what the player reported, and what it was.** Two defects, and the first one is
worth keeping because the guess was reasonable and wrong:

- *"the bars jump up and down -- is that SPRAY?"* No. The lane was the DSP's grain SLOT
  index, and slots rotate (`next = (next+1) % WV_GRAINS`), so the same grain changed lane
  every respawn -- at SPRAY 0, constantly. Measured, the upper bar's y went
  125,125,125,121,121,121,125,125 over eight frames. The lane now follows drawing order
  by position, so with two grains it changes only when they cross.
- *SPRAY is hard to see.* It was only the distance between the two bars, and at low
  settings that is a few pixels with nothing to compare it to. An intermediate version
  drew a dim dotted "playhead" tick to be the reference the scatter is relative to. That
  tick is gone now (see below); SPRAY is read as the separation between the two bars, and
  at SPRAY 0 they coincide, which is itself the reading.

**GRAN: three layers, one per visible parameter.** The design went through two failures
first, both measured rather than argued:

1. *one 2 px tick per grain* -- grains commonly land 5 px apart, so two ticks read as
   one. At SPRAY 48 one frame showed two ticks 5 px apart, the next 54 px apart. The
   picture could not answer "how many grains".
2. *the sin^2 window drawn as a shape* -- at the default GRAINSIZE of 45 ms a grain is
   4.0 px wide on the 2 s demo tone and 9.5 px on the 0.84 s kit. Four pixels cannot
   show a shape, and two overlapping four-pixel shapes cannot show a sum.

What works is noticing that a grain's length **is a length**: the band's width maps
GRAINSIZE 0..127 to 2..42 px of material on the 0.84 s slot, or 1..18 px on the 2 s one.
So the bar width is the grain length -- multiplied by `inc`, because a grain at pitch
consumes `length * rate` of material, which is why the same GRAINSIZE covers more of the
sample as you play higher. Drawing at 1x would picture the knob instead of the read.

```
measured on the demo kit, GRAINSIZE 45 ms, voice at note 53, band at its full 232 px:
  spray   0   two bars, each 16 px wide, at the same x
  spray  64   two bars, separation 45 px
  spray 127   two bars, separation 89 px
```

The bar measures 16 px, not the 12.4 px a linear reading predicts (45 ms of a 0.84 s slot
is 5.36 % of 232 = 12.4). Two things stack: the width is an integer, and at SPRAY 0 the
two bars start at the same x and their edges join into one 16 px block. The count is
still two -- the four fixed lanes say so -- and the test asserts 16 with the reason
written down, so a later reader does not "fix" the number.

Four fixed lanes (heights -7, +7, -3, +3 about the axis, assigned by drawing order) rather
than all bars on the axis: coincident grains are normal -- SPRAY 0 puts them at the same x
by definition -- and a merged bar would hide the count. The lanes are fixed, not
randomised, so nothing moves for no reason.

**SPRAY 0 showing one position is correct, not a bug.** Both grains read the same source
sample at the same instant, with windows half a grain apart, so sin^2 + cos^2 = 1 and the
sum is exactly the sample. That is why granular at SPRAY 0 is an ordinary sample player.
The two bars therefore coincide, and the lanes are what still say "two grains".

**There is no playhead.** There was one -- a dim dotted tick, then a parked one that
stayed after a key was released -- and both were removed by the same decision: a grain
voice restarts from the material's start on every note-on, so the tick is a constant at
the left edge and carries no information. A mark that says "reading here" must not be
drawn when nothing is reading.

**The bar at the right edge.** The loop wrap puts a grain on `loop_end` = `len - 1`, which
maps to the last pixel of the band. The first version clamped x into the band and then
shrank the width to whatever was left -- zero -- and skipped the bar, so the picture
blinked once per loop pass while the DSP reported two grains. The bar now slides back to
stay inside at full width: one pixel short of the edge is still a true statement that a
grain is reading there, while a missing bar says nothing is reading, which is false.

The loop marks are drawn 1 px inside the band's own edge. At the edge they coincided with
the border and were invisible; "inside the band, not on its border" is an assertion in the
test for that reason.

`app/tests/test_grainmap.py` (**71 checks**) asserts the three parameters separately --
two bars visible, each 16 px, separations 0 / 45 / 89 px across SPRAY -- that no bar is
drawn when nothing is reading (idle 0, playing 2, stopped 0, still 0 after 500 ms, 2 again
on the next note), and that the loop marks stay at x 5 and x 234 for the whole note.

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

So the scale is **linear in amplitude and fixed**: full scale is the band edge, 41 px
each side of the centre line at row 42, pre-master (MASTER is the listening level; a
scope that shrank when the pot turned down would be measuring the wrong thing).

There is **no headroom above 0 dBFS** -- `SCOPE_FS` is `SCOPE_HALF`, so full scale lands
on row 1 and row 83 exactly and anything past it clamps to the edge. Measured on the
host, MASTER full, keys held:

```
voices    top row   from centre   amplitude   level
  1         33          9 px        0.220     -13.2 dBFS
  4         20         22 px        0.537      -5.4 dBFS
  8         19         23 px        0.561      -5.0 dBFS
```

Eight voices do **not** reach the edge, and this is worth stating plainly because an
earlier revision of this file claimed they did: 8 x 0.256 = 2.05 of full scale would
clip only if the eight sounded in phase, and eight different keys do not. The real sum
measures 0.561, i.e. -5.0 dBFS. Adding voices past that barely moves the picture, which
is honest about what they add to the peak.

Amplitude IS level: velocity, envelope, polyphony and the shaper all move it.

**Velocity cannot be tested on the host**: `touch_vel()` is the app's only velocity path
and `APP_KEY_SENSE` is not enabled, so `key <n> down <vel>` changes nothing. Polyphony is
the stand-in, and the DSP-level sweep (velocity 1 -> 0.00201, 127 -> 0.25586, linear in
amplitude) is measured directly in a throwaway harness instead.

The ring is `int16` (units of full scale) rather than `int8`: a fixed scale has to be
able to see past 0 dBFS even though today's signals do not reach it (measured, eight
voices peak at 0.561 of full scale). `int8` would also quantise a quiet release tail
away. 768 samples of int16 is 1536 B of the 98304 available.

### The scope graticule

The band is a scope screen, not a plot, so it has a grid: five vertical lines (every
20 px) and the horizontal dB markers of the linear scale. The dB lines are NOT equally
spaced, and that is the point, not a bug: the scale is linear in amplitude, so 0 dBFS
is the band edge and the lines fall at

```
0 dB    row  1   41 px from the centre line (the band edge)
-10 dB  row 29   13 px from the centre
-20 dB  row 37    5 px
-30 dB  row 40    2 px
-40 dB  row 41    1 px
-50 dB  row 41   1 px  (coincides with -40: 41.6 and 41.9 both round to 41)
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

SMPL and GRAN run the picture the full band, 232 x 80, and print three values on top of
it, one per knob, in the corner that knob occupies on the panel:

    SLOT 1/2                              top right
    SPRAY 0                               bottom left    GRAN only
    GRAIN 45ms                            bottom right

SYNTH does not, and the reason is measurable rather than aesthetic. SYNTH's picture is a
wavetable frame drawn at **full ink** -- the frame *is* the picture. SMPL and GRAN draw a
min/max envelope at 2/5 ink and put 1 px bars on top of it. Bright text on a bright trace
is what the player objected to ("和标签冲突的厉害"); bright text on a dim one he accepted.
So SYNTH stops at 172 px (x 4..175) and keeps a 60 px column at x 176..235 for itself:

    BANK 1/8        top right, y 92
    SAW SQR         second line, y 104 -- the bank's NAME
    MORPH 0         bottom right, y 146

The 60 px is the widest label plus slack, measured with the font's advance table:

    BANK 1/8   46 px      SAW SQR    42 px  (widest bank name)
    MORPH 0    42 px      MORPH 100  54 px  <- worst case

180 + 60 is the whole screen with no margins, so the picture is 172 and the column is 60
at x 176..235: a 4 px gap after the picture, 4 px at the screen edge, 6 px of slack after
the widest string. The name line is what makes the column worth its width -- "BANK 3/8"
says which one you are on and nothing about what it is.

The name is drawn at 3/5 ink and the value at ink_hi, so a label reads as one phrase
whose subject is dim and whose number is bright. Widths come from the font's own advance
table (`txt_w()`), not from a fixed 7 px per glyph: FONT_XS is proportional and a digit
is 6 px while `/` is 3, which left the right-hand labels 4-9 px off the edge and drifting
with the digit count before.

**Every label names its parameter.** An earlier pass printed the numbers alone, on the
argument that a corner is a name the way CUT is a name, and the player rejected it in a
minute: the same corner is BANK in SYNTH but SLOT in SMPL and GRAN, MORPH in SYNTH but
GRAIN in GRAN. A position is only a name when it is the same name every time.

SMPL's bottom right is deliberately empty. MORPH indexes a wavetable frame and SMPL reads
no wavetable, so that knob is dead there, and a dead value is worse than a blank corner.
`test_screen.py` asserts the emptiness so nobody "fixes" it.

Cost, measured: the labels sit over the waveform. Ink already in each label's box, in the
same hue family but at 2/5 brightness for the envelope and 3-5/5 for the text -- the
separation is brightness, not hue. On the device this reads as text on a dim trace; if it
does not, the two fixes are an inset backdrop of the band's own colour (cuts a notch out
of the envelope) or dropping the value's brightness (loses the number's prominence).

The table's own shape keeps 100 px of height in the band: at 28 px the wave used 13 px
and the MORPH steps measured 0.59 px apart; at 100 px they measure 2.90 / 6.74 / 12.00 /
19.44 px.

SMPL and GRAN draw the material instead: a min/max envelope of the sample (8800 samples
into one column per pixel (232 at the full width): transients visible, individual cycles not), and for GRAN that envelope
predimmed under the grain bars -- bar width is GRAIN SIZE as a fraction of the material,
spread is SPRAY.

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
would have kept on. **It calls `plat_leds(lit, in_keys)` now**, at 30 Hz, with `lit`
built from state:

| Lamp | Lit when |
|---|---|
| `B_ENV` | the ENV page is up |
| `B_LFO` | the LFO page is up |
| `B_ARP` | the ARP page is up |
| `B_SEL` | SOURCE is not SYNTH (the power-on default) |
| `B_EDIT` | SOURCE is SMPL, where EDIT opens the region page |

Keys light while held, which is `plat_leds`'s own behaviour and the reason to call it
instead of `plat_lights`. `B_SEL` has three states and a two-state lamp, so the lamp
only says "moved"; the screen says which.

---

## What is not assigned

| Function | State | Why unassigned |
|---|---|---|
| **PANIC** / all-notes-off | not implemented | `B_HOME` is the natural home (idle except during restore) |
| Material page | not implemented | no button of its own; spec's `BTN_BANK` names a control this panel does not have |
| Octave shift | not implemented | `B_OCT-` / `B_OCT+`, power-on conflict only |
| Glide / portamento | not in the DSP | `B_GLO` is free |
| ~~LFO~~ | **done** | `B_LFO` is the LFO page |
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
| `B_SAVE` | save the current knob set as the boot state | `plat_store_save` exists, 3840 B per object; **a prerequisite for deep edit**, not an extra — an edit that is gone at power-off is a page nobody uses |
| `B_FX` | **filter type** (LP / BP / HP) | the SVF already computes all three every sample and only `lp` is used, so the switch is free of arithmetic — see `spec.md` §5d |
| ~~`B_LFO`~~ | **taken**: the LFO page | — |
| `B_ARP` | ~~arpeggiator~~ | **assigned** — see the ARP page above |
| `B_SEQ` | step sequencer | larger still; that is Saadet's territory |

**If only one is done**: `B_HOME` → PANIC. It costs a dozen lines and fixes the one
failure mode that makes an instrument unusable in front of people.

---

## Keys

27 keys, F3–G5 (`plat.h: NKEYS 27`), and **every key is a note in all three sources**.
SMPL pitches the selected slot from its root and applies that slot's region and loop;
`EN_SELECT` chooses the slot. There is no per-key mapping and no chop table.

That is a change from the first version, and the reason it went is worth keeping: the
chop map existed so a key could fire one detected slice at 1x, and the region replaced
it because what a sample instrument actually lacked was a loop and a way to trim it, not
a kit. `slices[]`, `reslice()`, `SLICE_BASE` and `wv_slice.h` are gone from `app.c`; the
whole slot is the instrument.

**The defect that the chop era hid, kept because it is the general shape of the bug**:
the fallback played `smp[0]` — the boot slot — so every key ignored `EN_SELECT`, and
`slices_from` was only set at boot, so the chop path kept playing the boot slot's slices
too. The screen read `smp_i` and said `SLOT 2/2` while the sound was `SLOT 1/2`. GRAN was
unaffected because it reads `smp[smp_i]` directly, and **that difference between the
sources is the only reason the bug was visible** — with all three wrong in the same way
there would have been nothing to compare against.

`app/tests/test_slot.py` still exists and now **skips itself** when the pool holds real
material rather than the demo kit its shape checks were written against. `wv_slice.c` is
still compiled and still unit-tested by `test_slice.c`; nothing in the app calls it.
Whether to keep a tested-but-unused reader is an open item, not an accident.

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
