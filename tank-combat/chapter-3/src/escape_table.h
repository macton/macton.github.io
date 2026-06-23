/* escape_table.h — the steer's precomputed escape directions.
 *
 * PATTERN_ESCAPE is the EXHAUSTIVE enumeration of every possible local
 * situation: all 16 four-neighbour patterns x all 32 travel directions. It has
 * nothing to do with any particular map — every entry is filled — and it never
 * changes: the values follow only from the fixed footprint/trig/scan geometry.
 * So it is a compile-time constant, generated once on the host by
 * tools/gen_escape.c (run from gen.sh), committed as src/escape_table.c, and
 * compiled in. Transforms reference it directly (like the trig table); it is not
 * runtime state and is never passed around.
 *
 * Indexed [pattern*N_DIRS + travel]: bit5 = handedness, bits0-4 = direction;
 * 0xFF (ESC_NONE) = no open direction. `pattern` is a cell's 4-neighbour wall
 * code (collide.c cell_pattern, read live each tick). */
#ifndef TANK_ESCAPE_TABLE_H
#define TANK_ESCAPE_TABLE_H

#include "defs.h"

#define ESC_NONE 0xFFu

extern const uint8_t PATTERN_ESCAPE[N_PATTERNS * N_DIRS];

#endif
