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
 *   set_view(wx0,wy0,wx1,wy1, hx,hy)  the world-space box the free pan/zoom camera
 *                                 shows + the cursor cell; the wasm emits only that
 * Pointers expose every table for the live debug widgets. The whole view is
 * rebuilt each frame (build_view); inst_count is the instance count to draw.
 *
 * CHAPTER 4 adds RENDER exports only — the draw list (per-kind counts) and the baked
 * meshes; the projection is host-side (a perspective MVP), so no basis is exported —
 * leaving every simulation export byte-identical to chapter 3. */

#include "sim.h"
#include "render.h"
#include "mesh_data.h"
#include "map_mesh_data.h"
#include "staticmap.h"

#define EXPORT(name) __attribute__((export_name(#name)))
#define MESH_TOTAL_COUNT (M_PROC_COUNT + MAP_MESH_COUNT)   /* procedural + town meshes */

static World    g_world;
static Inst     g_inst[INST_MAX];
static DrawList g_dl;
static uint32_t g_inst_count;
/* the deferred point lights (one per visible mite / FX), rebuilt every frame with the view */
static Inst     g_lights[LIGHT_MAX];
static uint32_t g_light_count;
/* the visible world-space box (subcells) + the cursor cell — all render-only. The
 * host owns the free pan/zoom camera and passes the box it can see; the wasm just
 * builds it. Defaults to the whole world. */
static int32_t  g_wx0, g_wy0, g_wx1 = ARENA_W_SUB - 1, g_wy1 = ARENA_H_SUB - 1;
static uint16_t g_hover = REC_EMPTY;

/* the STATIC TOWN — baked once from the frozen map (build_static_map), uploaded by the
 * host ONCE and instanced every frame with only a frustum cull. It is re-baked ONLY
 * when the map itself changes (a wall toggled, a nest moved); the version counter ticks
 * then so the host re-uploads exactly once per change, never per frame. tick() does NOT
 * touch it — the town is frozen while the swarm runs over it. */
static Inst      g_static_inst[STATIC_INST_MAX];
static StaticRun g_static_runs[STATIC_RUN_MAX];
static uint32_t  g_static_inst_count, g_static_run_count, g_static_version;
/* the static PROPS (trees on grass corners): a second baked-once layer, re-baked with the
 * town on the same version tick. Its own buffer — trees sit on corners, not one-per-cell. */
static Inst      g_prop_inst[STATIC_PROP_INST_MAX];
static StaticRun g_prop_runs[STATIC_PROP_RUN_MAX];
static uint32_t  g_prop_inst_count, g_prop_run_count;

static void rebuild(void) {
  g_inst_count = build_view(&g_world, g_inst, &g_dl, g_wx0, g_wy0, g_wx1, g_wy1, g_hover);
  g_light_count = build_lights(&g_world, g_lights, g_wx0, g_wy0, g_wx1, g_wy1);
}

/* RENDER-TIME INTERPOLATION (presentation-only). The fixed-timestep sim beats against the
 * free-running display, so the host smooths motion by drawing each moving thing partway
 * between two ticks. We keep the PREVIOUS tick's positions here; the host snapshots them
 * just before each tick (snapshot_prev), then per frame sets the lerp weight (set_interp)
 * — the fraction of a tick elapsed — before the build_view that emits the drawn frame.
 * Disabled (alpha 0) it is a pure no-op, so the sim exports stay byte-identical. */
static uint32_t g_prev_tank_xy[N_TANKS];
static uint32_t g_prev_mite_xy[N_MITES];
static uint32_t g_prev_proj_xy[N_TANKS * PROJ_MAX];
static void copy_u32(uint32_t* dst, const uint32_t* src, uint32_t n) { for (uint32_t i = 0; i < n; i++) dst[i] = src[i]; }
static void rebuild_static(void) {
  g_static_inst_count = build_static_map(g_world.grid, g_world.nest_cell,
                                         g_static_inst, g_static_runs, &g_static_run_count);
  g_prop_inst_count = build_static_props(g_world.grid, g_world.nest_cell,
                                         g_prop_inst, g_prop_runs, &g_prop_run_count);
  g_static_version++;
}

EXPORT(init) void init(void) {
  sim_init(&g_world);
  g_wx0 = 0; g_wy0 = 0; g_wx1 = ARENA_W_SUB - 1; g_wy1 = ARENA_H_SUB - 1; g_hover = REC_EMPTY;
  rebuild_static();
  rebuild();
}
EXPORT(tick) uint32_t tick(void) { sim_tick(&g_world); rebuild(); return g_inst_count; }

