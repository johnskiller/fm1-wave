# fm1-wave

A polyphonic wavetable instrument for the M-VAVE FM-1, with a waveshaper, a
sample-as-wavetable mode, slices, and granular.

MPL-2.0. Built on [fm1-base](https://github.com/zvenson/fm1-base).

## Documents

All of them are in **`app/docs/`**.

| File | What it is |
| --- | --- |
| [`app/docs/spec.md`](app/docs/spec.md) | **The spec.** What the instrument is and why, the signal path, the panel's design intent, the budgets, the storage format, what v1 does not do. Start here. |
| [`app/docs/panel-map.md`](app/docs/panel-map.md) | **What every control actually does**, per source and per page, and which controls are free. Authoritative over spec.md §3 for what is *wired*; checked by `app/tests/panel.txt`. |
| [`app/docs/MANUAL.md`](app/docs/MANUAL.md) | The player's manual: install, the screen, the controls, MIDI, troubleshooting. |
| [`app/docs/background-wavetable-and-waveshaper.md`](app/docs/background-wavetable-and-waveshaper.md) | Why a wavetable engine and a waveshaper, against the alternatives. The background reading behind spec.md §0. |
| [`platform/`](platform) | fm1-base as a submodule. Unmodified. |

Two rules about these files:

- **`spec.md` is the design intent; `panel-map.md` is what the code does.** When
  they disagree, panel-map.md wins for behaviour and spec.md wins for intent — and
  the disagreement is a bug in one of them.
- **Do not re-derive a control's meaning by grepping `app.c`.** panel-map.md is the
  answer, and `app/tests/panel.txt` fails the build if it stops being true.

## Build, test, install

From the directory that contains `platform/` and `app/`:

    export AC79_SDK=/path/to/fw-AC79_AIoT_SDK
    export JIELI_TOOLCHAIN=/path/to/jieli/toolchain

    python3 platform/tools/build.py app               # app/build/wave.fwsc
    python3 platform/tools/build.py app --host        # the host simulator
    python3 platform/tools/build.py app --web         # the browser emulator: app/build/emu/

Tests:

    sh app/tests/run_dsp.sh                           # the DSP, plain cc: 242 checks
    python3 app/tests/test_screen.py                  # the three bands, by pixel: 32 checks
    python3 app/tests/test_grainmap.py                # GRAN's material: 71 checks
    python3 app/tests/test_slot.py                    # slot selection: 8 checks

    app/build/host/wave_host app/tests/play.txt  app/build/host
    app/build/host/wave_host app/tests/smpl.txt  app/build/host
    app/build/host/wave_host app/tests/panel.txt app/build/host   # the panel map, 71 checks

`play.txt`/`smpl.txt`/`panel.txt` are host-simulator scripts: they drive the real
app through its controls and assert on its state and its audio. `panel.txt` is the
one that checks `docs/panel-map.md`. The three `test_*.py` scripts read the
framebuffer instead: they shoot the screen and count pixels, because the defects
they exist for (a band drawn outside its rows, a label with no name, a bar that is
not the grain length) do not show up in `app_expect`.

To listen without a device, render to a file:

    sh app/tools/render_smpl.sh                       # app/build/smpl.wav
    sh app/tools/render_gran.sh                       # app/build/gran.wav

Install:

    python3 platform/tools/fm1_install.py app/build/wave.fwsc

## What is in here

| Path | What |
| --- | --- |
| `app/app.c` | The app: input, the panel, the screen, the voice allocation hand-off |
| `app/dsp/` | The engine: `wv_dsp` (voices, shaper, SVF, envelope, the split render), `wv_sample` (the pool and its reader), `wv_slice` (chop detection), `wv_grain` (the granular cloud) |
| `app/tests/` | The DSP tests (plain `cc`, ASan/UBSan) and the host scripts |
| `app/tools/` | The pool packer, the demo-pool generator, the wavetable bank generator, the WAV renderers |
| `app/docs/` | The documents above |
| `app/fm1.toml` | Build and site settings: name, package identity, USB names, the resource region |
| `app/words.tsv` | The screen reader's words |
| `platform/` | fm1-base, unmodified, as a submodule |

## State

Playable on the host and in the browser emulator. **Not yet verified on hardware**
— no FM-1 has run this build, so dual-core gains, the 22.05 vs 44.1 kHz slice rate,
and real DAC headroom are all unanswered (see spec.md §9e and the Gate A/Gate B
notes).

fm1-base is MPL-2.0: changes to its files are published under MPL-2.0, and this
app's own files are MPL-2.0 as well.
