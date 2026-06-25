#!/bin/sh
# Build and run the sampling analysis on the host: which grid cells the
# collision/steer code samples, and the collapse to 16 local patterns.
set -e
cd "$(dirname "$0")"
out="$(mktemp)"
clang -O2 -Wall -Wextra -std=c11 -o "$out" \
  tools/analyze_samples.c tools/escape_build.c src/dirtab.c src/collide.c
"$out"
status=$?
rm -f "$out"
exit $status
