#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# fm1-wave's DSP tests: the real app/dsp/wv_dsp.c under plain cc, no platform.
#   sh app/tests/run_dsp.sh
# The banks are generated first if they are not there, so a clean clone runs this.
set -e
cd "$(dirname "$0")/../.."

CC=${CC:-cc}
OUT=app/tests/build
GEN=app/build/gen
mkdir -p "$OUT" "$GEN"

if [ ! -f "$GEN/wv_banks.h" ]; then
    python3 app/tools/gen_wv_banks.py "$GEN/wv_banks.h"
fi

# -ffp-contract=off: the AC79's FPU has no fused multiply-add, so the host must not
# fuse either, or the tests check arithmetic the device cannot do
$CC -O2 -ffp-contract=off -std=c99 -Wall -Wextra -Wno-unused-parameter \
    -I app/dsp -I platform/include -I "$GEN" \
    -o "$OUT/test_dsp" \
    app/tests/test_dsp.c app/dsp/wv_dsp.c -lm

"$OUT/test_dsp"
