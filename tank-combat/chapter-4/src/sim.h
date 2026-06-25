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
 * by a per-tick lookup.
 *
 * CHAPTER 3: a thousand mites — an enemy faction in a fixed pool (flat SoA, no
 * allocation). Two structures sit at opposite ends of the frequency spectrum:
 * the path tables rebuild on a wall edit (rare); the per-cell mite index
 * (mite_cnt/mite_list) rebuilds every tick (constant) from current positions and
 * is the swarm's acceleration structure — it turns "which mites are within one
 * cell of me" into reading the <=4 occupants of a 3x3 neighbourhood. Each mite
 * carries the minimal shared knowledge: one world cell + one timestamp, the
 * last-known tank position, double-buffered so the gossip is order-independent
 * (read the previous tick, write the next, swap). See mites.h. */
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

  /* ---- per-tank turret/firing (the body heading drives movement; the turret
   *      aims independently and fires a laser that mows down the line) --------- */
  uint16_t tank_turret  [N_TANKS];  /* turret aim angle (Q5.11), separate from the body heading */
  uint16_t tank_cooldown[N_TANKS];  /* ticks until the next shot (0 = ready) */
  uint16_t tank_target  [N_TANKS];  /* targeted mite index, or TGT_NONE */
  uint16_t tank_shot_cell[N_TANKS]; /* world cell where the last beam ended (wall hit), for drawing */
  uint8_t  tank_tracer  [N_TANKS];  /* ticks the fired beam is still drawn (LASER_TICKS at fire) */

  /* ---- destruction effects: a fixed ring of expanding/fading bursts, one per mite the
   *      laser destroys. Cosmetic only (no gameplay effect), written in tanks_fire and
   *      drawn by render; deterministic (no RNG), so it never perturbs the swarm. ----- */
  uint32_t fx_xy[N_FX];   /* burst position (packed subcells, the mite's last position) */
  uint16_t fx_t [N_FX];   /* ticks remaining (0 = inactive); set to FX_DURATION on spawn */
  uint8_t  fx_head;       /* ring write cursor (wraps; bursts overwrite the oldest) */

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

  /* ---- the mite pool (SoA), driven through agent_turn/agent_move ---------- */
  uint32_t mite_xy  [N_MITES];   /* packed position x|y<<16, subcells (Q8.8) */
  uint16_t mite_ang [N_MITES];   /* Q5.11 heading */
  uint8_t  mite_in  [N_MITES];   /* input bitfield (IN_*), derived by mites_step */
  uint32_t mite_vxy [N_MITES];   /* packed last-tick applied move (agent_move's sink) */
  uint8_t  mite_hit [N_MITES];   /* last tick's blocked-axis bitmask */
  uint8_t  mite_mode[N_MITES];   /* MM_WANDER / MM_HUNT / MM_HOME */
  uint16_t mite_dest[N_MITES];   /* destination world cell (hunt=record, home=nest), or REC_EMPTY when wandering */
  uint16_t mite_cell[N_MITES];   /* current centre world cell, recomputed each tick from position */
  uint16_t mite_tgt [N_MITES];   /* reserved destination cell (== mite_cell at rest); the cap slot it owns */
  uint16_t mite_resp[N_MITES];   /* respawn countdown: 0 = alive; >0 = dead (shot), ticks until it revives at its nest */

  /* the shared knowledge: last-known tank position (a world cell, or REC_EMPTY)
   * + the frame it was stamped. Double-buffered (read rec_buf, write 1-rec_buf,
   * swap) so the gossip does not depend on iteration order. Last-write-wins:
   * newer timestamp wins, lowest mite index breaks ties. */
  uint16_t mite_rec_cell[2][N_MITES];   /* believed tank cell, or REC_EMPTY */
  uint32_t mite_rec_time[2][N_MITES];   /* frame it was recorded (the timestamp) */
  uint8_t  rec_buf;                     /* which record buffer is current (0/1) */

  /* the per-cell mite index: O(N) acceleration structure rebuilt every tick from
   * mite positions. count per cell + the (<=MITE_CAP) occupant mite indices. */
  uint8_t  mite_cnt [N_WORLD_CELLS];            /* mites whose centre is in each cell (0..MITE_CAP) */
  uint16_t mite_list[N_WORLD_CELLS * MITE_CAP]; /* their indices, MITE_CAP per cell */

  /* the four nests (home cells), spread across the world; editable on the page */
  uint16_t nest_cell[NEST_COUNT];

  /* the shared route-field table: a remaining-distance vector (pg, keyed by edge
   * point, exactly the tank's) per active destination cell. Slots 0..NEST_COUNT-1
   * are the resident nests (re-folded only on a wall edit / nest move); the rest
   * cache tank-sighting goals, folded when first wanted and freed when no mite
   * wants them. field_dest is the slot's destination cell, or REC_EMPTY if free. */
  uint16_t field_dest[N_FIELDS];
  uint16_t field_pg  [N_FIELDS * N_EDGE_MAX];
  uint32_t field_active;         /* distinct active destinations this tick (incl. overflow) */
  uint32_t field_peak;           /* high-water mark of field_active (sizes N_FIELDS) */

  uint32_t rng;                  /* xorshift32 state: the whole swarm's randomness, one stream */

  uint32_t frame;
  uint16_t move_speed;           /* tank speed: subcells per tick */
  uint16_t turn_rate;            /* tank turn: angle units per tick (the body) */
  uint16_t turret_rate;          /* tank turret turn: angle units per tick (aims independently of the body) */
  uint16_t collide_scale;        /* speed scale while colliding, Q0.8 (256 = full, 128 = 50%) */
  /* mite tunables (the page edits these live) */
  uint16_t mite_speed;           /* mite speed: subcells per tick */
  uint16_t mite_turn;            /* mite turn: angle units per tick */
  uint8_t  mite_sense;           /* tank-sensing range, in cells (Chebyshev) */
  uint8_t  mite_cap;             /* crowding cap in effect, 1..MITE_CAP */
  uint8_t  mite_phunt;           /* P(hunt) on adopting a newer peer record, percent (0..100); else go home */
  uint16_t mite_seed;            /* the editable seed (re-seeds rng and re-scatters the swarm) */
  uint16_t fire_period;          /* tank shot interval in ticks (0 = firing off); 30 = 2/sec at 60 ticks/sec */
  uint16_t mite_respawn;         /* ticks a killed mite stays dead before reviving at its nest (300 = 5 s) */
};

