/* tank-combat — step 1: two tanks, grid obstacles, collision.
 *
 * Goal of the project: demonstrate a data-oriented approach to building a
 * game. This file is the whole simulation. It holds ONLY data and the
 * transforms over that data. No allocation, no hidden state, no objects.
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
 *   tick(dt)  -> inst_count         advance one frame, refill instance buffer
 *   set_input(tank, bits)           write a tank's input bitfield
 *
 *   pointers (into linear memory, read by JS each frame):
 *     inst_ptr  grid_ptr
 *     tank_x_ptr tank_y_ptr tank_angle_ptr tank_input_ptr tank_vx_ptr tank_vy_ptr
 *
 *   scalars: grid_w grid_h n_tanks n_dirs cell_size arena_w arena_h
 *            inst_count inst_stride frame
 *
 *   tunables (read + edited live from the debug panel):
 *     move_speed/set_move_speed   turn_rate/set_turn_rate
 *
 *   debug pokes (the panel mutating the data directly):
 *     set_tank_pose(tank,x,y,angle)   set_wall(cx,cy,v)   toggle_wall(cx,cy)
 *
 * INSTANCE LAYOUT: 10 floats, stride 40 bytes:
 *   [0]cx [1]cy [2]hx [3]hy [4]cos [5]sin [6]r [7]g [8]b [9]a   (world units)
 * ---------------------------------------------------------------------------
 */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef signed   int   i32;
typedef float          f32;

#define EXPORT(name) __attribute__((export_name(#name)))

/* --- fixed sizes (constraints we exploit, not parameters we expose) ------ */
#define GRID_W      20
#define GRID_H      15
#define N_CELLS     (GRID_W * GRID_H)
#define N_TANKS     2
#define N_DIRS      32          /* heading is discrete: angle>>11 -> 0..31  */
#define ANGLE_SHIFT 11          /* 65536 >> 11 == 32                        */
#define CELL        1.0f        /* world units per grid cell                */
#define ARENA_W     (GRID_W * CELL)
#define ARENA_H     (GRID_H * CELL)
#define TANK_R      0.36f       /* collision half-extent (square)           */
#define INST_FLOATS 10
#define MAX_INST    (N_CELLS + N_TANKS * 2)  /* every cell a wall + tank parts */

/* input bits */
#define IN_FWD   1u
#define IN_BACK  2u
#define IN_LEFT  4u
#define IN_RIGHT 8u
#define IN_FIRE  16u            /* reserved for a later step; carried, unused */

/* baked trig table: cos/sin of the 32 discrete headings. No runtime trig. */
static const float DIR_COS[32] = {
  +1.00000000f,+0.98078528f,+0.92387953f,+0.83146961f,+0.70710678f,+0.55557023f,+0.38268343f,+0.19509032f,+0.00000000f,-0.19509032f,-0.38268343f,-0.55557023f,-0.70710678f,-0.83146961f,-0.92387953f,-0.98078528f,-1.00000000f,-0.98078528f,-0.92387953f,-0.83146961f,-0.70710678f,-0.55557023f,-0.38268343f,-0.19509032f,-0.00000000f,+0.19509032f,+0.38268343f,+0.55557023f,+0.70710678f,+0.83146961f,+0.92387953f,+0.98078528f
};
static const float DIR_SIN[32] = {
  +0.00000000f,+0.19509032f,+0.38268343f,+0.55557023f,+0.70710678f,+0.83146961f,+0.92387953f,+0.98078528f,+1.00000000f,+0.98078528f,+0.92387953f,+0.83146961f,+0.70710678f,+0.55557023f,+0.38268343f,+0.19509032f,+0.00000000f,-0.19509032f,-0.38268343f,-0.55557023f,-0.70710678f,-0.83146961f,-0.92387953f,-0.98078528f,-1.00000000f,-0.98078528f,-0.92387953f,-0.83146961f,-0.70710678f,-0.55557023f,-0.38268343f,-0.19509032f
};

/* tank colors, indexed by tank id (render data, not sim data) */
static const float TANK_RGB[N_TANKS][3] = {
  { 0.95f, 0.62f, 0.16f },   /* tank 0: amber  */
  { 0.30f, 0.70f, 0.90f },   /* tank 1: cyan   */
};

