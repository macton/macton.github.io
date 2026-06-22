/* grid_paths.h — LEVEL 1 of the pathing: within one screen-grid, route any cell
 * to any cell. This is the all-pairs table the `grid-paths` project builds
 * (macton.github.io/grid-paths), sized down to its real domain.
 *
 * WHAT WE STORE: the symmetric all-pairs *distance* per screen, upper triangle.
 * WHAT WE DERIVE: the next step. dist(a,b) == dist(b,a) (the within-screen graph
 * is undirected), but the next direction a->b is NOT the next direction b->a, so
 * direction can't be folded by symmetry — distance can. So we store the metric
 * (one uint16 per unordered pair) and read the arrow off it at runtime: the next
 * step from s toward t is the in-screen neighbour whose dist to t is one less.
 * That single choice is "find the minimal data the answer depends on" — the
 * distance is also exactly what LEVEL 2 needs to weight its edge-point graph, so
 * one table serves both, and the per-tick step is a handful of lookups, never a
 * search.
 *
 * BUDGET: TRI = 300*301/2 = 45150 unordered pairs * 2 bytes = 90 KB/screen,
 * 1.41 MB for all 16 screens. (The raw within-screen distance domain is 0..299,
 * 9 bits; we spend a byte-aligned uint16 for simple, branch-free hot-path
 * indexing — a deliberate trade in a budget we are already raising. A 9-bit pack
 * is the listed tightening.) Built by BFS per cell on init and on a wall edit
 * (the rarely-changing input); read every tick. */
#ifndef TANK_GRID_PATHS_H
#define TANK_GRID_PATHS_H

#include "defs.h"

#define TRI    (N_CELLS * (N_CELLS + 1) / 2)   /* unordered (cell,cell) pairs */
#define L1_INF 0xFFFFu                          /* unreachable within the screen */

/* Index of the unordered pair {a,b} (a,b in [0,N_CELLS)) into the upper triangle.
 * a*(2N - a + 1)/2 + (b-a) with a<=b; the product is always even, so exact. */
static inline uint32_t tri_index(uint32_t a, uint32_t b) {
  if (a > b) { uint32_t t = a; a = b; b = t; }
  return a * (2u * N_CELLS - a + 1u) / 2u + (b - a);
}
static inline uint16_t l1_get(const uint16_t* dist_screen, uint32_t a, uint32_t b) {
  return dist_screen[tri_index(a, b)];
}
static inline int l1_reachable(const uint16_t* dist_screen, uint32_t a, uint32_t b) {
  return l1_get(dist_screen, a, b) != (uint16_t)L1_INF;
}

/* Build one screen's all-pairs distance (dist_screen holds TRI uint16). BFS from
 * every open cell, confined to the screen; wall cells and disconnected cells
 * stay L1_INF. */
void l1_build_screen(uint16_t* dist_screen, const uint32_t* grid, uint32_t screen);
/* Build all N_SCREENS triangles into dist (N_SCREENS*TRI uint16). */
void l1_build_all(uint16_t* dist, const uint32_t* grid);

/* The next cardinal step (DIR_N/E/S/W) from local cell `src` toward local cell
 * `dst`, both within one screen, read off that screen's distance triangle: the
 * in-screen neighbour with dist(neighbour,dst) == dist(src,dst)-1, canonical
 * tie-break N,E,S,W. DIR_NONE if src==dst (arrived) or dst is unreachable. */
uint8_t l1_next_dir(const uint16_t* dist_screen, uint32_t src, uint32_t dst);

#endif
