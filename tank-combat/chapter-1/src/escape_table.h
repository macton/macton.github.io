/* escape_table.h — the steer's precomputed escape directions.
 *
 * PATTERN_ESCAPE is fully determined by compile-time geometry (TANK_R, SUB, the
 * trig table, the scan order) — it depends on no runtime state at all. So it is
 * GENERATED ON THE HOST by tools/gen_escape.c (run from gen.sh) and committed as
 * src/escape_table.c, then compiled in — not computed at startup.
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