/* INTERPOLATION protocol (host-driven, presentation-only). snapshot_prev() saves the
 * positions of every moving thing as the PREVIOUS tick BEFORE the host calls tick(); after
 * its tick loop the host calls set_interp(alpha_q8) with the fraction of a tick elapsed
 * (0..255), which arms build_view to lerp prev->current. alpha 0 disables it entirely. */
EXPORT(snapshot_prev) void snapshot_prev(void) {
  copy_u32(g_prev_tank_xy, g_world.tank_xy,      N_TANKS);
  copy_u32(g_prev_mite_xy, g_world.mite_xy,      N_MITES);
  copy_u32(g_prev_proj_xy, g_world.tank_proj_xy, N_TANKS * PROJ_MAX);
}
EXPORT(set_interp) void set_interp(uint32_t alpha_q8) {
  render_set_interp(alpha_q8, g_prev_tank_xy, g_prev_mite_xy, g_prev_proj_xy);
}

EXPORT(set_input)   void set_input(uint32_t tank, uint32_t bits) { if (tank < N_TANKS) g_world.tank_in[tank] = (uint8_t)bits; }
EXPORT(cycle_tank)  void cycle_tank(uint32_t tank) { sim_cycle_tank(&g_world, tank); rebuild(); }
/* follow-cam picks a tank: SELECT it (into AUTOPATH) without cycling — mirrors
 * sim_cycle_tank's select branch, but never advances an already-selected tank. */
EXPORT(select_tank) void select_tank(uint32_t tank) {
  if (tank >= N_TANKS || g_world.selected == tank) return;
  if (g_world.selected != SEL_NONE) g_world.tstate[g_world.selected] = TS_UNSELECTED;
  g_world.selected = (uint8_t)tank;
  g_world.tstate[tank] = TS_AUTOPATH;
  rebuild();
}
EXPORT(set_dest)    void set_dest(uint32_t tank, uint32_t wcx, uint32_t wcy) { sim_set_dest(&g_world, tank, wcx, wcy); rebuild(); }
/* toggling a wall changes the MAP, so the static town re-bakes (a building appears/clears,
 * the road autotiling around it shifts); the version bump tells the host to re-upload. */
EXPORT(toggle_wall) void toggle_wall(uint32_t wcx, uint32_t wcy) { sim_toggle_wall(&g_world, wcx, wcy); rebuild_static(); rebuild(); }

/* the visible box the free pan/zoom camera shows (world subcells), and the cursor
 * cell highlight (wcx>=BIG_W clears it). Pure presentation; the host computes both
 * from its camera, the wasm just emits what falls inside. */
EXPORT(set_view) void set_view(int32_t wx0, int32_t wy0, int32_t wx1, int32_t wy1, uint32_t wcx, uint32_t wcy) {
  g_wx0 = wx0; g_wy0 = wy0; g_wx1 = wx1; g_wy1 = wy1;
  g_hover = (wcx < BIG_W && wcy < BIG_H) ? wc_pack((int32_t)wcx, (int32_t)wcy) : REC_EMPTY;
  rebuild();
}

/* ---- the swarm: re-seed / nest mutators, live pointers, scalars, tunables -- */
EXPORT(set_seed) void set_seed(uint32_t seed) { sim_set_seed(&g_world, seed); rebuild(); }   /* swarm only: the map is unchanged, so no static re-bake */
/* moving a nest moves its LANDMARK, so the static town re-bakes (and re-uploads). */
EXPORT(set_nest) void set_nest(uint32_t n, uint32_t wcx, uint32_t wcy) { sim_set_nest(&g_world, n, wcx, wcy); rebuild_static(); rebuild(); }

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
EXPORT(proj_speed)       uint32_t proj_speed(void)         { return g_world.proj_speed; }
EXPORT(set_proj_speed)   void     set_proj_speed(uint32_t v) { g_world.proj_speed = (uint16_t)(v < 16 ? 16 : (v > 4096 ? 4096 : v)); }

/* pointers — only the World fields the page reads (the chapter-2 path tables stay
 * simulation-internal in chapter 3, so they are no longer exported) */
