/* tank-combat — step 1: two tanks, grid obstacles, collision.
 *
 * Goal of the project: demonstrate a data-oriented approach to building a
 * game. This file is the whole simulation. It holds ONLY data and the
 * transforms over that data. No allocation, no hidden state, no objects.
 *
 * NUMBERS ARE INTEGER FIXED POINT — there is no floating point here.
 *   - positions/sizes: subcells, 256 subcells == 1 grid cell (Q8.8), as int16_t.
 *   - headings:        uint16_t angle, 0..65535 == full turn; angle>>11 picks one
 *                      of 32 discrete directions.
 *   - trig:            DIR_COS/DIR_SIN baked in Q14 (value == trig * 16384).
 *   - colors:          uint8_t per channel.
 *   - grid:            bitset, one uint32_t per row, bit c == column c.
 * The sim runs on a FIXED timestep (one tick == one frame), so there is no
 * dt to multiply: per-tick deltas (move_speed, turn_rate) are the data.
 *
 * Build: freestanding wasm32, no libc, no libm. All state lives in static
 * linear memory. WASM memory is never grown, so the JS-side typed-array
 * views over it stay valid for the life of the page.
 *
 * ---------------------------------------------------------------------------
 * DATA PROTOCOL (JS <-> WASM)
 * ---------------------------------------------------------------------------
 * C owns every byte layout. JS never computes an offset; it asks C for the
 * pointer/count it needs. The exported surface is the protocol:
 *
 *   init()                          build grid + place tanks
 *   tick()    -> inst_count         advance one fixed step, refill instances
 *   set_input(tank, bits)           write a tank's input bitfield
 *
 *   pointers (into linear memory, read by JS each frame):
 *     inst_ptr  grid_ptr
 *     tank_x_ptr tank_y_ptr tank_angle_ptr tank_input_ptr tank_vx_ptr tank_vy_ptr
 *
 *   scalars: grid_w grid_h n_tanks n_dirs subcell inst_count inst_stride frame
 *
 *   tunables (read + edited live from the debug panel):
 *     move_speed/set_move_speed   turn_rate/set_turn_rate
 *
 *   debug pokes (the panel mutating the data directly):
 *     set_tank_pose(tank, x_sub, y_sub, angle)  set_wall(cx,cy,v)  toggle_wall(cx,cy)
 *
 * INSTANCE LAYOUT: 16 bytes, all integer:
 *   int16_t cx, cy   (subcells)   off 0
 *   int16_t hx, hy   (subcells)   off 4
 *   int16_t co, si   (Q14 rot)    off 8
 *   uint8_t  r,g,b,a               off 12
 * ---------------------------------------------------------------------------
 */

#include <stdint.h>

/* exact-width types are part of the data contract (layout, the wasm<->JS
 * protocol, the packed Inst struct); stdint.h provides them directly. */

#define EXPORT(name) __attribute__((export_name(#name)))

/* --- fixed sizes (constraints we exploit, not parameters we expose) ------ */
#define GRID_W      20
#define GRID_H      15
#define N_CELLS     (GRID_W * GRID_H)
#define N_TANKS     2
#define N_DIRS      32          /* heading is discrete: angle>>11 -> 0..31  */
#define ANGLE_SHIFT 11          /* 65536 >> 11 == 32                        */
#define SUB         256         /* subcells per grid cell (Q8.8)            */
#define SUB_SHIFT   8
#define ARENA_W_SUB (GRID_W * SUB)
#define ARENA_H_SUB (GRID_H * SUB)
#define TANK_R      92          /* collision half-extent, subcells (~0.36c) */
#define TRIG_SHIFT  14          /* DIR_* are Q14                            */

/* input bits */
#define IN_FWD   1u
#define IN_BACK  2u
#define IN_LEFT  4u
#define IN_RIGHT 8u
#define IN_FIRE  16u            /* reserved for a later step; carried, unused */

