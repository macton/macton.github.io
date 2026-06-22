#!/bin/sh
# Build and run the simulation-only tests natively (no wasm, no render, no GPU).
# The sim core is plain portable C, so it compiles and runs on the host.
set -e
cd "$(dirname "$0")"
./gen.sh   # refresh the baked escape table + world map so tests reflect current geometry
out="$(mktemp)"
clang -O2 -Wall -Wextra -std=c11 -o "$out" \
  src/test.c src/sim.c src/tanks_path.c src/tanks_turn.c src/tanks_move.c \
  src/collide.c src/dirtab.c src/escape_table.c \
  src/grid_paths.c src/edge_paths.c src/map_data.c
"$out"
status=$?
rm -f "$out"
exit $status
