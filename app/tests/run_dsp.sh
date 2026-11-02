#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# fm1-wave's DSP tests: the real app/dsp/*.c under plain cc, no platform.
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
FLAGS="-O2 -ffp-contract=off -std=c99 -Wall -Wextra -Wno-unused-parameter"
INC="-I app/dsp -I platform/include -I $GEN"
SRC="app/dsp/wv_dsp.c app/dsp/wv_sample.c app/dsp/wv_slice.c app/dsp/wv_grain.c"

echo "== dsp =="
$CC $FLAGS $INC -o "$OUT/test_dsp"   app/tests/test_dsp.c   $SRC -lm
"$OUT/test_dsp"

echo "== sample =="
$CC $FLAGS $INC -o "$OUT/test_sample" app/tests/test_sample.c $SRC -lm
"$OUT/test_sample"

echo "== slice =="
$CC $FLAGS $INC -o "$OUT/test_slice"  app/tests/test_slice.c  $SRC -lm
"$OUT/test_slice"

echo "== grain =="
$CC $FLAGS $INC -o "$OUT/test_grain"  app/tests/test_grain.c  $SRC -lm
"$OUT/test_grain"

echo "== env =="
$CC $FLAGS $INC -o "$OUT/test_env"    app/tests/test_env.c    $SRC -lm
"$OUT/test_env"

echo "== split =="
$CC $FLAGS $INC -o "$OUT/test_split"  app/tests/test_split.c  $SRC -lm
"$OUT/test_split"
