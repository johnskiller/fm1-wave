#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# Build and run the demo renderer: a WAV you can listen to, from the real DSP.
#   sh app/tools/render_demo.sh [out.wav]
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
    -o app/tools/build/render_demo \
    app/tools/render_demo.c app/dsp/wv_dsp.c -lm

./app/tools/build/render_demo "${1:-app/build/demo.wav}"
