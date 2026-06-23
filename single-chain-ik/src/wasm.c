/* wasm.c — the WebAssembly boundary. The ONLY wasm-specific file: it holds the
 * single static World and instance buffer and exports the data protocol. sim.c,
 * gait.c, ik.c and render.c know nothing about wasm.
 *
 * DATA PROTOCOL (JS <-> WASM). C owns every byte layout; JS asks for the
 * pointer/count it needs.
 *   init() / tick()->quad count        advance and rebuild the view
 *   set_* / *()                        read/write the tunables
 *   set_show_stages / set_focus        drive the length/direction overlay
 * Pointers expose the per-leg solve results so the page can print them live; the
 * whole view is rebuilt each frame (build_view), inst_count is the quad count. */

#include "sim.h"
#include "render.h"

#define EXPORT(name) __attribute__((export_name(#name)))

static World    g_world;
static Inst     g_inst[INST_MAX];
static uint32_t g_inst_count;
static int      g_show_stages = 0;   /* teaching overlay off by default */
static int      g_focus = 0;

static void rebuild(void) { g_inst_count = build_view(&g_world, g_inst, g_show_stages, g_focus); }

/* init resets the SIM only; the overlay/focus are presentation state and persist
 * across a Reset (set by the host or the static defaults above). */
EXPORT(init) void init(void) { sim_init(&g_world); rebuild(); }
EXPORT(tick) uint32_t tick(void) { sim_tick(&g_world); rebuild(); return g_inst_count; }

/* overlay controls (presentation state — lives here, not in the sim) */
EXPORT(set_show_stages) void set_show_stages(uint32_t on) { g_show_stages = on ? 1 : 0; rebuild(); }
EXPORT(set_focus)       void set_focus(uint32_t leg) { g_focus = (int)(leg % N_LEGS); rebuild(); }
EXPORT(show_stages)     uint32_t show_stages(void) { return (uint32_t)g_show_stages; }
EXPORT(focus)           uint32_t focus(void) { return (uint32_t)g_focus; }

/* instance buffer */
EXPORT(inst_ptr)    Inst*    inst_ptr(void)    { return g_inst; }
EXPORT(inst_count)  uint32_t inst_count(void)  { return g_inst_count; }
EXPORT(inst_max)    uint32_t inst_max(void)    { return INST_MAX; }
EXPORT(inst_stride) uint32_t inst_stride(void) { return (uint32_t)sizeof(Inst); }

/* per-leg solve results (one entry per leg unless noted) */
EXPORT(joint_x_ptr)    int32_t*  joint_x_ptr(void)    { return g_world.joint_x; }   /* N_LEGS*N_JOINT */
EXPORT(joint_y_ptr)    int32_t*  joint_y_ptr(void)    { return g_world.joint_y; }
EXPORT(foot_x_ptr)     int32_t*  foot_x_ptr(void)     { return g_world.foot_x; }
EXPORT(foot_y_ptr)     int32_t*  foot_y_ptr(void)     { return g_world.foot_y; }
EXPORT(leg_curl_ptr)   uint16_t* leg_curl_ptr(void)   { return g_world.leg_curl; }
EXPORT(leg_reach_ptr)  int32_t*  leg_reach_ptr(void)  { return g_world.leg_reach; }
EXPORT(leg_goal_ptr)   int32_t*  leg_goal_ptr(void)   { return g_world.leg_goal; }
EXPORT(leg_clamped_ptr)uint8_t*  leg_clamped_ptr(void){ return g_world.leg_clamped; }
EXPORT(leg_dcos_ptr)   int16_t*  leg_dcos_ptr(void)   { return g_world.leg_dcos; }
EXPORT(leg_dsin_ptr)   int16_t*  leg_dsin_ptr(void)   { return g_world.leg_dsin; }
EXPORT(swinging_ptr)   uint8_t*  swinging_ptr(void)   { return g_world.swinging; }

/* scalars / shape */
EXPORT(n_legs)   uint32_t n_legs(void)   { return N_LEGS; }
EXPORT(n_seg)    uint32_t n_seg(void)    { return N_SEG; }
EXPORT(n_joint)  uint32_t n_joint(void)  { return N_JOINT; }
EXPORT(subcell)  uint32_t subcell(void)  { return SUB; }
EXPORT(ang_full) uint32_t ang_full(void) { return ANG_FULL; }
EXPORT(trig_one) uint32_t trig_one(void) { return TRIG_ONE; }
EXPORT(view_w)   uint32_t view_w(void)   { return VIEW_W; }
EXPORT(view_h)   uint32_t view_h(void)   { return VIEW_H; }
EXPORT(frame)    uint32_t frame(void)    { return g_world.frame; }
EXPORT(body_x)   int32_t  body_x(void)   { return g_world.body_x; }
EXPORT(body_y)   int32_t  body_y(void)   { return g_world.body_y; }
EXPORT(cam_y)    int32_t  cam_y(void)    { return CAM_Y; }
EXPORT(curl_max) uint32_t curl_max(void) { return g_world.curl_max; }

/* tunables */
EXPORT(walk_speed)     uint32_t walk_speed(void)     { return g_world.walk_speed; }
EXPORT(set_walk_speed) void set_walk_speed(uint32_t v) { g_world.walk_speed = (uint16_t)v; }
EXPORT(seg_len)        uint32_t seg_len(void)        { return g_world.seg_len; }
EXPORT(set_seg_len)    void set_seg_len(uint32_t v)  { sim_set_seg_len(&g_world, (uint16_t)v); }
EXPORT(stand_h)        uint32_t stand_h(void)        { return g_world.stand_h; }
EXPORT(set_stand_h)    void set_stand_h(uint32_t v)  { g_world.stand_h = (uint16_t)v; }
EXPORT(step_len)       uint32_t step_len(void)       { return g_world.step_len; }
EXPORT(set_step_len)   void set_step_len(uint32_t v) { g_world.step_len = (uint16_t)v; }
EXPORT(step_lift)      uint32_t step_lift(void)      { return g_world.step_lift; }
EXPORT(set_step_lift)  void set_step_lift(uint32_t v){ g_world.step_lift = (uint16_t)v; }
