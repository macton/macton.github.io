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
#   dynamic instance buffer  24-byte Inst * INST_MAX        ~= 75 KB (the swarm + tanks +
#                                          FX + overlays in view; terrain/nests are no
#                                          longer here — they are the static town below)
#   STATIC TOWN buffer  24-byte Inst * N_WORLD_CELLS (19200) ~= 460 KB (the baked map, one
#                                          instance/cell; built once at init, re-baked only
#                                          on a wall/nest edit; uploaded once, then culled)
#   static run table  StaticRun * (N_SCREENS*MAP_MESH_COUNT) ~= 12 KB
#   procedural meshes (src/mesh_data.c)                     ~= 2 KB (loaded once)
#   town meshes (src/map_mesh_data.c, baked Kenney art)     ~= 400 KB (data segment, once)
# The DEPTH buffer is a viewport-sized GPU texture, NOT wasm linear memory. The sim's
# high-water mark is unchanged from chapter 3 (~1.04 MiB); the static town + town meshes
# add ~0.9 MiB, comfortably inside the 8 MiB linear memory. Still all static, integer,
# allocation-free — and the per-FRAME map cost is now just a frustum cull, never a re-emit.
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
  src/grid_paths.c src/edge_paths.c src/map_data.c src/mesh_data.c src/map_mesh_data.c \
  src/staticmap.c

VER="$(date -u '+%Y.%m.%d-%H%M%S')"
printf 'window.TANK_VERSION = "%s";\n' "$VER" > version.js
# Stamp the ?v= cache-busts directly into index.html so a reload always fetches the
# app.js/version.js that match THIS page document. Routing the app.js bust through a
# separately-cached version.js can pin a stale app.js (new HTML, old JS); the page
# document is the one resource a reload reliably refreshes, so the version lives there.
sed -i -E "s|(version\.js\?v=)[^\"]*|\1$VER|; s|(app\.js\?v=)[^\"]*|\1$VER|" index.html

echo "built game.wasm: $(wc -c < game.wasm) bytes"
echo "version: $VER"