/* baked trig table: cos/sin of the 32 discrete headings, Q14. No libm. */
static const int16_t DIR_COS[32] = {
  16384,16069,15137,13623,11585,9102,6270,3196,0,-3196,-6270,-9102,-11585,-13623,-15137,-16069,-16384,-16069,-15137,-13623,-11585,-9102,-6270,-3196,0,3196,6270,9102,11585,13623,15137,16069
};
static const int16_t DIR_SIN[32] = {
  0,3196,6270,9102,11585,13623,15137,16069,16384,16069,15137,13623,11585,9102,6270,3196,0,-3196,-6270,-9102,-11585,-13623,-15137,-16069,-16384,-16069,-15137,-13623,-11585,-9102,-6270,-3196
};

/* one quad instance, 16 bytes, naturally packed (no padding). */
typedef struct { int16_t cx, cy, hx, hy, co, si; uint8_t r, g, b, a; } Inst;

/* colors, uint8_t rgba */
typedef struct { uint8_t r, g, b, a; } Rgba;
static const Rgba COL_WALL    = {  82,  87, 102, 255 };
static const Rgba COL_BODY[2] = { { 242, 158, 41, 255 }, {  77, 179, 230, 255 } };
static const Rgba COL_BARR[2] = { { 145,  95, 24, 255 }, {  46, 107, 138, 255 } };

/* --- the world: all simulation state, flat and static -------------------- */
/* tanks held as structure-of-arrays so each transform streams one field.   */
static int16_t g_tank_x   [N_TANKS];   /* subcells */
static int16_t g_tank_y   [N_TANKS];   /* subcells */
static uint16_t g_tank_ang [N_TANKS];
static uint8_t  g_tank_in  [N_TANKS];
static int16_t g_tank_vx  [N_TANKS];   /* last frame's applied move, subcells   */
static int16_t g_tank_vy  [N_TANKS];

/* grid as a bitset: one uint32_t per row, bit c == column c (LSB = col 0).  */
_Static_assert(GRID_W <= 32, "a grid row must fit in one uint32_t");
static uint32_t g_grid [GRID_H];        /* 1 bit per cell, wall = 1              */

static Inst g_inst [N_CELLS + N_TANKS * 2];
static uint32_t  g_inst_count;

static uint32_t g_frame;
static uint16_t g_move_speed = 20;      /* subcells per tick (~4.7 cells/sec)    */
static uint16_t g_turn_rate  = 400;     /* angle units per tick                  */

/* default map: '#' wall, '.' empty. This is data; init parses it once.     */
static const char* const MAP[GRID_H] = {
  "####################",
  "#..................#",
  "#..##..........##..#",
  "#..##....##....##..#",
  "#........##........#",
  "#....##.......##...#",
  "#....##.......##...#",
  "#.........#........#",
  "#....##.......##...#",
  "#....##.......##...#",
  "#........##........#",
  "#..##....##....##..#",
  "#..##..........##..#",
  "#..................#",
  "####################",
};

/* ------------------------------------------------------------------------ */
/* transforms                                                               */
/* ------------------------------------------------------------------------ */

/* turn each tank by its input. n is the batch; the 2-tank case is just     */
/* count==2. Branchy on input bits, but n is tiny and this is not hot.      */
static void tanks_turn(uint16_t* ang, const uint8_t* in, uint32_t n, uint16_t step) {
  for (uint32_t i = 0; i < n; i++) {
    int32_t d = (int32_t)((in[i] & IN_RIGHT) != 0) - (int32_t)((in[i] & IN_LEFT) != 0);
    ang[i] = (uint16_t)((int32_t)ang[i] + (int32_t)step * d);   /* wraps mod 65536       */
  }
}

