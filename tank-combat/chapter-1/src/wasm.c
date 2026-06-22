/* wasm.c — the WebAssembly boundary. This is the ONLY wasm-specific file: it
 * holds the single static World (so JS can read pointers into it) and the
 * instance buffer, and exports the data protocol below. The simulation
 * (sim.c) and renderer (render.c) know nothing about wasm.
 *
 * ---------------------------------------------------------------------------
 * DATA PROTOCOL (JS <-> WASM)
 * ---------------------------------------------------------------------------
 * C owns every byte layout. JS never computes an offset; it asks C for the
 * pointer/count it needs. The exported surface is the protocol:
 *
 *   init()                          load level + place tanks
 *   tick()    -> draw count         advance one fixed step, refill dynamic quads
 *   set_input(tank, bits)           write a tank's input bitfield
 *
 * The instance buffer is split by frequency of change (see render.h):
 *   [0 .. dyn_count)             dynamic quads, rebuilt every tick
 *   [wall_base .. +wall_count)   static walls, rebuilt only on a grid change
 * So each frame the host re-uploads only the dynamic front, and re-uploads the
 * walls only when `walls_version` changes. inst_count is the total to DRAW.
 *
 *   pointers (into linear memory, read by JS each frame):
 *     inst_ptr  grid_ptr
 *     tank_xy_ptr (packed x|y<<16) tank_angle_ptr tank_input_ptr
 *     tank_vxy_ptr (packed vx|vy<<16) tank_hit_ptr
 *
 *   scalars: grid_w grid_h n_tanks subcell inst_stride frame
 *            inst_count (draw total)  inst_max
 *            dyn_count  wall_base  wall_count  walls_version
 *   tunables: move_speed/set_move_speed  turn_rate/set_turn_rate  collide_scale/set_collide_scale
 *   debug pokes: set_tank_pose  toggle_wall  set_show_samples
 * ---------------------------------------------------------------------------
 */

#include "sim.h"
#include "render.h"

#define EXPORT(name) __attribute__((export_name(#name)))

/* dynamic quads up front, then up to N_CELLS wall quads */
#define INST_MAX (INST_DYN_MAX + N_CELLS)
static World    g_world;
static Inst     g_inst[INST_MAX];
static uint32_t g_nwall;             /* wall quads in g_inst[INST_DYN_MAX..]      */
static uint32_t g_walls_ver;         /* bumped whenever the walls are (re)built   */
static uint32_t g_show_samples = 1;  /* debug overlay of sampled cells, on by default */

/* the walls change rarely: rebuild them (and bump the version JS watches) only
 * on init and grid edits, never per tick */
static void rebuild_walls(void) {
  g_nwall = build_walls(&g_world, &g_inst[INST_DYN_MAX]);
  g_walls_ver++;
}
/* the dynamic front is the only thing rewritten every tick */
static void rebuild_dynamic(void) {
  build_dynamic(&g_world, &g_inst[0], (int)g_show_samples);
}

EXPORT(init) void init(void) {
  sim_init(&g_world);
  rebuild_walls();
  rebuild_dynamic();
}

EXPORT(tick) uint32_t tick(void) {
  sim_tick(&g_world);
  rebuild_dynamic();                 /* walls untouched: they did not change */
  return INST_DYN_MAX + g_nwall;
}

EXPORT(set_input) void set_input(uint32_t tank, uint32_t bits) {
  if (tank < N_TANKS) g_world.tank_in[tank] = (uint8_t)bits;
}

/* pointers */
EXPORT(inst_ptr)       Inst*     inst_ptr(void)       { return g_inst; }
EXPORT(grid_ptr)       uint32_t* grid_ptr(void)       { return g_world.grid; }
EXPORT(tank_xy_ptr)    uint32_t* tank_xy_ptr(void)    { return g_world.tank_xy; }   /* x|y<<16 */
EXPORT(tank_angle_ptr) uint16_t* tank_angle_ptr(void) { return g_world.tank_ang; }
EXPORT(tank_input_ptr) uint8_t*  tank_input_ptr(void) { return g_world.tank_in; }
EXPORT(tank_vxy_ptr)   uint32_t* tank_vxy_ptr(void)   { return g_world.tank_vxy; }  /* vx|vy<<16 */
EXPORT(tank_hit_ptr)   uint8_t*  tank_hit_ptr(void)   { return g_world.tank_hit; }

/* scalars */
EXPORT(grid_w)      uint32_t grid_w(void)      { return GRID_W; }
EXPORT(grid_h)      uint32_t grid_h(void)      { return GRID_H; }
EXPORT(n_tanks)     uint32_t n_tanks(void)     { return N_TANKS; }
EXPORT(subcell)     uint32_t subcell(void)     { return SUB; }
EXPORT(inst_count)  uint32_t inst_count(void)  { return INST_DYN_MAX + g_nwall; }  /* total to draw */
EXPORT(inst_max)    uint32_t inst_max(void)    { return INST_MAX; }
EXPORT(inst_stride) uint32_t inst_stride(void) { return (uint32_t)sizeof(Inst); }
EXPORT(frame)       uint32_t frame(void)       { return g_world.frame; }

/* instance-buffer split, so the host uploads only what changed */
EXPORT(dyn_count)     uint32_t dyn_count(void)     { return INST_DYN_MAX; }  /* re-uploaded each frame */
EXPORT(wall_base)     uint32_t wall_base(void)     { return INST_DYN_MAX; }  /* walls start here       */
EXPORT(wall_count)    uint32_t wall_count(void)    { return g_nwall; }
EXPORT(walls_version) uint32_t walls_version(void) { return g_walls_ver; }   /* re-upload walls on change */

/* tunables */
EXPORT(move_speed)     uint32_t move_speed(void)       { return g_world.move_speed; }
EXPORT(set_move_speed) void set_move_speed(uint32_t v) { g_world.move_speed = (uint16_t)v; }
EXPORT(turn_rate)      uint32_t turn_rate(void)        { return g_world.turn_rate; }
EXPORT(set_turn_rate)  void set_turn_rate(uint32_t v)  { g_world.turn_rate = (uint16_t)v; }
EXPORT(collide_scale)     uint32_t collide_scale(void)       { return g_world.collide_scale; }
EXPORT(set_collide_scale) void set_collide_scale(uint32_t v) { g_world.collide_scale = (uint16_t)v; }

/* debug pokes */
EXPORT(set_show_samples) void set_show_samples(uint32_t on) { g_show_samples = on ? 1u : 0u; rebuild_dynamic(); }
EXPORT(set_tank_pose) void set_tank_pose(uint32_t t, int32_t x_sub, int32_t y_sub, uint32_t ang) {
  if (t >= N_TANKS) return;
  g_world.tank_xy[t] = xy_pack(wrap_x(x_sub), wrap_y(y_sub));
  g_world.tank_ang[t] = (uint16_t)ang;
  rebuild_dynamic();                 /* only a tank moved */
}
EXPORT(toggle_wall) void toggle_wall(uint32_t cx, uint32_t cy) {
  if (cx < GRID_W && cy < GRID_H) {
    g_world.grid[cy] ^= (1u << cx);
    /* no escape rebuild: the steer reads the cell's pattern live, and the
     * pattern->escape table is grid-independent. Walls changed, so rebuild them
     * (bumps walls_version → the host re-uploads the wall region); the overlay
     * can also shift, so refresh the dynamic front too. */
    rebuild_walls();
    rebuild_dynamic();
  }
}
