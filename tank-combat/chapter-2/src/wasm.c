/* wasm.c — the WebAssembly boundary. The ONLY wasm-specific file: it holds the
 * single static World, the instance buffer, and the camera/slide state, and
 * exports the data protocol. sim.c and render.c know nothing about wasm.
 *
 * DATA PROTOCOL (JS <-> WASM). C owns every byte layout; JS asks for the
 * pointer/count it needs. Key calls:
 *   init() / tick()->draw count
 *   set_input(tank, bits)         drive the MANUAL tank
 *   cycle_tank(tank)              cycle a tank UNSELECTED->AUTOPATH->MANUAL->...
 *   set_dest(tank, wcx, wcy)      give a tank a destination (world cell)
 *   deselect()                    drop selection (paths keep running)
 *   toggle_wall(wcx, wcy)         flip a wall, rebuild affected tables
 *   set_camera(sx,sy) / set_slide(to_sx,to_sy,dx,dy) / clear_slide()
 *                                 the host drives the follow camera + screen slide
 * Pointers expose every table for the live debug widgets. The whole view is
 * rebuilt each frame (build_view); inst_count is the quad count to draw. */

#include "sim.h"
#include "render.h"
#include "edge_paths.h"

#define EXPORT(name) __attribute__((export_name(#name)))

static World    g_world;
static Inst     g_inst[INST_MAX];
static uint32_t g_inst_count;
static uint8_t  g_cam_sx, g_cam_sy;
static uint8_t  g_slide_active, g_to_sx, g_to_sy;
static int8_t   g_dx, g_dy;

static void rebuild(void) {
  g_inst_count = build_view(&g_world, g_inst, g_cam_sx, g_cam_sy,
                            g_slide_active, g_to_sx, g_to_sy, g_dx, g_dy);
}

EXPORT(init) void init(void) {
  sim_init(&g_world);
  g_cam_sx = 0; g_cam_sy = 0; g_slide_active = 0;
  rebuild();
}
EXPORT(tick) uint32_t tick(void) { sim_tick(&g_world); rebuild(); return g_inst_count; }

EXPORT(set_input)   void set_input(uint32_t tank, uint32_t bits) { if (tank < N_TANKS) g_world.tank_in[tank] = (uint8_t)bits; }
EXPORT(cycle_tank)  void cycle_tank(uint32_t tank) { sim_cycle_tank(&g_world, tank); rebuild(); }
EXPORT(set_dest)    void set_dest(uint32_t tank, uint32_t wcx, uint32_t wcy) { sim_set_dest(&g_world, tank, wcx, wcy); rebuild(); }
EXPORT(toggle_wall) void toggle_wall(uint32_t wcx, uint32_t wcy) { sim_toggle_wall(&g_world, wcx, wcy); rebuild(); }

/* trace a tank's full route into g_pathbuf (world cells); for the minimap overview */
static uint16_t g_pathbuf[BIG_W * BIG_H];
EXPORT(path_ptr)   uint16_t* path_ptr(void)         { return g_pathbuf; }
EXPORT(trace_path) uint32_t  trace_path(uint32_t t) { return t < N_TANKS ? path_trace(&g_world, t, g_pathbuf, BIG_W * BIG_H) : 0; }

/* camera + slide (the host computes follow / picker; the wasm just builds it) */
EXPORT(set_camera) void set_camera(uint32_t sx, uint32_t sy) {
  g_cam_sx = (uint8_t)(sx % SCREENS_X); g_cam_sy = (uint8_t)(sy % SCREENS_Y);
  g_slide_active = 0; rebuild();
}
EXPORT(set_slide) void set_slide(uint32_t to_sx, uint32_t to_sy, int32_t dx, int32_t dy) {
  g_to_sx = (uint8_t)(to_sx % SCREENS_X); g_to_sy = (uint8_t)(to_sy % SCREENS_Y);
  g_dx = (int8_t)dx; g_dy = (int8_t)dy; g_slide_active = 1; rebuild();
}
EXPORT(clear_slide) void clear_slide(void) { g_slide_active = 0; rebuild(); }

EXPORT(set_tank_pose) void set_tank_pose(uint32_t t, int32_t x_sub, int32_t y_sub, uint32_t ang) {
  if (t >= N_TANKS) return;
  g_world.tank_xy[t] = xy_pack(wrap_x(x_sub), wrap_y(y_sub));
  g_world.tank_ang[t] = (uint16_t)ang; rebuild();
}

