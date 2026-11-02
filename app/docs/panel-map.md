# Panel map

The FM-1 has **7 encoders and 14 buttons**. This file is the authority on what
each one does in fm1-wave **right now**. `docs/spec.md` §4 says what the panel is
*meant* to be; this file says what is wired.

Read this before assigning a control. Do not re-derive it from `app.c` — the point
of the file is that the answer is in one place.

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
| `EN_SELECT` | SELECT | — | — | **SPRAY** 0–127 | app | 243 |
| `EN_ALGO` | ALGORITHM | **MORPH** 0–127 | MORPH *(no effect)* | MORPH *(no effect)* | app | 213 |
| `EN_PRESET` | PRESETS | **BANK** 0–7, detented | **SLOT** | **SLOT** | app | 227 / 245 / 248 |
| `EN_K1` | KNOB 1 | **CUT** 0–127 | CUT | CUT | app | 207 |
| `EN_K2` | KNOB 2 | **RES** 0–127 | RES | RES | app | 208 |
| `EN_K3` | KNOB 3 | **SHAPE** 0–3 detented | SHAPE | SHAPE | app | 209 |
| `EN_K4` | KNOB 4 | **AMOUNT** 0–127 | AMOUNT | AMOUNT | app | 210 |

**Nothing changes position between modes.** That is a rule, not a coincidence: a
dead control is visible and stable, a control that moves gets turned by accident.
MORPH keeps `EN_ALGO` in all three modes even though the sample modes do not read
it, which is why SPRAY ended up on `EN_SELECT` instead.

**All knobs are relative.** They accumulate detents from wherever they are; there
is no absolute position to jump to. So a value survives a mode switch, and the
knob does not have to be swept back.

### Encoder detail

- **SELECT** is the only one whose meaning is mode-dependent (`SPRAY` in GRAN,
  dead otherwise). It is free in SYNTH and SMPL.
- **ALGORITHM**: MORPH addresses the 8 frames of the wavetable bank, 0–127 across
  them. In SMPL and GRAN the DSP never reads it.
- **PRESETS**: detented in SYNTH (8 banks, wrap-around). In SMPL and GRAN it is a
  material slot picker, and a *turn* loads the next slot rather than sweeping
  through them.
- **CUT / RES / SHAPE / AMOUNT** are live in every mode and shared by the table,
  sample and grain sources — that is the whole design of the voice chain
  (`spec.md` §5).

---

## Buttons

| Button | Printed | Current function | Owner | Where |
|---|---|---|---|---|
| `B_SEL` | SEL | **SOURCE** cycles SYNTH → SMPL → GRAN | app | app.c:220 |
| `B_EDIT` | EDIT | **re-chop** the current slot; dead in SYNTH and GRAN | app | app.c:250 |
| `B_ENV` | ENV | **nothing** (declared for the envelope page, never wired) | app, unused | — |
| `B_FX` | FX | nothing | **free** | — |
| `B_LFO` | LFO | nothing | **free** | — |
| `B_GLO` | GLO | nothing | **free** | — |
| `B_SAVE` | SAVE | nothing | **free** | — |
| `B_ARP` | ARP | nothing | **free** | — |
| `B_SEQ` | SEQ | nothing | **free** | — |
| `B_PLAY` | PLAY | **platform**: green LED, red while recording | platform | plat_fm1.c:134 |
| `B_REC` | REC | **platform**: held at power-on enters UPDATE MODE | platform | main_fm1.c:968 |
| `B_HOME` | HOME | **platform**: "no" in the RESTORE prompt | platform | unity.c:340 |
| `B_OCTDN` | OCT- | **platform**: with OCT+, held at power-on = calibration | platform | main_fm1.c:966 |
| `B_OCTUP` | OCT+ | **platform**: same | platform | main_fm1.c:966 |

### How much the platform ones really cost

Not all "platform" is equal, and it decides whether a button is actually usable:

- **`B_REC` and `B_OCTDN` + `B_OCTUP` are read only at power-on.** Using them
  while the instrument is running breaks nothing — the app may read them freely
  and should just avoid claiming them for something that must survive a reboot.
- **`B_HOME` is read only inside the RESTORE prompt**, which appears only when a
  backup is being restored. Otherwise it is completely idle — in practice the
  safest of the "platform" buttons to borrow.
