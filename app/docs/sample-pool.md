# Packing real material into the resource region

The region is fixed at 276 KiB and the format is fixed (16-bit mono, one rate per
slot). What changes is the material. This is how a recording becomes the pool the
firmware reads.

## The budget, before you choose a sample rate

    276 KiB region - 168 B header = 282,456 B of samples = 141,228 frames

    44.1 kHz    3.20 s
    22.05 kHz   6.40 s
    11.025 kHz  12.81 s

One rate per slot, chosen at pack time. The rate is not resampled at playback, so
it decides two things independently:

- **the length** that fits (above), and
- **the highest note that does not fold.** At root 57 the top key is 27 keys up =
  +22 semitones = 3.56x, so the audible band stops at `rate / 2 / 3.56`:

      44.1 kHz    6188 Hz
      22.05 kHz   3094 Hz
      11.025 kHz  1547 Hz

Pick the lowest rate whose folding limit is above everything actually in the
material. A vocal with sibilance needs 22 kHz or more; a 150 Hz drone is complete
at 11 kHz, and using 11 kHz buys four times the length.

## Measuring the material first

Do not guess the band. One pass tells you both the rate you need and whether the
file is what you think it is:

    ffprobe -v error -show_entries stream=sample_rate,channels -of default=nw=1 in.ogg
    ffmpeg -y -v error -i in.ogg -ac 1 -ar 44100 -c:a pcm_s16le /tmp/probe.wav
    python3 - <<'EOF'
    import numpy as np, wave
    w = wave.open('/tmp/probe.wav')
    d = np.frombuffer(w.readframes(w.getnframes()), dtype='<i2') / 32768.0
    X = np.abs(np.fft.rfft(d * np.hanning(len(d)))); f = np.fft.rfftfreq(len(d), 1/44100)
    for lo, hi in [(80,160),(160,320),(320,640),(640,1280),(1280,2560),(2560,5120),(5120,11025)]:
        m = (f >= lo) & (f < hi)
        print(f"{lo:5d}-{hi:5d} Hz: {100*np.sum(X[m]**2)/np.sum(X**2):6.3f} %")
    EOF

If 98 % of the energy is in one octave and everything above 320 Hz is 20 dB down,
the file is a drone, not a voice — worth knowing before it is on the instrument.

## Packing

    python3 app/tools/pack_samples.py app/build/gen/samples.bin \
        --slot "VOX:/abs/path/vox_11025.wav:R11025/50"

`NAME:path:R<rate>/<root>` — the rate must be one the reader honours, the root is
the MIDI note the material actually sounds at. The packer normalises gain per slot
(it printed `gain 0.89` here) and refuses to write a pool that overruns the region,
with the overshoot in bytes.

`make_demo_pool.py` only writes when the file is missing, so a packed pool survives
`build.py` runs. Delete `app/build/gen/samples.bin` to get the demo pool back.

## Delivering it

The pool is **not** in the `.fwsc` and **not** in `fm1_install.py`'s image. It goes
over USB-MIDI, from the machine that holds the file:

    python3 platform/tools/fm1_resources.py app/build/wave-res.bin --port <port>

`wave-res.bin` is the 32 B header plus the pool; `samples.bin` is not what that tool
wants. The region's CRC is bound to the firmware build, so a pool packed against a
different build is refused (`20 build mismatch`) rather than written.

## Tests that assume the demo material

`test_grainmap` and `test_slot` measure the SHAPE of the shipped demo slots — a
transient that decays in four segments, a tone that sustains, a playhead that parks
at x=67 after 300 ms. With real material those shapes are whatever was recorded.

They call `pool_material.skip_unless_demo()`, which reads the pool header the
firmware reads and SKIPs (exit 0, with a line saying why) when the pool is not the
demo pool. They are not weakened and they do not silently pass: delete the pool to
run them against the demo material again.
