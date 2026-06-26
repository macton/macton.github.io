#!/bin/sh
# Build the freestanding wasm module. No libc, no libm, no allocation.
# Output game.wasm is committed so GitHub Pages can serve it without a build.
# wasm.c is the only wasm-specific file; everything else is the portable sim +
# renderer (see test.sh for the native simulation-only build).
set -e
cd "$(dirname "$0")"

# Regenerate the host-baked data (escape table + world map), committed + compiled in.
./gen.sh

# Linear memory is sized statically (no dynamic allocation). The 8x8 world (64
# screens, 4096 mites) makes the Level-1 distance tables the dominant term:
#   Level-1 distances  64 * 45150 * 1  ~= 2.76 MB  (1 byte/pair; in-screen distances
#                                          provably <= 229, see grid_paths.h)
#   Level-2 dist2/nexthop (255 edge pts) ~= 195 KB
#   per-cell mite index (cnt u8 + list u16*4, 19200 cells) ~= 173 KB (rebuilt each tick)
#   mite pool SoA + double-buffered records (4096)         ~= 135 KB
#   shared route fields (128 * pg[255] u16)               ~=  65 KB
#   mites.c BFS/scatter/tally scratch (file-static)       ~= 115 KB
#   instance buffer (2 screens + up to N_MITES quads)     ~=  86 KB
#   trace scratch, combat/fx state, World scalars, 128 KB stack
# Total static high-water mark ~3.7 MiB, so the linear memory is raised to 8 MiB
# (128 pages, ~2x headroom). Still all static, integer, allocation-free.
clang \
  --target=wasm32 \
  -nostdlib \
  -ffreestanding \
  -O2 \
  -fno-builtin \
  -Wall -Wextra \
  -Wl,--no-entry \
  -Wl,--initial-memory=8388608 \
  -Wl,--max-memory=8388608 \
  -Wl,-z,stack-size=131072 \
  -Wl,--stack-first \
  -o game.wasm \
  src/wasm.c src/sim.c src/tanks_path.c src/agent_turn.c src/agent_move.c src/mites.c \
  src/tanks_fire.c src/render.c src/dirtab.c src/collide.c src/escape_table.c \
  src/grid_paths.c src/edge_paths.c src/map_data.c

VER="$(date -u '+%Y.%m.%d-%H%M%S')"
printf 'window.TANK_VERSION = "%s";\n' "$VER" > version.js

echo "built game.wasm: $(wc -c < game.wasm) bytes"
echo "version: $VER"
