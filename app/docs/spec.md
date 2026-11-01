# fm1-wave — specification (draft for discussion)

A polyphonic wavetable synth for the M-VAVE FM-1. Eight voices, one wavetable
oscillator per voice, a state-variable low-pass, and a waveshaper with two
knobs. Built on fm1-base, so Bluetooth MIDI, USB audio and the screen come from
the platform rather than from this firmware.

**Status: skeleton only.** The repo builds (`wave.fwsc`, identity `FM1-9400`,
221,884 B image) and contains nothing but the template's single-voice
oscillator. Nothing in this document is implemented.

Background on the two techniques, and the numbers behind the choices below:
[`background-wavetable-and-waveshaper.md`](background-wavetable-and-waveshaper.md).

---

## 1. What it is, and what it is not

**Is**: a keyboard instrument. 27 keys make sound. Turn it on, play it, it
sounds like a synth. Parameters are on the panel; there is no menu and no
sequencer.

**Is not**:

- a controller — that is [fm1-ctrl](https://github.com/johnskiller/fm1-ctrl),
  which sends MIDI and makes no sound. The two are separate firmwares and
  neither borrows the other's premise.
- a groovebox or sequencer — that is DriftSeq.
- a preset machine. Eight banks of eight frames is the whole timbre space, and
  every parameter is on the panel. No preset browsing in v1.

Both firmwares can live on the same unit; flashing one replaces the other.

---

## 2. The voice

```
keys ──► allocation ──► wavetable osc ──► waveshaper ──► SVF low-pass ──► VCA ──► mix
         (8 voices)     (bank, morph)     (shape, amt)    (cut, res)      (env)
```

**Eight voices**, allocated round-robin with oldest-note stealing. Eight is the
number the CPU budget was chosen around, not a guess (see §7). A ninth key
steals voice 0, and stealing crossfades over 2 ms rather than jumping — a
stolen voice clicking is the most audible defect a poly synth can have.

Per-voice state: phase accumulator (float), current pitch, envelope state,
filter state, velocity, note number, age. No wavetable copy per voice — all
voices read the same bank from flash (XIP).

**Velocity** comes from the platform's `APP_TOUCH` (which the template already
enables). Velocity scales the VCA ceiling; it does not scale the filter in v1
(that is a good feature and a cheap one, but it needs its own knob decision).

---

## 3. Panel map

Seven encoders, mapped to the roles the platform already names
(`EN_SELECT`, `EN_ALGO`, `EN_PRESET`, `EN_K1..K4`):

| encoder | role | parameter | range |
|---|---|---|---|
| EN_SELECT | `EN_SELECT` | **BANK** | 0–7, detented, dead stop at both ends |
| EN_ALGO | `EN_ALGO` | **MORPH** | 0–7 continuous (frame position, interpolated) |
| EN_PRESET | `EN_PRESET` | **CUT** | 0–127 → 30 Hz … 16 kHz, exponential |
| EN_K1 | `EN_K1` | **RES** | 0–127 → damping 2.0 … 0.15 |
| EN_K2 | `EN_K2` | **SHAPE** | 0–3, detented: SOFT / HARD / FOLD / SINE |
| EN_K3 | `EN_K3` | **AMOUNT** | 0–127 → drive 0 … 16× (pre-gain folded in) |
| EN_K4 | `EN_K4` | **ENV** | envelope page (see below) |

**No modal pages in normal playing.** All seven knobs are live at once, and
what each one does never changes while you are playing.

### The envelope problem, and the proposed answer

A playable synth wants at least attack and release. Seven knobs are already
taken by timbre, filter and shaper, and spending four of them on A/D/S/R would
mean losing MORPH or RES — both worse losses than a reduced envelope.

**Proposal: hold-to-edit.** Hold the panel's physical **ENV** button and the
seven encoders temporarily become the envelope page (ATTACK, DECAY, SUSTAIN,
RELEASE, plus three free); release it and they are the tone controls again.
Nothing on the screen changes permanently, so it needs no mode indicator and no
word — the bars themselves show what the knobs do while held. This is the
shift-key convention, not a mode.

