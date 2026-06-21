/* tank-combat — step 1: two tanks, grid obstacles, collision.
 *
 * Goal of the project: demonstrate a data-oriented approach to building a
 * game. This file owns the world *data* and the exported boundary; the actual
 * transforms live next to the data they touch, each in its own file:
 *   tanks_turn.c  rotate headings by input         (independent)
 *   tanks_move.c  advance + resolve grid collision (independent)
 *   tanks_steer.c on collision, steer toward slide (consumes move output)
 *   render.c      world -> instance buffer         (the renderer boundary)
 *   dirtab.c      baked Q14 cos/sin table          (shared by move + render)
 * Splitting them makes the independence structural, not just a comment.
 *
 * NUMBERS ARE INTEGER FIXED POINT — no floating point anywhere in the sim.
 *   - positions/sizes: subcells, 256 == 1 grid cell (Q8.8), int16_t.
 *   - headings:        uint16_t, a Q5.11 angle. The *direction* used for
 *                      movement/render is the top 5 bits (angle>>11 -> 0..31,
 *                      one of 32 directions); the low 11 bits are sub-direction
 *                      precision so turn_rate can be finer than one direction
 *                      per tick. 5 bits would suffice for the 32 directions
 *                      alone, but then turning could only snap whole steps;
 *                      8 bits (3 fractional) makes turn speed coarse, so the
 *                      native 16-bit word is the clean fit.
 *   - colors:          uint32_t packed RGBA8888 (see render.h).
 *   - grid:            bitset, one uint32_t per row, bit c == column c.
 * The sim runs on a FIXED timestep (one tick == one frame), so there is no
 * dt to multiply: per-tick deltas (move_speed, turn_rate) are the data.
 *
 * Build: freestanding wasm32, no libc, no libm. All state lives in static
 * linear memory, never grown, so the JS-side typed-array views stay valid.
 *
 * ---------------------------------------------------------------------------
 * DATA PROTOCOL (JS <-> WASM)
 * ---------------------------------------------------------------------------
 * C owns every byte layout. JS never computes an offset; it asks C for the
 * pointer/count it needs. The exported surface is the protocol:
 *
 *   init()                          place tanks + load the level
 *   tick()    -> inst_count         advance one fixed step, refill instances
 *   set_input(tank, bits)           write a tank's input bitfield
 *
 *   pointers (into linear memory, read by JS each frame):
 *     inst_ptr  grid_ptr
 *     tank_x_ptr tank_y_ptr tank_angle_ptr tank_input_ptr
 *     tank_vx_ptr tank_vy_ptr tank_hit_ptr
 *
 *   scalars: grid_w grid_h n_tanks n_dirs subcell inst_count inst_stride frame
 *
 *   tunables (read + edited live from the debug panel):
 *     move_speed/set_move_speed   turn_rate/set_turn_rate
 *
 *   debug pokes (the panel mutating the data directly):
 *     set_tank_pose(tank, x_sub, y_sub, angle)  set_wall(cx,cy,v)  toggle_wall(cx,cy)
 * ---------------------------------------------------------------------------
 */

#include "defs.h"
#include "render.h"
#include "tanks_turn.h"
#include "tanks_move.h"
#include "tanks_steer.h"

#define EXPORT(name) __attribute__((export_name(#name)))

/* --- the world: all simulation state, flat and static -------------------- */
/* tanks held as structure-of-arrays so each transform streams one field.   */
static int16_t  g_tank_x   [N_TANKS];   /* subcells */
static int16_t  g_tank_y   [N_TANKS];   /* subcells */
static uint16_t g_tank_ang [N_TANKS];   /* Q5.11 heading (see file header)   */
static uint8_t  g_tank_in  [N_TANKS];
static int16_t  g_tank_vx  [N_TANKS];   /* last tick's applied move, subcells */
static int16_t  g_tank_vy  [N_TANKS];
static uint8_t  g_tank_hit [N_TANKS];   /* 1 if it collided while moving      */

/* grid as a bitset: one uint32_t per row, bit c == column c (LSB = col 0).  */
_Static_assert(GRID_W <= 32, "a grid row must fit in one uint32_t");
static uint32_t g_grid [GRID_H];        /* 1 bit per cell, wall = 1           */

static Inst     g_inst [N_CELLS + N_TANKS * 2];
static uint32_t g_inst_count;

static uint32_t g_frame;
static uint16_t g_move_speed = 20;      /* subcells per tick (~4.7 cells/sec) */
static uint16_t g_turn_rate  = 400;     /* angle units per tick               */

/* initial level, stored directly as row bit-words (no parse step). The ASCII
 * picture in each comment is just documentation; the word is the data.       */
static const uint32_t GRID_INIT[GRID_H] = {  /* bit c == column c */
  0xFFFFF,  /* #################### */
  0x80001,  /* #..................# */
  0x98019,  /* #..##..........##..# */
  0x98619,  /* #..##....##....##..# */
  0x80601,  /* #........##........# */
  0x8C061,  /* #....##.......##...# */
  0x8C061,  /* #....##.......##...# */
  0x80401,  /* #.........#........# */
  0x8C061,  /* #....##.......##...# */
  0x8C061,  /* #....##.......##...# */
  0x80601,  /* #........##........# */
  0x98619,  /* #..##....##....##..# */
  0x98019,  /* #..##..........##..# */
  0x80001,  /* #..................# */
  0xFFFFF,  /* #################### */
};

