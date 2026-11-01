#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Render the sample path to a WAV: app/build/smpl.wav
#   sh app/tools/render_smpl.sh [POOL] [OUT]
set -e
cd "$(dirname "$0")/../.."

CC=${CC:-cc}
GEN=app/build/gen
mkdir -p "$GEN" app/build

if [ ! -f "$GEN/samples.bin" ]; then
    python3 app/tools/make_demo_pool.py "$GEN/samples.bin"
fi

# -ffp-contract=off: the AC79's FPU has no fused multiply-add
$CC -O2 -ffp-contract=off -std=c99 -Wall -Wextra \
    -I app/dsp -I platform/include -I "$GEN" \
    -o app/build/render_smpl \
    app/tools/render_smpl.c app/dsp/wv_dsp.c app/dsp/wv_sample.c app/dsp/wv_slice.c -lm

./app/build/render_smpl "${1:-$GEN/samples.bin}" "${2:-app/build/smpl.wav}"
