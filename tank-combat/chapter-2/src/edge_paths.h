/* edge_paths.h — LEVEL 2 of the pathing: route between screen-grids.
 *
 * The connecting edge points are the open border cells whose neighbour across
 * the screen border (an inner edge or the outer wrap) is also open — the
 * "matching open elements". Over those points we build an all-pairs next-hop
 * matrix: for a start point A and an end point B, nexthop[A][B] is the next edge
 * point on the shortest route, and dist2[A][B] its length.
 *
 * COMPOSITION (the data-oriented win): the matrix is built ON TOP OF Level 1.
 * Two edge points on the same screen are joined by an edge weighted with that
 * screen's Level-1 distance between their cells; crossing a matched inner edge
 * costs 1. Level 2 reuses the distances Level 1 already stored, and Floyd-Warshall
 * over the few edge points fills the matrix.
 *
 * A pathed tank's route composes the two levels without any per-destination
 * grid search: when the destination is set we fold the matrix into a small
 * per-tank vector pg[Y] = shortest remaining distance from edge point Y to the
 * goal (Level-2 distance to a goal-screen entry point + that screen's Level-1
 * distance in to the goal cell). Per tick the tank, in screen S, either follows
 * S's Level-1 flow straight to the goal (if it's in S and reachable) or picks the
 * exit edge point X of S minimising L1(cell->X) + 1 + pg[partner(X)] — a handful
 * of table reads. */
#ifndef TANK_EDGE_PATHS_H
#define TANK_EDGE_PATHS_H

#include "defs.h"
#include "grid_paths.h"

/* Static caps, sized to the curated world with headroom for live edits. The
 * default map's centred one-cell gaps give 4 edge points per screen = 64 total;
 * 128 leaves room to open more border cells before the (documented) cap. */
#define N_EDGE_MAX        128
#define EP_PER_SCREEN_MAX 70     /* one screen's perimeter: 2*GRID_W + 2*GRID_H */
#define D2_INF            0xFFFFu /* unreachable in the edge-point graph */

struct World;   /* defined in sim.h; Level-2 operates on its tables */

/* Rebuild the whole of Level 2 from w->grid and w->l1dist: enumerate edge
 * points, match partners and per-screen lists, then Floyd-Warshall the next-hop
 * matrix. Called on init and after any wall edit (rare). */
void l2_build(struct World* w);

/* Recompute pathed tank p's remaining-distance vector pg[] for its current
 * destination. Called when the destination (or grid) changes, not per tick. */
void l2_compute_pg(struct World* w, uint32_t p);

/* The next cardinal step for pathed tank p standing at local `cell` of `screen`
 * toward its destination: DIR_N/E/S/W, or DIR_NONE if arrived or no route.
 * Reads only the precomputed tables (l1dist, dist2, pg) — no search. */
uint8_t route_next_dir(const struct World* w, uint32_t p, uint32_t screen, uint32_t cell);

/* Dry-run the route for pathed tank p from its current cell to its destination,
 * writing the world cells it traverses into out[] (cap `max`). Returns the count
 * (0 if no destination / no path). Used for the on-screen path highlight — the
 * highlight is read straight out of the same tables the tank follows. */
uint32_t path_trace(const struct World* w, uint32_t p, uint16_t* out, uint32_t max);

/* World cell (0..BIG_W*BIG_H-1) of edge point e, for rendering/highlight. */
uint32_t ep_world_cell(const struct World* w, uint32_t e);

#endif
