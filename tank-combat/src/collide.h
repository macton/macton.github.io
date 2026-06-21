/* collide.h — grid collision queries shared by the move and steer transforms.
 * Pure functions over the grid bitset; no state. */
#ifndef TANK_COLLIDE_H
#define TANK_COLLIDE_H

#include "defs.h"

/* Is a tank's square centred at (px,py) subcells blocked by a wall or the
 * arena edge? The edge counts as blocked, so a tank kept inside stays in
 * [TANK_R, ARENA-TANK_R] and grid row indices are always valid. */
int blocked(const uint32_t* grid, int32_t px, int32_t py);

#endif
