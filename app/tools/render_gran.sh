#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Render the granular path to app/build/gran.wav with the real engine.
#   sh app/tools/render_gran.sh
set -e
cd "$(dirname "$0")/../.."

CC=${CC:-cc}
GEN=app/build/gen
mkdir -p app/build

if [ ! -f "$GEN/samples.bin" ]; then
    python3 app/tools/make_demo_pool.py "$GEN/samples.bin"
fi

$CC -O2 -ffp-contract=off -std=c99 -I app/dsp -I platform/include -I "$GEN" \
    -o app/build/render_gran \
    app/tools/render_gran.c app/dsp/wv_dsp.c app/dsp/wv_sample.c app/dsp/wv_slice.c app/dsp/wv_grain.c \
    -lm

app/build/render_gran "$GEN/samples.bin" app/build/gran.wav
ls -l app/build/gran.wav