_Static_assert(GRID_W <= 32, "a screen-grid row must fit in one uint32_t");
_Static_assert(N_EDGE_MAX <= 255, "edge-point ids (and 0xFF sentinel) fit a uint8_t");
_Static_assert(N_MITES - 1 < REC_EMPTY, "mite indices in mite_list fit a uint16_t below the sentinel");
_Static_assert(N_WORLD_CELLS - 1 < REC_EMPTY, "a world cell fits a uint16_t below REC_EMPTY");
_Static_assert(MITE_R < SUB / 2, "a mite's footprint spans at most two cells per axis");

void sim_init(World* w);    /* load the level, place tanks, build all path tables */
void sim_tick(World* w);    /* advance one fixed step: path-follow, turn, move */

/* mutators (rebuild only the tables a change can affect; not per tick) */
void sim_toggle_wall(World* w, uint32_t wcx, uint32_t wcy);            /* flip a wall, rebuild L1(screen)+L2+pg */
void sim_set_dest(World* w, uint32_t tank, uint32_t wcx, uint32_t wcy);/* set a tank's destination */
void sim_cycle_tank(World* w, uint32_t tank);                         /* cycle a tank's state; manage `selected` */
void sim_set_seed(World* w, uint32_t seed);                           /* re-seed + re-scatter the mite swarm */
void sim_set_nest(World* w, uint32_t nest, uint32_t wcx, uint32_t wcy);/* move a nest, re-fold its resident field */

#endif
