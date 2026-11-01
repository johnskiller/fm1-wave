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

Seven encoders. Grouped by function, so the pairs sit side by side on the panel.

| encoder | role | parameter | range |
|---|---|---|---|
| EN_SELECT | `EN_SELECT` | **SOURCE** | SYNTH / SMPL, detented — which oscillator this voice uses |
| EN_ALGO | `EN_ALGO` | **MORPH** | 0–7 continuous: frame position (SYNTH) or grain position (SMPL) |
| EN_K1 | `EN_K1` | **CUT** | 0–127 → 30 Hz … 16 kHz, exponential |
| EN_K2 | `EN_K2` | **RES** | 0–127 → damping 2.0 … 0.15 |
| EN_K3 | `EN_K3` | **SHAPE** | 0–3 detented: SOFT / HARD / FOLD / SINE |
| EN_K4 | `EN_K4` | **AMOUNT** | 0–127 → shaper drive 0 … 16× |
| EN_PRESET | `EN_PRESET` | **BANK** / **SPRAY** | SYNTH: bank 0–7, detented · SMPL: granular scatter 0–127 |

**The pairs are adjacent**: CUT/RES are the filter, SHAPE/AMOUNT are the shaper.
That is why CUT moved off `EN_PRESET` — a filter with its resonance two knobs
away is a filter you operate by reaching across the panel.

### The one encoder that follows the mode

`EN_PRESET` is **BANK in SYNTH and SPRAY in SMPL**, because the two halves of
the instrument want different things there:

- In **SYNTH** the eight banks are a timbre sweep you audition fast, exactly
  like MORPH — it belongs on a knob under a hand.
- In **SMPL** there is no bank to sweep; what you play is the material itself,
  and **SPRAY is the live control** (how far the grains scatter). Choosing
  *which* sample is a load, not a sweep — it is on the material page (below).

So the encoder is never dead: eight synth banks in one mode, grain scatter in
the other. That is better than a greyed bar in SYNTH, which is what it was
before this change.

**This does not violate "a knob means one thing while you play"**: the knob
changes exactly when **SOURCE** changes, and SOURCE is the one control that
redefines what the instrument is made of. The screen's middle band already shows
material in both modes, so what the knob is pointing at is on screen at the
moment you touch it.

**The cost, stated plainly**: the material picker is a live knob in SYNTH but a
page action in SMPL. The justification is real (a sweep you play versus a load
you choose) but it *is* asymmetric. If that asymmetry bothers you more than the
dead knob did, the alternative is BANK on the material page in both modes and
`EN_PRESET` = SPRAY always — then SYNTH greys that encoder again.

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
entry c × 16 B   { offset u32, len u32, rate u16, bits u8, root u8 }
```

Eight entries cost 128 B; the rest is samples, packed back to back, and a
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
the sample, N overlapping grains each have their own position, envelope and
pan.

**The bill, stated honestly: grains are voices.** A granular voice that layers
8 grains costs what 8 oscillator voices cost. At 8 polyphonic voices × 8 grains
the CPU is 64 oscillator-equivalents, which will not fit in 5.8 ms on one core.

So granular is **per-voice grain count, with a hard total budget**: the voice
allocator treats a grain as taking a unit of the budget (the platform's
`units()` concept in Saadet is the same idea), and the grain count of a voice
drops when more keys are held. Concretely:

- **SYNTH mode**: 8 voices, no grains. Full polyphony.
- **SMPL mode**: grain count × held voices ≤ budget. Start at **4 grains at 4
  voices**, and let the allocator halve grains before it steals a voice —
  dense texture and fewer notes beats clipped texture and more notes.
- **SPRAY** (EN_PRESET) scatters grain start positions; at 0 the grains are
  contiguous and SMPL behaves like an ordinary sample player, which is the
  escape hatch if the budget does not stretch.

Grain envelope: Hann, computed from a 256-entry quarter-table, not
per-sample `sin`.

**Grain shape is not in v1**: no per-grain filter, no per-grain pan in v1 (the
mix is mono per voice until the part's stereo stage). Named, not forgotten.

---

## 6. Screen (240 × 240)

Three bands. No mode words, no label for a value that is not on screen.

```
┌────────────────────────────────────────┐  y 0
│        real-time waveform scope        │  240 × 112
├────────────────────────────────────────┤  y 112
│  ▁▃▅ ▂▄▆ ▃▅▇ ▄▆█ ▅▇█ ▆██ ▇██ ███       │  240 × 64
│   current table's frames, MORPH marked │
├────────────────────────────────────────┤  y 176
│ B▏▁▂▃  M▏▁▂▃  C▏▁▂▃  R▏▁▂▃  S▏▁▂▃       │  240 × 64
│ A▏▁▂▃  P▏▁▂▃                            │
└────────────────────────────────────────┘  y 240
```

**Band 1 — scope.** The actual post-mix output, peak-held: captured in
`app_render` into a 240-sample ring (one sample per 184 rendered samples, peak
kept so transients survive), drawn in `app_frame`. Silence draws a flat line —
silence is real data and is not dressed up.

**Band 2 — the material.** The current table as waveform thumbnails with the
MORPH position marked. In SYNTH mode that is the 8 frames of the bank; **in
SMPL mode it is the sample's envelope with the grain window drawn over it** —
so the band always answers "what am I playing and where in it am I", in both
modes, with no mode word.

When **SOURCE** or the bank picker is turned, this band becomes the chooser for
one second (candidates, current one marked), then returns. Transient overlay,
no label: the content says what it is.

**Band 3 — knobs.** Seven bars in panel order (SOURCE, MORPH, CUT, RES, SHAPE, AMOUNT,
and the mode's own last knob: BANK or SPRAY), single-letter name and a filled bar. The one just turned highlights for 300 ms. **A numeric readout appears
only next to the encoder being turned**, then goes away: six numbers you cannot
read while playing are six pixels of noise. When a page is up (ENV, or the
material page) the bars become that page's parameters — the bottom band always
shows the seven things the seven encoders would do right now.

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

---

## 8. Storage

**Parameters**: one store object, saved on change with a debounce, written by
`plat_store_save()` (atomic across its A/B sectors — an interrupted save cannot
leave a half-written state).

**Sample data**: not in the store. The resource region (`resources = FILE`)
holds it, with its own CRC-32 commit, and the platform's installer/restore path
writes it. `backup = true` is required for `resources`, which also gives the
user a way to get the samples back out.

Nothing else persists in v1: no presets, no per-bank settings, no names.

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

### 9e. Planned

- envelope timing; a stolen voice never leaves a non-zero level (partly there)
- grain overlap-add sums to a constant
- EN_PRESET is BANK in SYNTH and SAMPLE in SMPL; BTN_EDIT re-chops the current slot
- the screen redraws the band that changed and not the others
- on hardware: both gates, and audio *heard*, not measured

### 9f. The panel, as built (differs from §3 in one place)

`B_SEL` toggles SOURCE. §3 gave SOURCE to `EN_SELECT` as a dead stop and the ENV
page to `B_ENV`; the ENV page does not exist yet, so the switch that makes the
sample path reachable took the first free button. When the ENV page is written it
takes `B_ENV` back and SOURCE returns to `EN_SELECT`. Recorded here so the
difference is a decision with an expiry, not a drift.

`SOURCES` in the UI band shows what the keys are playing: the bank's name in SYNTH,
the slot's name and its chop count in SMPL, and `NONE` when the resource region is
missing or not this build's.

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
