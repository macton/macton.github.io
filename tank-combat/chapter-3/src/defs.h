/* defs.h — shared vocabulary: fixed sizes, input bits, exact-width types.
 * These are constraints the whole module exploits, not parameters to expose.
 *
 * Chapter 2 grows chapter 1's single 20x15 screen into a 4x4 arrangement of
 * screen-grids. Mechanically the world is ONE big (4*GRID_W)x(4*GRID_H)
 * toroidal grid; the 4x4 screen split is an organisation for display,
 * scrolling, and the two-level pathing (see grid_paths.h / edge_paths.h). */
#ifndef TANK_DEFS_H
#define TANK_DEFS_H

#include <stdint.h>

/* ---- one screen-grid (a chapter-1-sized grid) ---------------------------- */
#define GRID_W      20
#define GRID_H      15
#define N_CELLS     (GRID_W * GRID_H)   /* 300 cells per screen */

/* ---- the 4x4 world of screen-grids --------------------------------------- */
#define SCREENS_X   4
#define SCREENS_Y   4
#define N_SCREENS   (SCREENS_X * SCREENS_Y)        /* 16 */
#define BIG_W       (SCREENS_X * GRID_W)           /* 80 cells wide  */
#define BIG_H       (SCREENS_Y * GRID_H)           /* 60 cells tall  */

/* ---- tanks: all identical; each can path itself or be driven ------------- */
#define N_TANKS     4
/* per-tank interaction state, cycled by clicking the tank. At most one tank is
 * "selected" (AUTOPATH or MANUAL); the rest are UNSELECTED. */
#define TS_UNSELECTED 0   /* not selected; keeps following its path if it has one, else idle */
#define TS_AUTOPATH   1   /* selected; click a cell to set its destination, it routes there   */
#define TS_MANUAL     2   /* selected; driven by the keyboard / on-screen controls            */
#define SEL_NONE      0xFFu

#define N_DIRS      32          /* discrete headings: angle >> ANGLE_SHIFT   */
#define N_PATTERNS  16          /* a cell's 4 orthogonal-neighbour walls: NESW */
#define ANGLE_SHIFT 11          /* 65536 >> 11 == 32                         */
#define SUB         256         /* subcells per grid cell (Q8.8)             */
#define SUB_SHIFT   8
#define ARENA_W_SUB (BIG_W * SUB)       /* 20480 < 32768, still fits int16   */
#define ARENA_H_SUB (BIG_H * SUB)       /* 15360                              */
#define TANK_R      92          /* collision half-extent, subcells (~0.36c)  */
#define TRIG_SHIFT  14          /* the DIR_COS table is Q14                   */

/* ---- mites: a thousand small units, an enemy faction (chapter 3) ----------
 * A mite is a quarter-cell driven body on the SAME grid as the tanks: it shares
 * agent_turn/agent_move, colliding with walls at its own smaller radius. There
 * are N_MITES of them in a fixed pool (flat SoA, no allocation). Where a tank is
 * routed by the path tables, a mite is a dumb cheap agent: it wanders, copies a
 * timestamped last-known-tank-position record on contact, and swarms greedily. */
#define N_MITES     1000        /* fixed pool, all alive for the whole chapter */
#define MITE_R      (SUB / 8)   /* collision half-extent = 32 subcells; diameter ~= a quarter cell */

/* The crowding cap: at most MITE_CAP mite centres per world cell, every tick. It
 * is a decision-level rule (mites don't physically collide with each other), and
 * it doubles as the width of the per-cell occupant list — so it sizes a real
 * structure, not just a threshold. */
#define MITE_CAP    4

/* a mite's movement mode (its record + mode derive the input byte) */
#define MM_WANDER   0           /* drunk walk over the open cell graph */
#define MM_SEEK     1           /* greedily reduce distance to the recorded cell */

/* tank/mite input bitfield (shared: both flow through agent_turn/agent_move) */
#define IN_FWD   1u
#define IN_BACK  2u
#define IN_LEFT  4u
#define IN_RIGHT 8u

/* ---- cardinal directions, the alphabet of the path tables ----------------
 * Codes 0..3 = N,E,S,W. The path tables speak in these; movement speaks in the
 * 32-direction heading. CARD_DI maps a cardinal to its discrete heading (the
 * one whose velocity is exactly axis-aligned), so a pathed tank aligned to
 * CARD_DI[d] moves purely along one axis. Screen/world rows increase downward,
 * so North is -y (up). */