EXPORT(inst_ptr)        Inst*     inst_ptr(void)        { return g_inst; }
EXPORT(grid_ptr)        uint32_t* grid_ptr(void)        { return g_world.grid; }
/* the WALL SHAKE the sim computes (a struck wall buzzes for WALL_SHAKE_DUR ticks). In chapter 3
 * render.c re-emitted walls and jittered them; here the walls are the STATIC town, so the host
 * reads this state and jolts the struck building's static instance — same effect, render-side. */
EXPORT(wall_shake_cell_ptr) const uint16_t* wall_shake_cell_ptr(void) { return g_world.wall_shake_cell; }
EXPORT(wall_shake_t_ptr)    const uint8_t*  wall_shake_t_ptr(void)    { return g_world.wall_shake_t; }
EXPORT(wall_shake_max)      uint32_t        wall_shake_max(void)      { return WALL_SHAKE_MAX; }
EXPORT(wall_shake_dur)      uint32_t        wall_shake_dur(void)      { return WALL_SHAKE_DUR; }
EXPORT(wall_shake_amp)      uint32_t        wall_shake_amp(void)      { return WALL_SHAKE_AMP; }
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
EXPORT(inst_count)  uint32_t inst_count(void)  { return g_inst_count; }
EXPORT(inst_max)    uint32_t inst_max(void)    { return INST_MAX; }
EXPORT(inst_stride) uint32_t inst_stride(void) { return (uint32_t)sizeof(Inst); }
/* the deferred point lights — same Inst layout (a light volume: centre + radius + colour),
 * rebuilt each frame; the host uploads light_count of them and draws them additively. */
EXPORT(light_ptr)   Inst*    light_ptr(void)    { return g_lights; }
EXPORT(light_count) uint32_t light_count(void)  { return g_light_count; }
EXPORT(light_max)   uint32_t light_max(void)    { return LIGHT_MAX; }
EXPORT(frame)       uint32_t frame(void)       { return g_world.frame; }

/* ---- chapter-4 RENDER exports (all render-side; sim exports above unchanged) ---
 * the draw list: instances are grouped by kind, so the host draws one range per
 * opaque kind (binding that kind's mesh) then one sorted translucent range. */
EXPORT(k_opaque_count)    uint32_t k_opaque_count(void)            { return K_OPAQUE_COUNT; }
EXPORT(draw_opaque)       uint32_t draw_opaque(uint32_t kind)      { return kind < K_OPAQUE_COUNT ? g_dl.opaque[kind] : 0; }
EXPORT(draw_translucent)  uint32_t draw_translucent(void)          { return g_dl.translucent; }
/* the projection is now a host-side perspective MVP matrix (see app.js); the wasm
 * exports no projection basis — it just emits the world placements the host views. */

/* the baked low-poly meshes (loaded once at startup, instanced every frame). Two
 * tables share one GPU buffer: the PROCEDURAL meshes [0,M_PROC_COUNT) for the dynamic
 * things, then the TOWN meshes [M_PROC_COUNT,MESH_TOTAL_COUNT) for the static map. The
 * page uploads the two vertex buffers back to back (procedural first), so mesh_voff()
 * returns offsets into that combined buffer; the static runs carry already-offset ids. */
