#!/bin/sh
# Build the freestanding wasm module. No libc, no libm, no allocation.
# Output ik.wasm is committed so GitHub Pages serves it without a build step.
# wasm.c is the only wasm-specific file; everything else is the portable sim +
# renderer (see test.sh for the native simulation-only build).
set -e
cd "$(dirname "$0")"

./gen.sh   # refresh the baked trig table

# Linear memory is static (no dynamic allocation): the World (~1 KB), the 512-quad
# instance buffer (8 KB), the trig table (2 KB), and a 64 KB stack — well inside
# 256 KB. No build-time tables beyond trig; everything else is per-tick.
clang \
  --target=wasm32 \
  -nostdlib \
  -ffreestanding \
  -O2 \
  -fno-builtin \
  -Wall -Wextra \
  -Wl,--no-entry \
  -Wl,--initial-memory=262144 \
  -Wl,--max-memory=262144 \
  -Wl,-z,stack-size=65536 \
  -Wl,--stack-first \
  -o ik.wasm \
  src/wasm.c src/sim.c src/gait.c src/ik.c src/render.c src/terrain.c src/trig_table.c

VER="$(date -u '+%Y.%m.%d-%H%M%S')"
printf 'window.IK_VERSION = "%s";\n' "$VER" > version.js

echo "built ik.wasm: $(wc -c < ik.wasm) bytes"
echo "version: $VER"
