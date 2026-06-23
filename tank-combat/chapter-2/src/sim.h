/* sim.h — the simulation core: all world data + the per-tick update.
 *
 * This layer has NO dependency on rendering or on wasm. It is plain C over the
 * World struct and can be built and run natively (see test.c / test.sh). The
 * wasm boundary (wasm.c) and the renderer (render.c) sit on top of it.
 *
 * NUMBERS ARE INTEGER FIXED POINT (see defs.h / dirtab.h):
 *   positions/sizes  subcells, 256 == 1 cell (Q8.8), int16_t
 *   heading          uint16_t Q5.11 (top 5 bits = 1 of 32 directions)
 *   grid             one uint32_t per screen-row, bit c == column c (1 = wall)
 * Fixed timestep: one sim_tick == one frame; per-tick deltas are the data.
 *
 * CHAPTER 2: a 4x4 world and self-pathing tanks. All tanks are identical; each
 * carries a per-tank interaction state (UNSELECTED / AUTOPATH / MANUAL) and may
 * have a destination it routes to. The big pathing tables (Level 1 per-screen
 * distances, Level 2 edge-point next-hop) live here because they are simulation
 * data, rebuilt only when the rarely-changing grid/destination changes and read
 * by a per-tick lookup. */
#ifndef TANK_SIM_H
#define TANK_SIM_H

#include "defs.h"
#include "grid_paths.h"
#include "edge_paths.h"

/* per-tank routing status, for the page readout and the tests */
#define PS_IDLE    0   /* no destination */
#define PS_ROUTING 1   /* following a route */
#define PS_ARRIVED 2   /* reached the destination cell */
#define PS_NOPATH  3   /* destination set but unreachable */

typedef struct World World;
struct World {
  /* ---- per-tank movement state (SoA) ------------------------------------- */
  uint32_t tank_xy [N_TANKS];   /* packed position x|y<<16, subcells (Q8.8) */
  uint16_t tank_ang[N_TANKS];   /* Q5.11 heading */
  uint8_t  tank_in [N_TANKS];   /* input bitfield (IN_*) */
  uint32_t tank_vxy[N_TANKS];   /* packed last-tick applied move vx|vy<<16 */
  uint8_t  tank_hit[N_TANKS];   /* last tick's blocked-axis bitmask (bit0 x, bit1 y) */

  /* ---- per-tank interaction + routing (every tank can path) -------------- */
  uint8_t  tstate      [N_TANKS];   /* TS_* (unselected/autopath/manual) */
  uint8_t  pdest_screen[N_TANKS];   /* destination screen (0..15) */
  uint16_t pdest_cell  [N_TANKS];   /* destination local cell (0..N_CELLS-1) */
  uint8_t  phas        [N_TANKS];   /* 1 if a destination is set */
  uint8_t  pgoal       [N_TANKS];   /* cardinal currently followed (DIR_*) or DIR_NONE */
  uint8_t  pstatus     [N_TANKS];   /* PS_* (idle/routing/arrived/nopath) */
  uint16_t pg[N_TANKS * N_EDGE_MAX]; /* remaining distance from each edge point to the goal */
  uint8_t  selected;                /* the one selected tank (AUTOPATH/MANUAL), or SEL_NONE */

  /* ---- the rarely-rebuilt world ----------------------------------------- */
  uint32_t grid[N_SCREENS * GRID_H];  /* wall bitset, screen-major, one word/row */

  /* ---- LEVEL 1: per-screen all-pairs distance (upper triangle, 1 byte) ---- */
  uint8_t  l1dist[N_SCREENS * TRI];
  uint16_t l1_screen_max[N_SCREENS];  /* measured longest distance per screen (telemetry/detection) */
  uint16_t l1_maxdist;                /* measured longest distance over all screens */

  /* ---- LEVEL 2: connecting edge points + next-hop matrix ----------------- */
  uint32_t ep_count;                          /* number of edge points (<= N_EDGE_MAX) */
  uint8_t  ep_screen [N_EDGE_MAX];            /* each edge point's screen */
  uint16_t ep_cell   [N_EDGE_MAX];            /* its local cell (on a border) */
  uint8_t  ep_cross  [N_EDGE_MAX];            /* cardinal to step across the border */
  uint8_t  ep_partner[N_EDGE_MAX];            /* the matched edge point across (0xFF none) */
  uint8_t  screen_ep_count[N_SCREENS];        /* per-screen edge-point counts */
  uint8_t  screen_ep[N_SCREENS * EP_PER_SCREEN_MAX]; /* per-screen edge-point ids */
  uint16_t dist2  [N_EDGE_MAX * N_EDGE_MAX];  /* all-pairs edge-point distance */
  uint8_t  nexthop[N_EDGE_MAX * N_EDGE_MAX];  /* all-pairs next edge point (0xFF none) */

  uint32_t frame;
  uint16_t move_speed;           /* subcells per tick */
  uint16_t turn_rate;            /* angle units per tick */
  uint16_t collide_scale;        /* speed scale while colliding, Q0.8 (256 = full, 128 = 50%) */
};

_Static_assert(GRID_W <= 32, "a screen-grid row must fit in one uint32_t");
_Static_assert(N_EDGE_MAX <= 255, "edge-point ids (and 0xFF sentinel) fit a uint8_t");

void sim_init(World* w);    /* load the level, place tanks, build all path tables */
void sim_tick(World* w);    /* advance one fixed step: path-follow, turn, move */

/* mutators (rebuild only the tables a change can affect; not per tick) */
void sim_toggle_wall(World* w, uint32_t wcx, uint32_t wcy);            /* flip a wall, rebuild L1(screen)+L2+pg */
void sim_set_dest(World* w, uint32_t tank, uint32_t wcx, uint32_t wcy);/* set a tank's destination */
void sim_cycle_tank(World* w, uint32_t tank);                         /* cycle a tank's state; manage `selected` */

#endif
