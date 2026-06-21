/* tanks_steer — keeps a throttled tank from getting stuck. When a tank is
 * holding forward or backward and its travel direction is blocked, rotate it
 * toward the nearest direction it can actually move, so it slides along flat
 * walls, turns out of convex corners, and rotates around to the opening of a
 * concave pocket. See CONTRACT.md.
 *
 * Pipeline stage after tanks_move. Self-contained: it re-probes the grid from
 * the tank's position rather than depending on the move result. */
#ifndef TANK_STEER_H
#define TANK_STEER_H

#include "defs.h"

/* For each of n tanks that is driving (forward or backward) but cannot move in
 * its current travel direction, rotate ang[i] by up to `rate` toward the
 * nearest of the 32 directions in which it could move. No effect on tanks in
 * open space, stopped tanks, or tanks with no reachable opening. */
void tanks_steer(const int16_t* x, const int16_t* y, uint16_t* ang,
                 const uint8_t* in, uint32_t n, uint16_t rate,
                 int32_t speed, const uint32_t* grid);

#endif