/* is the tank's square at (px,py) subcells blocked by a wall or the edge?  */
/* out-of-range policy: edge counts as blocked (reject), so the tank stays  */
/* inside [TANK_R, ARENA-TANK_R] and grid indices are always valid.         */
static int32_t blocked(const uint32_t* grid, int32_t px, int32_t py) {
  if (px < TANK_R || py < TANK_R ||
      px > ARENA_W_SUB - TANK_R || py > ARENA_H_SUB - TANK_R) return 1;
  int32_t c0 = (px - TANK_R) >> SUB_SHIFT, c1 = (px + TANK_R) >> SUB_SHIFT;
  int32_t r0 = (py - TANK_R) >> SUB_SHIFT, r1 = (py + TANK_R) >> SUB_SHIFT;
  uint32_t span = ((1u << (c1 - c0 + 1)) - 1u) << c0;   /* the columns the tank covers */
  for (int32_t r = r0; r <= r1; r++)
    if (grid[r] & span) return 1;                        /* any wall in those columns?  */
  return 0;
}

/* move each tank along its heading by move_speed, resolving against the     */
/* grid one axis at a time so a tank slides along a wall it hits at an angle.*/
static void tanks_move(int16_t* x, int16_t* y, int16_t* vx, int16_t* vy,
                       const uint16_t* ang, const uint8_t* in, uint32_t n,
                       int32_t speed, const uint32_t* grid) {
  for (uint32_t i = 0; i < n; i++) {
    int32_t thr = (int32_t)((in[i] & IN_FWD) != 0) - (int32_t)((in[i] & IN_BACK) != 0);
    uint32_t di  = ang[i] >> ANGLE_SHIFT;
    int32_t mx  = (DIR_COS[di] * speed * thr) >> TRIG_SHIFT;   /* subcells/tick  */
    int32_t my  = (DIR_SIN[di] * speed * thr) >> TRIG_SHIFT;
    int32_t nx  = x[i] + mx;
    if (blocked(grid, nx, y[i])) mx = 0; else x[i] = (int16_t)nx;
    int32_t ny  = y[i] + my;
    if (blocked(grid, x[i], ny)) my = 0; else y[i] = (int16_t)ny;
    vx[i] = (int16_t)mx; vy[i] = (int16_t)my;
  }
}

/* emit one quad instance */
static uint32_t push_quad(uint32_t k, int32_t cx, int32_t cy, int32_t hx, int32_t hy,
                     int32_t co, int32_t si, Rgba col) {
  Inst* o = &g_inst[k];
  o->cx = (int16_t)cx; o->cy = (int16_t)cy; o->hx = (int16_t)hx; o->hy = (int16_t)hy;
  o->co = (int16_t)co; o->si = (int16_t)si;
  o->r = col.r; o->g = col.g; o->b = col.b; o->a = col.a;
  return k + 1;
}

/* transform world state -> flat instance buffer for the renderer.          */
static uint32_t build_instances(void) {
  uint32_t k = 0;
  for (int32_t r = 0; r < GRID_H; r++)
    for (int32_t c = 0; c < GRID_W; c++)
      if ((g_grid[r] >> c) & 1u)
        k = push_quad(k, c * SUB + SUB / 2, r * SUB + SUB / 2,
                      123, 123, 16384, 0, COL_WALL);
  for (uint32_t i = 0; i < N_TANKS; i++) {
    uint32_t di = g_tank_ang[i] >> ANGLE_SHIFT;
    int32_t co = DIR_COS[di], si = DIR_SIN[di];
    int32_t cx = g_tank_x[i], cy = g_tank_y[i];
    k = push_quad(k, cx, cy, 87, 67, co, si, COL_BODY[i]);
    /* barrel: a short bar offset forward (0.34 cell == 87 subcells) */
    int32_t bx = cx + ((co * 87) >> TRIG_SHIFT);
    int32_t by = cy + ((si * 87) >> TRIG_SHIFT);
    k = push_quad(k, bx, by, 56, 18, co, si, COL_BARR[i]);
  }
  return k;
}

/* ------------------------------------------------------------------------ */
/* exports                                                                  */
/* ------------------------------------------------------------------------ */

