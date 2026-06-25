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
 *   toggle_wall(wcx, wcy)         flip a wall, rebuild affected tables
 *   set_camera(sx,sy) / set_slide(to_sx,to_sy,dx,dy) / clear_slide()
 *                                 the host drives the follow camera + screen slide
 * Pointers expose every table for the live debug widgets. The whole view is
 * rebuilt each frame (build_view); inst_count is the instance count to draw.
 *
 * CHAPTER 4 adds RENDER exports only — the draw list (per-kind counts), the iso
 * basis (so the shader reads one source of truth), and the baked meshes — leaving
 * every simulation export byte-identical to chapter 3. */

#include "sim.h"
#include "render.h"
#include "iso.h"
#include "mesh_data.h"

#define EXPORT(name) __attribute__((export_name(#name)))

static World    g_world;
static Inst     g_inst[INST_MAX];
static DrawList g_dl;
static uint32_t g_inst_count;
static uint8_t  g_cam_sx, g_cam_sy;
static uint8_t  g_slide_active, g_to_sx, g_to_sy;
static int8_t   g_dx, g_dy;

static void rebuild(void) {
  g_inst_count = build_view(&g_world, g_inst, &g_dl, g_cam_sx, g_cam_sy,
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

/* ---- the swarm: re-seed / nest mutators, live pointers, scalars, tunables -- */
EXPORT(set_seed) void set_seed(uint32_t seed) { sim_set_seed(&g_world, seed); rebuild(); }
EXPORT(set_nest) void set_nest(uint32_t n, uint32_t wcx, uint32_t wcy) { sim_set_nest(&g_world, n, wcx, wcy); rebuild(); }

EXPORT(mite_xy_ptr)   uint32_t* mite_xy_ptr(void)   { return g_world.mite_xy; }
EXPORT(mite_angle_ptr) uint16_t* mite_angle_ptr(void) { return g_world.mite_ang; }
EXPORT(mite_mode_ptr) uint8_t*  mite_mode_ptr(void) { return g_world.mite_mode; }
EXPORT(mite_dest_ptr) uint16_t* mite_dest_ptr(void) { return g_world.mite_dest; }   /* per-mite destination cell */
EXPORT(mite_resp_ptr) uint16_t* mite_resp_ptr(void) { return g_world.mite_resp; }   /* per-mite respawn timer (0 = alive) */
EXPORT(mite_cnt_ptr)  uint8_t*  mite_cnt_ptr(void)  { return g_world.mite_cnt; }   /* per-cell occupancy (heatmap) */
EXPORT(nest_cell_ptr) uint16_t* nest_cell_ptr(void) { return g_world.nest_cell; }  /* the NEST_COUNT home cells */
/* the record arrays are double-buffered; expose the CURRENT (post-tick) buffer so
 * the belief-field widget reads each mite's live last-known cell + timestamp. */
EXPORT(mite_rec_cell_ptr) uint16_t* mite_rec_cell_ptr(void) { return g_world.mite_rec_cell[g_world.rec_buf]; }
EXPORT(mite_rec_time_ptr) uint32_t* mite_rec_time_ptr(void) { return g_world.mite_rec_time[g_world.rec_buf]; }

EXPORT(n_mites)       uint32_t n_mites(void)       { return N_MITES; }
EXPORT(mite_r)        uint32_t mite_r(void)        { return MITE_R; }
EXPORT(mite_cap_max)  uint32_t mite_cap_max(void)  { return MITE_CAP; }
EXPORT(n_world_cells) uint32_t n_world_cells(void) { return N_WORLD_CELLS; }
EXPORT(mite_empty)    uint32_t mite_empty(void)    { return REC_EMPTY; }
EXPORT(nest_count)    uint32_t nest_count(void)    { return NEST_COUNT; }
EXPORT(n_fields)      uint32_t n_fields(void)      { return N_FIELDS; }
EXPORT(field_active)  uint32_t field_active(void)  { return g_world.field_active; }  /* distinct active destinations */
EXPORT(field_peak)    uint32_t field_peak(void)    { return g_world.field_peak; }    /* high-water mark */

EXPORT(mite_speed)     uint32_t mite_speed(void)       { return g_world.mite_speed; }
EXPORT(set_mite_speed) void     set_mite_speed(uint32_t v) { g_world.mite_speed = (uint16_t)v; }
EXPORT(mite_turn)      uint32_t mite_turn(void)        { return g_world.mite_turn; }
EXPORT(set_mite_turn)  void     set_mite_turn(uint32_t v)  { g_world.mite_turn = (uint16_t)v; }
EXPORT(turret_rate)     uint32_t turret_rate(void)        { return g_world.turret_rate; }
EXPORT(set_turret_rate) void     set_turret_rate(uint32_t v) { g_world.turret_rate = (uint16_t)v; }
EXPORT(mite_sense)     uint32_t mite_sense(void)       { return g_world.mite_sense; }
EXPORT(set_mite_sense) void     set_mite_sense(uint32_t v) { g_world.mite_sense = (uint8_t)(v > BIG_W ? BIG_W : v); }
EXPORT(mite_cap)       uint32_t mite_cap(void)         { return g_world.mite_cap; }
EXPORT(set_mite_cap)   void     set_mite_cap(uint32_t v)   { g_world.mite_cap = (uint8_t)(v < 1 ? 1 : (v > MITE_CAP ? MITE_CAP : v)); }
EXPORT(mite_phunt)     uint32_t mite_phunt(void)       { return g_world.mite_phunt; }
EXPORT(set_mite_phunt) void     set_mite_phunt(uint32_t v) { g_world.mite_phunt = (uint8_t)(v > 100 ? 100 : v); }
EXPORT(mite_seed)      uint32_t mite_seed(void)        { return g_world.mite_seed; }
/* combat tunables: shot interval in ticks (0 = firing off) and respawn delay in ticks */
EXPORT(fire_period)      uint32_t fire_period(void)        { return g_world.fire_period; }
EXPORT(set_fire_period)  void     set_fire_period(uint32_t v)  { g_world.fire_period = (uint16_t)(v > 6000 ? 6000 : v); }
EXPORT(mite_respawn)     uint32_t mite_respawn(void)       { return g_world.mite_respawn; }
EXPORT(set_mite_respawn) void     set_mite_respawn(uint32_t v) { g_world.mite_respawn = (uint16_t)(v < 1 ? 1 : (v > 60000 ? 60000 : v)); }

/* pointers — only the World fields the page reads (the chapter-2 path tables stay
 * simulation-internal in chapter 3, so they are no longer exported) */
EXPORT(inst_ptr)        Inst*     inst_ptr(void)        { return g_inst; }
EXPORT(grid_ptr)        uint32_t* grid_ptr(void)        { return g_world.grid; }
EXPORT(tank_xy_ptr)     uint32_t* tank_xy_ptr(void)     { return g_world.tank_xy; }
EXPORT(tank_angle_ptr)  uint16_t* tank_angle_ptr(void)  { return g_world.tank_ang; }
EXPORT(tstate_ptr)      uint8_t*  tstate_ptr(void)      { return g_world.tstate; }
EXPORT(pdest_screen_ptr)uint8_t*  pdest_screen_ptr(void){ return g_world.pdest_screen; }
EXPORT(pdest_cell_ptr)  uint16_t* pdest_cell_ptr(void)  { return g_world.pdest_cell; }
EXPORT(phas_ptr)        uint8_t*  phas_ptr(void)        { return g_world.phas; }
EXPORT(pstatus_ptr)     uint8_t*  pstatus_ptr(void)     { return g_world.pstatus; }

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
EXPORT(selected)    uint32_t selected(void)    { return g_world.selected; }
EXPORT(cam_sx)      uint32_t cam_sx(void)      { return g_cam_sx; }
EXPORT(cam_sy)      uint32_t cam_sy(void)      { return g_cam_sy; }
EXPORT(inst_count)  uint32_t inst_count(void)  { return g_inst_count; }
EXPORT(inst_max)    uint32_t inst_max(void)    { return INST_MAX; }
EXPORT(inst_stride) uint32_t inst_stride(void) { return (uint32_t)sizeof(Inst); }
EXPORT(frame)       uint32_t frame(void)       { return g_world.frame; }

/* ---- chapter-4 RENDER exports (all render-side; sim exports above unchanged) ---
 * the draw list: instances are grouped by kind, so the host draws one range per
 * opaque kind (binding that kind's mesh) then one sorted translucent range. */
EXPORT(k_opaque_count)    uint32_t k_opaque_count(void)            { return K_OPAQUE_COUNT; }
EXPORT(draw_opaque)       uint32_t draw_opaque(uint32_t kind)      { return kind < K_OPAQUE_COUNT ? g_dl.opaque[kind] : 0; }
EXPORT(draw_translucent)  uint32_t draw_translucent(void)          { return g_dl.translucent; }

/* the dimetric iso basis + depth range — the projection's source of truth, fed to
 * the shader's view uniform so the host and the GPU agree on one transform. */
EXPORT(iso_x)          uint32_t iso_x(void)          { return ISO_X; }
EXPORT(iso_y)          uint32_t iso_y(void)          { return ISO_Y; }
EXPORT(iso_z)          uint32_t iso_z(void)          { return ISO_Z; }
EXPORT(iso_depth_span) uint32_t iso_depth_span(void) { return ISO_DEPTH_SPAN; }

/* the baked low-poly meshes (loaded once at startup, instanced every frame) */
EXPORT(mesh_data_ptr)   const int8_t* mesh_data_ptr(void)      { return MESH_VERT; }
EXPORT(mesh_vert_total) uint32_t mesh_vert_total(void)         { return MESH_VERT_TOTAL; }
EXPORT(mesh_vstride)    uint32_t mesh_vstride(void)            { return MESH_VSTRIDE; }
EXPORT(mesh_count)      uint32_t mesh_count(void)              { return M_COUNT; }
EXPORT(mesh_cube)       uint32_t mesh_cube(void)               { return M_CUBE; }
EXPORT(mesh_voff)       uint32_t mesh_voff(uint32_t m)         { return m < M_COUNT ? MESH_VOFF[m] : 0; }
EXPORT(mesh_vcnt)       uint32_t mesh_vcnt(uint32_t m)         { return m < M_COUNT ? MESH_VCNT[m] : 0; }
EXPORT(mesh_for_kind)   uint32_t mesh_for_kind(uint32_t kind)  { return kind < K_OPAQUE_COUNT ? MESH_FOR_KIND[kind] : 0; }
