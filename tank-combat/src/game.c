/* tank-combat — step 1: two tanks, grid obstacles, collision.
 *
 * Goal of the project: demonstrate a data-oriented approach to building a
 * game. This file is the whole simulation. It holds ONLY data and the
 * transforms over that data. No allocation, no hidden state, no objects.
 *
 * NUMBERS ARE INTEGER FIXED POINT — there is no floating point here.
 *   - positions/sizes: subcells, 256 subcells == 1 grid cell (Q8.8), as i16.
 *   - headings:        u16 angle, 0..65535 == full turn; angle>>11 picks one
 *                      of 32 discrete directions.
 *   - trig:            DIR_COS/DIR_SIN baked in Q14 (value == trig * 16384).
 *   - colors:          u8 per channel.
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
 *   i16 cx, cy   (subcells)   off 0
 *   i16 hx, hy   (subcells)   off 4
 *   i16 co, si   (Q14 rot)    off 8
 *   u8  r,g,b,a               off 12
 * ---------------------------------------------------------------------------
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef signed   short i16;
typedef signed   int   i32;

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
static const i16 DIR_COS[32] = {
  16384,16069,15137,13623,11585,9102,6270,3196,0,-3196,-6270,-9102,-11585,-13623,-15137,-16069,-16384,-16069,-15137,-13623,-11585,-9102,-6270,-3196,0,3196,6270,9102,11585,13623,15137,16069
};
static const i16 DIR_SIN[32] = {
  0,3196,6270,9102,11585,13623,15137,16069,16384,16069,15137,13623,11585,9102,6270,3196,0,-3196,-6270,-9102,-11585,-13623,-15137,-16069,-16384,-16069,-15137,-13623,-11585,-9102,-6270,-3196
};

/* one quad instance, 16 bytes, naturally packed (no padding). */
typedef struct { i16 cx, cy, hx, hy, co, si; u8 r, g, b, a; } Inst;

/* colors, u8 rgba */
typedef struct { u8 r, g, b, a; } Rgba;
static const Rgba COL_WALL    = {  82,  87, 102, 255 };
static const Rgba COL_BODY[2] = { { 242, 158, 41, 255 }, {  77, 179, 230, 255 } };
static const Rgba COL_BARR[2] = { { 145,  95, 24, 255 }, {  46, 107, 138, 255 } };

/* --- the world: all simulation state, flat and static -------------------- */
/* tanks held as structure-of-arrays so each transform streams one field.   */
static i16 g_tank_x   [N_TANKS];   /* subcells */
static i16 g_tank_y   [N_TANKS];   /* subcells */
static u16 g_tank_ang [N_TANKS];
static u8  g_tank_in  [N_TANKS];
static i16 g_tank_vx  [N_TANKS];   /* last frame's applied move, subcells   */
static i16 g_tank_vy  [N_TANKS];

static u8  g_grid [N_CELLS];       /* 0 empty, 1 wall                       */

static Inst g_inst [N_CELLS + N_TANKS * 2];
static u32  g_inst_count;

static u32 g_frame;
static u16 g_move_speed = 20;      /* subcells per tick (~4.7 cells/sec)    */
static u16 g_turn_rate  = 400;     /* angle units per tick                  */

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
static void tanks_turn(u16* ang, const u8* in, u32 n, u16 step) {
  for (u32 i = 0; i < n; i++) {
    i32 d = (i32)((in[i] & IN_RIGHT) != 0) - (i32)((in[i] & IN_LEFT) != 0);
    ang[i] = (u16)((i32)ang[i] + (i32)step * d);   /* wraps mod 65536       */
  }
}

