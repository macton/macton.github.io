#!/bin/sh
# Build and run the simulation-only tests natively (no wasm, no render, no GPU).
# The sim core is plain portable C, so it compiles and runs on the host.
set -e
cd "$(dirname "$0")"
out="$(mktemp)"
clang -O2 -Wall -Wextra -std=c11 -o "$out" \
  src/test.c src/sim.c src/tanks_turn.c src/tanks_move.c \
  src/dirtab.c src/collide.c
"$out"
status=$?
rm -f "$out"
exit $status