Default envelope at power-on: A 5 ms, D 300 ms, S 0.7, R 250 ms — a sound on
the first key press, not a silent instrument waiting to be configured.

**If you would rather not have hold-to-edit**, the alternative is a fixed
single `ENV` knob controlling release only, with attack fixed at 5 ms. Simpler,
strictly worse, and never modal. Say which.

---

## 4. The waveshaper

Two knobs, per voice, **before** the filter.

- **SHAPE** — detented, 4 positions. Discrete, never morphed: intermediate
  transfer curves between clip and fold are just muddy, and morphing them is a
  knob that sounds broken in the middle.
  - `SOFT` — `tanh` via a 512-entry Q16 lookup
  - `HARD` — clamp
  - `FOLD` — triangle fold, the widest spectrum of the four
  - `SINE` — `sin(k·x·π/2)`, cheapest to compute on this chip
- **AMOUNT** — continuous drive. Pre-gain is folded into this knob rather than
  given its own: without pre-gain, a shaper knob is mostly a volume knob with
  grit at the top, so drive *is* the parameter.

Placement is before the filter so that CUT tames the grit, and so SHAPE
interacts with CUT/RES instead of fighting them. See the background document
for why this placement matters more than which of the four shapes is chosen.

**No oversampling in v1.** The shaper aliases; 2× would fix most of it and the
decimation filter would cost more than the shaper itself. Build without it,
measure, then decide. A compile-time switch, off by default.

---

## 5. Wavetables

**8 banks × 8 frames × 256 points × int16 = 4 KB per bank, 32 KB of flash
total.** Generated at build time by a Python script into a header, read
directly from XIP — never copied to RAM.

Banks share partials across frames so MORPH never snaps, and each bank is
normalised as a whole (not per frame) so MORPH is a timbre control and not a
volume control. Bank list is a spec-time decision; the starting eight:

1. SAW → PULSE (the classic sweep)
2. RESO (a formant bump walking the harmonics)
3. HOLLOW (odd harmonics fading in over a saw)
4. VOX (two formants moving in opposite directions)
5. → 8. *(to be named — proposals welcome)*

**Aliasing: accepted in v1.** Linear interpolation between table points reduces
staircase error; it does not remove harmonics that were already above Nyquist
when the table was built. Mip-mapped band-limiting is the real fix and costs 4×
the flash plus an index per voice. It is a **named future change**, not a
surprise. The worst of the aliasing sits in the top octaves where it is least
offensive.

---

## 6. Screen (240 × 240)

Three bands, no mode words, no redundant labels. Same information never appears
twice.

```
┌────────────────────────────────────────┐  y 0
│                                        │
│        real-time waveform scope        │  240 × 112
│                                        │
├────────────────────────────────────────┤  y 112
│  ▁▃▅ ▂▄▆ ▃▅▇ ▄▆█ ▅▇█ ▆██ ▇██ ███       │  240 × 64
│   the 8 frames of the current bank,    │
│   MORPH position marked                │
├────────────────────────────────────────┤  y 176
│ B▏▁▂▃  M▏▁▂▃  C▏▁▂▃  R▏▁▂▃             │  240 × 64
│ S▏▁▂▃  A▏▁▂▃  E▏▁▂▃                    │
└────────────────────────────────────────┘  y 240
```

**Band 1 — scope.** The actual post-mix output, not a computed drawing of the
waveform. Captured in `app_render` into a 240-sample peak-held ring (one sample
per 184 rendered samples, keeping the peak so transients are not lost), drawn in
`app_frame`. When silent, the band shows a flat line — silence is real data and
should not be dressed up.

**Band 2 — the bank.** The eight frames of the current bank as small waveform
thumbnails, with the MORPH position marked. This is always visible, because
MORPH's position is always real data you own.

