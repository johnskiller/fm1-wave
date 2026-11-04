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
keys ─► allocation ─┤                   ├─► shaper ─► SVF ─► VCA ─► mix ─► scope
       (8 voices)   └── granular ───────┘   (shape,   (cut,res, (env)
                     per voice, one of         amount)  type,ktrack)
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

### Pages: a button that advances, and the voice as a page

Things that do not fit on the seven encoders are a **page**: press the panel button
and the encoders become that page. A short label in the header says which page is up
while it is up.

**The page's own button does not close it.** It advances: press again and you are on
the page's second screen, then the third, then back on the first — a cycle, because it
is the only way to walk the screens. Moving to another page is just pressing that
page's button. Coming back always starts at screen 1. A one-screen page (ENV, ARP)
therefore rounds onto itself, so a second press is a no-op.

**The voice is a page, and `B_HOME` enters it.** So the panel has no "close" and no
"exit": every button names a page and presses it, and the page you are on is whichever
button you pressed last. The voice page has one screen today — one oscillator — so
`B_HOME` on it is a no-op like ENV; **with the second VCO it becomes two screens (VCO 1
/ VCO 2) and `B_HOME` rounds between them**. The 2-VCO plan of using `B_HOME` to choose
the oscillator therefore needs no separate button.

Decided on the instrument 2026-11-03, when the LFO gained a second screen: one button
cannot mean "next" and "close" at once, and the version that meant whichever the page
happened to need would have surprised the player the first time they pressed a
two-screen page expecting it to close.

Screen counts are one table (`PAGE_SCREENS[]` in `app.c`); the LFO has two and is the
page expected to grow (a second LFO, LFO waveform choice, modulation targets). The lamp
belongs to the page rather than the screen, and the caption carries the number
(`LFO PAGE 2`). Full detail, including the lamp rule, is in `panel-map.md`.

**Toggle, not hold.** Holding needs a hand, and the point of an envelope is to
turn four knobs while playing — a hold gesture spends the hand that should be
on the keys. Hold is also invisible: nothing on the panel says "you are in a
page" until the bars change under your fingers.

- **B_ENV** → **envelope page**: ATTACK, DECAY, SUSTAIN, RELEASE on K1..K4,
  three encoders spare. Live in both modes. One screen, so a second press is a no-op.
- **BTN_BANK** → **material page**: in SMPL, the samples in the resource region
  (and grain size, window, overlap); in SYNTH, nothing — the bank is already on
  a knob, so the button stays **dark** in SYNTH. A dark LED is the panel's own
  way of saying "not in this mode", which is a convention the instrument
  already uses elsewhere.

Pages are mutually exclusive (opening one closes the rest), and **turning any encoder
whose page is up does not leave the page** — you move with another page button
(`B_HOME` for the voice). Leaving by timeout was considered and rejected: an envelope
tweak with a pause in it should not silently revert to the tone page under your fingers.

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

**[`wavetables.md`](wavetables.md) is the reference for these**: every frame of every
bank drawn, what each bank's spectrum actually is, the measured statistics (crest, level,
centroid per frame), where each of the eight came from, and — importantly — what is
*not* true about them. Two of the five that share WT's names are a different sound;
SAW SQR and HOLLOW are near-duplicates of each other.

**Aliasing: no bank reaches it on this keybed.** Linear interpolation between points
removes staircase error, not harmonics that were above Nyquist when the table was built.
Measured at the highest key, G5 = 440 Hz, the worst partial in any frame of any bank is
DIGI's 24th at 10,560 Hz against 22,050 Hz of Nyquist — 10.5 kHz of headroom, so nothing
folds anywhere on the instrument (F3..G5). This is a property of the 27-key keybed, not
of the tables: more partials, or a keyboard extended upward, would reach it. Mip-mapped
band-limiting is then the real fix, costs 4× the flash and an index per voice, and is a
**named future change**. Per-bank numbers: [`wavetables.md`](wavetables.md).

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

### 5c1. Granulating a bank — **NOT BUILT**

*Written 2026-11-04 as a design note at the player's request. Nothing here is
implemented; §5c describes what the code does today.*

Today GRAN reads a **sample slot**: got from flash by `plat_res()`, seconds long, at some
rate, pitched from a `root` note. A second possibility is to point the same grain engine
at a **synth bank** — the 8 × 256 int16 tables of §5a — so the cloud is made of the
instrument's own synthetic material instead of a recording.

**The engine barely needs to know.** `wv_grains` touches a `wv_sample` for five fields
only — `data`, `len`, `rate`, `root`, `gain` (`wv_grain.c:79`, `read_at`). And
`WV_BANKS[WV_BANK_COUNT][WV_FRAMES * WV_POINTS]` is one **contiguous** array of int16
(`build/gen/wv_banks.h`), which means:

| material | samples | at rate 1 | what it is |
|---|---|---|---|
| one bank | 2,048 | 46.4 ms | that bank's 8 frames, in order |
| all eight banks | 16,384 | 371 ms | the whole bank table as one corpus |

So a bank can be handed to the cloud as a `wv_sample` view with `rate = 44100`,
`gain = 256`, and `len` set to either figure. No DSP change, no new buffer, **no RAM** —
the tables are already in flash and read in place (XIP). This is the cheapest thing in
this document: an adapter, a source check, and a decision about `root`.

#### Why it is worth considering at all

Granulating a *sample* scatters a recorded sound. Granulating a *bank* scatters a
designed spectrum: each frame of a bank is a different cycle, so reading across frames
means the grains are made of the morph itself. That is a different instrument, not a
second way to play the same one, and it is the reason this is a spectrum-design idea
rather than a memory-saving idea.

**How this relates to PPG / Waldorf, precisely.** Checked 2026-11-04 against the sources
below, because the names were being used loosely:

- **PPG Wave 2.3**: 30 wavetables, 64 waves each, 256 points per wave, **8-bit**; only
  about **250 waveforms are actually stored**, and most table positions are interpolated
  between them (Hermann Seib's table document, quoting the "over 1800 waveforms"
  marketing figure).
