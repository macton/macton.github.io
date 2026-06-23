#!/bin/sh
# Build and run the simulation-only tests natively (no wasm, no render, no GPU).
# The sim core (sim/gait/ik/terrain) is plain portable C, so it runs on the host.
set -e
cd "$(dirname "$0")"
./gen.sh
out="$(mktemp)"
clang -O2 -Wall -Wextra -std=c11 -o "$out" \
  src/test.c src/sim.c src/gait.c src/ik.c src/terrain.c src/trig_table.c
"$out"
status=$?
rm -f "$out"
exit $status