- **`B_PLAY` is a lamp, not a function**: `plat_fm1.c` paints it green (red for
  recording) and nothing acts on a press. The app can use it; the LED will still
  behave, because the platform drives the colour regardless.

So the honest reading is: **only `B_REC` (power-on) and the OCT pair (power-on) are
genuinely owned.** Everything else is a courtesy claim.

### Caution: the lights are off

`app.c:344` calls `plat_lights(0, 0, 0, 0, 0)` every 33 ms — a template
placeholder. **No button lights up when pressed.** Any button assignment that
relies on the LED saying "this button is active" does not have that yet, and the
call needs to go before it does.

---

## What is not assigned

These are things the panel has no control for, listed so they are not forgotten.
None of them is scheduled.

| Function | State | Why unassigned |
|---|---|---|
| **GRAIN SIZE** (10–200 ms) | hardwired to 64 | No encoder left; decided to wait for a device listen before taking one |
| **Envelope** A/D/S/R | not implemented | Planned as the `B_ENV` page (K1–K4 become A/D/S/R) |
| **Material page** | not implemented | Planned as the `B_BANK` page (samples, grain size, window, overlap) |
| Octave shift | not implemented | `B_OCTDN` / `B_OCTUP` are the obvious home |
| PANIC / all-notes-off | not implemented | — |

`B_BANK` is **not** in the platform's enum (`B_FX, B_SEL, B_ENV, B_LFO, B_EDIT,
B_GLO, B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC, B_OCTDN, B_OCTUP`). The
envelope page is therefore planned on `B_ENV`, and the material page has no button
of its own yet — the spec's `BTN_BANK` names a control this panel does not have.

---

## Free buttons, and what each could carry

Eleven buttons are unused by the app. Five are unclaimed by anything at all.
Since there are more free buttons than there are pending functions, the list is
deliberately shorter than the button list — and the suggestions are grouped by
what they would *cost*, not by what they would do.

**Before any of these is wired**, the `plat_lights(0,0,0,0,0)` call has to go, or
the button will do something invisible.

| Button | Could carry | Cost / caveat |
|---|---|---|
| `B_ENV` | **envelope page** (A/D/S/R on K1–K4) | the spec's plan; K1–K4 leave CUT/RES/SHAPE/AMOUNT while the page is up |
| `B_FX` | the **second waveshaper** stage, or a filter-type switch | no DSP behind it yet |
| `B_LFO` | LFO rate/depth page | no LFO in the DSP |
| `B_GLO` | **glide / portamento** on or off | trivial in the DSP, needs a per-voice pitch slew |
| `B_SAVE` | save the current knob set as the boot state | `plat_store_save` exists; nothing to store yet |
| `B_ARP` | arpeggiator | needs a clock and a note queue; not small |
| `B_SEQ` | step sequencer | larger still; Saadet/Melodee territory, and this instrument is not that |
| `B_HOME` | **PANIC** — all notes off | cheapest useful thing on the panel; the platform only reads it during RESTORE |
| `B_PLAY` | tap tempo / the transport's play | the LED already behaves; nothing consumes it |
| `B_OCTDN` | octave down | power-on calibration is the only conflict |
| `B_OCTUP` | octave up | same |

**The recommendation, if one is wanted**: `B_HOME` → PANIC, and `B_ENV` → the
envelope page. PANIC costs a dozen lines and fixes the one failure mode that
makes a synth unusable in front of people; the envelope page is already designed
in the spec and is the difference between a preset and an instrument.

---

## Keys

27 keys, F3–G5 (`plat.h: NKEYS 27`). In SYNTH and GRAN every key is a note. In
SMPL the mapping is two-part:

- A key at or above `SLICE_BASE` (note 53, the lowest) **and** below
  `SLICE_BASE + slice_count` plays that chop at 1×, from the chop's own start.
- Every other key — including all of them when nothing has been chopped — falls
  back to `wv_note_on_sample`.

**Known defect in that fallback**: it always plays `smp[0]`, not the slot
`EN_PRESET` selected. So in SMPL with slot 2 chosen and no chops, the high keys
play slot 0. The chop path is correct (it uses `slices_from`); only the fallback
hardcodes index 0. Not fixed yet — recorded here so it is not rediscovered by
accident.