/* is the tank's square at (px,py) subcells blocked by a wall or the edge?  */
/* out-of-range policy: edge counts as blocked (reject), so the tank stays  */
/* inside [TANK_R, ARENA-TANK_R] and grid indices are always valid.         */
static i32 blocked(const u8* grid, i32 px, i32 py) {
  if (px < TANK_R || py < TANK_R ||
      px > ARENA_W_SUB - TANK_R || py > ARENA_H_SUB - TANK_R) return 1;
  i32 c0 = (px - TANK_R) >> SUB_SHIFT, c1 = (px + TANK_R) >> SUB_SHIFT;
  i32 r0 = (py - TANK_R) >> SUB_SHIFT, r1 = (py + TANK_R) >> SUB_SHIFT;
  for (i32 r = r0; r <= r1; r++)
    for (i32 c = c0; c <= c1; c++)
      if (grid[r * GRID_W + c]) return 1;
  return 0;
}

/* move each tank along its heading by move_speed, resolving against the     */
/* grid one axis at a time so a tank slides along a wall it hits at an angle.*/
static void tanks_move(i16* x, i16* y, i16* vx, i16* vy,
                       const u16* ang, const u8* in, u32 n,
                       i32 speed, const u8* grid) {
  for (u32 i = 0; i < n; i++) {
    i32 thr = (i32)((in[i] & IN_FWD) != 0) - (i32)((in[i] & IN_BACK) != 0);
    u32 di  = ang[i] >> ANGLE_SHIFT;
    i32 mx  = (DIR_COS[di] * speed * thr) >> TRIG_SHIFT;   /* subcells/tick  */
    i32 my  = (DIR_SIN[di] * speed * thr) >> TRIG_SHIFT;
    i32 nx  = x[i] + mx;
    if (blocked(grid, nx, y[i])) mx = 0; else x[i] = (i16)nx;
    i32 ny  = y[i] + my;
    if (blocked(grid, x[i], ny)) my = 0; else y[i] = (i16)ny;
    vx[i] = (i16)mx; vy[i] = (i16)my;
  }
}

/* emit one quad instance */
static u32 push_quad(u32 k, i32 cx, i32 cy, i32 hx, i32 hy,
                     i32 co, i32 si, Rgba col) {
  Inst* o = &g_inst[k];
  o->cx = (i16)cx; o->cy = (i16)cy; o->hx = (i16)hx; o->hy = (i16)hy;
  o->co = (i16)co; o->si = (i16)si;
  o->r = col.r; o->g = col.g; o->b = col.b; o->a = col.a;
  return k + 1;
}

/* transform world state -> flat instance buffer for the renderer.          */
static u32 build_instances(void) {
  u32 k = 0;
  for (i32 r = 0; r < GRID_H; r++)
    for (i32 c = 0; c < GRID_W; c++)
      if (g_grid[r * GRID_W + c])
        k = push_quad(k, c * SUB + SUB / 2, r * SUB + SUB / 2,
                      123, 123, 16384, 0, COL_WALL);
  for (u32 i = 0; i < N_TANKS; i++) {
    u32 di = g_tank_ang[i] >> ANGLE_SHIFT;
    i32 co = DIR_COS[di], si = DIR_SIN[di];
    i32 cx = g_tank_x[i], cy = g_tank_y[i];
    k = push_quad(k, cx, cy, 87, 67, co, si, COL_BODY[i]);
    /* barrel: a short bar offset forward (0.34 cell == 87 subcells) */
    i32 bx = cx + ((co * 87) >> TRIG_SHIFT);
    i32 by = cy + ((si * 87) >> TRIG_SHIFT);
    k = push_quad(k, bx, by, 56, 18, co, si, COL_BARR[i]);
  }
  return k;
}

/* ------------------------------------------------------------------------ */
/* exports                                                                  */
/* ------------------------------------------------------------------------ */

