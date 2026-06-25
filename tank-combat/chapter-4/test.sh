#!/bin/sh
# Build and run the native tests (no wasm, no GPU). The sim core AND the renderer are
# plain portable C over the World, so both compile and run on the host: chapter 4
# links render.c too, so the projection / depth / visibility tests exercise the real
# build_view, and the sim tests prove the model is byte-identical to chapter 3.
set -e
cd "$(dirname "$0")"
./gen.sh   # refresh the baked escape table + world map + meshes so tests reflect current data
out="$(mktemp)"
clang -O2 -Wall -Wextra -std=c11 -o "$out" \
  src/test.c src/sim.c src/tanks_path.c src/agent_turn.c src/agent_move.c src/mites.c \
  src/tanks_fire.c src/render.c src/collide.c src/dirtab.c src/escape_table.c \
  src/grid_paths.c src/edge_paths.c src/map_data.c
"$out"
status=$?
rm -f "$out"
exit $status
