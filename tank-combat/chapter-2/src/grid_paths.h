/* grid_paths.h — LEVEL 1 of the pathing: within one screen-grid, route any cell
 * to any cell. This is the all-pairs table the `grid-paths` project builds
 * (macton.github.io/grid-paths), sized down to its real domain.
 *
 * WHAT WE STORE: the symmetric all-pairs *distance* per screen, upper triangle.
 * WHAT WE DERIVE: the next step. dist(a,b) == dist(b,a) (the within-screen graph
 * is undirected), but the next direction a->b is NOT the next direction b->a, so
 * direction can't be folded by symmetry — distance can. So we store the metric
 * (one byte per unordered pair) and read the arrow off it at runtime: the next
 * step from s toward t is the in-screen neighbour whose dist to t is one less.
 * The distance is also exactly what LEVEL 2 needs to weight its edge-point graph,
 * so one table serves both, and the per-tick step is a handful of lookups.
 *
 * ONE BYTE, not two: the distance is build-time-known data, not adversarial
 * runtime input, so we size to what it can actually be — and here we can *prove*
 * it fits a byte. A 2x2 block of cells can't all lie on an induced path (they
 * form a 4-cycle), so at most 3 of every 4 do; the longest in-screen shortest
 * path is therefore <= N_CELLS - (GRID_W/2)*(GRID_H/2) - 1 = 229 for a 20x15
 * screen, well under the 255 a byte holds (L1_INF reserves 0xFF for
 * unreachable). The _Static_assert below makes that a compile-time guarantee; if
 * a future screen size ever broke it, the build fails and you fix the design (or
 * escalate to a per-screen bit depth). l1_build_* also returns the *measured*
 * max so the page can show it.
 *
 * BUDGET: TRI = 300*301/2 = 45150 pairs * 1 byte = 45 KB/screen, ~706 KB for all
 * 16 screens. Built by BFS per cell on init and on a wall edit; read every tick. */
#ifndef TANK_GRID_PATHS_H
#define TANK_GRID_PATHS_H

#include "defs.h"

#define TRI    (N_CELLS * (N_CELLS + 1) / 2)   /* unordered (cell,cell) pairs */
#define L1_INF 0xFFu                            /* unreachable within the screen */

/* Provable ceiling on an in-screen shortest path (2x2-block / induced-path
 * bound). Guarantees the distance fits a byte alongside the L1_INF sentinel. */
#define L1_DIST_CEIL (N_CELLS - (GRID_W / 2) * (GRID_H / 2) - 1)
_Static_assert(L1_DIST_CEIL < L1_INF, "in-screen distances provably fit in a byte (2x2-block bound)");

/* Index of the unordered pair {a,b} (a,b in [0,N_CELLS)) into the upper triangle.
 * a*(2N - a + 1)/2 + (b-a) with a<=b; the product is always even, so exact. */
static inline uint32_t tri_index(uint32_t a, uint32_t b) {
  if (a > b) { uint32_t t = a; a = b; b = t; }
  return a * (2u * N_CELLS - a + 1u) / 2u + (b - a);
}
static inline uint8_t l1_get(const uint8_t* dist_screen, uint32_t a, uint32_t b) {
  return dist_screen[tri_index(a, b)];
}
static inline int l1_reachable(const uint8_t* dist_screen, uint32_t a, uint32_t b) {
  return l1_get(dist_screen, a, b) != L1_INF;
}

/* Build one screen's all-pairs distance (dist_screen holds TRI bytes). BFS from
 * every open cell, confined to the screen; wall/disconnected cells stay L1_INF.
 * Returns the largest finite distance found (telemetry / overflow detection). */
uint32_t l1_build_screen(uint8_t* dist_screen, const uint32_t* grid, uint32_t screen);
/* Build all N_SCREENS triangles into dist (N_SCREENS*TRI bytes); fills
 * screen_max[s] with each screen's measured max and returns the global max. */
uint32_t l1_build_all(uint8_t* dist, const uint32_t* grid, uint16_t* screen_max);

/* The next cardinal step (DIR_N/E/S/W) from local cell `src` toward local cell
 * `dst`, both within one screen, read off that screen's distance triangle: the
 * in-screen neighbour with dist(neighbour,dst) == dist(src,dst)-1, canonical
 * tie-break N,E,S,W. DIR_NONE if src==dst (arrived) or dst is unreachable. */
uint8_t l1_next_dir(const uint8_t* dist_screen, uint32_t src, uint32_t dst);

#endif
