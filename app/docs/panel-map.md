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

**The screen is laid out as the panel is.** The material band keeps its picture on the
left and puts the labels on the right in the panel's own arrangement: SELECT top-right,
and the bottom row split PRESET left / ALGO right. A label printed where its knob is does
not have to be read; the previous version stacked all of them in a single column beside
the picture, which made a legend where a map was wanted. The picture is 112 px wide as a
result, down from 176.

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

Each source draws what it actually is, at the same 176 x 80 px:

| source | picture | what carries which parameter |
|---|---|---|
| SYNTH | the wavetable's shape at the current MORPH, fixed phase | shape = the table; MORPH = which frame, printed at the right |
| SMPL | min/max envelope of the whole slot, one column per 53 samples on the demo kit | the slot's own dynamics; SLOT printed over it |
| GRAN | the same envelope at 2/5 brightness, with one bar per live grain | bar WIDTH = GRAINSIZE, their SPREAD = SPRAY |

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
- *SPRAY is hard to see.* It was only the distance between the two bars, which at low
  settings is 17 px in a 176 px band with nothing to compare it to. The picture now draws
  the reference the scatter is relative to: `gr->play`, where a grain would spawn at
  SPRAY 0, as a dim dotted tick. SPRAY is then the standoff from it.

```
measured, GRAINSIZE 45 ms, note 53
  spray   0   playhead x 140   bars x 114,114   standoff  0 px
  spray  64   playhead x 114   bars x  71,105   standoff 43 px
  spray 127   playhead x 114   bars x  28, 95   standoff 85 px
```

A note on testing this: 61 ms between frames, not 90. The hop is 998 samples = 22.63 ms
and four slots rotate every 4 x hop = 90.52 ms, so sampling at 90 ms aliased against the
rotation and the first version of the lane check passed with the bug deliberately
reinjected. At 61 ms the reinjected bug shows four distinct lane pairs --
(121,131) (121,135) (125,131) (125,135) -- which is exactly the jumping the player saw.

**GRAN: three layers, one per visible parameter.** The design went through two failures
first, both measured rather than argued:

1. *one 2 px tick per grain* -- grains commonly land 5 px apart, so two ticks read as
   one. At SPRAY 48 one frame showed two ticks 5 px apart, the next 54 px apart. The
   picture could not answer "how many grains".
2. *the sin^2 window drawn as a shape* -- at the default GRAINSIZE of 45 ms a grain is
   4.0 px wide on the 2 s demo tone and 9.5 px on the 0.84 s kit. Four pixels cannot
   show a shape, and two overlapping four-pixel shapes cannot show a sum.

What works is noticing that a grain's length **is a length**: 176 px across the slot maps
GRAINSIZE 0..127 to 2..42 px of material (0.84 s slot) or 1..18 px (2 s slot). So the bar
width is the grain length -- multiplied by `inc`, because a grain at pitch consumes
`length * rate` of material, which is why the same GRAINSIZE covers more of the sample as
you play higher. Drawing at 1x would picture the knob instead of the read.

```
measured on the demo kit, GRAINSIZE 45 ms, voice at note 53:
  spray   0   2 bars  w=12,12  x 114,114   (same material, separate lanes)
  spray  64   2 bars  w=12,12  x  71,105
  spray 127   2 bars  w=12,12  x  28, 95
```

Four fixed lanes (heights -7, +7, -3, +3 about the axis, assigned by drawing order)
rather than all bars on the axis:
coincident grains are normal -- SPRAY 0 puts them at the same x by definition -- and a
merged bar would hide the count. The lanes are fixed, not randomised, so nothing moves
for no reason.

**SPRAY 0 showing one position is correct, not a bug.** Both grains read the same source
sample at the same instant, with windows half a grain apart, so sin^2 + cos^2 = 1 and the
sum is exactly the sample. That is why granular at SPRAY 0 is an ordinary sample player.
The two bars therefore coincide, and the lanes are what still say "two grains".

**The bar at the right edge.** The loop wrap puts the playhead on `loop_end` = `len - 1`,
which maps to x = 179.98 -> 180, the outer edge. The first version clamped x into the band
and then shrank the width to whatever was left -- zero -- and skipped the bar, so the
picture blinked once per loop pass while the DSP reported two grains. The bar now slides
back to stay inside at full width: one pixel short of the edge is still a true statement
that a grain is reading there, while a missing bar says nothing is reading, which is false.

`app/tests/test_grainmap.py` (58 checks) asserts the three parameters separately -- two
bars visible, each 12 px, standoffs 0 / 43 / 85 px from the playhead tick -- scans 24
frames of a full loop pass for a lost bar, and 12 frames at 61 ms for lane stability. The
only frame allowed to show one bar is the wrap itself.

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

The picture runs the full band, 232 x 80. Three values are printed on top of it, one per
knob, in the corner that knob occupies on the panel:

                             BANK 1/8     top right
    SPRAY 0                               bottom left    GRAN only
                             GRAIN 45ms   bottom right

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
into 176 columns: transients visible, individual cycles not), and for GRAN that envelope
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

Both paths follow `EN_PRESET`. That was not always true, and the defect is worth
keeping written down because of how it hid:

- the fallback played `smp[0]` -- the boot slot -- so every key above the chops
  ignored the knob entirely;
- `slices_from` was set once at boot, and `EN_PRESET` moved `smp_i` without
  re-chopping, so the chop path also kept playing the boot slot's slices.

The screen read `smp_i`, so it said `SLOT 2/2` while the sound was `SLOT 1/2`. GRAN
was unaffected (it reads `smp[smp_i]` directly), and **that difference between the
sources is the only reason the bug was visible at all** -- with all three behaving
alike there would have been nothing to compare against.

Fixed by `reslice()`, which chops the selected slot and records it in `slices_from`
in the same breath, called at boot, on every `EN_PRESET` turn, and on `B_EDIT`. The
chop path additionally refuses to use chops whose `slices_from` is not the selected
slot, so a future path that moves `smp_i` without re-chopping falls back to the whole
slot instead of playing one sample's boundaries inside another sample. Re-chopping on
a slot change discards a manual `B_EDIT`, which is right: a chop belongs to the
material it was cut from.

Checked by `app/tests/test_slot.py` (8 checks), not by `smpl.txt` -- the two demo
slots peak only 0.7 dB apart, so a level threshold cannot tell them apart, while their
envelopes differ completely (slot 0: a transient gone in 4 of 16 segments; slot 1:
11 of 16 flat). Injecting the old `smp[0]` makes the two envelopes identical, and the
test fails 3 checks.

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
