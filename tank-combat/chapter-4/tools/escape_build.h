/* escape_build.h — HOST-ONLY generation of the steer escape table.
 *
 * Not part of the wasm. tools/gen_escape.c runs this on the host to emit
 * src/escape_table.c; tools/analyze_samples.c reuses patterns_build_open to show
 * the 16-pattern collapse. The runtime never builds the table — it reads the
 * generated PATTERN_ESCAPE (see src/escape_table.h). */
#ifndef TANK_ESCAPE_BUILD_H
#define TANK_ESCAPE_BUILD_H

#include "../src/defs.h"

/* Open-direction mask for each of the 16 local patterns (centre-origin probe;
 * only the 4 orthogonal neighbours matter, so the mask is a pure function of the
 * 4-bit pattern). */
void patterns_build_open(uint32_t pattern_open[N_PATTERNS]);

/* Nearest-open escape per (pattern, travel) from the open masks; out must hold
 * N_PATTERNS*N_DIRS bytes. bit5 = handedness, bits0-4 = direction, 0xFF = none. */
void build_escape_table(uint8_t* pattern_escape, const uint32_t* pattern_open);

#endif
