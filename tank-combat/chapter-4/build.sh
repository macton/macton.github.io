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
#   combat state (per-tank turret/cooldown/target + mite_resp[1000]) ~= 2 KB
#   trace scratch, World scalars, 128 KB stack
# CHAPTER 4 changes only the RENDER half's memory, and the sim is untouched:
#   instance buffer  20-byte 3-D Inst * INST_MAX (~6500)   ~= 130 KB (worst case: the
#                                          whole world in view — every cell, the whole
#                                          swarm, tanks, FX, and the overlays; the free
#                                          camera culls to far fewer when zoomed in)
#   baked meshes (src/mesh_data.c, 162 verts * 8 bytes)    ~= 1.3 KB (loaded once)
# The DEPTH buffer is a viewport-sized GPU texture, NOT wasm linear memory. The sim's
# high-water mark is unchanged from chapter 3 (~1.04 MiB), and the bigger instance
# buffer is still tiny against it, so the linear memory stays 2 MiB (32 pages). Still
# all static, integer, allocation-free.
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
  src/grid_paths.c src/edge_paths.c src/map_data.c src/mesh_data.c

VER="$(date -u '+%Y.%m.%d-%H%M%S')"
printf 'window.TANK_VERSION = "%s";\n' "$VER" > version.js
# Stamp the ?v= cache-busts directly into index.html so a reload always fetches the
# app.js/version.js that match THIS page document. Routing the app.js bust through a
# separately-cached version.js can pin a stale app.js (new HTML, old JS); the page
# document is the one resource a reload reliably refreshes, so the version lives there.
sed -i -E "s|(version\.js\?v=)[^\"]*|\1$VER|; s|(app\.js\?v=)[^\"]*|\1$VER|" index.html

echo "built game.wasm: $(wc -c < game.wasm) bytes"
echo "version: $VER"