/* pointers */
EXPORT(inst_ptr)        Inst*     inst_ptr(void)        { return g_inst; }
EXPORT(grid_ptr)        uint32_t* grid_ptr(void)        { return g_world.grid; }
EXPORT(tank_xy_ptr)     uint32_t* tank_xy_ptr(void)     { return g_world.tank_xy; }
EXPORT(tank_angle_ptr)  uint16_t* tank_angle_ptr(void)  { return g_world.tank_ang; }
EXPORT(tank_input_ptr)  uint8_t*  tank_input_ptr(void)  { return g_world.tank_in; }
EXPORT(tank_vxy_ptr)    uint32_t* tank_vxy_ptr(void)    { return g_world.tank_vxy; }
EXPORT(tank_hit_ptr)    uint8_t*  tank_hit_ptr(void)    { return g_world.tank_hit; }
EXPORT(tstate_ptr)      uint8_t*  tstate_ptr(void)      { return g_world.tstate; }
EXPORT(pdest_screen_ptr)uint8_t*  pdest_screen_ptr(void){ return g_world.pdest_screen; }
EXPORT(pdest_cell_ptr)  uint16_t* pdest_cell_ptr(void)  { return g_world.pdest_cell; }
EXPORT(phas_ptr)        uint8_t*  phas_ptr(void)        { return g_world.phas; }
EXPORT(pgoal_ptr)       uint8_t*  pgoal_ptr(void)       { return g_world.pgoal; }
EXPORT(pstatus_ptr)     uint8_t*  pstatus_ptr(void)     { return g_world.pstatus; }
EXPORT(pg_ptr)          uint16_t* pg_ptr(void)          { return g_world.pg; }
EXPORT(l1dist_ptr)      uint8_t*  l1dist_ptr(void)      { return g_world.l1dist; }
EXPORT(l1_screen_max_ptr) uint16_t* l1_screen_max_ptr(void) { return g_world.l1_screen_max; }
EXPORT(ep_screen_ptr)   uint8_t*  ep_screen_ptr(void)   { return g_world.ep_screen; }
EXPORT(ep_cell_ptr)     uint16_t* ep_cell_ptr(void)     { return g_world.ep_cell; }
EXPORT(ep_cross_ptr)    uint8_t*  ep_cross_ptr(void)    { return g_world.ep_cross; }
EXPORT(ep_partner_ptr)  uint8_t*  ep_partner_ptr(void)  { return g_world.ep_partner; }
EXPORT(screen_ep_count_ptr) uint8_t* screen_ep_count_ptr(void) { return g_world.screen_ep_count; }
EXPORT(screen_ep_ptr)   uint8_t*  screen_ep_ptr(void)   { return g_world.screen_ep; }
EXPORT(dist2_ptr)       uint16_t* dist2_ptr(void)       { return g_world.dist2; }
EXPORT(nexthop_ptr)     uint8_t*  nexthop_ptr(void)     { return g_world.nexthop; }

/* scalars */
EXPORT(grid_w)      uint32_t grid_w(void)      { return GRID_W; }
EXPORT(grid_h)      uint32_t grid_h(void)      { return GRID_H; }
EXPORT(n_cells)     uint32_t n_cells(void)     { return N_CELLS; }
EXPORT(screens_x)   uint32_t screens_x(void)   { return SCREENS_X; }
EXPORT(screens_y)   uint32_t screens_y(void)   { return SCREENS_Y; }
EXPORT(n_screens)   uint32_t n_screens(void)   { return N_SCREENS; }
EXPORT(big_w)       uint32_t big_w(void)       { return BIG_W; }
EXPORT(big_h)       uint32_t big_h(void)       { return BIG_H; }
EXPORT(n_tanks)     uint32_t n_tanks(void)     { return N_TANKS; }
EXPORT(subcell)     uint32_t subcell(void)     { return SUB; }
EXPORT(tri)         uint32_t tri(void)         { return TRI; }
EXPORT(n_edge_max)  uint32_t n_edge_max(void)  { return N_EDGE_MAX; }
EXPORT(ep_per_screen_max) uint32_t ep_per_screen_max(void) { return EP_PER_SCREEN_MAX; }
EXPORT(ep_count)    uint32_t ep_count(void)    { return g_world.ep_count; }
EXPORT(l1_maxdist)  uint32_t l1_maxdist(void)  { return g_world.l1_maxdist; }
EXPORT(l1_dist_ceil)uint32_t l1_dist_ceil(void){ return L1_DIST_CEIL; }
EXPORT(selected)    uint32_t selected(void)    { return g_world.selected; }
EXPORT(cam_sx)      uint32_t cam_sx(void)      { return g_cam_sx; }
EXPORT(cam_sy)      uint32_t cam_sy(void)      { return g_cam_sy; }
EXPORT(inst_count)  uint32_t inst_count(void)  { return g_inst_count; }
EXPORT(inst_max)    uint32_t inst_max(void)    { return INST_MAX; }
EXPORT(inst_stride) uint32_t inst_stride(void) { return (uint32_t)sizeof(Inst); }
EXPORT(frame)       uint32_t frame(void)       { return g_world.frame; }

/* tunables */
EXPORT(move_speed)        uint32_t move_speed(void)        { return g_world.move_speed; }
EXPORT(set_move_speed)    void set_move_speed(uint32_t v)  { g_world.move_speed = (uint16_t)v; }
EXPORT(turn_rate)         uint32_t turn_rate(void)         { return g_world.turn_rate; }
EXPORT(set_turn_rate)     void set_turn_rate(uint32_t v)   { g_world.turn_rate = (uint16_t)v; }
EXPORT(collide_scale)     uint32_t collide_scale(void)     { return g_world.collide_scale; }
EXPORT(set_collide_scale) void set_collide_scale(uint32_t v) { g_world.collide_scale = (uint16_t)v; }