static uint32_t rebuild_instances(void) {
  return build_instances(g_inst, g_grid, g_tank_x, g_tank_y, g_tank_ang, N_TANKS);
}

/* ------------------------------------------------------------------------ */
/* exports                                                                  */
/* ------------------------------------------------------------------------ */

EXPORT(init) void init(void) {
  for (int32_t r = 0; r < GRID_H; r++) g_grid[r] = GRID_INIT[r];

  /* place the two tanks in known-open cells, facing each other */
  g_tank_x[0] = (int16_t)(2  * SUB + SUB / 2); g_tank_y[0] = (int16_t)(7 * SUB + SUB / 2);
  g_tank_ang[0] = 0;        /* east */
  g_tank_x[1] = (int16_t)(17 * SUB + SUB / 2); g_tank_y[1] = (int16_t)(7 * SUB + SUB / 2);
  g_tank_ang[1] = 0x8000;   /* west */
  for (uint32_t i = 0; i < N_TANKS; i++) {
    g_tank_in[i] = 0; g_tank_vx[i] = 0; g_tank_vy[i] = 0; g_tank_hit[i] = 0;
  }
  g_frame = 0;
  g_inst_count = rebuild_instances();
}

EXPORT(tick) uint32_t tick(void) {
  tanks_turn(g_tank_ang, g_tank_in, N_TANKS, g_turn_rate);
  tanks_move(g_tank_x, g_tank_y, g_tank_vx, g_tank_vy, g_tank_hit,
             g_tank_ang, g_tank_in, N_TANKS, (int32_t)g_move_speed, g_grid);
  /* when a tank slid along a wall, steer it to face along the slide */
  tanks_steer(g_tank_ang, g_tank_vx, g_tank_vy, g_tank_hit, g_tank_in, N_TANKS, g_turn_rate);
  g_inst_count = rebuild_instances();
  g_frame++;
  return g_inst_count;
}

EXPORT(set_input) void set_input(uint32_t tank, uint32_t bits) {
  if (tank < N_TANKS) g_tank_in[tank] = (uint8_t)bits;
}

/* pointers */
EXPORT(inst_ptr)       Inst*     inst_ptr(void)       { return g_inst; }
EXPORT(grid_ptr)       uint32_t* grid_ptr(void)       { return g_grid; }
EXPORT(tank_x_ptr)     int16_t*  tank_x_ptr(void)     { return g_tank_x; }
EXPORT(tank_y_ptr)     int16_t*  tank_y_ptr(void)     { return g_tank_y; }
EXPORT(tank_angle_ptr) uint16_t* tank_angle_ptr(void) { return g_tank_ang; }
EXPORT(tank_input_ptr) uint8_t*  tank_input_ptr(void) { return g_tank_in; }
EXPORT(tank_vx_ptr)    int16_t*  tank_vx_ptr(void)    { return g_tank_vx; }
EXPORT(tank_vy_ptr)    int16_t*  tank_vy_ptr(void)    { return g_tank_vy; }
EXPORT(tank_hit_ptr)   uint8_t*  tank_hit_ptr(void)   { return g_tank_hit; }

/* scalars */
EXPORT(grid_w)      uint32_t grid_w(void)      { return GRID_W; }
EXPORT(grid_h)      uint32_t grid_h(void)      { return GRID_H; }
EXPORT(n_tanks)     uint32_t n_tanks(void)     { return N_TANKS; }
EXPORT(n_dirs)      uint32_t n_dirs(void)      { return N_DIRS; }
EXPORT(subcell)     uint32_t subcell(void)     { return SUB; }
EXPORT(inst_count)  uint32_t inst_count(void)  { return g_inst_count; }
EXPORT(inst_stride) uint32_t inst_stride(void) { return (uint32_t)sizeof(Inst); }
EXPORT(frame)       uint32_t frame(void)       { return g_frame; }

/* tunables */
EXPORT(move_speed)     uint32_t move_speed(void)       { return g_move_speed; }
EXPORT(set_move_speed) void set_move_speed(uint32_t v) { g_move_speed = (uint16_t)v; }
EXPORT(turn_rate)      uint32_t turn_rate(void)        { return g_turn_rate; }
EXPORT(set_turn_rate)  void set_turn_rate(uint32_t v)  { g_turn_rate = (uint16_t)v; }

/* debug pokes */
EXPORT(set_tank_pose) void set_tank_pose(uint32_t t, int32_t x_sub, int32_t y_sub, uint32_t ang) {
  if (t >= N_TANKS) return;
  g_tank_x[t] = (int16_t)x_sub; g_tank_y[t] = (int16_t)y_sub; g_tank_ang[t] = (uint16_t)ang;
  g_inst_count = rebuild_instances();
}
EXPORT(set_wall) void set_wall(uint32_t cx, uint32_t cy, uint32_t v) {
  if (cx < GRID_W && cy < GRID_H) {
    if (v) g_grid[cy] |= (1u << cx); else g_grid[cy] &= ~(1u << cx);
    g_inst_count = rebuild_instances();
  }
}
EXPORT(toggle_wall) void toggle_wall(uint32_t cx, uint32_t cy) {
  if (cx < GRID_W && cy < GRID_H) {
    g_grid[cy] ^= (1u << cx);
    g_inst_count = rebuild_instances();
  }
}
