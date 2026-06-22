#!/bin/sh
# Generate the baked host-determined data on the host, then commit it:
#   src/escape_table.c  — the steer's nearest-open escape (geometry-only)
#   src/map_data.c      — the initial 4x4 world map (authoring-time data)
# Both depend on no runtime state, so they are source, not a runtime build.
# build.sh and test.sh run this first so the tables are always fresh with
# respect to the geometry/map they are derived from. The escape generator reuses
# the game's own collision code (src/collide.c) so the table can't drift from
# runtime behaviour.
set -e
cd "$(dirname "$0")"

gen="$(mktemp)"
clang -O2 -Wall -Wextra -std=c11 -o "$gen" \
  tools/gen_escape.c tools/escape_build.c src/collide.c src/dirtab.c
"$gen" > src/escape_table.c

clang -O2 -Wall -Wextra -std=c11 -o "$gen" tools/gen_map.c
"$gen" > src/map_data.c

rm -f "$gen"
