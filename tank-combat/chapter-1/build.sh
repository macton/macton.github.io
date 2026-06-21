#!/bin/sh
# Build the freestanding wasm module. No libc, no libm, no allocation.
# Output game.wasm is committed so GitHub Pages can serve it without a build.
# wasm.c is the only wasm-specific file; everything else is the portable sim +
# renderer (see test.sh for the native simulation-only build).
set -e
cd "$(dirname "$0")"

# Regenerate the precomputed escape table (committed source, baked into the wasm).
./gen.sh

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
  -Wl,--stack-first \
  -o game.wasm \
  src/wasm.c src/sim.c src/tanks_turn.c src/tanks_move.c \
  src/render.c src/dirtab.c src/collide.c src/escape_table.c

# Stamp a version (build timestamp). version.js is loaded by the page and shown
# in the corner, and app.js appends it to the game.wasm URL to defeat caching,
# so a visitor can always confirm they are looking at the latest build.
VER="$(date -u '+%Y.%m.%d-%H%M%S')"
printf 'window.TANK_VERSION = "%s";\n' "$VER" > version.js

echo "built game.wasm: $(wc -c < game.wasm) bytes"
echo "version: $VER"