- **Waldorf Microwave**: 32 ROM tables (the Wave 2.3's 30 plus two), **61 waves plus
  tri/saw = 64 positions**, waves stored as **64 bytes, 8-bit, positive half-cycle only**,
  the negative half mirrored — a 12-bit output derived from that. Its name for the method
  is **DSWS, Dynamic Spectral Wavetable Synthesis**.
- **Neither machine is documented as granulating its own wavetables.** What they do is
  *scan* a table from a modulation source. So this section is **not** "PPG did it, we
  copy it".

What *is* borrowed from that lineage is the model: **a cycle that changes, and a position
in that change as something a player travels.** What is different here is that the grains
read the table as **material**, so the position in the morph becomes a dimension the cloud
is scattered over instead of a line a single oscillator walks. §5c3 records that the
lineage's actual mechanism — the scan position as a **modulation destination** — is the
planned next stage and is **not built yet**; this section is the thing that comes after
it, not a substitute for it.

#### The constraint that decides the whole design

**One cycle is 256 points, so a grain must advance 256·hz/44100 samples per output sample
to stay in tune.** A grain can be no longer than the material divided by that rate, which
makes grain length a function of pitch — something a sample-based cloud never has to
think about, because a sample is seconds long.

With all eight banks contiguous (16,384 samples):

| key | Hz | rate | material available | longest possible grain |
|---|---|---|---|---|
| F3 | 174.6 | 1.01 | 16,164 | 366 ms |
| A3 | 220.0 | 1.28 | 12,829 | 291 ms |
| C4 | 261.6 | 1.52 | 10,788 | 245 ms |
| C5 | 523.3 | 3.04 | 5,394 | 122 ms |
| G5 | 784.0 | 4.55 | 3,600 | **82 ms** |

`python3 app/tools/grain_bank_ceiling.py` reproduces this table and the one below it from
the DSP headers, so the day `WV_POINTS` changes the arithmetic gets re-derived instead of
going stale.

The GRAIN knob spans 10–200 ms (§5c above). **The top of that knob is unreachable from
E4 upward** — measured, the ceiling is 194 ms at E4, 163 ms at G4, 122 ms at C5. With one
bank instead of eight the whole table shrinks by 8× and even F3 only reaches 46 ms — so
**one bank as material is not a usable configuration at the top of the keybed; all eight
contiguous is the only version worth building.**

Three ways to handle the ceiling, none chosen:

1. **Clamp the knob to what the key allows.** The knob's own range becomes pitch-dependent.
   Honest, and it makes the limit visible instead of audible.
2. **Exceed it and let the grain read past the end**, where `read_at` returns silence
   (§5c): the grain window still runs, so it fades rather than clicks, and the cloud gets
   quieter as the knob passes the limit. Simple, but it turns a limit into a level
   change, which is the kind of thing that reads as a fault.
3. **Ignore it in v1** and note it in the manual. Worst of the three.

#### What else changes, and must be decided

- **`root` has no meaning for a bank.** The sample path uses `wv_smp_inc`, which is
  `rate/44100 × 2^((note−root)/12)`: it is *relative* to where the material sits. A bank
  is an oscillator table, where SYNTH instead uses `hz/WAVE_HZ` — *absolute*. Something
  has to pick a reference note for bank material. F3 (the lowest key) making rate 1.0 is
  the least surprising choice, and it is a decision, not a default.
- **MORPH becomes a dead control in GRAN.** §5c's existing text already says this for the
  current GRAN ("MORPH has nothing to read in a grain voice"), and that is why `EN_ALGO`
  is GRAIN SIZE there. If banks become grain material, MORPH plausibly gains a job —
  *which part of the bank table the cloud may wander in* — and by the player's
  what-you-see rule (§3) a control that is visible must do what its label says. Either
  MORPH restricts the corpus window, or the GRAN page stops showing it. **Note that §5c3
  makes MORPH a modulation destination**, which would give it a second job in the same
  source; the two sections have to agree before either is built.
- **Eight banks contiguous is a corpus, not eight corpora.** Reading across a bank
  boundary means a grain can span SAW SQR's frame 7 into RESO's frame 0. That is either
  the feature (371 ms of continuous, related material) or a click, depending on the
  window and where the boundary falls. It has not been measured.

#### Where it would live on the panel

No new control. `B_SEL` cycles SYNTH → SMPL → GRAN; a fourth state (**bank-fed granular**)
would be a fourth press, or a second parameter of the existing GRAN state. The panel has
no spare switch for it and the page model (§3) has no spare page. Whatever is chosen, it
must not take a knob from GRAN's existing seven, and it must not make the source
indicator ambiguous — the three-colour icon (amber / green / cyan) has no fourth colour.

#### Costs

- **Flash: 0.** The tables are already there.
- **RAM: 0.** XIP, read in place.
- **CPU: 0.** A grain reading a bank does exactly the work a grain reading a sample does
  — one interpolated read. §5c's bill (`grains × voices`) is unchanged.
- **The real cost is the constraint above**: pitch-dependent grain length, and a corpus
  measured in hundreds of milliseconds rather than seconds.

#### Not decided, and not to be assumed

Whether this ships, which of the three ceiling policies applies, what `root` is, what
MORPH does, and whether a grain may read across a bank boundary. This section exists so
that the arithmetic is on record before anyone writes the adapter and discovers the
grain-length ceiling by ear.

#### The version that removes the ceiling: morph into a ring, then granulate it

*The player's suggestion, 2026-11-04: "动态的 morph 之后再喂给 gran".* It is the better
idea of the two, and it dissolves the constraint above rather than living with it.

The adapter reads the bank **as** material, so the material is a 256-point cycle and pitch
sets how fast it is consumed. If instead the SYNTH voice renders normally — correct pitch,
correct morph, correct envelope — and its output is **written into a ring buffer**, then
the grains read ordinary 44.1 kHz audio:

| | adapter (read the bank) | ring (record the synth) |
|---|---|---|
| material | 16,384 samples of *table* | N samples of *audio* |
| grain rate | `256·hz/44100` (pitch-dependent) | **1.0** (pitch-independent) |
| longest grain | 366 ms at F3 → **82 ms at G5** | **the same at every key** |
| pitch comes from | the key, via the read rate | the synth, already correct |
| morph | a position in the corpus | **history: the buffer holds the sweep** |
| RAM | 0 | the buffer |

So the pitch-dependent ceiling **does not exist here**, and `root` stops being a question —
the synth already decided the pitch. That removes two of the three hard problems in one
move.

**RAM: measured, not estimated.** `build.py app` reports **pool 97800 B of 344064** — 240.5
KiB free — while `.bss` has only 10.8 KiB, so the ring has to live in the pool:

| length | mono int16 at 44.1 kHz | |
|---|---|---|
| 1.0 s | 86.1 KiB | |
| 2.0 s | 172.3 KiB | comfortable |
| 2.5 s | 215.3 KiB | near the limit |
| 3.0 s | 258.4 KiB | does not fit |

Mono is what a grain engine needs, so 2 s is a real budget rather than a round number.

**What this buys that neither PPG nor Waldorf could do.** Their oscillators are one cycle
wide: they have a *position* in a table but no *past*. A ring of a swept morph means the
cloud can contain **several different morph positions at once** — grains started at
different points in the buffer are different timbres sounding simultaneously. The thing the
grains scatter over stops being time (a sample) or spectrum-at-one-instant (the adapter)
and becomes **the instrument's own recent history**. That is a genuinely new axis, and it is
the one reason to prefer this over the adapter.

**The timing problem it creates, which has to be decided first.** Grains read material from
the *past*, so:

- If the cloud is the **only** voice path, a note's first 10–200 ms has no material to read
  and is silent. `wv_grains` places two grains on the first block so a note does not fade
  in (`wv_grain.c:19`, "why a note starts with two grains") — but that trick works by
  reading a grain that *would have* started a hop earlier, which only makes sense when the
  material is a sample that already exists. A ring that is still filling has nothing at
  that position, and `read_at` returns silence past the end (`wv_grain.c:69`), so the
  attack problem is real and is not solved by the existing code.
- If the SYNTH's direct output **plays alongside** the cloud, the attack is clean and the
  grains fade in as the buffer fills — but then the source is two things at once, and
  "GRAN" would no longer mean "grain only".
- §5c3's second ENV and second LFO are what would make either version expressive; the ring
  is most useful once the morph can be swept by something other than a hand.

Not chosen, not designed, not scheduled. Recorded so the numbers are on the table before
anyone writes it.

#### Better implementations to weigh against the adapter

The adapter above is the cheap version: pretend the bank is a sample. It is worth being
explicit that it is also the crude version, and that granulating a *table* invites things
granulating a *sample* does not.

- **Read the cycle, not the timeline.** A bank is 8 × 256 — a cycle that changes 8 times,
  not 2,048 samples of timeline. A grain could instead read **one frame for its whole
  life** and let successive grains take successive frames, so every grain is a clean
  single cycle and MORPH becomes "which frame the grains are drawn from". That also
  removes the pitch dependence of the *material* — a grain needs one cycle, not a
  fragment of the corpus, so what it consumes per grain is 256 points at the grain's own
  rate rather than a slice of a fixed 16,384 — at the cost of a per-grain frame index.
- **Phase-locked grains.** Because the material is periodic, grains can start on cycle
  boundaries instead of at arbitrary offsets. That is what would keep a cloud in tune
  without the `rate` arithmetic, and it is the difference between "granulating a wavetable"
  and "granulating a recording of a wavetable".
- **The morph axis as the grain axis.** Use MORPH to set which frame a grain reads and
  let the 8 frames be the scatter dimension: the cloud becomes a distribution over
  timbre rather than over time. This is the one idea here that has no sample-based
  analogue at all, and it is probably the reason to build this rather than the adapter.
- **Band-limiting by construction.** Every grain reads an integer number of cycles, so a
  grain's spectrum is exactly the frame's spectrum — no interpolation across a
  discontinuity, and none of §5a's aliasing question arises inside a grain.

None of these is designed, costed or chosen. The **adapter** is still the thing to build
first, despite the ring being the better instrument: the adapter is a day's work and it
answers the only question that matters — whether granulated table material is worth the
rest — without spending 172 KiB of pool on the answer.

### 5c2. The filter's tap, and key tracking

Both live on the **LFO page's second screen** (`B_LFO` twice). They had a `B_FX` page of
their own while they were new; the player's judgment was that they are not a subject of
their own — they are how the LFO's filter behaves — so they belong to the LFO's page.
That also frees `B_FX` for the effects the instrument does not have yet. CUT and RES
stay on K1 and K2 on both screens, so the two screens agree about where the filter is.

**TYPE (K3): LP / HP / BP.** Zavalishin's topology-preserving SVF computes `lp`, `bp`
and `hp` every sample as intermediate results of one iteration; the voice read `lp`
and discarded the other two. So the selector is a move out of registers that are live
anyway — no second filter, no extra arithmetic. Measured at CUT 40 (217 Hz) against a
262 Hz tone, the three taps give distinctly different output, and the **order is
HP > BP > LP**, not the textbook BP > HP > LP: this SVF's `bp` is the integrator state
(`bp = g*hp + s1`), which scales differently from the normalised `H_bp`. The tests
assert the measured order and the fact that the curves cross as CUT moves (LP rises
past the tone, HP falls), which is what a selector that was stored and never read
could not produce.

**KEY TRK (K4): 0 / 25 / 50 / 75 / 100 %.** How far the corner follows the note, in
percent of one octave of corner per octave played, from **C4 (60)**. At 100 % a note an
octave up has its corner an octave up; at 0 % every note has the corner the CUT knob
says. Five settings rather than a 0..127 sweep on purpose: a knob that can land on 37 %
makes two patches that should match not match.

The ratio is applied as a **multiplier on the shared `cut_z` ramp**, not as a per-voice
smoother and not as a per-voice frequency. The ratio between two notes is exact either
way, but only this way keeps one ramp for eight voices — a per-voice smoother would make
the knob glide at whatever rate the last note set, and re-deriving `g` from a scaled
frequency would add a `tan()` per voice per sample for the same picture.

Cost: one `fm_exp2f` per voice per block (8 per block, against 2048 voice-samples), and
one multiply per voice per sample when the setting is on. The chip's FPU divides in
hardware — `pi32v2 -mcpu=r3 -mfprev1` compiles `a/b` to one instruction and the image
holds 34 such divides, none calling a helper — so the division this page's arithmetic
could have added was never the constraint. Only `double` falls back to `__divdf3`.

The reference note is C4, so turning tracking on never moves the note a patch was voiced
on. Measured: C4's output is bit-identical at 0 % and 100 %.

### 5c3. Modulation — **BUILT** (LFO half) / **NOT BUILT** (second ENV)

*Recorded 2026-11-04 because it was being mistaken for a gap in the design. It is not: it
is the next stage of it, and the panel was built to take it.*

The player's plan, stated 2026-11-04: **MORPH as a modulation destination, multiple LFO
targets, a second LFO, a second ENV.**

**Now built** (`41a1f40` DSP, `8e3f32f` + `b317ec6` + the K3 change UI): multiple LFO
destinations, seven of them, and a second LFO. **Not built:** the second ENV — that half of
this section is still a schedule, and the tables below keep their `NOT BUILT` reading for it.

#### What exists, exactly (as built)

| | now | in `wv_state` |
|---|---|---|
| LFOs | **2**, each per instrument, not per voice | `lfo[2]`, each with `rate`, `phase`, `shape`, `trig`, `sync_div` |
| LFO destinations | **7 in the DSP, 6 reachable from the table** | `lfo[2].amt[WV_DESTS]`: CUT, RES, SHAPE, MORPH, GRAIN, SPRAY, PITCH — PITCH has no ring, see below |
| LFO shape | SINE / TRI / S&H | `lfo[i].shape` (`WV_LFOSH_*`) |
| LFO retrigger | free-running or note-on reset | `lfo[i].trig` (`WV_LFO_FREE` / `WV_LFO_TRIG`) |
| LFO rate | Hz, or a note division when synced | `lfo[i].rate` + `lfo[i].sync_div`, `wv_lfo_hz_for()` |
| ENVs | **1** (the VCA's ADSR) | `eg_a`, `eg_d`, `eg_s`, `eg_r` |
| ENV destinations | **1: the VCA level** | `level`/`target` |
| MORPH destinations | LFO 1 and 2, via `WV_DEST_MORPH` | one add in the block hoist (`morph_pos()`) |

The LFO page reaches this in **three screens**:

| screen | LFO | K1 | K2 | K3 | K4 | SELECT | PRESET | ALGO |
|---|---|---|---|---|---|---|---|---|
| 1 | 1 | CUT | RES | RATE | **DEPTH** | — | — | — |
| 2 | 1 | SHAPE | RES | **PITCH** | **DEPTH** | MORPH | (GRAIN) | (SPRAY) |
| 3 | 2 | SHAPE | RES | **RATE** | **DEPTH** | MORPH | (GRAIN) | (SPRAY) |

**One word per byte, in one place.** `K4` on screens 1, 2 and 3 is the same byte,
`lfo_amt[i][WV_DEST_CUT]`, so they carry the same name -- **DEPTH** -- and sit on the same
knob. SHAPE moved to `K1` on the two tables to keep it there (player, 2026-11-04: *"P2 P3 把
depth 和 shape 交换一下，这样就可以保持 depth 始终在 K4"*). They used to be called
"DEPTH" and "CUT", which collided with screen 1's `K1` -- the corner itself, 30 Hz..16 kHz --
and gave two different numbers one word (player, 2026-11-04: *"P1 还有一个 K1-CUT，指的就是
真正的 CUT 频率…而且此处无法统一成 CUT"*). A ring on this table is always an AMOUNT, so
`DEST_NAME[]` says DEPTH for that destination. The SELECT/PRESET/ALGO row above the band reads
the same table and needs no second spelling: it holds only MORPH, GRAIN and SPRAY (`ROUTE_ORDER`
slots 3..5), so it can never name CUT and cannot disagree with it.

**Signs: bipolar prints none, unipolar that matters prints `+`.** The panels have no glyph
above ASCII 126, so `±` cannot be drawn. A route that swings either side of its destination
(CUT's DEPTH) therefore prints a bare number -- a lone `+` would claim it goes up only. PITCH
is the one amount whose direction carries meaning (0..+6 semitones, up from the note) and keeps
its `+`. The player's rule: *"无法表示双向就不表示了吧，连+号都不要。单向的用+"*.

`B_LFO`'s own button walks all three, which is the page rule (§4): a page's button cycles its
screens and never closes it, and a button's meaning belongs to the panel rather than to
whichever screen is up.

**LFO 2 gets no picture screen, and that is the correct shape, not a saving.** The only thing
on screen 1 that belongs to LFO 1 alone is DEPTH — which *is* the CUT route's amount, and
routes live on the table. RATE is the one LFO setting that has no ring anywhere, because RATE
is not a destination and cannot be one. So LFO 2's table spends K3 on RATE and gives up
PITCH; LFO 1's table spends K3 on PITCH and gives up nothing, because its speed already has a
knob on screen 1.

It was two screens plus `B_REC` as a selector between the two LFOs, and that was a defect
against the same rule: the same key did two different things depending on where you were,
and the selector could disagree with the caption. **`lfo_edit` is not a stored byte any
more** — it follows from `page_no` (1–2 = LFO 1, 3 = LFO 2), so there is nothing that can
fall out of sync and nothing to save.

The same rule fixed a second defect on screen 2: `B_SEL` switched source on screen 1 and did
nothing on screen 2, because the route table `return`ed out of `update()` after taking its
six knobs and skipped the button handlers further down. A screen may borrow the **knobs**
(it covers the picture they are drawn on); it may never borrow the **buttons**.

Each table is **six destinations plus K3's RATE.** A ring there has three states, and they
are told apart by shape and not only by brightness, because amount zero and a slot the
source does not have read identically when both were a dim ring:

| state | drawing |
|---|---|
| amount > 0 | ring filled to the amount, the value in the hole |
| route exists, amount 0 | **no ring**, a bright `OFF` — "here, turned down" |
| no route on this source | dim ring, `NOT` over `USED`, dim — "this knob does nothing here" |

A zero route has no arc to fill, so an empty ring there was claiming a scale that is not
there. K3's RATE uses the same three shapes: an LFO at 0 Hz is a route turned down, not a
knob the source lacks. `B_REC` selects which LFO both
screens edit, and its lamp shows which — the button matrix gives each button one bit and
one colour, and only PLAY has a second LED, so lit/unlit is the vocabulary this hardware has.

**PITCH is the one destination with no ring.** The table is seven positions wide and there
are seven destinations, but K3 holds RATE instead, and the player chose which one to drop
(2026-11-04): PITCH is the only route whose value is already reachable two other ways —
its own encoder, and pitch bend — so it is the least load-bearing of the seven. The DSP
still routes it (`wv_pitch_scale`, `inc_base`/`rate_base`); only the panel ring is gone,
so a patch saved with a PITCH amount keeps it. That asymmetry is deliberate and is the kind
of thing that reads as a bug later, hence this paragraph.

#### What the second ENV still costs — **NOT BUILT**

Everything below is about the ENV half only. The LFO costs it once listed here are now
measured rather than estimated, and they came out at what this section predicted.

Three facts that matter for the schedule:

- **MORPH is already resolved once per block, not per sample** (`wv_dsp.c:675`), with the
  comment saying so. That was a measured optimisation (the frame pair was 12–16 % of the
  read path). It means a modulated MORPH costs **one add before `wv_frames_at`** per voice
  per block — the interpolation itself is unchanged. This is the cheapest of the four.
- **The LFO is shared across voices on purpose** — one sine, not eight. That is the right
  shape for a filter sweep and the wrong shape for anything per-note, which is why a
  **second** LFO is a different instrument rather than a second copy: one shared, one
  per-voice, and the per-voice one is what a wavetable scan wants.
- **`depth` means octaves today** (`0..127` → `0..4` octaves either side of CUT,
  symmetric in octaves, see `lfo_oct`). A destination that is not a frequency cannot use
  that scale. PANEL: `DEPTH` therefore has to mean *"of this destination's own range"*,
  not octaves, the moment there is more than one destination — or the page needs one more
  number than it has knobs. **How it was resolved:** DEPTH stayed octaves because it stayed
  CUT's amount, and every other destination got its own amount on the route table, each
  printed in its own unit (`dest_text()`: octaves for CUT, percent for RES and SHAPE,
  frames for MORPH, semitones for PITCH, milliseconds for GRAIN, percent for SPRAY).
  **The span itself lives in `wv_dsp.h`** (`WV_CUT_SPAN`, `WV_MORPH_SPAN`, `WV_PITCH_SPAN`),
  one copy for both the DSP and the ring: the ring promises a sweep and the DSP has to make
  exactly that sweep, and two literals is how a readout starts lying (the MORPH readout did,
  for a release). PITCH's span is **6 semitones, unipolar** -- up only, half an octave at full
  amount, one encoder detent per 5 cents -- cut from 12 on 2026-11-04 because an octave either
  way made the control unusable for the vibrato it is for: *"pitch 范围太大，不实用了，
  细微的 pitch 抖动才是有用的，先看一半"*. The
  page did not need one more number than it has knobs; it needed a second screen, which it
  had.

#### What each addition costs, before anything is designed

- **MORPH as a destination: nearly free.** The block hoist already exists; the value is
  `k_morph + mod` saturated to the frame range. No new state per voice.
- **Multiple destinations: an amount per destination.** One shared LFO with N targets
  needs N depth values, and the panel has **one** DEPTH knob. Either the page grows a
  screen per destination, or the model becomes a small table (destination, amount) that
  a page edits one row at a time. The second is the one the existing page model (§3) can
  already express: pages have screens, screens have four knobs.
- **A second LFO: the same `.bss` question as everything else.** State is small
  (`rate`, `depth`, `ph` ≈ 6 B); the cost is that the page, the lamp rule and the save
  record all have to learn about it. The save record is **190 B with `PLAT_STORE_MAX`
  3840** (§8), so a second LFO and a second ENV fit with room to spare; what does not fit
  is unbounded growth of the (destination, amount) table. **Measured after building it:**
  the record went from 190 B to **v3**, which carries two full LFO field sets, and the
  seven-amount table is 2 × 7 bytes. It fits. The real cost was not storage but the page:
  three screens and a record version bump that refuses v2 rather than reinterpreting it.
  A selector key turned out not to be needed — the page's own button walks the three
  screens, and the first attempt (REC) was reverted because it broke the page rule.
- **A second ENV: per-voice state.** The VCA envelope is per-voice already — `level`,
  `target` (4 B each), `estage` plus 3 B of padding = **12 B per voice**, so a second one
  is **96 B at 8 voices** plus four shared stage bytes in `wv_state` where `eg_a..eg_r`
  already live. Not a constraint. The constraint is the **ENV page has one screen and
  four knobs**, all of them taken (§4). A second ENV needs either a second screen on
  `B_ENV` (which the page model already supports — `PAGE_SCREENS[]`) or a `B_HOME`-style
  selector.

#### Order that the code suggested, and how it went

1. **MORPH as a destination, from the existing LFO.** — **done** (`7f157ef`). One add in
   the block hoist, exactly as this section said it would be.
2. **A destination table** — **done** (`41a1f40`). The one-DEPTH-knob problem resolved the
   way the section predicted: the model became a (destination, amount) table that a page
   edits one row at a time, and the page model (§3) expressed it as a second screen.
3. **Second LFO** — **done** (`41a1f40`). This went before the second ENV, which is not the
   order written here; the reason is that the route table was being drawn and needed a
   second LFO to be worth drawing, whereas a second ENV changes no picture.
4. **Second ENV** — **NOT BUILT.** The cost estimate below still stands and has not been
   re-measured, because nothing has been built against it.

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
frame, 8 bytes total, in `OBJ_SETTINGS`). **Built** — see §5d-i below.

Why it is not a shortcut: `wv_table_read2(fa, fb, phase, t)` reads both frames at the
*same* phase, so MORPH between two frames is a sample-by-sample sum of two shapes exactly
in phase. Giving each frame its own offset makes MORPH a phase scan of the table — the
idea a wavetable synth uses per frame, and it is audible well below anything that sounds
broken.

#### 5d-i. Built: the per-frame phase offset, and what it actually does

**Storage: 8 bytes of table + 2 bytes of setting, in `OBJ_SETTINGS`.** No flash, no RAM
copy: the tables stay `static const` in flash and the offset is applied at read time.

**The read.** `wv_table_read2o(fa, fb, ph, t, o0, o1)` is the offset form of
`wv_table_read2`. The fractional part of the phase is *shared*: an offset of n whole points
moves `i0` and `i1` by n and leaves `ff` alone, so a per-frame offset costs **one add and
one mask per frame read per sample** — not a second phase wrap and not a second float
multiply. The two frames' offsets are resolved **once per block** in `voice_block`, next to
the frame pair, because MORPH does not move inside a block. When every offset is zero the
per-sample test `(o0 | o1)` takes the old path, so an untouched bank runs the code it
always ran — asserted bit for bit, not by ear.

**What it sounds like, measured.** Same note, same MORPH position, same shaper; the only
difference is the offsets. `app/tools/render_foff.sh` writes one WAV per pair — the note
with offsets off, then the same note with them on, back to back:

| pair | rms | spectral centroid |
|---|---|---|
| SAW SQR, 32 pt/frame | 0.1695 → 0.1522 (−10.3 %) | 1908 → 1359 Hz (−28.8 %) |
| SAW SQR, 8 pt/frame | 0.1695 → 0.1682 (−0.8 %) | 1908 → 1476 Hz (−22.6 %) |
| SAW SQR, 64 pt/frame | 0.1695 → 0.1302 (−23.2 %) | 1908 → 1446 Hz (−24.2 %) |
| OPEN2, 32 pt/frame | 0.1786 → 0.1793 (+0.4 %) | 1966 → 1644 Hz (−16.4 %) |
| VOX, 32 pt/frame | 0.1530 → 0.1707 (+11.6 %) | 6381 → 5990 Hz (−6.1 %) |

The direction is **down** in every case: two frames in phase sum to a wave with sharper
features, and turning them against each other softens the sum before the shaper. The
level does not move by a fixed amount — it is a timbre change that on SAW SQR also loses
1 dB — which is the honest description: this is not a volume trim, it is a different
waveform.

**The renderer has to clear its buffer, and that is a real trap.** `wv_render` *adds* into
`out` (wv_dsp.h: "added to what is there: the caller may mix several parts"); the platform
gets away with it because its buffer is the DMA bank it just handed back. A file renderer
that reuses one buffer must clear per block. Measured the wrong way: five identical takes
of one note gave rms **0.17, 0.33, 0.46, 0.56, 0.62** — a rising staircase that looks like
the offset "building up" and is nothing of the kind. Every number in the table above was
taken after that was found and fixed, and the fixed renderer is deterministic: plain
0.169523 twice, offset 0.152147 twice.

**The page.** SYNTH's `B_EDIT` (§9f-bis) — K1 picks a frame, K2 turns it, K3 fans all
eight out, K4 picks the shape of the fan. `B_EDIT` used to mean SMPL's region editor and
nothing else; it now means "edit this source's structure", which is the model §5d asks for
and the one a second oscillator would extend rather than replace.

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
| **density** (grain count) | overlap other than 2 | the expensive one: the sin²/cos² gain argument is **tied to overlap = 2** (§5c), so any other value needs real window-sum compensation |
| **stereo spread** | per-grain pan across the field | the render path is mono today (§5c: one `out` buffer, one write). Panning each grain needs the voice to produce L and R, which is the same change a stereo SYNTH would need — so this axis is priced by the "per-grain filter and pan; stereo grains" line in §10 (Not in v1), not by a `pan` field. A per-grain pan whose output is summed to mono would be inaudible, which is the trap to avoid |

The first is the one that matters most and costs least: a cloud over a chosen span is the
central control of a granular instrument — scanning position and length — and the
parameter already exists in the engine. It is also the same class of defect as the loop
that was wired to `start, end`: an API with the right arguments, a call site that does not
pass them. The measurement that will guard it is the one that guards the loop: hold a note
and watch where the cloud's grains actually read, not where the page says they should.

#### The order, and the thing that has to be settled first

Ordered by what is already true, not by what sounds best:

0. **~~SYNTH's per-frame phase~~ — BUILT** (`e1a14ab`, page `3d7daa4`). Eight bytes, no
   flash, no RAM copy, and the hot-path measurement came back fine. K1 FRAME, K2 OFFSET
   (points of 256), K3 SPREAD (points per frame), K4 SHAPE. It is also the page that
   proved the model in §5d: a source's structure, on EDIT, with nothing else competing
   for the button.
1. **GRAN's cloud region** — a call site. `wv_grains_start()` takes `[start, end)` and
   wraps inside it today; `app.c` passes `0u, 0`. Reuses SMPL's region marks, needs no
   storage, and is the axis a granular instrument is built around. The reference frame is
   one press of `B_EDIT` while SOURCE is GRAN.
2. **GRAN's reverse grains** — a sign and a clamp on `g->inc`; `read_at()` returns 0 for
   `pos < 0`, so the start has to move back by the window. The player's first named GRAN
   edit control.
3. **GRAN's density, then stereo spread** — both are named as GRAN edit controls, and both
   are engine work rather than page work: density needs the window-sum compensation, and
   spread needs a stereo path before a per-grain pan means anything.
4. **SYNTH's frame operations** — the first item that costs flash, and therefore the
   first that is a trade against the 276 KiB pool. It waits until there is a reason.
5. **GRAN's density** — overlap other than 2 needs real gain compensation; it is the only
   one whose cost is a rewrite of the window argument, not a parameter.

**Persistence is upstream of all of them, and it is now built** (§8): `B_SAVE` writes the
whole panel state to `OBJ_SETTINGS`. A deep edit that is lost at power-off is a page nobody
uses, so this had to land first. The next two items still cost no storage, which is why
they come before the frame operations.

**The page model, decided** (player, 2026-11-03). The collision above does not happen, and
the reason is that the two axes were never the same axis:

  - **`B_EDIT` edits a SOURCE.** Not a voice, not an oscillator -- the thing SOURCE is
    currently on. SMPL has its region editor, SYNTH has the frame rack, GRAN will have
    what a grain needs: reverse, density, how far the cloud is spread across the stereo
    field. Each of those is a property of the material, so each of them belongs on the
    button that is already pointed at the material.
  - **`B_HOME` chooses between the two VCOs of one patch** (planned, not built), as
    **screen 2 of the voice page**: two oscillators are a property of the VOICE, and the
    voice is a page (§3), so the VCO selector is that page's second screen rather than a
    separate mode. Pressing `B_HOME` walks voice screen 1 → 2 → 1, which is the same rule
    as every other page.

That split is what makes the three source editors stop competing: adding GRAN's page does
not take anything from SMPL's or SYNTH's, and a second VCO does not take anything from any
of them. It also settles what the word "EDIT" has meant all along -- §5d's opening line
says edit the voice, and the pages built since then have all been source structure. The
button was named for the deeper idea and has been used for the narrower one; the narrower
one is what it does.

`B_HOME` is the right button for it: the platform reads it only inside the RESTORE
prompt (`unity.c:340`, "no" to a factory restore) and PANIC was the only other candidate.
A VCO selector is a per-patch mode, which is closer to "home" than to the panic the panel
already reaches through all-notes-off at power-cycle — and with the voice modelled as a
page it is not even a new mode, just that page's second screen.

Still to settle when a second VCO is actually built: what the screen does with two
oscillators' worth of picture (the material band draws one source today), and whether the
four knobs follow the selected VCO or stay on the voice's shared filters. Neither is
blocking, because both are visible the moment the button exists.

---

## 6. Screen (240 × 240)

Three bands, heights derived from what each has to draw, with a compile-time
`#error` if they do not sum to 240. No mode word, no label for a value that is not
on screen.

**One transfer per band, and that is not a style choice.** `lcd_data()` calls `lcd_sync()`
first — it waits for the previous DMA to finish before starting the next — and the panel
runs at 60 MHz, ~44 MHz effective measured, so a 240 × 92 band is about **8 ms** of wall
time. The material band had **five** `cv_blit(0, SCOPE_H)` calls in a row (the `SCOPE_H`
argument is where the band is sent, no longer what it holds), left over from when
that band was drawn in layers: four of them re-sent the same pixels, one transfer after another,
≈ **32 ms of every frame** spent waiting to send a picture that was already on the glass.
The last blit is the one that wins, so nothing on screen ever depended on the other four.
One per band now — scope, material, knobs — and `frame_blits` in `app.c` is a probe on the
count, so `test_screen.py` fails if a second transfer appears (injected: 4 per frame, 2
checks fail).

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

**Band 3 — knobs, y 176..239.** Four knobs, each a name and a 270-degree ring with the
value inside it.
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
is true whenever the live record differs from the one in flash, and the screen draws a line
until it doesn't. It is compared by the whole record once a frame rather than set by hand at
each of the ~40 writers — one forgotten `dirty = 1` would be that same silent bug, and
comparing 190 B at 60 Hz is cheaper than the risk.

**The line, and why it replaced the words.** It used to be the text `NOT SAVED` at the top
left of the material band, and that was measured as damage: in the label's own 13 x 62 box on
SYNTH, 443 px of the waveform were erased to make room (403 px on SMPL and GRAN), in the
brightest part of the picture. The replacement is two rows in the padding the band already
had — `y 87..88` — `MAT_PAD` rows 3..4 (`SAVE_LINE_ROW`), not its top two. The top two were the
first attempt and the player rejected them on sight: `y 83` is the scope's 0 dBFS graticule,
a full-width 240 px grey line at `(98,97,106)`, so the save line started on the very next
row with no air, and two full-width bars on consecutive rows read as one crowded block
("离scope的下线太近"). Three rows of gap now separate them, and the wave's first ink is
`y 91`, so a row of air remains below the line at its heaviest.

It spans the full 240 px, not the picture's 232: it is a readout of the panel, not a picture
of the material, and starting at `x 4` left a 4 px gap that read as a mistake.

It carries two numbers on two axes, because both questions are worth answering and neither
fits the other:

- **length** = how many groups differ from flash, over `SAVE_LINE_FULL_CHANGES` (8) groups
  across 240 px, so 30 px per group. A group is a saved field, and a region counts once —
  dragging a loop boundary is one edit, not four. The ceiling is 28 (17 parameters, 3
  selectors, 8 regions); 8 was chosen because a run of edits is short and 8 px per group
  would make the first few invisible.
- **colour** = how long they have been left that way, from `dim(ink, 3, 5)` to `ink` over
  `SAVE_LINE_FULL_MS` (5 minutes) in four steps, then flat. The floor is 3/5, not 2/5:
  measured against each skin's own ground the 2/5 step sat only dLum +26 (SYNTH), +34 (SMPL)
  and +26 (GRAN) from the background, which the player reported as not standing out
  ("一开始的红色不够醒目"). At 3/5 the same numbers are +56 / +68 / +58. Four steps are
  kept rather than three because at three, 2 minutes and 0 seconds drew the same colour —
  a ramp that does not move for the first third of its life.
  `ink_hi` is deliberately not the ceiling: that is the colour a knob arc fills with, and a
  line in it reads as a fifth control.

**B_SAVE's lamp says the same thing, on the button you would press about it.** Three levels,
because that is what the matrix can show: the glow every unlit button has (saved), `mid`
(felucca-bsp's steady ~12 % level, an edit waiting) and `lit` (the line is full). The top
level is tied to `SAVE_LINE_FULL_MS` rather than given a threshold of its own, so "the line
is full" and "the button is fully lit" are one event said in two places and cannot drift.

**What the tests cannot see about that lamp.** `plat_host.c`'s `plat_lights_mid` takes a
button mask and a key mask and discards the buttons (`(void)b; mid_keys = k & ~lit_keys`), so
the simulator cannot record a mid *button*. Measured: deleting the `plat_lights_mid` call
leaves `test_save.py` at 55/55 green. The app's own state machine (`save_led` 0/1/2, and the
299 s / 301 s boundary around the line's threshold) is covered; a B_SAVE that sat at glow
while an edit waited would be caught by eye on the device and by nothing here. Closing that
means teaching the host to record mid buttons, which is a change to `platform/`.
The lit path *is* checked end to end — `leds` prints the lit set, and `SAVE` must be out of
it while unsaved and in it once the line fills.

Because the colour is the source's own `ink`, the line is amber in SYNTH, green in SMPL and
cyan in GRAN. That is the skin's rule applied consistently, not the line having a palette of
its own; the player asked, and the answer is "it follows whatever source you are looking at".

Nothing is drawn when the record matches flash. A permanent empty track would be two rows of
grey meaning "saved", which is the absence of news.

The count is `save_change_count()`, which the save line added to `save.c`. `save_scan()`
compares field by field rather than in one `memcmp`, because it has to say *how many* and not
just *whether*; at 190 B once a frame this is not worth optimising.

**A unit with no record has no baseline.** `B_SAVE` is the only writer, so a fresh unit's
`OBJ_SETTINGS` is empty — and the boot defaults are not all zero (`region_reset_all()` gives
every slot its whole self; `region_end` is 52369 for the demo PAD). Comparing a real record
against a zeroed one reported "unsaved" from the first frame. Measured: `expect save_dirty 0`
passed before any frame ran and read 1 after `wait 100`. `save_adopt()` now takes the state
`app_init()` settled on as the baseline when flash held nothing. This was already wrong
before the line existed; it was invisible because the old mark was two words whose meaning at
boot nobody had asked about.

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
2. **MORPH was a volume knob.** OPEN3 swings 3.2 dB from frame 0 to frame 7 under
   whole-bank peak normalisation alone, because a formant climbing the harmonic
   series gets spikier at equal partial count. Peak normalisation cannot see this;
   per-frame RMS equalisation fixes it (now 0.00 dB spread, all eight banks).
   Measured by rebuilding the bank with the RMS step removed. (This entry said
   12.8 dB for a while; that number does not reproduce and was wrong.)
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

- `app/tests/test_screen.py`, **88 checks** — band geometry, filled envelope, the page
  walk (advance, round, `B_HOME`), the lamp rule and the scope graticule,
  SYNTH's label column, the bank name line, every corner label carrying a name,
  and the 0 dBFS line sitting on row 1.
- `app/tests/test_grainmap.py`, **71 checks** — lane separation against SPRAY,
  bar width against GRAIN SIZE, and "no read, no mark" for the grain bars.

### 9f. Built: the EDIT page (`app/tests/test_edit.py`)

45 checks over the smallest page on the panel, because it is the one with the most ways to
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

### 9f-bis. Built: SYNTH's EDIT page, the phase ramp (`app/tests/test_synth_edit.py`)

56 checks. The page is the first whose four knobs are **not four independent values**: K3
(SPREAD) rewrites all eight frames at once, so the page has a ramp the eight values are
derived from, and K2 pulls one frame off it. That is why the checks are mostly about
*relationships* between frames rather than about one number.

- **the table is the sum of its parts** — `foff_sum` is a probe on purpose. LIN at 8
  points a frame is 8 × (0+1+…+7) = **224**, and that number cannot be reached by an
  implementation that stores the knob position and forgets to multiply. Injected: K3
  storing SPREAD without calling `foff_ramp()` fails three checks.
- **one frame is not the ramp** — after K2 moves frame 0, frame 1 stays where the ramp put
  it. Injected: an edit path that rebuilds the ramp on every knob fails here.
- **ALT is not half of LIN** — ALT puts the odd frames at the full step and the even ones
  at zero, so its sum is 4 × 8 = **32**, not 112. The first version of this check asserted
  "half" and was wrong about its own arithmetic; the number now comes from the formula.
- **EN_PRESET clears all eight, and SPREAD with them** — `foff_sum` 0 *and* `foff_spread`
  0. Injected: clearing only the selected frame leaves 224 of table behind while the page
  would read zero.
- **the offsets are saved but the frame index is not** — the table and the two ramp
  settings round-trip through `reboot`; `edit_frame` deliberately does not, because which
  frame the page points at is a view, like which page is open, and saving it would open a
  page on power-up. There is no check on the absent field: an assertion about a field that
  is not in the record is a check that cannot fail. Its absence is visible where it belongs,
  in `save_rec`'s definition.
- **the reboot is not trusted to clear statics** — `plat_host.c` calls `app_host_reboot()`
  under `#ifdef APP_HOST_REBOOT`, which this app never defines, so a simulated reboot keeps
  every static. A record that is written and never read back therefore passes a naive
  round-trip. The script turns the value *off* between the save and the reboot; a restore
  that does nothing then reads 0 and the check fails. Injected: dropping the restore fails
  one check.
- **SMPL's page is still SMPL's** — after `B_SEL` to SMPL, `B_EDIT` opens `edit_src == 1`.

### 9g-bis. Built: the page walk, and the lamp rule (`test_screen.py`, `test_arp.py`, `test_fx.py`)

The page model is the kind of rule that is cheap to implement and expensive to get
subtly wrong, because every page's button is one branch and five of them could each
disagree. Checked as behaviour, on the host:

- **the walk and the round** — `tap LFO` → `page_no 1`, again → `2`, again → `1`, again
  → `2`. Injected: a version that stops on the last screen fails one check.
- **one screen is a no-op, not a special case** — ENV and ARP pressed twice stay up.
- **`B_HOME` enters the voice page from any page and any screen**, and the page's knobs
  come back: after HOME, `cut` reads 127 again on each of the four pages. Injected: a
  HOME that does nothing fails 8 checks in `test_screen` and 2 in `test_fx`.
- **HOME on the voice page changes nothing**, and the voice's knobs still work afterwards
  (`cut` 127 → 111 after one detent). A button that quietly edited a parameter from the
  voice page would be worse than one that is dark. Injected: giving the voice page two
  screens fails 2 checks.
- **the voice is page 0 with one screen** — `page` and `page_no` after HOME, and three
  presses in a row leave `page_no` at 1.
- **screen 2 is still the LFO's page** — `K3` there moves `ftype` and does *not* move
  `lfo_rate`, and the LFO lamp stays lit. Injected: letting screen 1's knob handler
  answer on screen 2 fails two checks.
- **the lamp rule** — LFO blinks when armed and its page is shut; ARP likewise
  (`rate != 0`); ENV is solid while up and **never** blinks; `B_FX` lights nothing.
  Injected: ARP not blinking fails two; FX blinking anyway fails one; EDIT's lamp back
  to `src == SRC_SMPL` fails one.

### 9h. Built: persistence and the save line (`app/tests/test_save.py`)

55 checks. The host simulator's `reboot` re-runs boot against a flash image that outlives
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
- **the save line, both of its axes** — length is asserted through `save_line` (0 px at
  boot, 30 px for one changed group, 0 again after the save), colour through `save_ink`
  (0..3, reaching 3 only after 5 minutes) and through the pixels: the fully-bright line must
  be exactly `ink`, and `ink_hi` must not appear. The line is also counted on both of its
  rows while unsaved and on neither once saved, and the picture's top four rows (`y 86..89`)
  must stay empty so a long line cannot become a lid on the waveform. SMPL and GRAN are
  checked separately because the line takes the source's own colour — and they measure
  60 px, not 30, because switching SOURCE is itself a saved field, which is the check that
  caught length counting knobs instead of groups.

### 9i. Built: the knob art (`app/tests/test_knob.py`)

25 checks on the smallest drawing in the app, which is also the one a player sees most:
the four knobs under the screen. A bar became a 270-degree arc with a tick, then the ring
grew to `r = 23` with a 5 px stroke and the value moved inside it, and every property that
makes the arc readable was got wrong in some draft of it, so each is measured from a host
screenshot rather than asserted from the source.

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
- **the value is centred inside the hole, the name is not** — the four values have to read as
  four objects, so the value's ink is centred within 2 px of the column's middle while the
  name starts at its left edge. The value is sampled **by row**, over the 9 rows its glyphs
  occupy, not through a circle: a radial window at the hole's radius catches the stroke's
  rounded shoulder once the stroke is thick, and read a centre of 25 against 31.
- **the value fits inside the hole** — its measured width against the **chord** at glyph
  height, 35 px, which is what bounds the stroke at 5 px and the EDIT page's text format at
  seconds with three decimals.
- **the ENV page has an arc under all four knobs** — a page that leaks a knob from the page
  before it is visible here.
- **the stroke carries no pinholes** — the background is flood-filled from outside each
  knob's box; anything still background, inside it, unreachable from the edge and inside the
  stroke's own radial band is a hole. 8-connected, because a 4-connected flood splits a
  diagonal contact and over-reports holes a person cannot see. `r_lo = 18.5` excludes the
  glyphs' own counters -- the hole inside a "6" is the letter, not a defect. **This check
  exists because the art shipped with 6 holes per knob:** drawing the stroke as five 1 px
  Bresenham chains cannot tile a 5 px band. Putting the old five-chain drawing back fails all
  four values at 6 px each.

Five mistakes this file made about itself, all found by the checks failing on correct
code: it computed the knob's centre from the source and was 3.5 px off (the band's canvas
origin is not where the reading assumed); it counted ink over the whole band, where the
value's text shares the fill's colour; it expected the ENV page's arcs to be drawn in
`sk.ink`, when a page's knobs are lit and fill at `sk.ink_hi`; it took the value's ink
through a radial window at r = 19, which is the stroke's own innermost radius at a 5 px
stroke, so it measured the ring; and it computed the hole's chord with the font's cell
height of 15 px instead of the 9 rows its glyphs actually ink, which read as "a 5 px stroke
cannot fit the widest value" when it does. The centre is measured by a circle fit
(31.5, 216.0) and recorded, the value is sampled by row, and the chord uses the measured
glyph height.

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

1. **Pages**: the page model is settled (§3) — a page button advances through its
   screens and rounds, the voice is a page entered by `B_HOME`, coming back starts at
   screen 1. Open:
   does the header need the page's name, or is the changed bar set enough of a signal?
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