EXPORT(mesh_data_ptr)     const int8_t* mesh_data_ptr(void)        { return MESH_VERT; }       /* procedural verts */
EXPORT(map_mesh_data_ptr) const int8_t* map_mesh_data_ptr(void)    { return MAP_MESH_VERT; }   /* town verts */
EXPORT(mesh_proc_total)   uint32_t mesh_proc_total(void)           { return MESH_VERT_TOTAL; }  /* procedural vertex count */
EXPORT(mesh_vert_total)   uint32_t mesh_vert_total(void)           { return MESH_VERT_TOTAL + MAP_MESH_VERT_TOTAL; }
EXPORT(mesh_vstride)      uint32_t mesh_vstride(void)              { return MESH_VSTRIDE; }
EXPORT(mesh_count)        uint32_t mesh_count(void)                { return MESH_TOTAL_COUNT; }
EXPORT(mesh_proc_count)   uint32_t mesh_proc_count(void)           { return M_PROC_COUNT; }   /* town mesh id = M_PROC_COUNT + base */
EXPORT(map_lod2_offset)   uint32_t map_lod2_offset(void)           { return MAP_LOD2_OFFSET; }   /* LOD2 massing of a town mesh id = id + this */
EXPORT(mesh_cube)         uint32_t mesh_cube(void)                 { return M_CUBE; }
EXPORT(mesh_voff)         uint32_t mesh_voff(uint32_t m) {          /* offset into the COMBINED buffer */
  if (m < M_PROC_COUNT) return MESH_VOFF[m];
  return m < MESH_TOTAL_COUNT ? MESH_VERT_TOTAL + MAP_MESH_VOFF[m - M_PROC_COUNT] : 0;
}
EXPORT(mesh_vcnt)         uint32_t mesh_vcnt(uint32_t m) {
  if (m < M_PROC_COUNT) return MESH_VCNT[m];
  return m < MESH_TOTAL_COUNT ? MAP_MESH_VCNT[m - M_PROC_COUNT] : 0;
}
EXPORT(mesh_for_kind)     uint32_t mesh_for_kind(uint32_t kind)    { return kind < K_OPAQUE_COUNT ? MESH_FOR_KIND[kind] : 0; }
/* the LOD1 IMPOSTERS: a SEPARATE 16-byte vertex table (they carry UVs into town_atlas.png),
 * indexed by base town-mesh m in [0,MAP_IMPOSTER_COUNT). The page uploads them to their own
 * vertex buffer + a textured pipeline; the atlas PNG is loaded separately by the page. */
EXPORT(map_imposter_data_ptr)  const int8_t* map_imposter_data_ptr(void) { return MAP_IMPOSTER_VERT; }
EXPORT(map_imposter_count)     uint32_t map_imposter_count(void)         { return MAP_IMPOSTER_COUNT; }
EXPORT(map_imposter_vert_total)uint32_t map_imposter_vert_total(void)    { return MAP_IMPOSTER_VERT_TOTAL; }
EXPORT(map_imposter_voff)      uint32_t map_imposter_voff(uint32_t m)    { return m < MAP_IMPOSTER_COUNT ? MAP_IMPOSTER_VOFF[m] : 0; }
EXPORT(map_imposter_vcnt)      uint32_t map_imposter_vcnt(uint32_t m)    { return m < MAP_IMPOSTER_COUNT ? MAP_IMPOSTER_VCNT[m] : 0; }
EXPORT(map_atlas_w)            uint32_t map_atlas_w(void)                { return MAP_ATLAS_W; }
EXPORT(map_atlas_h)            uint32_t map_atlas_h(void)                { return MAP_ATLAS_H; }

/* ---- the STATIC TOWN: the bake the host uploads once and frustum-culls per frame ----
 * static_inst_ptr/count/stride describe the instance buffer (same 24-byte Inst layout as
 * the dynamic one — same vertex pipeline). The run table groups those instances by
 * (screen, mesh): one run is a contiguous range sharing a town mesh, within one source
 * screen — so the host culls whole screens, then draws the visible runs. static_version
 * ticks whenever the map re-bakes (wall toggled / nest moved), so the host re-uploads the
 * instance buffer + re-reads the runs exactly then, and otherwise touches neither. */
EXPORT(static_inst_ptr)   const Inst*      static_inst_ptr(void)   { return g_static_inst; }
EXPORT(static_inst_count) uint32_t         static_inst_count(void) { return g_static_inst_count; }
EXPORT(static_inst_stride)uint32_t         static_inst_stride(void){ return (uint32_t)sizeof(Inst); }
EXPORT(static_run_ptr)    const StaticRun* static_run_ptr(void)    { return g_static_runs; }
EXPORT(static_run_count)  uint32_t         static_run_count(void)  { return g_static_run_count; }
EXPORT(static_run_stride) uint32_t         static_run_stride(void) { return (uint32_t)sizeof(StaticRun); }
EXPORT(static_version)    uint32_t         static_version(void)    { return g_static_version; }
/* the static PROPS (trees): same Inst/StaticRun layout + stride as the town, its own buffer.
 * Re-baked on the same static_version tick, so the host re-uploads it in the same sync. */
EXPORT(prop_inst_ptr)     const Inst*      prop_inst_ptr(void)     { return g_prop_inst; }
EXPORT(prop_inst_count)   uint32_t         prop_inst_count(void)   { return g_prop_inst_count; }
EXPORT(prop_run_ptr)      const StaticRun* prop_run_ptr(void)      { return g_prop_runs; }
EXPORT(prop_run_count)    uint32_t         prop_run_count(void)    { return g_prop_run_count; }
