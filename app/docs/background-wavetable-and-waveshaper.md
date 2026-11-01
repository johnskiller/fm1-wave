# Wavetables and waveshapers: what they are, what they cost

Background for fm1-wave. Two sections, because these are two different things
that get confused: a wavetable is *what the oscillator reads*, a waveshaper is
*what happens to the signal after the mixer*. Read the second half knowing that
it cannot fix a thin-sounding table, and the first knowing that a rich table
can be ruined by a careless shaper.

Numbers here are for this platform: **44100 Hz, 256-frame blocks (5.8 ms),
360 MHz single-core, single-precision FPU without FMA.** The 5.8 ms is the
whole budget for one `app_render` call. Nothing below is faster than that
number; it is the wall.

---

## Part 1 — Wavetables

### The idea

A classic oscillator computes its waveform every sample: a sine is four
multiplies, a saw is a phase wrap. That is fine for simple shapes and hopeless
for anything else — try computing "the spectrum of a clarinet" in four
multiplies.

A wavetable inverts this. You precompute **one cycle** of the waveform into an
array, then the oscillator becomes a lookup: a phase accumulator that walks
through the array and wraps. Any shape at all costs the same at runtime,
because the shape was paid for at build time.

Two numbers define a table:

- **N**, the points per cycle. 256 is the usual choice. More N means more
  high harmonics fit before the table itself aliases; 256 points hold 128
  harmonics, and 128 harmonics of a 100 Hz note is 12.8 kHz — above that the
  table is the limit, not the ear.
- **F**, the frames. A single cycle is a static timbre. A *bank* of F cycles,
  walked by a knob, is a sweep: this is what "wavetable" usually means in a
  synth. Eight frames is the classic minimum; Melodee's banks use 8×256.

Storage: 8 frames × 256 points × 2 bytes (int16) = **4 KB per bank**. Flash is
generous here (581 KB app slot), so banks are not the constraint; RAM is.

### Reading it: the two interpolations

Reading a bank needs two independent interpolations, and both are on the hot
path:

1. **Along the wave** (between sample i and i+1). The phase rarely lands on a
   point. Linear interpolation is the default: `y = a + (b-a)·frac`, two
   multiplies and a subtract.
2. **Between frames** (between frame f and f+1, from MORPH's position). Same
   linear form, computed once per block if MORPH does not move per-sample, or
   per sample if it does.

That is roughly **4 multiplies per sample per oscillator** before any filtering.
For 8 voices at 44100 Hz that is 1.41 M multiplies/second — at 360 MHz, about
0.4% of the CPU in multiplies alone, which sounds free and is not: the real
cost is the memory traffic, the pointer arithmetic, the envelope, the filter,
and the fact that all of it sits inside a 5.8 ms interrupt.

### The problem nobody escapes: aliasing

This is the part worth understanding properly, because it decides the
architecture.

A table is sampled data. A 256-point table of a saw contains harmonics up to
the 128th. Play that table at 440 Hz and the 128th harmonic is 56 kHz —
**above Nyquist (22.05 kHz)**. Everything the table holds above Nyquist does not
disappear; it folds back down and lands on frequencies that were not in the
signal. The folded partials are inharmonic, they move the wrong way when you
play up the keyboard, and they sound like grit or a metallic buzz. This is
aliasing, and it is the single biggest quality difference between a toy
wavetable oscillator and a good one.

Three ways out, in increasing cost:

**a) Band-limit per octave (mip maps).** Build several versions of each frame,
each low-passed to suit a range of notes; the oscillator picks the version by
its current pitch. A 4-level mip set (Nyquist for the top octave, then half, a
quarter, an eighth) costs 4× the flash and a table lookup by note. This is what
Serum-class software synths do, and what "band-limited wavetable" means.
Costs here: 4 KB → 16 KB per bank, plus an index computation per voice.

**b) Oversample inside the oscillator.** Not usually worth it — it is the
*table* that is band-limited badly, not the arithmetic.

**c) Do nothing, and accept it for the top two octaves.** Melodee's engine does
this: `eng_wt.c` reads its bank with a linearly interpolated frame and a
linearly interpolated sample, and no mip selection at all. It is a deliberate
trade — at 149 lines it stays comprehensible, and the worst of the aliasing
sits where the ear is least offended (high notes, dense spectra). If fm1-wave
starts here, it should start here *knowingly*, and the mip option should be
left as a named future change rather than a surprise.