EXPORT(init) void init(void) {
  for (int32_t r = 0; r < GRID_H; r++) {
    uint32_t row = 0;
    for (int32_t c = 0; c < GRID_W; c++)
      if (MAP[r][c] == '#') row |= (1u << c);
    g_grid[r] = row;
  }

  /* place the two tanks in known-open cells, facing each other */
  g_tank_x[0] = (int16_t)(2  * SUB + SUB / 2); g_tank_y[0] = (int16_t)(7 * SUB + SUB / 2);
  g_tank_ang[0] = 0;        /* east */
  g_tank_x[1] = (int16_t)(17 * SUB + SUB / 2); g_tank_y[1] = (int16_t)(7 * SUB + SUB / 2);
  g_tank_ang[1] = 0x8000;   /* west */
  for (uint32_t i = 0; i < N_TANKS; i++) {
    g_tank_in[i] = 0; g_tank_vx[i] = 0; g_tank_vy[i] = 0;
  }
  g_frame = 0;
  g_inst_count = build_instances();
}

EXPORT(tick) uint32_t tick(void) {
  tanks_turn(g_tank_ang, g_tank_in, N_TANKS, g_turn_rate);
  tanks_move(g_tank_x, g_tank_y, g_tank_vx, g_tank_vy,
             g_tank_ang, g_tank_in, N_TANKS, (int32_t)g_move_speed, g_grid);
  g_inst_count = build_instances();
  g_frame++;
  return g_inst_count;
}

EXPORT(set_input) void set_input(uint32_t tank, uint32_t bits) {
  if (tank < N_TANKS) g_tank_in[tank] = (uint8_t)bits;
}

/* pointers */
EXPORT(inst_ptr)       Inst* inst_ptr(void)      { return g_inst; }
EXPORT(grid_ptr)       uint32_t* grid_ptr(void)        { return g_grid; }
EXPORT(tank_x_ptr)     int16_t* tank_x_ptr(void)      { return g_tank_x; }
EXPORT(tank_y_ptr)     int16_t* tank_y_ptr(void)      { return g_tank_y; }
EXPORT(tank_angle_ptr) uint16_t* tank_angle_ptr(void)  { return g_tank_ang; }
EXPORT(tank_input_ptr) uint8_t*  tank_input_ptr(void)  { return g_tank_in; }
EXPORT(tank_vx_ptr)    int16_t* tank_vx_ptr(void)     { return g_tank_vx; }
EXPORT(tank_vy_ptr)    int16_t* tank_vy_ptr(void)     { return g_tank_vy; }

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
EXPORT(move_speed)     uint32_t move_speed(void)        { return g_move_speed; }
EXPORT(set_move_speed) void set_move_speed(uint32_t v)  { g_move_speed = (uint16_t)v; }
EXPORT(turn_rate)      uint32_t turn_rate(void)         { return g_turn_rate; }
EXPORT(set_turn_rate)  void set_turn_rate(uint32_t v)   { g_turn_rate = (uint16_t)v; }

/* debug pokes */
EXPORT(set_tank_pose) void set_tank_pose(uint32_t t, int32_t x_sub, int32_t y_sub, uint32_t ang) {
  if (t >= N_TANKS) return;
  g_tank_x[t] = (int16_t)x_sub; g_tank_y[t] = (int16_t)y_sub; g_tank_ang[t] = (uint16_t)ang;
  g_inst_count = build_instances();
}
EXPORT(set_wall) void set_wall(uint32_t cx, uint32_t cy, uint32_t v) {
  if (cx < GRID_W && cy < GRID_H) {
    if (v) g_grid[cy] |= (1u << cx); else g_grid[cy] &= ~(1u << cx);
    g_inst_count = build_instances();
  }
}
EXPORT(toggle_wall) void toggle_wall(uint32_t cx, uint32_t cy) {
  if (cx < GRID_W && cy < GRID_H) {
    g_grid[cy] ^= (1u << cx);
    g_inst_count = build_instances();
  }
}