EXPORT(init) void init(void) {
  for (i32 r = 0; r < GRID_H; r++)
    for (i32 c = 0; c < GRID_W; c++)
      g_grid[r * GRID_W + c] = (MAP[r][c] == '#') ? 1u : 0u;

  /* place the two tanks in known-open cells, facing each other */
  g_tank_x[0] = (i16)(2  * SUB + SUB / 2); g_tank_y[0] = (i16)(7 * SUB + SUB / 2);
  g_tank_ang[0] = 0;        /* east */
  g_tank_x[1] = (i16)(17 * SUB + SUB / 2); g_tank_y[1] = (i16)(7 * SUB + SUB / 2);
  g_tank_ang[1] = 0x8000;   /* west */
  for (u32 i = 0; i < N_TANKS; i++) {
    g_tank_in[i] = 0; g_tank_vx[i] = 0; g_tank_vy[i] = 0;
  }
  g_frame = 0;
  g_inst_count = build_instances();
}

EXPORT(tick) u32 tick(void) {
  tanks_turn(g_tank_ang, g_tank_in, N_TANKS, g_turn_rate);
  tanks_move(g_tank_x, g_tank_y, g_tank_vx, g_tank_vy,
             g_tank_ang, g_tank_in, N_TANKS, (i32)g_move_speed, g_grid);
  g_inst_count = build_instances();
  g_frame++;
  return g_inst_count;
}

EXPORT(set_input) void set_input(u32 tank, u32 bits) {
  if (tank < N_TANKS) g_tank_in[tank] = (u8)bits;
}

/* pointers */
EXPORT(inst_ptr)       Inst* inst_ptr(void)      { return g_inst; }
EXPORT(grid_ptr)       u8*  grid_ptr(void)        { return g_grid; }
EXPORT(tank_x_ptr)     i16* tank_x_ptr(void)      { return g_tank_x; }
EXPORT(tank_y_ptr)     i16* tank_y_ptr(void)      { return g_tank_y; }
EXPORT(tank_angle_ptr) u16* tank_angle_ptr(void)  { return g_tank_ang; }
EXPORT(tank_input_ptr) u8*  tank_input_ptr(void)  { return g_tank_in; }
EXPORT(tank_vx_ptr)    i16* tank_vx_ptr(void)     { return g_tank_vx; }
EXPORT(tank_vy_ptr)    i16* tank_vy_ptr(void)     { return g_tank_vy; }

/* scalars */
EXPORT(grid_w)      u32 grid_w(void)      { return GRID_W; }
EXPORT(grid_h)      u32 grid_h(void)      { return GRID_H; }
EXPORT(n_tanks)     u32 n_tanks(void)     { return N_TANKS; }
EXPORT(n_dirs)      u32 n_dirs(void)      { return N_DIRS; }
EXPORT(subcell)     u32 subcell(void)     { return SUB; }
EXPORT(inst_count)  u32 inst_count(void)  { return g_inst_count; }
EXPORT(inst_stride) u32 inst_stride(void) { return (u32)sizeof(Inst); }
EXPORT(frame)       u32 frame(void)       { return g_frame; }

/* tunables */
EXPORT(move_speed)     u32 move_speed(void)        { return g_move_speed; }
EXPORT(set_move_speed) void set_move_speed(u32 v)  { g_move_speed = (u16)v; }
EXPORT(turn_rate)      u32 turn_rate(void)         { return g_turn_rate; }
EXPORT(set_turn_rate)  void set_turn_rate(u32 v)   { g_turn_rate = (u16)v; }

/* debug pokes */
EXPORT(set_tank_pose) void set_tank_pose(u32 t, i32 x_sub, i32 y_sub, u32 ang) {
  if (t >= N_TANKS) return;
  g_tank_x[t] = (i16)x_sub; g_tank_y[t] = (i16)y_sub; g_tank_ang[t] = (u16)ang;
  g_inst_count = build_instances();
}
EXPORT(set_wall) void set_wall(u32 cx, u32 cy, u32 v) {
  if (cx < GRID_W && cy < GRID_H) {
    g_grid[cy * GRID_W + cx] = v ? 1u : 0u;
    g_inst_count = build_instances();
  }
}
EXPORT(toggle_wall) void toggle_wall(u32 cx, u32 cy) {
  if (cx < GRID_W && cy < GRID_H) {
    g_grid[cy * GRID_W + cx] ^= 1u;
    g_inst_count = build_instances();
  }
}
