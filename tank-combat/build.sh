#!/bin/sh
# Build the freestanding wasm module. No libc, no libm, no allocation.
# Output game.wasm is committed so GitHub Pages can serve it without a build.
set -e
cd "$(dirname "$0")"
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
  src/game.c src/tanks_turn.c src/tanks_move.c src/tanks_steer.c src/render.c src/dirtab.c
echo "built game.wasm: $(wc -c < game.wasm) bytes"
