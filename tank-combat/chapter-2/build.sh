#!/bin/sh
# Build the freestanding wasm module. No libc, no libm, no allocation.
# Output game.wasm is committed so GitHub Pages can serve it without a build.
# wasm.c is the only wasm-specific file; everything else is the portable sim +
# renderer (see test.sh for the native simulation-only build).
set -e
cd "$(dirname "$0")"

# Regenerate the host-baked data (escape table + world map), committed + compiled in.
./gen.sh

# Linear memory is sized statically to the path tables (no dynamic allocation):
#   Level-1 distances  16 * 45150 * 2  ~= 1.41 MB   (the dominant term)
#   Level-2 dist2/nexthop              ~= 48 KB
#   instance buffer, trace scratch, World scalars, stack
# 4 MiB covers it with headroom. This is the deliberate static sizing decision
# the chapter calls for (chapter 1 fit in 1 MiB; the all-pairs tables need more).
clang \
  --target=wasm32 \
  -nostdlib \
  -ffreestanding \
  -O2 \
  -fno-builtin \
  -Wall -Wextra \
  -Wl,--no-entry \
  -Wl,--initial-memory=4194304 \
  -Wl,--max-memory=4194304 \
  -Wl,--stack-first \
  -o game.wasm \
  src/wasm.c src/sim.c src/tanks_path.c src/tanks_turn.c src/tanks_move.c \
  src/render.c src/dirtab.c src/collide.c src/escape_table.c \
  src/grid_paths.c src/edge_paths.c src/map_data.c

VER="$(date -u '+%Y.%m.%d-%H%M%S')"
printf 'window.TANK_VERSION = "%s";\n' "$VER" > version.js

echo "built game.wasm: $(wc -c < game.wasm) bytes"
echo "version: $VER"
