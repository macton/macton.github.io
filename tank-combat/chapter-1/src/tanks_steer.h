/* tanks_steer — keeps a throttled tank from getting stuck. When a tank holding
 * forward or backward collided this tick, rotate it toward the nearest direction
 * whose next cell is open, so it slides along flat walls, turns out of convex
 * corners, and rotates around to the opening of a concave pocket. See
 * CONTRACT.md.
 *
 * Pipeline stage after tanks_move; it reads that transform's `hit` (the gate)
 * and re-probes the grid to choose the way out. */
#ifndef TANK_STEER_H
#define TANK_STEER_H

#include "defs.h"

/* For each of n tanks that collided this tick (hit[i] != 0) while driving,
 * rotate ang[i] by up to `rate` toward the nearest of the 32 directions whose
 * neighbouring cell is open. No effect on tanks moving freely, stopped tanks,
 * or tanks with no reachable opening. */
void tanks_steer(const int16_t* x, const int16_t* y, uint16_t* ang,
                 const uint8_t* in, const uint8_t* hit, uint32_t n, uint16_t rate,
                 const uint32_t* grid);

#endif