**The honest summary:** linear interpolation between table points is *not* an
anti-aliasing measure. It reduces the staircase error of reading a table; it
does nothing about harmonics that were already too high when the table was
built. Those are separate concerns and the second one is the expensive one.

### Choosing the frames

A bank's frames should share partials, so a sweep never snaps: frame f and f+1
should differ in *amplitude* of the same harmonics, not in *which* harmonics
exist. Walking from a saw to a square is smooth (the even harmonics fade); saw
to a bell is not (partials appear from nowhere and the sweep clicks).

The practical recipe, straight from the Melodee generator: define each frame as
a sum of sines with per-harmonic amplitude and phase, then **normalise the
whole bank together**, not each frame separately. Normalising per frame makes
every frame peak at full scale, which means MORPH changes loudness as it
sweeps — the sweep sounds like a volume knob. One peak across the bank, and
MORPH is a timbre control.

---

## Part 2 — Waveshapers

### The idea

A waveshaper is a memoryless non-linear function applied sample by sample:
`y = f(x)`. No state, no delay line, no filter. That "memoryless" property is
what makes it cheap and what makes it brutal.

What it does is bend the waveform, and bending a waveform **changes its
spectrum**. That is the whole trick: you get new harmonics without a second
oscillator. A sine through a hard clipper is a square-ish wave; the harmonics
came from the bend, not from an added partial.

Three consequences follow immediately, and they explain most of what a shaper
sounds like when it sounds bad:

**1. It is amplitude-dependent.** `f(x)` bends the peaks first. A quiet signal
passes through nearly unchanged; only loud material gets distorted. This is why
shapers have a **pre-gain** — you push the signal into the bend, and the bend
is where the character is. Without pre-gain, a shaper knob is mostly a volume
knob with grit at the top.

**2. It is not band-limited, at all.** This is the big one. Hard clipping
generates *infinite* harmonics (a square has them all). At 44.1 kHz, everything
above Nyquist folds back. A hard-clipped sine at 1 kHz sprays inharmonic
partials across the whole spectrum — which reads as "bright and aggressive" on
a bass note and as "broken" on anything harmonic.

**3. Odd vs even matters.** Symmetric functions (`f(-x) = -f(x)`) produce only
odd harmonics — hollow, clarinet-like. Asymmetric ones produce even harmonics
too — fuller, more "tube". `tanh` and hard clip are symmetric; a slightly
offset soft clip is not.

### The four shapes worth having

| shape | formula | harmonics | character |
|---|---|---|---|
| **soft clip** | `tanh(k·x)/tanh(k)` | odd, decaying | smooth saturation, stays musical at high drive |
| **hard clip** | `clamp(k·x, -1, 1)` | odd, slow decay | aggressive, square-ish, aliases hard |
| **fold** | triangle-fold `k·x` back into range | odd, chaotic | screaming, synth-like; the fold count rises with drive |
| **sine** | `sin(k·x·π/2)` | odd | smooth but with ripples; the cheapest to compute here |

`tanh` is the one that would be expensive if it had to be written here — the
stock `tanhf` is a library call. **This platform already has it**: `fm_tanhf` in
fastmath.h, a Taylor series to x^13 under |x| < 0.5 (which is where a clipper
spends most of its samples, so it takes the cheap branch) and `fm_exp2f` above,
saturating past 9. That is better than a LUT: no table, no interpolation error,
and no 1 KB of flash. **Do not write a lookup table for this** unless a
measurement says `fm_tanhf` is the bottleneck.

Fold is the interesting one and the one worth getting right. Its transfer curve
is a triangle wave, so it folds back every time the input crosses the range —
one fold at moderate drive, many at high drive. It produces the widest and
strangest spectrum of the four, and it is where aliasing is most audible.
It is also where 2× oversampling earns its keep most clearly.

### Oversampling: what it actually buys

A waveshaper's aliasing comes from the *bend*, so the fix is to do the bend at a
higher rate: upsample 2×, apply `f(x)` at 88.2 kHz, then low-pass and decimate
back to 44.1 kHz. New harmonics up to 44.1 kHz are generated, and the low-pass
removes everything above 22.05 kHz before it can fold.

