/* collide.h — grid collision queries shared by the move and steer transforms.
 * Pure functions over the grid bitset; no state. The arena is toroidal: there
 * is no edge to hit, only walls — coordinates and the tank's footprint wrap, so
 * a position near one edge is tested against the cells on the opposite edge. */
#ifndef TANK_COLLIDE_H
#define TANK_COLLIDE_H

#include "defs.h"

/* Is a tank's square centred at (px,py) subcells overlapping a wall? px,py and
 * the footprint wrap around the arena, so this works across the seam. */
int blocked(const uint32_t* grid, int32_t px, int32_t py);

#endif