When **BANK** is turned, this band becomes the picker for one second: the eight
*candidate banks* shown as thumbnails, the current one marked, then it returns
to the frames view. Transient overlay, no label, no mode — the content itself
says what it is.

**Band 3 — knobs.** Seven bars, one per encoder, in panel order. Each shows the
parameter's single-letter name and a filled bar proportional to its value. The
bar the player just turned is highlighted for 300 ms (the platform's own
convention). Nothing else on this band: no numbers except the one being turned,
because six numbers you cannot read while playing are six pixels of noise.

**Readouts shown only when owned.** A value appears next to a bar while that
encoder is being turned (300 ms), then the number goes away and only the bar
remains. There is no value on screen for a parameter that is not being changed.

---

## 7. Budget

The render has **5.8 ms per 256-frame block** (`APP_BLOCK 256`, 44.1 kHz). The
whole voice chain for all eight voices has to fit inside it, along with
anything else that runs in the audio interrupt.

Platform facts this relies on: 360 MHz single core, single-precision FPU, no
FMA, no `tanhf` (hence the LUT).

Measured baseline, template skeleton, this machine, `2026-11-01`:

```
image 221,884 B;  RAM .data+.bss 76,388 B of 98,304;  pool 93,960 B of 344,064
```

That leaves **21.9 KB of static RAM** and 250 KB of pool for the wavetable
work. The banks are flash, not RAM, so the RAM cost of this feature is small —
voice state (8 × ~64 B) and the scope ring (480 B).

`plat_cpu_pct()` is the only honest measure and it will be read, not estimated:

- **Gate: ≤ 60% with an 8-voice chord, FOLD shape, AMOUNT at maximum.** Fold is
  the worst case. Above 60% the ceiling is voices, not features.
- `plat_xruns()` must stay at zero through the same test.

If the gate fails, in order: drop to 6 voices, then drop the shaper to
post-filter (one instance instead of eight), then add `cpu2`.

---

## 8. Storage

One store object for the parameters (`OBJ_SETTINGS`), saved on change with a
debounce. Envelope, bank, morph, cut, res, shape, amount. Written with
`plat_store_save()`, which is atomic across its A/B sectors, so an interrupted
save cannot leave a half-written state.

Nothing else is persisted in v1: no presets, no per-bank settings, no name.

---

## 9. Tests

The platform's host simulator (`build.py app --host`) with scripted `.txt`
tests, the same way the template's `tests/basic.txt` works. Plus a plain C unit
test for the DSP under `cc`, no platform at all:

- **DSP unit tests** (`app/tests/`, `sh app/tests/run.sh`): wavetable read at
  known phases matches expected values; frame interpolation at 0.5 lands
  midway; each shaper's transfer curve is monotonic where it should be and
  bounded to ±1; envelope reaches its target in the specified time; voice
  stealing never leaves a voice at a non-zero level.
- **Host simulator**: keys make notes, eight simultaneous keys allocate eight
  voices and the ninth steals the oldest, encoders change the right parameter
  and nothing else, the screen redraws the bands that changed.
- **On hardware**: CPU percentage and xruns under the gate above; audio heard,
  not measured.

---

## 10. Not in v1

Named so they are decisions and not omissions:

- Band-limited (mip-mapped) tables — 4× flash, one index per voice
- 2× oversampled shaper — decimation filter is the expensive half
- Unison / detune — multiplies the oscillator cost directly
- Filter envelope, velocity → cutoff
- Presets, patch names, MIDI program change
- Arpeggiator, sequencer — DriftSeq's territory
- MIDI Thru / controller mode — fm1-ctrl's territory

---

## 11. Open questions

1. **Envelope**: hold-to-edit ENV button (§3), or a single fixed ENV knob?
2. **Banks 5–8**: which four timbres? Current four are SAW→PULSE, RESO, HOLLOW,
   VOX.
3. **UNISON**: out of v1 as written. If it matters, it should displace
   something rather than be added on top.
4. **Scope**: post-mix output (proposed) or per-voice? Post-mix is what you
   hear; per-voice is a debugging view, not an instrument view.