The cost is not "2× the shaper". The cost is:

- 2× the shaper evaluation, plus
- an interpolation filter up (cheap: a half-band or even linear for a soft
  shaper) and a decimation filter down (**not** cheap: needs real stopband
  attenuation, typically 16–32 taps per output sample).

For 8 voices that decimation filter alone can exceed the cost of everything
else. This is why the sensible plan is: **build it without oversampling, then
measure, then decide.** A useful middle option is to oversample only the
shaper's *own* stage while leaving the oscillator at base rate, which is what
most cheap implementations do and which is where most of the benefit is.

### Shaper placement in the chain

Two placements, and the choice matters more than the shape:

```
oscillator → [shaper A] → filter → [shaper B] → master
```

- **Before the filter** (pre-filter): the shaper's harmonics get filtered like
  any other partial. Turning CUT down tames the grit. Sounds like an
  overdriven oscillator inside a synth voice.
- **After the filter** (post-filter): the harmonics are generated after the
  filtering, so CUT cannot remove them. Sounds like a distortion pedal on the
  output. Brighter, harsher, and the filter behaves "wrong" in a way that is
  either the point or a bug depending on what you wanted.

A polyphonic synth almost always wants it **before** the filter, per voice.
A monosynth or a "drive the output" effect wants it after. fm1-wave should put
it per voice, before the filter — that is the placement that makes SHAPE
interact with CUT and RES instead of fighting them.

### Two knobs, and why not one

The user's instinct — two knobs — is right, and the reason is structural rather
than aesthetic:

With one knob you must choose what it controls: drive amount, or shape
selection. Whichever you pick, the other is fixed at build time and the whole
feature collapses into "one flavour of distortion". With two knobs, **AMOUNT**
sets drive (how far into the bend) and **SHAPE** sets which bend (soft → hard →
fold). Amount is a continuous sweep you play; shape is a discrete choice you
set and leave. They are different kinds of control, which is exactly why they
want different knobs.

The one thing to avoid: making SHAPE continuous in a way that morphs between
the four curves. Morphing between transfer functions produces intermediate
curves that are neither — usually just muddy clip. Discrete steps, chosen by
detented encoder, is right here.

---

## What this means for fm1-wave's spec

Concrete numbers to hold the design to:

- **Table**: 8 frames × 256 points × int16 = 4 KB per bank. Ten banks = 40 KB
  flash. Read directly from XIP; do not copy to RAM.
- **Oscillator cost**: 4 multiplies per sample per voice (two interpolations),
  8 voices = 32 multiplies per sample = 1.4 M/s. Affordable.
- **Aliasing**: start with linear interpolation and no mip maps (Melodee's
  trade, knowingly). Mip levels are a named future change, 4× the flash.
- **Shaper**: 4 shapes, 2 knobs (AMOUNT continuous, SHAPE detented 4 steps),
  per voice, **before** the filter. Pre-gain is folded into AMOUNT — do not add
  a third knob for it.
- **`tanh`**: LUT, 512 entries Q16, symmetric, linear interpolation.
- **Oversampling**: not in v1. Add a compile-time switch, measure with
  `plat_cpu_pct()`, then decide. Expect the decimation filter to be the
  expensive part if it ever happens.
- **Budget check**: measure, do not estimate. `plat_cpu_pct()` after a 8-voice
  chord with all shapes; if it is over ~60%, 8 voices is the ceiling and UNISON
  is off the table without `cpu2`.

### Sources

- Melodee's `firmware/src/eng_wt.c` (149 lines, MPL-2.0) — a working
  linear-interpolated, non-mipmapped wavetable oscillator, and
  `tools/gen_wt_tables.py` (88 lines) — the bank generator and its
  normalise-the-whole-bank rule. **The generator is the transferable part**;
  the engine is bound to Melodee's `voice_t`/`vmod_t`/`dsp.c` and those are
  GPL-3.0, so its code cannot be copied — only its approach.
- fm1-base's `template/app.c` — a float oscillator in `app_render`, the
  `master_q12` / `fm_exp2f` conventions this firmware should follow.
- Standard background, no citation needed but worth naming: band-limited mip
  wavetables and half-band decimation are decades-old practice; nothing here is
  novel, and the trade-offs are well documented in the synth-DSP literature.
