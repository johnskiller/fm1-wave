# fm1-wave — specification (draft for discussion)

A polyphonic wavetable synth for the M-VAVE FM-1, built on fm1-base, that goes
two steps past what the Melodee WT engine does:

1. **a waveshaper** — not in WT
2. **samples as wavetables, and a granular mode** — not in WT

Everything else is deliberately the cheapest thing that works, because those
three features are where the effort belongs.

**Status: skeleton only.** The repo builds (`wave.fwsc`, identity `FM1-9400`,
221,884 B image, `02f422c` base) and contains nothing but the template's
single-voice oscillator. Nothing in this document is implemented.

Background, and the numbers behind the choices:
[`background-wavetable-and-waveshaper.md`](background-wavetable-and-waveshaper.md).

---

## 0. Why this exists when Saadet will have a wavetable engine

The WT engine in [Melodee PR #48](https://github.com/keremimo/melodee/pull/48)
(`eng_wt.c` + `gen_wt_tables.py`, MPL-2.0) is going into
[Saadet](https://github.com/keremimo/saadet) as a fifth engine. That is settled
and it is the right home for a wavetable engine inside a multi-engine synth.

fm1-wave is a different thing, and the difference is not "a second
implementation": **it is the two features WT does not have.** Synth tables, a
waveshaper, and sampled/granular material are an instrument with a point of
view, not another engine in a menu. If those features land in Saadet later,
this repo has served its purpose and can be retired — that is a success
condition, not a failure.

---

## 1. What it is

**Is**: a keyboard instrument. 27 keys make sound, 7 encoders shape it, the
screen shows what it is doing. Samples you feed it become playable material.

**Is not**: a controller (fm1-ctrl), a multi-engine rompler (Saadet), a
sequencer (DriftSeq). Both firmwares and Saadet can live on the same unit;
flashing one replaces the other.

---

## 2. The signal path

```
                    ┌── wavetable osc ──┐
keys ─► allocation ─┤                   ├─► shaper ─► SVF low-pass ─► VCA ─► mix ─► scope
       (8 voices)   └── granular ───────┘   (shape,     (cut, res)    (env)
                     per voice, one of         amount)
```

One oscillator per voice, and **the oscillator is one of two sources**: a
wavetable bank (read-only, from flash) or a sample with a granular reader. Both
produce a phase-driven signal and share everything downstream, so the shaper,
filter, envelope and voice allocation are written once.

**Eight voices.** Stealing is round-robin with oldest-note priority and a 2 ms
crossfade — a stolen voice clicking is the most audible defect a poly synth can
have.

### Levels, and why the bus has a soft clip

Not a detail: the platform hands `app_render`'s buffer straight to the ALNK DMA as
int32 with 24 valid bits, and **nothing on the way down clips**. A sample past
full scale there does not distort, it *wraps* — the loudest click the hardware can
make. So the app owns the ceiling.

Allocation, measured rather than assumed:

```
voice gain        1/sqrt(8)        eight uncorrelated voices sum in power
a solo note       -16 .. -20 dBFS  (bank-dependent; OPEN3 is the quietest)
eight at FOLD@127 +0.9 .. +2.4x    before the soft clip
```

Two decisions came out of measuring that:

- **No headroom factor in the velocity map.** The first version had 6 dB there and
  6 dB in the voice gain; neither knew about the other, and a solo note landed at
  -25.5 dBFS end to end. Headroom is one problem with one owner.
- **A tanh soft clip on the bus, then a hard clamp behind it.** A bare clamp at
  full scale clipped 2-9% of samples in an eight-note chord and sounded like a bad
  fuzzbox. `fm_tanhf` (already in `fastmath.h`; no lookup table needed) is ~identity
  below 0.5, so single notes and small chords are untouched, and it brings the
  eight-voice case to 0.92-0.99 with zero clipped samples. One call per sample
  against eight voices' worth of work, so the cost is where it is cheapest.

A solo note at -16 dBFS is quiet, and that is the price of 1/sqrt(8) rather than a
limiter: a limiter would colour every note to protect the rare eight-note chord.
If it turns out too quiet on the device, the answer is MASTER's travel, not the
voice gain — see §11.

---

## 3. Panel map

> **What is actually wired is in [`panel-map.md`](panel-map.md)** — that file is
> the authority on current assignments, including which buttons are free and what
> each one could carry. This section is the design intent; §9f records where the
> built panel differs from it.

Seven encoders. Grouped by function, so the pairs sit side by side on the panel.

| encoder | panel | SYNTH | SMPL | GRAN |
|---|---|---|---|---|
| EN_SELECT | SELECT | **BANK** 0–7 | **SLOT** | **SLOT** |
| EN_PRESET | PRESETS | — | — | **SPRAY** 0–127 |
| EN_ALGO | ALGORITHM | **MORPH** 0–127 | *(dead)* | **GRAIN SIZE** 10–200 ms |
| EN_K1 | KNOB 1 | **CUT** | CUT | CUT |
| EN_K2 | KNOB 2 | **RES** | RES | RES |
| EN_K3 | KNOB 3 | **SHAPE** | SHAPE | SHAPE |
| EN_K4 | KNOB 4 | **AMOUNT** | AMOUNT | AMOUNT |

- **CUT** 0–127 → 30 Hz … 16 kHz, exponential
- **RES** 0–127 → damping 2.0 … 0.15
- **SHAPE** 0–3 detented: SOFT / HARD / FOLD / SINE
- **AMOUNT** 0–127 → shaper drive 0 … 16×

**The pairs are adjacent**: CUT/RES are the filter, SHAPE/AMOUNT are the shaper.
K1–K4 do not move in any source.

### The three mode knobs follow the panel, not the function

The physical panel is a 2×2 block left of the screen:

```
    MASTER   SELECT        (top row)
    PRESETS  ALGORITHM     (bottom row)
```

MASTER is a pot (`plat_master()`, 0–4096) and is not on screen. The other three
are encoders, and each one's job was chosen so that **the same corner of the
screen always means the same knob**:

- **SELECT, top right** — which material is loaded. BANK in SYNTH, SLOT in the
  two sample modes. All three ask the same question.
- **ALGORITHM, bottom right** — MORPH in SYNTH, GRAIN SIZE in GRAN, dead in SMPL.
- **PRESETS, bottom left** — SPRAY in GRAN, nothing in SYNTH or SMPL.

**MORPH is dead in SMPL, and that is why the knob is not given a job there.**
MORPH indexes a wavetable frame; SMPL reads no wavetable. A dead knob is visible
and stable; a knob that does something else next mode gets turned by accident.
The screen agrees by leaving that corner empty rather than printing a value that
predicts nothing.

An earlier version had BANK/SLOT on PRESETS and SPRAY on SELECT. It was wrong in
the hand and wrong on the screen: the knob labelled SELECT did nothing in two of
three modes, and the knob labelled PRESETS was choosing the bank. Position is the
name, so the name has to match the position.

### Toggle pages: ENV, and the material page

Things that do not fit on the seven encoders are a **toggle**: press the panel
button and the encoders become that page; press it again and they are the tone
controls. A short label in the header says which page is up while it is up.

**Toggle, not hold.** Holding needs a hand, and the point of an envelope is to
turn four knobs while playing — a hold gesture spends the hand that should be
on the keys. Hold is also invisible: nothing on the panel says "you are in a
page" until the bars change under your fingers.

- **BTN_ENV** → **envelope page**: ATTACK, DECAY, SUSTAIN, RELEASE on K1..K4,
  three encoders spare. Live in both modes.
- **BTN_BANK** → **material page**: in SMPL, the samples in the resource region
  (and grain size, window, overlap); in SYNTH, nothing — the bank is already on
  a knob, so the button stays **dark** in SYNTH. A dark LED is the panel's own
  way of saying "not in this mode", which is a convention the instrument
  already uses elsewhere.

The two pages are mutually exclusive (pressing one leaves the other), and
**turning any encoder whose page is up does not leave the page** — you leave by
pressing the button again, or by pressing PLAY. Leaving by timeout was
considered and rejected: an envelope tweak with a pause in it should not silently
revert to the tone page under your fingers.

At power-on the envelope is A 5 ms, D 300 ms, S 0.7, R 250 ms: a sound on the
first key press, not a silent instrument waiting to be configured.

---

## 4. The waveshaper — the first feature WT does not have

Two knobs, per voice, **before** the filter.

- **SHAPE**, detented, 4 positions, never morphed. Intermediate curves between
  clip and fold are just muddy, and a knob that sounds broken in the middle is
  worse than a knob with detents.
  - `SOFT` `tanh` — the platform already ships `fm_tanhf` (fastmath.h: Taylor to
    x^13 under 0.5, `fm_exp2f` above, saturating past 9). No LUT, no table, no
    interpolation error.
  - `HARD` clamp
  - `FOLD` triangle fold — the widest spectrum, the one worth having
  - `SINE` `sin(k·x·π/2)`
- **AMOUNT** continuous drive. Pre-gain is folded into this knob rather than
  given its own: without pre-gain a shaper knob is mostly a volume knob with
  grit at the top, so drive *is* the parameter.

Before the filter, so CUT tames the grit and SHAPE interacts with CUT/RES
instead of fighting them. Per voice, not on the mix bus — a poly synth's
distortion belongs inside each voice.

**No oversampling in v1.** The shaper aliases; 2× fixes most of it and the
decimation filter costs more than the shaper. Compile-time switch, off by
default, measure before deciding.

---

## 5. Two kinds of table

### 5a. Synth banks — read-only, in flash

**8 banks × 8 frames × 256 points × int16 = 4 KB per bank, 32 KB total**,
generated at build time by a Python script, read directly from XIP.

Banks share partials across frames so MORPH never snaps, and each bank is
normalised **as a whole** (not per frame) so MORPH is a timbre control and not
a volume control.

Starting eight (the five from WT, three more to be named):

1. SAW→PULSE · 2. RESO · 3. HOLLOW · 4. VOX · 5. DIGI · 6–8. *(open)*

**Aliasing: accepted in v1.** Linear interpolation between points removes
staircase error, not harmonics that were above Nyquist when the table was
built. Mip-mapped band-limiting is the real fix, costs 4× the flash and an
index per voice, and is a **named future change**.

### 5b. Samples as wavetables — the second feature WT does not have

This is what the platform makes possible that is worth taking.

**Where the data lives.** The flash data area `0x97000..0xDFFFF` is **292 KB**
for an app's own big data, and fm1-base gives it a first-class mechanism:
`resources = FILE` in `fm1.toml` builds `build/wave-res.bin` **beside** the
package (not inside the image), written by `tools/fm1_resources.py` over
USB-MIDI after an update, read in place through `plat_res()` with a CRC-32
header so a torn or foreign region is simply absent. That is the storage design
already solved — no flash format to invent, no partitioning to get wrong.

**What that buys, per slot, at each rate (16-bit, the default — see below):**

| rate | B/s | 1 slot of ~250 KB | 8 slots × 30 KB each | audio band |
|---|---|---|---|---|
| 8 kHz | 16,000 | 15.6 s | 1.9 s | 4 kHz |
| 11.025 kHz | 22,050 | 11.3 s | 1.4 s | 5.5 kHz |
| **22.05 kHz** | **44,100** | **5.7 s** | 0.7 s | **11 kHz** |
| 44.1 kHz | 88,200 | 2.8 s | 0.3 s | 22 kHz |

**16-bit is not negotiable; 8-bit is a colour, not a rate.**
8-bit is `6.02 × 8 ≈ 48 dB` of SNR, and the place it fails is not loud material
— it is a decaying envelope tail sitting around −48 dB, where the quantisation
noise is all that is left. Undithered 8-bit is not hiss either, it is
*correlated* distortion that sits on the harmonics: it reads as "muddy", not as
"gritty". A per-slot 8-bit flag is a legitimate *timbre* (and costs a quarter of
the space); the default is 16-bit.

**The rate is per slot, not one number for the build.** This is the decision
that matters, and it is not a compromise between length and fidelity — it is a
refusal to make one:

- **A wavetable source** (extracted single cycle, §5b-1) wants a **low rate**:
  it is read at 20× its own rate when a high note plays it, so every Hz of band
  costs ~20 Hz at the top of the keyboard, and a 512-point table already holds
  256 harmonics. 11.025 kHz here.
- **Sliced material** wants a **high rate**: read at 1× with no transposition,
  so its bandwidth is simply what you can hear. 22.05 kHz here, and 44.1 kHz
  when the material *is* the sound.

A build-wide constant cannot serve both, and picking one silently degrades the
other. **11.025 kHz is the default** (÷4 of 44.1: an exact octave ratio, and its
5.5 kHz band covers the 2.2 octaves a 27-key keyboard actually transposes) — but
the slot header sets the rate, so the format does not need changing when the
first 22.05 kHz slice arrives.

**Storage: a pool, not fixed-size slots.** Fixed slots waste space on short
material and cannot hold a long one. The region holds a **variable-length pool**
with an offset table:

```
header   magic(4) + version(1) + count(1) + reserved(2)
entry c × 20 B   { offset u32, len u32, rate u16, bits u8, root u8,
                   gain i16 (Q8), name[6] }
```

`offset` is absolute from the start of the blob, and `gain` is per slot because
banks and samples have different crest factors — measured, the eight synth banks
differ by 4.3 dB from each other, so samples differ by more; it is set by ear.

Eight entries cost 176 B; the rest is samples, packed back to back, and a
sample may span several entries' worth of the pool up to the whole thing. So
"how many" and "how long" stop competing: one 8 s slice, or eight 1.4 s slices,
or anything between — the user decides per sound, not the format.

**Cap: 8 slots**, matching the 8 synth banks, so switching SOURCE keeps the
same "eight things" under the same gesture.

**Three ways a sample becomes playable material, and they are different instruments:**

1. **Extracted single cycle** — find the pitch period, cut one cycle, resample
   to 512 points. Playable chromatically, sounds like a classic wavetable
   oscillator with someone else's timbre. Cheap at runtime (identical to a
   synth bank), and the analysis is a build-time/host-side job, not a device
   job. **Needs a low rate**: 100 Hz at 11.025 kHz is 110 points per period,
   plenty to resample to 512; 500 Hz at the same rate is 22 points, and
   resampling that is invention, not analysis. Extract from the low register.
2. **The sample as a timeline** — the whole thing, read at variable speed. Now
   the "table" is a phrase, and playing a chord slides through it. This is what
   becomes granular.
3. **Slices** — the sample cut into (or simply addressed in) segments, each
   played at 1× from its own start, and each assignable to keys. **This is the
   case that forces a high rate**, and it is why the rate lives in the slot
   header rather than in the build: a slice is not transposed, so its band is
   simply what the material has, and anything below 22.05 kHz is audible
   missing top end on cymbals, breaths and consonants.

**v1 takes all three.** (1) is a host-side tool; (2) and (3) are the same
reader with different position control, so the second and third cost one
implementation between them.

### 5c. Granular — the third feature, and the one with the real bill

Granular is the same data with a different reader: instead of one phase walking
the sample, N overlapping grains each have their own position and window.

**The bill, stated honestly: grains are voices.** A grain is one read (one flash
fetch, one window, one interpolation) — exactly the work an oscillator does. So
the cost is `grains × held voices` oscillator-equivalents, and at 8 × 8 that is 64,
which does not fit in a 5.8 ms block on one core.

### The window is sin², and the overlap is exactly 2 — and that is arithmetic

`sin²(πt) + sin²(π(t+½)) = 1`. Two grains half a grain apart sum to *exactly* one
at every sample: no table, no approximation, no gain compensation, no ripple.
Any other overlap or a peak-normalised wavetable leaves a ripple that is audible
as a slow tremolo whenever the grain rate beats against the pitch.

So `WV_GRAIN_OVERLAP = 2` is not a taste, and the spawn hop is `grain length / 2`.

**The consequence is the escape hatch**: at SPRAY 0 the grains tile, every live
grain reads the *same* source sample at the same instant, the windows sum to 1,
and the cloud **is** the sample. Not approximately — measured worst error 1e-5
against the material (`test_grain.c`). SPRAY therefore degrades granular into an
ordinary sample player by construction rather than by a special case.

One honest caveat: a note places **two** grains on its first sample (one at window
phase ½, one at 0). With a single grain the note would start at window 0 — silence —
and fade in over half a grain, which is a soft attack on every drum hit.

### A granular note sustains

The playhead wraps inside `[start, end)` when the note loops. Without the wrap a
cloud over a 2 s sample goes silent after 2 s of holding a key — and it goes
silent *earlier* the more SPRAY is on, because a scattered grain that lands past
the end reads nothing. Both were observed before it was fixed; the second one
looked like "spray kills the sound".

Past the end of its material a grain reads **silence, not the last sample held**.
Holding the last sample makes a windowed constant, and two windowed constants sum
to a DC offset — a one-shot that does not decay but sits on the material's final
value at full level. That was observed too.

### Grain size and spray

- **GRAIN** (0–127 → **10–200 ms**, exponential; the midpoint is the geometric
  mean √(10·200) = 44.7 ms, not 105 ms). Fixed at 64 until the material page
  exists: no encoder is spare for it.
- **SPRAY** (0–127) scatters each grain's start over ±(spray/127) × half the slot.
  The normalisation lives inside the cloud, not at the call site: passing a raw
  0–127 scatters by 127 × len/2, so every grain clamps to the same two edges and
  two different seeds produce identical clouds (which happened).
- Measured at spray 0: the spectrum peaks at the material's own 219.5 Hz with a
  1 Hz bandwidth (the analysis resolution). At spray 127 the peak wanders over
  188–273 Hz and the bandwidth widens: that is the granular smear, and it is
  exactly the energy that at spray 0 is zero.

**Grain shape is not in v1**: no per-grain filter, no per-grain pan, no reverse
grains (the reader supports `dir = -1`; the cloud does not use it yet).

### The budget, and where it is spent

- **SYNTH**: 8 voices, no grains. Full polyphony.
- **GRAN**: 4 grains per voice (`WV_GRAINS`), so 2 sound at any instant and two
  more cover a spawn landing on the sample another grain ends. 8 voices × 4 grains
  = 32 reads per sample, against SYNTH's 8.
- `WV_GRAINS` is the whole granular budget knob. Lowering it to 2 makes the cloud
  exactly the tiling pair and nothing more.

### 5d. EDIT — what a deeper edit can actually reach

EDIT means **edit the voice**, not turn a knob. A knob is a performance control and has
to be reachable with one hand while the other plays; a page is for structure — a
boundary, a frame, a grain's direction — where the value is not something a hand sweeps
but a decision about what the material *is*.

Only SMPL has one today (§5b: the region and its loop). This section is what the other
two sources could carry, measured against the code rather than guessed, so the argument
happens before anything is built.

#### SYNTH: the bank is editable, one bank at a time

The constraints are all facts about the current build, and **one of them decides the
design**: there is no free storage in the data area.

| what | value | consequence |
|---|---|---|
| `WV_BANKS` | `static const int16_t[8][2048]` | in flash, XIP; editing means building a mutable copy |
| one bank | 8 × 256 × 2 = **4096 B** | — |
| `PLAT_STORE_MAX` | **3840 B** per object | a bank is 1.07 objects: storing the frames needs **two** |
| store slots in the data area | `OBJ_PANEL` 0xDC000, `OBJ_BLE` 0xDE000, then `PLAT_DATA_HI` 0xE0000 | **both are taken. Zero free pairs.** Each object is a pair of 4 KiB sectors, so a bank would need 16 KiB that does not exist |
| data area below them | 0x97000..0xDC000 = 69 sectors, **all** the resource region | the only place to take 16 KiB from is the sample pool's ceiling (276 KiB → 260 KiB) |
| RAM free | **11,756 B** | a 4 KB working copy fits |
| `OBJ_SETTINGS` | 0xFC000, Felucca's own pair, outside the data area | already holds this app's settings |

So the arithmetic splits the feature in two, and the split is the useful part:

- **A per-frame phase offset is 8 bytes** — one byte per frame. It fits in `OBJ_SETTINGS`
  as it stands, needs no new flash, and no RAM copy: it is a read-time offset, the table
  stays as it is in flash. That is the version that costs nothing to store.
- **Anything that changes the samples** — copy a frame onto another, rotate one — needs
  the 4096 B, therefore two objects, therefore 16 KiB carved out of the pool's ceiling.
  That is a real trade against polyphony of material, and it should be made on purpose,
  not discovered.

Eight banks would be 128 KiB, which does not fit under any layout, so "which bank did you
edit" is answered by SELECT, not by a list.

**What an edit is, though, is not "draw the wave".** 256 points across 240 px with no zoom
is a guessing game, and a cursor is a UI this panel does not have. The axis that fits both
the panel and the storage is the **per-frame phase offset** (0..255 points, one byte per
frame, 8 bytes total, in `OBJ_SETTINGS`).

Why it is not a shortcut: `wv_table_read2(fa, fb, phase, t)` reads both frames at the
*same* phase, so MORPH between two frames is a sample-by-sample sum of two shapes exactly
in phase. Giving each frame its own offset makes MORPH a phase scan of the table — the
idea a wavetable synth uses per frame, and it is audible well below anything that sounds
broken.

The cost is honest and **unmeasured**: the offset lands in the per-sample hot path — an
add and a wrap per frame read, per voice, at 44.1 kHz. That is measured before it is
promised, not estimated, and the measurement is cheap: the split renderer already reports
per-core CPU.

**Frame operations** — copy frame *n* onto *m*, rotate a frame by *n* points — reach
material that was never curated, and are the other half of the same page. They are the
half that needs the 4096 B, so they are the half that trades pool ceiling for flash. Two
page parameters, no editor.

**Not proposed: drawing a wave point by point.** 256 points across 240 px with no zoom,
and the panel has no cursor — it is a different instrument's UI, not a missing feature of
this one.

#### GRAN: the grain's motion, four axes, very different bills

The cloud today is deliberately uniform. Every grain reads at `g->inc = gr->rate` — the
same rate as every other grain in every other voice on that note; `wv_grain` has no `dir`
field; the window is fixed sin² and `WV_GRAIN_OVERLAP` is fixed at 2; `spawn()` clamps
position to `[0, len-1]`, which is why SPRAY is one-sided.

| axis | what changes | cost |
|---|---|---|
| **cloud region** | the slot stops being the whole cloud. `wv_grains_start()` already takes `[start, end)` and already wraps inside it — `app.c` passes `0, 0` | **call site only**, and it reuses the region marks SMPL already has |
| **reverse grains** | `g->inc` negative | a sign and a clamp: `read_at()` returns 0 for `pos < 0`, so a reverse grain must start at least `len_out × rate` into the material or its own window fades it out |
| **pitch spread** | per-grain random rate multiple at spawn; shimmer | one line in `spawn()` |
| **density** | overlap other than 2 | the expensive one: the sin²/cos² gain argument is **tied to overlap = 2** (§5c), so any other value needs real window-sum compensation |

The first is the one that matters most and costs least: a cloud over a chosen span is the
central control of a granular instrument — scanning position and length — and the
parameter already exists in the engine. It is also the same class of defect as the loop
that was wired to `start, end`: an API with the right arguments, a call site that does not
pass them. The measurement that will guard it is the one that guards the loop: hold a note
and watch where the cloud's grains actually read, not where the page says they should.

#### The order, and the thing that has to be settled first

Ordered by what is already true, not by what sounds best:

1. **GRAN's cloud region** — a call site. `wv_grains_start()` takes `[start, end)` and
   wraps inside it today; `app.c` passes `0u, 0`. Reuses SMPL's region marks, needs no
   storage, and is the axis a granular instrument is built around. The reference frame is
   one press of `B_EDIT` while SOURCE is GRAN.
2. **SYNTH's per-frame phase** — 8 bytes, no flash, no RAM copy. Blocked only on the
   hot-path measurement: if two extra phase operations per voice per sample cost more CPU
   than the split can absorb, the answer is no and it costs nothing to have found out.
3. **GRAN's reverse grains** — a sign and a clamp on `g->inc`; `read_at()` returns 0 for
   `pos < 0`, so the start has to move back by the window.
4. **SYNTH's frame operations** — the first item that costs flash, and therefore the
   first that is a trade against the 276 KiB pool. It waits until there is a reason.
5. **GRAN's density** — overlap other than 2 needs real gain compensation; it is the only
   one whose cost is a rewrite of the window argument, not a parameter.

**Persistence is upstream of all of them, and it is now built** (§8): `B_SAVE` writes the
whole panel state to `OBJ_SETTINGS`. A deep edit that is lost at power-off is a page nobody
uses, so this had to land first. The next two items still cost no storage, which is why
they come before the frame operations.

**And the page model is upstream of persistence.** If a second oscillator arrives, EDIT
stops meaning "this source's structure" and starts meaning "this voice's structure", and
the page model changes with it: SMPL's four boundaries, SYNTH's frame offsets and GRAN's
grain motion would compete for one button and one row of knobs. Deciding the voice-level
model first means all three have somewhere to go; building them first means re-homing all
three later.

---

## 6. Screen (240 × 240)

Three bands, heights derived from what each has to draw, with a compile-time
`#error` if they do not sum to 240. No mode word, no label for a value that is not
on screen.

```
┌────────────────────────────────────────┐  y 0
│  real-time scope  84 px, fixed scale   │
│  graticule: 5 verticals, 6 dB lines    │
├────────────────────────────────────────┤  y 84
│  material            92 px             │
│  SYNTH: the table frame + 60 px column │
│  SMPL/GRAN: envelope (+ grain bars)    │
├────────────────────────────────────────┤  y 176
│  CUT   RES   SHAPE   AMOUNT            │  64 px
└────────────────────────────────────────┘  y 240
```

**Band 1 — scope, y 0..83.** The actual post-mix output, captured in
`app_render` into a 768-sample `int16` ring (384 visible = 8.7 ms), drawn in
`app_frame`.

The vertical scale is **fixed and linear in amplitude**, pre-master: full scale
(0 dBFS) is the band edge at row 1 and row 83 around the centre at row 42, and
there is **no headroom above 0 dBFS** — anything louder clamps to the edge. A dB
axis is wrong for a *trace*: the value crosses zero twice a cycle and in dB a zero
crossing is -infinity, so the trace plunges to the floor every crossing (measured:
47 px of ink in all 240 columns).

The graticule's horizontal lines mark fixed dB values placed through the same
amplitude mapping, which is why they crowd towards the centre:

```
  0 dBFS  row  1   41 px from centre  (the band edge; labelled "+0")
 -10 dB   row 29   13 px
 -20 dB   row 37    5 px
 -30 dB   row 40    2 px
 -40 dB   row 41    1 px
 -50 dB   row 41    1 px  (coincides with -40)
```

Measured, MASTER full, every key at velocity 127: one note peaks at -13.2 dBFS
(9 px from centre), four at -5.4, eight at -5.0. A full chord does not reach the
edge — 8 × 0.256 = 2.05 only if the notes were in phase, and eight keys are not.

The trace triggers on the newest rising zero crossing and pins it to the right
edge, so a held note does not swim. Columns keep min/max, because a column can
span more than one sample.

**Band 2 — material, y 84..175.** What is being read, and by how much.

- **SYNTH** — the current wavetable frame, 172 px wide (x 4..175), drawn at full
  ink. The right-hand 60 px is a clean column for three lines: `BANK 1/8`, the
  bank's **name** (`SAW SQR`, `RESO`, `HOLLOW`, `VOX`, `DIGI`, `OPEN1..3`), and
  `MORPH 37`. The picture gives up the width because a frame drawn at full ink
  and bright text on top of it fight; 60 px is the widest label (`MORPH 100`,
  54 px) plus slack.
- **SMPL** — the slot's min/max envelope across the full 232 px, filled, at 2/5
  ink, with the slot name and `SLOT 2/2` over it.
- **GRAN** — the same envelope dimmed as the background, plus one bar per grain:
  **bar width = GRAIN SIZE** as a fraction of the material, **bar position =
  where the grain is reading**, **spread across four lanes = SPRAY**. Labels are
  `SLOT 2/2` (top right), `SPRAY 0` (bottom left), `GRAIN 45ms` (bottom right).

Every corner label names its parameter. A position is only a name when it is the
same name in every mode, and the top right is BANK in SYNTH but SLOT elsewhere.

**Band 3 — knobs, y 176..239.** Four knobs, each a name, a value and a bar.
`CUT / RES / SHAPE / AMOUNT` normally; `A / D / S / R` while the ENV page is up.
No encoder numbers: the position is the name. The one just turned highlights.

---

## 7. Budget

**5.8 ms per 256-frame block** (`APP_BLOCK 256`, 44.1 kHz). Platform: 360 MHz,
single core available for v1, single-precision FPU, no FMA, no `tanhf`.

Measured baseline, this machine, `2026-11-01`:

```
image 221,884 B;  RAM .data+.bss 76,388 B of 98,304;  pool 93,960 B of 344,064
```

**21.9 KB static RAM** and 250 KB of pool are free. Sample data is flash, not
RAM, so the RAM cost of the sample feature is a grain state array (bounded by
the budget below), not the audio.

`plat_cpu_pct()` is the only honest measure and it will be read, not estimated:

- **Gate A, SYNTH**: 8 voices, FOLD, AMOUNT max → **≤ 60%**, `plat_xruns()` 0.
- **Gate B, SMPL**: the worst legal configuration under the grain budget
  (4 voices × 4 grains) → **≤ 60%**, `plat_xruns()` 0.

If a gate fails, in order: cut grains, then voices, then move the shaper to the
mix bus (one instance instead of eight), then `cpu2`.

### The second core: yes, and the split is by voice, not by sample

`fm1.toml` sets `cpu2 = true` and `cpu2_fallback = true`. The platform's second
core (`plat_cpu2_run/wait/sync`, felucca-bsp's `fm1_cpu1.h`) is not an async
worker: **the render hands it a job, does its own share, and waits for it inside
the same block**. So it does not add a thread, it halves a block. That is why the
split has to be a *partition* and not a pipeline — there is no second buffer and
no place to put one.

**Cut by voice, not by sample.** A voice's state is a pile of floats written
every sample: four grain positions, two filter integrators, a VCA ramp. Splitting
by sample would have both cores touch every voice's floats every block. Splitting
by voice means each range's state has exactly one writer for the whole block, and
the only cross-core data is the output buffer — where the ranges add into
*different* addresses.

**One shared thing had to move out**: the per-sample smoothing of CUT and AMOUNT.
If each half advanced the same smoother, turning a knob would glide at twice the
rate whenever the split was on. So `voice_block` reads a local copy and the caller
advances the real value **once per block**. Same for the bus limiter: `tanh(a) +
tanh(b) ≠ tanh(a+b)`, so each half must not limit its own partial sum. The limiter
runs after both halves are in.

**Verified, not assumed**: `test_split.c` renders the same eight-voice scene
through one core and through two, and compares sample by sample — worst difference
**1.19e-7** (float summation order), including a case with the knobs moving. Two
fallbacks are tested too: a core that refuses the job, and a core that takes it and
then reports it was retired. Both render the full block on one core, because a
dropped job whose half is not re-rendered means four voices vanish — "slower", not
"wrong", is the requirement.

`cpu2_fallback = true` rather than the default: without it, any CPU1 misread
**restarts the FM-1**, and a restart mid-set is worse than losing half the
polyphony. With it the app keeps playing on one core until power-off, and the split
path degrades to exactly the single-core path.

**Measured on the device**: the split halves the time. GRAN at four notes read 51% of
the audio budget before this and 26% after, and eight notes read 51% in both.

That comparison is what proves the second core does the work, and it had to be measured
because nothing else can answer it. `plat_cpu2_wait()` returns -1 when the core has been
given up, and the caller then renders that half itself: a dead core yields complete,
correct audio and an unchanged load figure. Only a *drop* in the figure distinguishes
the two, and only once the cut moves — at a fixed cut through the pool's middle, four
notes sat in voices 0..3 and the reading was the same as eight.

The debug corner reports the job count and the per-job handover interval (`c2 94u240`),
and the second core's stack watermark (`s412`), which only that core can write. Neither
is a substitute for the load comparison: the handover interval covers CPU0's own half
too, so it reads about half a block whether or not the core did anything.

The stack is measured too, and the reading is the second proof. The debug corner shows
the second core's stack watermark as `s268`: 268 bytes of its 2816-byte user stack, 9.5%,
leaving 2548. Only CPU1 writes that area, so a nonzero depth is evidence it executed that
CPU0 cannot produce -- independent of the load figure, and it agrees with it. (The other
2816 bytes above are the supervisor stack, 256 bytes for exceptions only: this core takes
no interrupts, and a fault reaching that stack would be recorded by the platform instead
of passing silently, so the absence of faults is the reading for it.)

Still unmeasured: the 8 ms job timeout, and the cost of a job running *from flash* while
CPU0 reads the same flash. Those remain on the Gate B list. The clock cannot be reported at all: `C0_TL_CKCNT` does not
count on this part (see `fm1_perf.h` — "not verified on hardware" is now settled), so
the 360 MHz is configuration read from the PLL registers by the BSP, not a measurement,
and the app cannot see those registers.

---

## 8. Storage

**What is built today: the whole panel state survives a power cycle, and only when `B_SAVE`
is pressed.** `app/save.c` holds one record in `OBJ_SETTINGS`; `app_init()` loads it and
`B_SAVE` writes it. Turn a knob, pull the power, come back: the knob is where you left it.

**Sample data** — built. Not in the store: the resource region (`resources = FILE`) holds
it with its own CRC-32 commit, and the platform's installer path writes it over USB. Read
in place, 276 KiB of the data area.

**Parameters** — built. One object, `OBJ_SETTINGS` at 0xFC000 (Felucca's own pair,
outside the data area, already declared), written by `plat_store_save()` which is atomic
across its A/B sectors: an interrupted save cannot leave a half-written state.
`PLAT_STORE_MAX` is 3840 B per object; the record is **190 B** of it.

**The record is the parameters, not the performance.** In: the twelve knobs, the shape
amount, MORPH, the grain/SPRAY pair, the four envelopes, the LFO pair, the four arp
settings, the bank, the slot, the played source, and all eight slots' region + loop
(20 B each). Out: the grain playhead, the arp's held set, which page is open, and
`edit_zoom`/`edit_live` — how you were *looking* at the material, not what it is. A power
cycle returns the instrument to a sound, not to a moment.

**Why only `B_SAVE`, and not a save on change.** A save erases a 4 KiB sector, programs it
256 B at a time and then reads the whole thing back to verify it (`storage.c`'s `st_save`);
on the device that is tens of milliseconds of flash work. This app executes from the same
flash (XIP). Putting that behind a knob being turned is a stall you would hear as a click.
So the write happens when the player says so, including while notes are held — a save that
waits for silence is a save that does not happen when the chord is the thing worth keeping.

The price is that an edit lives in RAM until saved, and a parameter that turns and never
persists is exactly the kind of bug you cannot hear. So the panel says it: `save_dirty()`
is true whenever the live record differs from the one in flash, and the screen draws a dim
`NOT SAVED` mark in the top-left of the material band until it doesn't. It is compared by
the whole record once a frame rather than set by hand at each of the ~40 writers — one
forgotten `dirty = 1` would be that same silent bug, and comparing 190 B at 60 Hz is
cheaper than the risk.

**The version gate is real.** A record whose `version` is not `SAVE_VERSION` is rejected
and the defaults boot, so a firmware update that changes the layout cannot read an old
record's bytes as new fields.

**There is no free store slot in the data area.** `OBJ_PANEL` is at 0xDC000 and `OBJ_BLE`
at 0xDE000, and `PLAT_DATA_HI` is 0xE0000: the two pairs exactly fill the space above the
resource region, and every byte from 0x97000 to 0xDC000 is the sample pool. So anything
that needs a *second* object — a 4 KB bank, a preset list — is a pair of 4 KiB sectors
taken from either the pool's ceiling (276 KiB → 260 KiB) or `OBJ_SETTINGS`' own headroom.
That is a deliberate trade, and it is why §5d splits SYNTH's edit into an 8-byte version
and a 4 KB version.

Nothing else is planned for v1: no presets, no patch names, no per-bank settings.

---

## 9. Tests

Three layers, and each answers a different question. What is written so far is
marked; the rest is the plan.

### 9a. Built: the DSP alone (`sh app/tests/run_dsp.sh`)

The real `app/dsp/wv_dsp.c` compiled by plain `cc`, no platform, 43 checks. Every
check is written to fail on a plausible mistake, and several did (see §9d):

- table read at known phases, wrap at 1.25 and -0.75, morph clamping past the
  last frame, the read bounded to ±1
- each shaper curve is bounded, odd, and monotonic on -1..1 (fold is not
  monotonic outside it, which is the point); the four are distinct at x = 0.5;
  fold turns around past its peak where hard clip pins
- amount 0 leaves the signal alone; more amount drives harder
- eight keys take eight voices; the ninth steals the oldest; note-off releases
  only its own voice
- A4 renders at 441 Hz by zero-crossing count; the buffer stays bounded; a
  released voice frees itself; no voices means no output at all
- **the invariant that makes MORPH a timbre knob**: frames in a bank have equal
  RMS within 1.5 dB

### 9b. Built: a WAV from the real DSP (`sh app/tools/render_demo.sh`)

17.7 s: MORPH across a bank, the four shapes, AMOUNT swept, CUT swept under RES,
all eight banks, and an eight-note chord at the worst setting. Its purpose is to
be listened to — and to be *measured*, because listening and measurement catch
different bugs. Spectral centroid per step, measured on the rendered file:

```
MORPH      1193 -> 932 Hz, monotonic, pitch constant at 220.5 Hz
SHAPE      SOFT 2716 / HARD 2929 / FOLD 5683 / SINE 3145 Hz centroid
AMOUNT     985 / 2450 / 4587 / 5689 Hz as FOLD drive rises
CUT@RES100 270 / 612 / 1252 Hz as the corner opens
```

### 9c. Built: the app in the host simulator (`app/tests/play.txt`, `app/tests/smpl.txt`)

Wiring, not sound: a key reaches the engine and goes out as MIDI, a chord is
louder than one note, and after release the output returns to silence. Plus a pass
through the simulator's own recorder; the rendered chord measures 261.7 / 330.0 /
391.9 Hz for keys 7 / 11 / 14 (C4 / E4 / G4).

`smpl.txt` does the same for the sample path: SOURCE switches the pool in, a chop's
key plays that chop, a key above the chops falls back to the whole slot at its own
pitch, and switching back to SYNTH plays a wavetable note again. The demo pool is a
generated kit and tone (`tools/make_demo_pool.py`), so these checks are about the
reader, not about anyone's recordings.

### 9c-bis. Built: the sample path to a WAV (`sh app/tools/render_smpl.sh`)

5.6 s: the kit's four chops in order, then the tone at its root, an octave up and an
octave down, then four chops and the tone at full velocity together. Measured on the
rendered file, with the real reader and the real demo pool:

```
chops      4 slices, each 0.210 s except the last (0.029 s: its own captured tail)
           mutual correlation -0.001 -> four different hits, not one repeated
pitch      220.0 / 440.0 / 108.0 Hz at root / +12 / -12
           (108, not 110, is the increment's integer truncation at 11.025 kHz)
levels     whole file peak -5.2 dBFS, 0 samples at full scale
```

### 9d. What these found

Recorded because each one was a real defect that a reading of the code would not
have shown:

1. **FOLD inverted the signal.** The triangle was centred on 0 instead of +2, so
   the curve was -f(x): a phase inversion dressed up as a fold. Caught by "is odd"
   plus "is monotonic" — the obvious fold test (does it turn around) passed.
2. **MORPH was a volume knob.** OPEN3 swung 12.8 dB from frame 0 to frame 7,
   because a formant climbing the harmonic series gets spikier at equal partial
   count. Whole-bank peak normalisation cannot see this; per-frame RMS equalisation
   fixes it (now 0.0 dB spread). Caught by measuring the rendered demo.
3. **A solo note was at -19 dBFS.** Two separate 6 dB headroom factors, in the
   velocity map and in the voice gain, neither aware of the other. The demo
   measured -25.5 dBFS end to end.
4. **Eight voices clipped 2-9% of samples** at full amount, and the platform does
   not clip on the way to the DMA — it *wraps*. Fixed with a tanh bus soft clip:
   0.92-0.99 peak, zero clipping, and a solo note unchanged.
5. **The scope never appeared.** `cv_begin` without `cv_blit`: the band was drawn
   and never shown. Then, once shown, it occupied five columns out of 240, because
   the bucket index divided an absolute position by 184 (a bucket width) instead of
   wrapping the column index — a 256-frame block landed in one or two columns.
   Both caught by decoding the simulator's PNG and counting lit pixels per column.
6. **A chop ran to the end of the whole slot.** `wv_note_on_sample` was given a
   start but no end, so a 0.21 s chop of a 0.84 s kit played the rest of the kit.
   It does not sound like a bug; it sounds like the sampler ignoring the chop.
   Caught by rendering the chop and measuring where it stopped (11886 samples
   instead of the expected ~11891 — but the *first* measurement said 5999, which
   was the probe's own 6000-sample buffer, i.e. the test was measuring itself).
7. **A chop lost its tail.** The detector ended each slice at its first quiet
   sample, and a drum's decay crosses any fixed threshold repeatedly on the way
   down: a 0.18 s hit came out 0.029-0.050 s long. Fixed by making the slices
   *tile* — each ends where the next begins — with the last one ending at its own
   last audible sample. The old test asserted "slices do not overlap", which was
   the wrong invariant; it now asserts that they tile.
8. **The demo pool was rejected by the parser and nothing said so.** The generator
   wrote 16-byte entries against the format's 20 (offset, len, rate, bits, root,
   gain, name). The parser validates every entry before returning any, so the whole
   pool came back as 0 slots — and the app, correctly, played the wavetable
   instead. The symptom was "SOURCE does nothing". Two identical WAVs from two
   different SOURCE states is what pointed at the pool rather than at the switch.

### 9e. Built: the envelope (`sh app/tests/run_dsp.sh`, `test_env.c`)

The VCA used to be two `#define`s -- `WV_ATK 0.0113` (~2 ms) and `WV_REL 0.0012`
-- with a `target` and no sustain, so a held note was a gate: it reached full level
and stayed there. A patch with sustain 0, or any decay, could not be expressed.

Now `eg_a/eg_d/eg_s/eg_r` and a per-voice stage walk. What is asserted, against
rendered audio rather than against fields:

- the time curves' end points: attack 1 ms..4 s, decay and release 2 ms..8 s
- a 100 ms attack reaches 93% of its level at its nominal time, and is not a step
  at a quarter of it
- a 1 ms attack's largest sample-to-sample step is 0.0189, so it does not click
- **sustain 0 decays to silence with the key still down** -- the case the old code
  could not produce at all
- note-off during a long attack releases from the level it had reached (0.096 ->
  0.005 over 10 ms) rather than jumping to full first
- the defaults (A=0, D=64, S=127, R=32) hold a level exactly as the old gate did,
  so a patch made before the envelope sounds the same after it
- one voice releasing leaves the other seven at their levels

### 9e-bis. Built: the panel map (`app/tests/panel.txt`)

**71 assertions** in the host simulator, through `app_expect`. They expect
**exact** values, not bounds, because every encoder is relative and one detent is
known. They cover all seven encoders in all three sources, the ENV page's
takeover and return, that a page's edits do not leak into the tone controls,
BANK/SLOT on `EN_SELECT`, SPRAY on `EN_PRESET`, and GRAIN SIZE on `EN_ALGO`
without moving MORPH.

Confirmed the script can fail: putting MORPH back on `EN_ALGO` in GRAN produces
three failures naming `grain`. A test that cannot fail is not a test.

Two more suites guard the screen, and both were rewritten after they passed the
bugs they were written for:

- `app/tests/test_screen.py`, **32 checks** — band geometry, filled envelope,
  SYNTH's label column, the bank name line, every corner label carrying a name,
  and the 0 dBFS line sitting on row 1.
- `app/tests/test_grainmap.py`, **71 checks** — lane separation against SPRAY,
  bar width against GRAIN SIZE, and "no read, no mark" for the grain bars.

### 9f. Built: the EDIT page (`app/tests/test_edit.py`)

41 checks over the smallest page on the panel, because it is the one with the most ways to
be wrong while looking right. Both pictures and numbers are asserted, and the two defects
the page actually had are each pinned by a check that fails when the defect is put back.

What is checked, and the measurement behind each:

- **the region reaches the engine** — `region_on` / `region_start` / `region_end` /
  `region_lstart` / `region_lend` / `region_loop` are separate probes, because
  `app_expect` takes one name and one value and a page's state is not one number.
- **the four boundaries are four colours** — each hue is looked up as its RGB565 value at
  both its live brightness and its 3/5 brightness, and they are required to be in four
  different places. Injected: a shared colour makes every lookup land on the theme ink.
- **the playhead exists and moves** — white, in the tab rows of the material band, at two
  or more positions while a key is held.
- **a loop repeats between LOOP S and LOOP E** — followed over time, not looked at once:
  with a loop at 59..152 px of 232 the head reads 22, 51, 90, 129, 76, 115, 154. In from
  START, then 129 → 76 is the wrap. Injected: wiring the engine's loop to `start, end`
  puts the head at 169, 208, 15.
- **release leaves the loop** — with a 1.2 s release the head reads 132, 142, 161, 174 at
  20/120/320/450 ms after key-up, crossing LOOP E. Injected: keeping the loop through
  release turns that into 132, 142, 69, 81.
- **one detent is one pixel** — the step is compared against the window's own samples per
  pixel at 1×, 4× and 16×; a stale `len/1000 >> zoom` reads ratio 0.230.
- **the window centres on the boundary being turned** — measured at 128×, where a 1×
  window cannot show it at all (`span == len` makes `view` identically 0, which is why the
  first version of this check passed while the feature was broken).
- **the region cannot invert** — START and END driven past each other, both directions.
- **EDIT does nothing outside SMPL** — from SYNTH and from GRAN, `src` and `edit` both
  unchanged afterwards. Injected: the old "switch the source there" behaviour fails both.

### 9h. Built: persistence (`app/tests/test_save.py`)

45 checks. The host simulator's `reboot` re-runs boot against a flash image that outlives
it, so "turn a knob, reboot, read it back" is a real test and not a mock.

- **a knob survives a reboot** — CUT turned to 40, `reboot`, `cut` reads 40. Injected:
  skipping the `save_load()` call in `app_init()` puts it back at 127.
- **the region and its loop survive** — all four boundaries plus `region_on`, in a slot
  that is not slot 0, so a save that only wrote the current slot reads the defaults back.
- **the source and slot survive** — `src` and `smp_i` come back as set.
- **only `B_SAVE` writes** — turning a knob does not change `save_count`; pressing SAVE
  changes it by exactly 1. Injected: a save in the input handler's common path makes the
  first check fail.
- **a failed write is not a save** — with `flash_fail 1`, `save_last` is −1, `save_count`
  stays 0, and `save_dirty` stays 1, so the mark correctly says the edit is still only in
  RAM. Then the same edit with flash working reports 0.
- **the dirty baseline moves with the write** — `save_dirty` is 0 right after a successful
  save and 1 again after a further knob move. This is the one that pinned a real bug: when
  `save_now()` wrote the record but left `rec_written` stale, every later frame marked the
  panel dirty again, so the mark never cleared.
- **the version gate both ways** — a planted record of another version is rejected and the
  defaults boot; the same record at this version is accepted and its values boot. Both
  halves, because a gate that refuses everything passes the first half alone.
- **the mark is drawn, and out of the way** — the `NOT SAVED` ink is counted in the
  material band while dirty and gone once saved, and its extent stays left of x 176, the
  picture's edge. That second one came from a measured regression: anchored to the band's
  right edge the 42 px string reached back into SYNTH's label column and closed the 12 px
  gap `test_screen.py` asserts, so the mark moved to the top left. A clean-columns test
  could not catch it (the labels ink those columns anyway); the check is about the mark's
  own extent, measured by differencing the two screenshots.

### 9i. Built: the knob art (`app/tests/test_knob.py`)

20 checks on the smallest drawing in the app, which is also the one a player sees most:
the four knobs under the screen. A bar became a 270-degree arc with a tick, and every
property that makes the arc readable was got wrong in some draft of it, so each is
measured from a host screenshot rather than asserted from the source.

- **the gap is at the bottom and about a quarter turn** — the track colour's angles are
  collected on the ring, and the widest run of empty degrees has to be centred on straight
  down within 25 deg and be 70..110 deg wide. Injected: a full-circle span makes it 11 deg
  and fails.
- **the track spans about 270 degrees** — 360 minus the gap, 250..290.
- **the fill grows and the track gives way** — the filled and track pixel counts inside one
  knob's annulus are both monotonic across v = 0, 32, 64, 96, and the fill's range is more
  than 5x. Injected: a fill condition that is always true gives 82, 83, 83, 83 and no track
  at all.
- **it fills from the lower left, clockwise** — the fill's angles at v = 0 start at -135 and
  at v = 64 the run ends on the value's own angle from the formula (-135 + v/127 * 270).
  Injected: filling the last N segments instead of the first N puts the fill at -3..135.
- **the gap stays empty at every value** — 0 ink pixels inside +-30 deg of straight down,
  at all four values. Injected: a hand from the centre puts ink in the gap at v = 0.
- **the value is centred, the name is not** — the four values have to read as four objects,
  so the value's ink is centred within 6 px of the column's middle while the name starts at
  its left edge.
- **the ENV page has an arc under all four knobs** — a page that leaks a knob from the page
  before it is visible here.

Three mistakes this file made about itself, all found by the checks failing on correct
code: it computed the knob's centre from the source and was 3.5 px off (the band's canvas
origin is not where the reading assumed); it counted ink over the whole band, where the
value's text shares the fill's colour; and it expected the ENV page's arcs to be drawn in
`sk.ink`, when a page's knobs are lit and fill at `sk.ink_hi`. The centre is now measured
by a circle fit (31.5, 224.5) and recorded, the counts are taken inside an annulus, and the
mask includes all three brightnesses.

### 9g. The panel, as built

`docs/panel-map.md` is the authority and is checked by `panel.txt`; this section
records where the built panel differs from §3's intent.

§3 once gave SOURCE to `EN_SELECT` as a detented stop. It is on `B_SEL` instead,
and **that is the intended arrangement, not a stopgap**: a three-state switch on a
button plus an on-screen indicator costs one button, while a detented encoder for
the same thing spends a knob GRAN needs for SPRAY. `B_SEL` cycles SYNTH → SMPL →
GRAN and the whole screen changes colour with it (amber / green / blue-white).

The mode knobs are where §3 and the built panel agree, after one correction the
player made by looking at the panel: SELECT chooses the material (BANK or SLOT)
and is top right; ALGORITHM is MORPH or GRAIN SIZE and is bottom right; PRESETS
is SPRAY and is bottom left, GRAN only. The earlier assignment had the knob
labelled SELECT doing nothing in two modes.

**MORPH in GRAN is the one genuine override**: `EN_ALGO` is GRAIN SIZE there
rather than MORPH, because a grain voice never reads the wavetable. K1–K4 do not
move in any source.

---

## 10. Not in v1

Named so they are decisions and not omissions:

- Mip-mapped band-limited tables (4× flash, index per voice)
- 2× oversampled shaper (the decimation filter is the expensive half)
- Per-grain filter and pan; stereo grains
- Sample *recording* on the device — samples arrive over USB (the platform's
  resource path), not from the FM-1's own input
- Rate conversion between slots' rates — each slot is read at its own rate; a
  22.05 kHz slice and an 11.025 kHz table in the same voice do not have to
  match, because only one is sounding at a time in v1
- Unison / detune; filter envelope; velocity → cutoff
- Presets, patch names, program change
- Arpeggiator, sequencer (DriftSeq), MIDI thru / controller mode (fm1-ctrl)
- A second oscillator — and it is the reason §5d settles the page model before the
  features, not after

---

## 11. Open questions

1. **Pages**: ENV is a toggle, BTN_BANK is SMPL-only (§3) — settled. Open: does the
   header need the page's name, or is the changed bar set enough of a signal?
2. **Granular budget**: 4 grains × 4 voices as the starting point — does that
   match the texture you want, or is a dense 8-grain single voice more the
   point?
3. **Sample rate**: per-slot, 11.025 kHz default, 22.05 kHz for slices (§5b) —
   settled in the format. Open: is 22.05 kHz enough for sliced cymbals, or does
   the first slice that matters need 44.1 kHz (0.7 s per 30 KB slot)?
4. **Banks 6–8**: which three timbres, after SAW→PULSE, RESO, HOLLOW, VOX, DIGI?
5. **Scope**: post-mix output (proposed) or per-voice? Post-mix is what you
   hear; per-voice is a debugging view.
6. **Deep edit, and which source first** (§5d). Three candidates, in cost order:
   GRAN's cloud region (a call site), SYNTH's per-frame phase (a hot-path cost that
   has to be measured), GRAN's reverse grains (a sign and a clamp). Open: which one
   is worth the page, and does SYNTH's promise survive its CPU measurement?
7. **The page model, before a second oscillator exists** (§5d). EDIT is "this
   source's structure" today; with a second VCO it becomes "this voice's structure"
   and all three pages re-home. Open: decide the voice-level model now, or accept
   re-homing all three later.
