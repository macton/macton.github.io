/* wasm.c — the WebAssembly boundary. This is the ONLY wasm-specific file: it
 * holds the single static World (so JS can read pointers into it) and the
 * instance buffer, and exports the data protocol below. The simulation (sim.c)
 * and renderer (render.c) know nothing about wasm.
 *
 * ---------------------------------------------------------------------------
 * DATA PROTOCOL (JS <-> WASM)
 * ---------------------------------------------------------------------------
 * C owns every byte layout. JS never computes an offset; it asks C for the
 * pointer/count it needs. The exported surface is the protocol:
 *
 *   init()                       load level + place tanks + build path tables
 *   tick() -> draw count         advance one fixed step, refill dynamic quads
 *   set_input(tank, bits)        write a tank's input bitfield (manual tank)
 *   scroll(dx,dy)                move the viewport by whole screens (toroidal)
 *   set_selected(tank)           choose the pathed tank to highlight / target
 *   set_dest(tank, wcx, wcy)     set a pathed tank's destination (world cell)
 *   clear_dest(tank)             clear it
 *   toggle_wall(wcx, wcy)        flip a wall (world cell), rebuild affected tables
 *
 * The instance buffer is split by frequency of change (see render.h):
 *   [0 .. dyn_count)             dynamic quads, rebuilt every tick
 *   [wall_base .. +wall_count)   the camera screen's walls, rebuilt on camera
 *                                move or grid edit (watch walls_version)
 *
 * Pointers expose every table for the live debug widgets (Level-1 distance,
 * edge points, the next-hop matrix, per-tank goal/status). See the export list.
 * ---------------------------------------------------------------------------
 */

#include "sim.h"
#include "render.h"
#include "edge_paths.h"

#define EXPORT(name) __attribute__((export_name(#name)))

#define INST_MAX (INST_DYN_MAX + N_CELLS)
static World    g_world;
static Inst     g_inst[INST_MAX];
static uint32_t g_nwall;
static uint32_t g_walls_ver;
static uint32_t g_show_path = 1;

static void rebuild_walls(void) {
  g_nwall = build_walls(&g_world, &g_inst[INST_DYN_MAX]);
  g_walls_ver++;
}
static void rebuild_dynamic(void) {
  build_dynamic(&g_world, &g_inst[0], (int)g_show_path);
}

EXPORT(init) void init(void) {
  sim_init(&g_world);
  rebuild_walls();
  rebuild_dynamic();
}

EXPORT(tick) uint32_t tick(void) {
  sim_tick(&g_world);
  rebuild_dynamic();                 /* walls untouched: camera/grid did not change */
  return INST_DYN_MAX + g_nwall;
}

EXPORT(set_input) void set_input(uint32_t tank, uint32_t bits) {
  if (tank < N_TANKS) g_world.tank_in[tank] = (uint8_t)bits;
}

/* viewport + selection + destination (rebuild what each affects) */
EXPORT(scroll) void scroll(int32_t dx, int32_t dy) { sim_scroll(&g_world, dx, dy); rebuild_walls(); rebuild_dynamic(); }
EXPORT(set_selected) void set_selected(uint32_t tank) { sim_set_selected(&g_world, tank); rebuild_dynamic(); }
EXPORT(set_dest) void set_dest(uint32_t tank, uint32_t wcx, uint32_t wcy) { sim_set_dest(&g_world, tank, wcx, wcy); rebuild_dynamic(); }
EXPORT(clear_dest) void clear_dest(uint32_t tank) { sim_clear_dest(&g_world, tank); rebuild_dynamic(); }
EXPORT(toggle_wall) void toggle_wall(uint32_t wcx, uint32_t wcy) { sim_toggle_wall(&g_world, wcx, wcy); rebuild_walls(); rebuild_dynamic(); }

/* pointers into linear memory (read by JS) */
EXPORT(inst_ptr)        Inst*     inst_ptr(void)        { return g_inst; }
EXPORT(grid_ptr)        uint32_t* grid_ptr(void)        { return g_world.grid; }          /* N_SCREENS*GRID_H words */
EXPORT(tank_xy_ptr)     uint32_t* tank_xy_ptr(void)     { return g_world.tank_xy; }
EXPORT(tank_angle_ptr)  uint16_t* tank_angle_ptr(void)  { return g_world.tank_ang; }
EXPORT(tank_input_ptr)  uint8_t*  tank_input_ptr(void)  { return g_world.tank_in; }
EXPORT(tank_vxy_ptr)    uint32_t* tank_vxy_ptr(void)    { return g_world.tank_vxy; }
EXPORT(tank_hit_ptr)    uint8_t*  tank_hit_ptr(void)    { return g_world.tank_hit; }
EXPORT(l1dist_ptr)      uint16_t* l1dist_ptr(void)      { return g_world.l1dist; }         /* N_SCREENS*TRI */
EXPORT(ep_screen_ptr)   uint8_t*  ep_screen_ptr(void)   { return g_world.ep_screen; }
EXPORT(ep_cell_ptr)     uint16_t* ep_cell_ptr(void)     { return g_world.ep_cell; }
EXPORT(ep_cross_ptr)    uint8_t*  ep_cross_ptr(void)    { return g_world.ep_cross; }
EXPORT(ep_partner_ptr)  uint8_t*  ep_partner_ptr(void)  { return g_world.ep_partner; }
EXPORT(screen_ep_count_ptr) uint8_t* screen_ep_count_ptr(void) { return g_world.screen_ep_count; }
EXPORT(screen_ep_ptr)   uint8_t*  screen_ep_ptr(void)   { return g_world.screen_ep; }      /* N_SCREENS*EP_PER_SCREEN_MAX */
EXPORT(dist2_ptr)       uint16_t* dist2_ptr(void)       { return g_world.dist2; }          /* N_EDGE_MAX^2 */
EXPORT(nexthop_ptr)     uint8_t*  nexthop_ptr(void)     { return g_world.nexthop; }        /* N_EDGE_MAX^2 */
EXPORT(pg_ptr)          uint16_t* pg_ptr(void)          { return g_world.pg; }             /* N_PATHED*N_EDGE_MAX */
EXPORT(pdest_screen_ptr) uint8_t* pdest_screen_ptr(void){ return g_world.pdest_screen; }
EXPORT(pdest_cell_ptr)  uint16_t* pdest_cell_ptr(void)  { return g_world.pdest_cell; }
EXPORT(phas_ptr)        uint8_t*  phas_ptr(void)        { return g_world.phas; }
EXPORT(pgoal_ptr)       uint8_t*  pgoal_ptr(void)       { return g_world.pgoal; }
EXPORT(pstatus_ptr)     uint8_t*  pstatus_ptr(void)     { return g_world.pstatus; }

