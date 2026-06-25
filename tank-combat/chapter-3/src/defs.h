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
 * are N_MITES of them in a fixed pool (flat SoA, no allocation). A mite does not
 * own a route: it wanders until the gossip gives it a destination, then NAVIGATES
 * with the tank path tables through a SHARED route field (edge_paths.h). Across
 * 1000 mites the distinct destinations are few (the nests + a handful of recent
 * sightings), so a handful of shared fields serve the whole swarm. */
#define N_MITES     1000        /* fixed pool, all alive for the whole chapter */
#define MITE_R      (SUB / 8)   /* collision half-extent = 32 subcells; diameter ~= a quarter cell */

/* The crowding cap: a cell is quartered into MITE_CAP=4 sub-segments (a 2x2 grid),
 * and a mite is assigned to one of them by index (`seg_of` = index & 3). At most ONE
 * mite per (cell, sub-segment): a mite can enter a cell only if its own sub-segment
 * there is free (and the cell holds fewer than `mite_cap` segments total). It parks at
 * its sub-segment's centre — a quarter-cell off the cell centre — so the swarm spreads
 * inside cells instead of stacking on the centre. MITE_CAP also sizes the per-cell
 * occupant list (one slot per sub-segment). It is a decision-level rule (mites don't
 * physically collide with each other). */
#define MITE_CAP    4
#define SUBQ        (SUB / 4)   /* 64: a sub-segment centre's offset from the cell centre */
/* a mite's sub-segment, 0..3 (NW,NE,SW,SE). Keyed off the UPPER index bits, NOT m&3 —
 * because nest_of(m) is m&3, and if the segment were m&3 too then every mite of a nest
 * would want the SAME sub-segment of that one nest cell, serializing revival to one at a
 * time. Using (m>>2)&3 spreads a nest's mites over all four segments. */
static inline int seg_of(uint32_t mite) { return (int)((mite >> 2) & 3u); }
static inline int seg_ox(uint32_t mite) { return (seg_of(mite) & 1) ? SUBQ : -SUBQ; }  /* east half? +x : -x */
static inline int seg_oy(uint32_t mite) { return (seg_of(mite) & 2) ? SUBQ : -SUBQ; }  /* south half? +y : -y */
static inline int popcount4(uint32_t b) { return (int)((b & 1u) + ((b >> 1) & 1u) + ((b >> 2) & 1u) + ((b >> 3) & 1u)); }

/* Every mite belongs to one of NEST_COUNT home cells, assigned by index. A nest
 * is a destination a mite can path home to (the 20% that carry a sighting home).
 * There is one nest per screen EXCEPT the tank start screen (0,0) — 15 of the 16
 * screens. Spreading the homes across all the screens spreads the spawn/revival
 * load: with a single screenful of nests every revived mite funnelled out through
 * that one screen's two or three border openings; one nest per screen gives each
 * its own exits and roughly N_MITES/15 residents. */
#define NEST_COUNT  15
static inline uint32_t nest_of(uint32_t mite) { return mite % NEST_COUNT; }

/* The shared route-field table: a fixed pool of remaining-distance fields keyed by
 * destination cell (the NEST_COUNT resident nests + cached tank-sighting goals).
 * Sized from a MEASURED peak distinct-destination count, not a guess: across
 * representative scenarios (idle / roaming / fleeing tanks, several simultaneous
 * sightings) the high-water mark is ~22 (see test.c's peak test), so 64 gives ~3x
 * headroom. If the live count ever exceeds N_FIELDS those mites fall back to greedy
 * steering that tick (overflow is a safety net, not a crash). 64 fields * (a
 * pg[N_EDGE_MAX] u16 vector) ~= 16 KB. */
#define N_FIELDS    64

/* a mite's movement mode (its record + mode pick the destination, then the field) */
#define MM_WANDER   0           /* no destination: drunk walk over the open cell graph */
#define MM_HUNT     1           /* path to the recorded tank cell */
#define MM_HOME     2           /* path to this mite's nest */

/* ---- combat: the tanks shoot the swarm (chapter-3 gameplay) ----------------
 * Each tank's turret turns at a fixed rate toward the mite it has line of sight to
 * that is MOST LIKELY TO BE HIT — the one closest to the turret's current direction
 * (least rotation), found over the per-cell index — and fires at a fixed rate once
 * it has swung onto the target. The shot is a PROJECTILE: a bolt that travels from the
 * muzzle along the firing heading, destroying every mite it passes through and PIERCING
 * on (it does not stop on a kill) until it meets a wall (or flies its max range), then
 * expires. The turret is freed the instant the bolt leaves the muzzle, so it can swing
 * onto the next target while the shot is still in flight (one bolt per tank at a time).
 * A kill is a "death cry": every mite within 2x sensing range of the dead one learns the
 * firing tank's cell (record + hunt), so the swarm turns on its attackers — emergent
 * escalation from the same gossip. */
#define TARGET_MAX_R 12         /* turret search radius, in cells */
#define TGT_NONE     0xFFFFu    /* tank has no target */
#define PROJ_RANGE   64         /* max bolt flight in cells before it expires (also stops at a wall) */
#define PROJ_HW      40         /* bolt kill half-width, subcells (< SUBQ, so off-line mites dodge) */
#define PROJ_MAX     4          /* a tank can have up to this many bolts in flight at once */
#define PROJ_SPEED_DEFAULT SUB  /* default bolt travel per tick in subcells: 1 cell/tick — a slow,
                                 * deliberate bolt. Live-tunable on the page as `w->proj_speed`. (A far
                                 * open-field shot can still be in flight when the next would fire; the
                                 * single-bolt-per-tank slot then paces the cadence to the bolt's flight.) */

/* destruction effects: a small fixed ring of expanding/fading bursts, one per mite a
 * bolt destroys — pure cosmetic, written by tanks_fire, drawn by render (no allocation). */
#define N_FX         256        /* effect ring capacity (bursts overwrite the oldest) */
#define FX_DURATION  8          /* ticks a burst is drawn before it expires */
#define FX_KILL      0          /* burst kind: a destroyed mite (a white pop) */
#define FX_IMPACT    1          /* burst kind: a bolt splashing on a wall (a hot orange spark) */

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
