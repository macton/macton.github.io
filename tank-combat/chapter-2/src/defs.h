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

/* ---- tanks: one manual + a few pathed (class == index partition) --------- */
#define N_PATHED    3                  /* self-routing tanks */
#define N_TANKS     (1 + N_PATHED)     /* index 0 = manual, 1..N_PATHED = pathed */
#define TANK_MANUAL 0
#define is_pathed(i) ((i) >= 1)        /* the index range IS the class tag */

#define N_DIRS      32          /* discrete headings: angle >> ANGLE_SHIFT   */
#define N_PATTERNS  16          /* a cell's 4 orthogonal-neighbour walls: NESW */
#define ANGLE_SHIFT 11          /* 65536 >> 11 == 32                         */
#define SUB         256         /* subcells per grid cell (Q8.8)             */
#define SUB_SHIFT   8
#define ARENA_W_SUB (BIG_W * SUB)       /* 20480 < 32768, still fits int16   */
#define ARENA_H_SUB (BIG_H * SUB)       /* 15360                              */
#define TANK_R      92          /* collision half-extent, subcells (~0.36c)  */
#define TRIG_SHIFT  14          /* the DIR_COS table is Q14                   */

/* tank input bitfield */
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

/* Two int16 always-together values packed in one uint32_t (low = a, high = b).
 * One named owner of the layout, so call sites pack/unpack by intent, not by
 * ad-hoc shifts. Used for tank_xy (position) and tank_vxy (last applied move). */
static inline uint32_t xy_pack(int32_t a, int32_t b) {
  return (uint32_t)(uint16_t)(int16_t)a | ((uint32_t)(uint16_t)(int16_t)b << 16);
}
static inline int32_t xy_lo(uint32_t p) { return (int16_t)(uint16_t)(p & 0xFFFFu); }
static inline int32_t xy_hi(uint32_t p) { return (int16_t)(uint16_t)(p >> 16); }

#endif