/* scalars: fixed sizes + live counts */
EXPORT(grid_w)      uint32_t grid_w(void)      { return GRID_W; }
EXPORT(grid_h)      uint32_t grid_h(void)      { return GRID_H; }
EXPORT(n_cells)     uint32_t n_cells(void)     { return N_CELLS; }
EXPORT(screens_x)   uint32_t screens_x(void)   { return SCREENS_X; }
EXPORT(screens_y)   uint32_t screens_y(void)   { return SCREENS_Y; }
EXPORT(n_screens)   uint32_t n_screens(void)   { return N_SCREENS; }
EXPORT(big_w)       uint32_t big_w(void)       { return BIG_W; }
EXPORT(big_h)       uint32_t big_h(void)       { return BIG_H; }
EXPORT(n_tanks)     uint32_t n_tanks(void)     { return N_TANKS; }
EXPORT(n_pathed)    uint32_t n_pathed(void)    { return N_PATHED; }
EXPORT(subcell)     uint32_t subcell(void)     { return SUB; }
EXPORT(tri)         uint32_t tri(void)         { return TRI; }
EXPORT(n_edge_max)  uint32_t n_edge_max(void)  { return N_EDGE_MAX; }
EXPORT(ep_per_screen_max) uint32_t ep_per_screen_max(void) { return EP_PER_SCREEN_MAX; }
EXPORT(ep_count)    uint32_t ep_count(void)    { return g_world.ep_count; }
EXPORT(cam_sx)      uint32_t cam_sx(void)      { return g_world.cam_sx; }
EXPORT(cam_sy)      uint32_t cam_sy(void)      { return g_world.cam_sy; }
EXPORT(selected)    uint32_t selected(void)    { return g_world.selected; }
EXPORT(inst_count)  uint32_t inst_count(void)  { return INST_DYN_MAX + g_nwall; }
EXPORT(inst_max)    uint32_t inst_max(void)    { return INST_MAX; }
EXPORT(inst_stride) uint32_t inst_stride(void) { return (uint32_t)sizeof(Inst); }
EXPORT(frame)       uint32_t frame(void)       { return g_world.frame; }

/* instance-buffer split */
EXPORT(dyn_count)     uint32_t dyn_count(void)     { return INST_DYN_MAX; }
EXPORT(wall_base)     uint32_t wall_base(void)     { return INST_DYN_MAX; }
EXPORT(wall_count)    uint32_t wall_count(void)    { return g_nwall; }
EXPORT(walls_version) uint32_t walls_version(void) { return g_walls_ver; }

/* tunables */
EXPORT(move_speed)        uint32_t move_speed(void)        { return g_world.move_speed; }
EXPORT(set_move_speed)    void set_move_speed(uint32_t v)  { g_world.move_speed = (uint16_t)v; }
EXPORT(turn_rate)         uint32_t turn_rate(void)         { return g_world.turn_rate; }
EXPORT(set_turn_rate)     void set_turn_rate(uint32_t v)   { g_world.turn_rate = (uint16_t)v; }
EXPORT(collide_scale)     uint32_t collide_scale(void)     { return g_world.collide_scale; }
EXPORT(set_collide_scale) void set_collide_scale(uint32_t v) { g_world.collide_scale = (uint16_t)v; }

/* debug pokes */
EXPORT(set_show_path) void set_show_path(uint32_t on) { g_show_path = on ? 1u : 0u; rebuild_dynamic(); }
EXPORT(set_tank_pose) void set_tank_pose(uint32_t t, int32_t x_sub, int32_t y_sub, uint32_t ang) {
  if (t >= N_TANKS) return;
  g_world.tank_xy[t] = xy_pack(wrap_x(x_sub), wrap_y(y_sub));
  g_world.tank_ang[t] = (uint16_t)ang;
  rebuild_dynamic();
}