/* --- the world: all simulation state, flat and static -------------------- */
/* tanks held as structure-of-arrays so each transform streams one field.   */
static f32 g_tank_x   [N_TANKS];
static f32 g_tank_y   [N_TANKS];
static u16 g_tank_ang [N_TANKS];
static u8  g_tank_in  [N_TANKS];
static f32 g_tank_vx  [N_TANKS];   /* last frame's applied move (debug)     */
static f32 g_tank_vy  [N_TANKS];

static u8  g_grid [N_CELLS];       /* 0 empty, 1 wall                       */

static f32 g_inst [MAX_INST * INST_FLOATS];
static u32 g_inst_count;

static u32 g_frame;
static f32 g_move_speed = 4.0f;    /* world units / second                  */
static u32 g_turn_rate  = 24000;   /* angle units / second (65536 == full)  */

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

/* is the tank's square at (px,py) blocked by a wall or the arena edge?     */
/* out-of-range policy: edge counts as blocked (reject), so the tank stays  */
/* inside [TANK_R, ARENA-TANK_R] and grid indices are always valid.         */
static i32 blocked(const u8* grid, f32 px, f32 py) {
  if (px < TANK_R || py < TANK_R ||
      px > ARENA_W - TANK_R || py > ARENA_H - TANK_R) return 1;
  i32 c0 = (i32)(px - TANK_R), c1 = (i32)(px + TANK_R);   /* CELL == 1      */
  i32 r0 = (i32)(py - TANK_R), r1 = (i32)(py + TANK_R);
  for (i32 r = r0; r <= r1; r++)
    for (i32 c = c0; c <= c1; c++)
      if (grid[r * GRID_W + c]) return 1;
  return 0;
}

/* move each tank along its heading by throttle, resolving against the grid */
/* one axis at a time so a tank slides along a wall it hits at an angle.    */
static void tanks_move(f32* x, f32* y, f32* vx, f32* vy,
                       const u16* ang, const u8* in, u32 n,
                       f32 dist, const u8* grid) {
  for (u32 i = 0; i < n; i++) {
    f32 thr = (f32)((in[i] & IN_FWD) != 0) - (f32)((in[i] & IN_BACK) != 0);
    u32 di  = ang[i] >> ANGLE_SHIFT;
    f32 mx  = DIR_COS[di] * dist * thr;
    f32 my  = DIR_SIN[di] * dist * thr;
    f32 nx  = x[i] + mx;
    if (blocked(grid, nx, y[i])) mx = 0.0f; else x[i] = nx;
    f32 ny  = y[i] + my;
    if (blocked(grid, x[i], ny)) my = 0.0f; else y[i] = ny;
    vx[i] = mx; vy[i] = my;
  }
}

/* emit one quad instance */
static u32 push_quad(u32 k, f32 cx, f32 cy, f32 hx, f32 hy,
                     f32 co, f32 si, f32 r, f32 g, f32 b, f32 a) {
  f32* o = &g_inst[k * INST_FLOATS];
  o[0]=cx; o[1]=cy; o[2]=hx; o[3]=hy; o[4]=co; o[5]=si;
  o[6]=r;  o[7]=g;  o[8]=b;  o[9]=a;
  return k + 1;
}

