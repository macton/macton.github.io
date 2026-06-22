#include "sim.h"
#include "map.h"
#include "tanks_path.h"
#include "tanks_turn.h"
#include "tanks_move.h"

/* place a tank centred in world cell (wcx,wcy), facing discrete direction di */
static void place(World* w, uint32_t t, uint32_t wcx, uint32_t wcy, uint32_t di) {
  w->tank_xy[t]  = xy_pack((int32_t)wcx * SUB + SUB / 2, (int32_t)wcy * SUB + SUB / 2);
  w->tank_ang[t] = (uint16_t)(di << ANGLE_SHIFT);
  w->tank_in[t] = 0; w->tank_vxy[t] = 0; w->tank_hit[t] = 0;
}

void sim_init(World* w) {
  for (uint32_t i = 0; i < N_SCREENS * GRID_H; i++) w->grid[i] = GRID_INIT[i];

  /* one manual tank + N_PATHED pathed tanks, in known-open cells (the central
   * cross of each screen is open). The selected pathed tank starts in screen 0
   * so it can be driven immediately; the others sit in farther screens to show
   * cross-screen routing and that a tank paths whether or not it is in view. */
  place(w, 0, 10,  7, 0);    /* manual, screen 0 */
  place(w, 1, 10, 11, 0);    /* pathed, screen 0 */
  place(w, 2, 30, 22, 0);    /* pathed, screen 5 (1,1) */
  place(w, 3, 50, 37, 0);    /* pathed, screen 10 (2,2) */
  for (uint32_t p = 0; p < N_PATHED; p++) {
    w->phas[p] = 0; w->pgoal[p] = DIR_NONE; w->pstatus[p] = PS_IDLE;
    w->pdest_screen[p] = 0; w->pdest_cell[p] = 0;
  }

  w->cam_sx = 0; w->cam_sy = 0;
  w->selected = 1;            /* first pathed tank */
  w->frame = 0;
  w->move_speed = 16;         /* subcells per tick (16 ticks per cell) */
  w->turn_rate  = 1024;       /* angle units/tick: ~16 ticks per 90° — snappy enough to watch a route */
  w->collide_scale = 128;     /* Q0.8: 50% while colliding */

  /* Build the rarely-changing path tables once. Level 1 first (per-screen
   * all-pairs distance), then Level 2 on top of it (edge-point next-hop), then
   * each tank's remaining-distance vector (empty until a destination is set). */
  l1_build_all(w->l1dist, w->grid);
  l2_build(w);
  for (uint32_t p = 0; p < N_PATHED; p++) l2_compute_pg(w, p);
}

void sim_tick(World* w) {
  tanks_path(w);                                    /* pathed input from the lookup */
  tanks_turn(w->tank_xy, w->tank_ang, w->tank_in, w->tank_hit,
             N_TANKS, w->turn_rate, w->grid);       /* player turn + auto-steer */
  tanks_move(w->tank_xy, w->tank_vxy, w->tank_hit, w->tank_ang, w->tank_in,
             N_TANKS, (int32_t)w->move_speed, (int32_t)w->collide_scale, w->grid);
  w->frame++;
}

/* ---- rare mutators: rebuild only the tables the change can affect --------- */

void sim_toggle_wall(World* w, uint32_t wcx, uint32_t wcy) {
  wcx = (uint32_t)wrap_wcx((int32_t)wcx); wcy = (uint32_t)wrap_wcy((int32_t)wcy);
  uint32_t screen = (wcy / GRID_H) * SCREENS_X + (wcx / GRID_W);
  uint32_t cx = wcx % GRID_W, cy = wcy % GRID_H;
  w->grid[screen * GRID_H + cy] ^= (1u << cx);

  /* the toggled cell lives in `screen`, so only that screen's Level-1 distances
   * change; but a border edit can change connectivity, so Level 2 (and every
   * tank's remaining-distance vector) is rebuilt wholesale. All rare. */
  l1_build_screen(w->l1dist + screen * TRI, w->grid, screen);
  l2_build(w);
  for (uint32_t p = 0; p < N_PATHED; p++) l2_compute_pg(w, p);
}

void sim_set_dest(World* w, uint32_t tank, uint32_t wcx, uint32_t wcy) {
  if (tank < 1 || tank >= N_TANKS) return;          /* only pathed tanks have a destination */
  uint32_t p = tank - 1;
  wcx = (uint32_t)wrap_wcx((int32_t)wcx); wcy = (uint32_t)wrap_wcy((int32_t)wcy);
  w->pdest_screen[p] = (uint8_t)((wcy / GRID_H) * SCREENS_X + (wcx / GRID_W));
  w->pdest_cell[p]   = (uint16_t)((wcy % GRID_H) * GRID_W + (wcx % GRID_W));
  w->phas[p] = 1;
  l2_compute_pg(w, p);                              /* fold the matrix for this goal (rare) */
}

void sim_clear_dest(World* w, uint32_t tank) {
  if (tank < 1 || tank >= N_TANKS) return;
  uint32_t p = tank - 1;
  w->phas[p] = 0; w->pstatus[p] = PS_IDLE; w->pgoal[p] = DIR_NONE;
  l2_compute_pg(w, p);
}

void sim_set_selected(World* w, uint32_t tank) {
  if (tank >= 1 && tank < N_TANKS) w->selected = (uint8_t)tank;
}

void sim_scroll(World* w, int dx, int dy) {         /* viewport only: never touches the sim */
  w->cam_sx = (uint8_t)(((int)w->cam_sx + dx + SCREENS_X) % SCREENS_X);
  w->cam_sy = (uint8_t)(((int)w->cam_sy + dy + SCREENS_Y) % SCREENS_Y);
}
