#!/bin/sh
# Generate src/escape_table.c on the host from compile-time geometry, then commit
# it. The steer escape table depends on no runtime state, so it is source, not a
# runtime build. build.sh and test.sh run this first so the table is always fresh
# with respect to the geometry it is derived from. Reuses the game's own collision
# code (src/collide.c) so the table can't drift from runtime behaviour.
set -e
cd "$(dirname "$0")"
gen="$(mktemp)"
clang -O2 -Wall -Wextra -std=c11 -o "$gen" \
  tools/gen_escape.c tools/escape_build.c src/collide.c src/dirtab.c
"$gen" > src/escape_table.c
rm -f "$gen"