/* transform world state -> flat instance buffer for the renderer.          */
static u32 build_instances(void) {
  u32 k = 0;
  for (i32 r = 0; r < GRID_H; r++)
    for (i32 c = 0; c < GRID_W; c++)
      if (g_grid[r * GRID_W + c])
        k = push_quad(k, (f32)c + 0.5f, (f32)r + 0.5f, 0.48f, 0.48f,
                      1.0f, 0.0f, 0.32f, 0.34f, 0.40f, 1.0f);
  for (u32 i = 0; i < N_TANKS; i++) {
    u32 di = g_tank_ang[i] >> ANGLE_SHIFT;
    f32 co = DIR_COS[di], si = DIR_SIN[di];
    f32 cx = g_tank_x[i], cy = g_tank_y[i];
    const float* rgb = TANK_RGB[i];
    k = push_quad(k, cx, cy, 0.34f, 0.26f, co, si, rgb[0], rgb[1], rgb[2], 1.0f);
    /* barrel: a short bar offset forward along the heading */
    k = push_quad(k, cx + co * 0.34f, cy + si * 0.34f, 0.22f, 0.07f,
                  co, si, rgb[0]*0.6f, rgb[1]*0.6f, rgb[2]*0.6f, 1.0f);
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
  g_tank_x[0] = 2.5f;  g_tank_y[0] = 7.5f;  g_tank_ang[0] = 0;        /* east */
  g_tank_x[1] = 17.5f; g_tank_y[1] = 7.5f;  g_tank_ang[1] = 0x8000;   /* west */
  for (u32 i = 0; i < N_TANKS; i++) {
    g_tank_in[i] = 0; g_tank_vx[i] = 0.0f; g_tank_vy[i] = 0.0f;
  }
  g_frame = 0;
  g_inst_count = build_instances();
}

EXPORT(tick) u32 tick(f32 dt) {
  u16 step = (u16)((f32)g_turn_rate * dt);
  f32 dist = g_move_speed * dt;
  tanks_turn(g_tank_ang, g_tank_in, N_TANKS, step);
  tanks_move(g_tank_x, g_tank_y, g_tank_vx, g_tank_vy,
             g_tank_ang, g_tank_in, N_TANKS, dist, g_grid);
  g_inst_count = build_instances();
  g_frame++;
  return g_inst_count;
}

EXPORT(set_input) void set_input(u32 tank, u32 bits) {
  if (tank < N_TANKS) g_tank_in[tank] = (u8)bits;
}

/* pointers */
EXPORT(inst_ptr)       f32* inst_ptr(void)       { return g_inst; }
EXPORT(grid_ptr)       u8*  grid_ptr(void)       { return g_grid; }
EXPORT(tank_x_ptr)     f32* tank_x_ptr(void)     { return g_tank_x; }
EXPORT(tank_y_ptr)     f32* tank_y_ptr(void)     { return g_tank_y; }
EXPORT(tank_angle_ptr) u16* tank_angle_ptr(void) { return g_tank_ang; }
EXPORT(tank_input_ptr) u8*  tank_input_ptr(void) { return g_tank_in; }
EXPORT(tank_vx_ptr)    f32* tank_vx_ptr(void)    { return g_tank_vx; }
EXPORT(tank_vy_ptr)    f32* tank_vy_ptr(void)    { return g_tank_vy; }

/* scalars */
EXPORT(grid_w)      u32 grid_w(void)      { return GRID_W; }
EXPORT(grid_h)      u32 grid_h(void)      { return GRID_H; }
EXPORT(n_tanks)     u32 n_tanks(void)     { return N_TANKS; }
EXPORT(n_dirs)      u32 n_dirs(void)      { return N_DIRS; }
EXPORT(cell_size)   f32 cell_size(void)   { return CELL; }
EXPORT(arena_w)     f32 arena_w(void)     { return ARENA_W; }
EXPORT(arena_h)     f32 arena_h(void)     { return ARENA_H; }
EXPORT(inst_count)  u32 inst_count(void)  { return g_inst_count; }
EXPORT(inst_stride) u32 inst_stride(void) { return INST_FLOATS; }
EXPORT(frame)       u32 frame(void)       { return g_frame; }

/* tunables */
EXPORT(move_speed)     f32 move_speed(void)        { return g_move_speed; }
EXPORT(set_move_speed) void set_move_speed(f32 v)  { g_move_speed = v; }
EXPORT(turn_rate)      u32 turn_rate(void)         { return g_turn_rate; }
EXPORT(set_turn_rate)  void set_turn_rate(u32 v)   { g_turn_rate = v; }

/* debug pokes */
EXPORT(set_tank_pose) void set_tank_pose(u32 t, f32 x, f32 y, u32 ang) {
  if (t >= N_TANKS) return;
  g_tank_x[t] = x; g_tank_y[t] = y; g_tank_ang[t] = (u16)ang;
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
