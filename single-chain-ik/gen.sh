#!/bin/sh
# Generate the baked, host-determined data and commit it:
#   src/trig_table.c — the Q14 cosine table (geometry-only, no runtime input).
# It depends on no runtime state, so it is source, not a runtime build. build.sh
# and test.sh run this first so the table is always fresh w.r.t. trig.h.
set -e
cd "$(dirname "$0")"

gen="$(mktemp)"
clang -O2 -Wall -Wextra -std=c11 -o "$gen" tools/gen_trig.c -lm
"$gen" > src/trig_table.c
rm -f "$gen"
