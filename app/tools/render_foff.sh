#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Build and run the per-frame offset A/B renderer.
#   sh app/tools/render_foff.sh [out.wav]
set -e
cd "$(dirname "$0")/../.."
CC=${CC:-cc}
GEN=app/build/gen
mkdir -p "$GEN" app/tools/build
if [ ! -f "$GEN/wv_banks.h" ]; then
    python3 app/tools/gen_wv_banks.py "$GEN/wv_banks.h"
fi
$CC -O2 -ffp-contract=off -std=c99 -Wall -Wextra -Wno-unused-parameter \
    -I app/dsp -I platform/include -I "$GEN" \
    -o app/tools/build/render_foff \
    app/tools/render_foff.c app/dsp/wv_dsp.c app/dsp/wv_sample.c \
    app/dsp/wv_slice.c app/dsp/wv_grain.c -lm
OUT=${1:-app/build/foff_pairs}
mkdir -p "$OUT"
./app/tools/build/render_foff "$OUT"
