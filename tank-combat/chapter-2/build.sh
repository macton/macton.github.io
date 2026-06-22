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
#   Level-1 distances  16 * 45150 * 1  ~= 706 KB   (the dominant term; 1 byte/pair —
#                                          in-screen distances provably <= 229, see
#                                          grid_paths.h, so they fit a byte)
#   Level-2 dist2/nexthop              ~= 48 KB
#   instance buffer, trace scratch, World scalars, 128 KB stack
# ~780 KB of statics + stack fit in 1 MiB. Storing the distance as a byte instead
# of two (sized to the real, build-time-known data) brings chapter 2 back under
# chapter 1's 1 MiB.
clang \
  --target=wasm32 \
  -nostdlib \
  -ffreestanding \
  -O2 \
  -fno-builtin \
  -Wall -Wextra \
  -Wl,--no-entry \
  -Wl,--initial-memory=1048576 \
  -Wl,--max-memory=1048576 \
  -Wl,-z,stack-size=131072 \
  -Wl,--stack-first \
  -o game.wasm \
  src/wasm.c src/sim.c src/tanks_path.c src/tanks_turn.c src/tanks_move.c \
  src/render.c src/dirtab.c src/collide.c src/escape_table.c \
  src/grid_paths.c src/edge_paths.c src/map_data.c

VER="$(date -u '+%Y.%m.%d-%H%M%S')"
printf 'window.TANK_VERSION = "%s";\n' "$VER" > version.js

echo "built game.wasm: $(wc -c < game.wasm) bytes"
echo "version: $VER"