#define DIR_N 0
#define DIR_E 1
#define DIR_S 2
#define DIR_W 3
#define DIR_NONE 0xFF                    /* "no step": arrived or no path */
static const int8_t  CARD_DCX[4] = {  0, +1,  0, -1 };   /* N E S W */
static const int8_t  CARD_DCY[4] = { -1,  0, +1,  0 };
static const uint8_t CARD_DI [4] = { 24,  0,  8, 16 };   /* heading index */

/* The arena is a torus: positions wrap. There is no hard edge — whether you can
 * cross an edge depends only on the wall on the far side. */
static inline int32_t wrap_x(int32_t x) { x %= ARENA_W_SUB; return x < 0 ? x + ARENA_W_SUB : x; }
static inline int32_t wrap_y(int32_t y) { y %= ARENA_H_SUB; return y < 0 ? y + ARENA_H_SUB : y; }

/* world cell coordinates wrap on the big grid (BIG_W/BIG_H aren't powers of
 * two, so add/sub-wrap one step suffices for the +/-1 neighbour probes). */
static inline int32_t wrap_wcx(int32_t c) { return c < 0 ? c + BIG_W : (c >= BIG_W ? c - BIG_W : c); }
static inline int32_t wrap_wcy(int32_t r) { return r < 0 ? r + BIG_H : (r >= BIG_H ? r - BIG_H : r); }

/* screen index from screen coordinates (sx,sy) */
static inline uint32_t screen_of(uint32_t sx, uint32_t sy) { return sy * SCREENS_X + sx; }

/* ---- world cells: one flat index over the BIG_W x BIG_H toroidal grid ------
 * A world cell is wcy*BIG_W + wcx, in [0, N_WORLD_CELLS). The mite record stores
 * one such cell (the last-known tank position); REC_EMPTY is the out-of-range
 * sentinel for "no cell" (4800 cells, so 0xFFFF can never be a real one). */
#define N_WORLD_CELLS (BIG_W * BIG_H)        /* 4800 */
#define REC_EMPTY     0xFFFFu                 /* empty record: a readable, propagating value */
static inline uint16_t wc_pack(int32_t wcx, int32_t wcy) { return (uint16_t)(wcy * BIG_W + wcx); }
static inline int32_t  wc_x(uint32_t c) { return (int32_t)(c % BIG_W); }
static inline int32_t  wc_y(uint32_t c) { return (int32_t)(c / BIG_W); }

/* toroidal distances between world cells. tor1 is the wrapped 1-D distance on an
 * axis of length n; the swarm seeks on the 4-connected graph (Manhattan), and
 * "within one cell" is the 3x3 neighbourhood (Chebyshev <= 1). */
static inline int32_t tor1(int32_t a, int32_t b, int32_t n) { int32_t d = a > b ? a - b : b - a; return d < n - d ? d : n - d; }
static inline int32_t cell_manhattan(uint32_t a, uint32_t b) {
  return tor1(wc_x(a), wc_x(b), BIG_W) + tor1(wc_y(a), wc_y(b), BIG_H);
}
static inline int32_t cell_chebyshev(uint32_t a, uint32_t b) {
  int32_t dx = tor1(wc_x(a), wc_x(b), BIG_W), dy = tor1(wc_y(a), wc_y(b), BIG_H);
  return dx > dy ? dx : dy;
}

/* Grid-tracking geometry, shared by every body that follows the cell graph (the
 * routing tanks and the mites): has a body driving discrete heading `cur_di`,
 * offset (ox,oy) from its cell centre, reached the centre on its travel axis?
 * Driving a cardinal keeps the perpendicular axis exactly centred, so only the
 * travel axis can be off-centre — and only briefly, before a turn. A non-cardinal
 * heading is mid-turn (already snapped at the centre), so it counts as centred. */
static inline int at_cell_centre(uint32_t cur_di, int ox, int oy) {
  switch (cur_di) {
    case 0:  return ox >= 0;   /* E (+x) */
    case 16: return ox <= 0;   /* W (-x) */
    case 8:  return oy >= 0;   /* S (+y) */
    case 24: return oy <= 0;   /* N (-y) */
    default: return 1;         /* mid-turn */
  }
}

/* Two int16 always-together values packed in one uint32_t (low = a, high = b).
 * One named owner of the layout, so call sites pack/unpack by intent, not by
 * ad-hoc shifts. Used for tank_xy (position) and tank_vxy (last applied move). */
static inline uint32_t xy_pack(int32_t a, int32_t b) {
  return (uint32_t)(uint16_t)(int16_t)a | ((uint32_t)(uint16_t)(int16_t)b << 16);
}
static inline int32_t xy_lo(uint32_t p) { return (int16_t)(uint16_t)(p & 0xFFFFu); }
static inline int32_t xy_hi(uint32_t p) { return (int16_t)(uint16_t)(p >> 16); }

#endif
