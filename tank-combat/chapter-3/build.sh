#!/bin/sh
# Build the freestanding wasm module. No libc, no libm, no allocation.
# Output game.wasm is committed so GitHub Pages can serve it without a build.
# wasm.c is the only wasm-specific file; everything else is the portable sim +
# renderer (see test.sh for the native simulation-only build).
set -e
cd "$(dirname "$0")"

# Regenerate the host-baked data (escape table + world map), committed + compiled in.
./gen.sh

# Linear memory is sized statically (no dynamic allocation). The dominant term is
# still chapter 2's path tables; chapter 3 adds the swarm and its index:
#   Level-1 distances  16 * 45150 * 1  ~= 706 KB   (1 byte/pair; in-screen distances
#                                          provably <= 229, see grid_paths.h)
#   Level-2 dist2/nexthop              ~=  48 KB
#   per-cell mite index (cnt u8 + list u16*4, 4800 cells) ~= 43 KB  (rebuilt each tick)
#   mite pool SoA + double-buffered records (1000)        ~= 35 KB
#   mites.c BFS/scatter/tally scratch (file-static)       ~= 29 KB
#   instance buffer (2 screens + up to N_MITES quads)     ~= 43 KB
#   combat state (per-tank turret/cooldown/target + mite_resp[1000]) ~= 2 KB
#   trace scratch, World scalars, 128 KB stack
# Measured high-water mark (__heap_base) ~1.04 MiB (1,088,408 bytes) — chapter 2's
# 1 MiB no longer holds it (the mite pool, its per-cell index, and the bigger
# instance buffer push it over), so the linear memory is raised to 2 MiB (32
# pages, ~1.9x headroom). Still all static, integer, allocation-free.
clang \
  --target=wasm32 \
  -nostdlib \
  -ffreestanding \
  -O2 \
  -fno-builtin \
  -Wall -Wextra \
  -Wl,--no-entry \
  -Wl,--initial-memory=2097152 \
  -Wl,--max-memory=2097152 \
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
