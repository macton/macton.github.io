#include "sim.h"
#include "map.h"
#include "tanks_path.h"
#include "agent_turn.h"
#include "agent_move.h"
#include "mites.h"
#include "tanks_fire.h"

/* place a tank centred in world cell (wcx,wcy), facing discrete direction di */
static void place(World* w, uint32_t t, uint32_t wcx, uint32_t wcy, uint32_t di) {
  w->tank_xy[t]  = xy_pack((int32_t)wcx * SUB + SUB / 2, (int32_t)wcy * SUB + SUB / 2);
  w->tank_ang[t] = (uint16_t)(di << ANGLE_SHIFT);
  w->tank_in[t] = 0; w->tank_vxy[t] = 0; w->tank_hit[t] = 0;
  w->tank_turret[t] = w->tank_ang[t]; w->tank_cooldown[t] = 0;
  w->tank_target[t] = TGT_NONE; w->tank_shot_cell[t] = 0; w->tank_tracer[t] = 0;
}

void sim_init(World* w) {
  for (uint32_t i = 0; i < N_SCREENS * GRID_H; i++) w->grid[i] = GRID_INIT[i];

  /* all tanks are identical; start them on the always-open ring of screen 0 so
   * they are visible immediately, each UNSELECTED with no destination */
  place(w, 0,  1,  7, 0);
  place(w, 1, 18,  7, 0);
  place(w, 2, 10,  1, 8);
  place(w, 3, 10, 13, 24);
  for (uint32_t t = 0; t < N_TANKS; t++) {
    w->tstate[t] = TS_UNSELECTED; w->phas[t] = 0; w->pgoal[t] = DIR_NONE; w->pstatus[t] = PS_IDLE;
    w->pdest_screen[t] = 0; w->pdest_cell[t] = 0;
  }
  w->selected = SEL_NONE;

  w->frame = 0;
  for (uint32_t i = 0; i < N_FX; i++) w->fx_t[i] = 0;   /* no destruction bursts yet */
  w->fx_head = 0;
  w->move_speed = 16;         /* subcells per tick (16 ticks per cell) */
  w->turn_rate  = 1024;       /* body angle units/tick: ~16 ticks per 90 degrees */
  w->turret_rate = 2048;      /* turret angle units/tick: ~8 ticks per 90 degrees (swings independently) */
  w->collide_scale = 128;     /* Q0.8: 50% while colliding */

  /* mite tunables (the page edits these live) */
  w->mite_speed = 12;         /* a touch slower than the tanks, so the swarm gives chase */
  w->mite_turn  = 2048;       /* nimble: ~8 ticks per 90 degrees */
  w->mite_sense = 2;          /* sense a tank within two cells (Chebyshev) */
  w->mite_cap   = MITE_CAP;   /* at most 4 mite centres per cell */
  w->mite_phunt = 80;         /* 80% hunt the sighting on adopting a record; 20% carry it home */
  w->mite_seed  = 1337;       /* the editable swarm seed */
  w->fire_period  = 30;       /* tank shots every 30 ticks = 2/sec (0 = firing off) */
  w->mite_respawn = 300;      /* a killed mite revives at its nest after 300 ticks = 5 s */

  /* the four nests (home cells), spread across the world on open ring cells. Nest 0
   * sits in screen (1,1), NOT in screen (0,0) where all four tanks start — so revived
   * mites don't pop up under a tank. The other three are the far corner screens. */
  w->nest_cell[0] = wc_pack(21, 22);   /* screen (1,1) ring — off the tank start screen */
  w->nest_cell[1] = wc_pack(78,  7);   /* screen (3,0) */
  w->nest_cell[2] = wc_pack( 1, 52);   /* screen (0,3) */
  w->nest_cell[3] = wc_pack(78, 52);   /* screen (3,3) */

  /* Build the rarely-changing path tables once: Level 1, then Level 2 on top, then
   * each tank's remaining-distance vector (empty until a destination is set). */
  w->l1_maxdist = (uint16_t)l1_build_all(w->l1dist, w->grid, w->l1_screen_max);
  l2_build(w);
  for (uint32_t t = 0; t < N_TANKS; t++) l2_compute_pg(w, t);

  /* fold the resident nest route fields (needs Level 2); reset the field peak */
  mites_fold_nests(w);
  w->field_active = 0; w->field_peak = 0;

  /* scatter the thousand mites across the open reachable cells from the seed,
   * then build the per-cell index once so it is valid before the first tick. */
  mites_spawn(w);
  mites_build_index(w);
}

void sim_tick(World* w) {
  /* 1. the swarm's acceleration structure, rebuilt from current positions */
  mites_build_index(w);
  /* 1b. combat respawn: revive timed-out dead mites at their nests, INTO the freshly
   *     built index (so the crowding cap and this tick's gossip/movement see them as
   *     ordinary live mites). Must precede mites_step, whose cap reservation reads the
   *     index — reviving after it could push a nest cell over the cap. */
  mites_respawn(w);
  /* 2. tank input (controls for MANUAL, route lookup for the rest) — unchanged */
  tanks_path(w);
  /* 3. records: gossip the last-known tank position (double-buffered LWW); each
   *    mite's mode + destination fall out (sense->hunt, adopt->hunt/home, arrive) */
  mites_records(w);
  /* 4. route fields: a handful of shared routes keyed by destination (the nests +
   *    the active sighting cells) — fold the new, reuse resident, free dropped */
  mites_update_fields(w);
  /* 5. mite movement: crowding cap (in-order reservation) + steer along the field */
  mites_step(w);
  /* 5b. combat: aim every turret at the nearest mite in line of sight and fire — a
   *     kill is a "death cry" that feeds the firing tank's position back into the
   *     gossip near the dead mite (revival happened in step 1b). */
  tanks_fire(w);
  /* 6. turn + auto-steer: tanks at TANK turn rate, mites at the mite turn rate */
  agent_turn(w->tank_xy, w->tank_ang, w->tank_in, w->tank_hit, N_TANKS, w->turn_rate, w->grid);
  agent_turn(w->mite_xy, w->mite_ang, w->mite_in, w->mite_hit, N_MITES, w->mite_turn, w->grid);
  /* 7. move + wall collision: tanks pass TANK_R, mites pass the smaller MITE_R */
  agent_move(w->tank_xy, w->tank_vxy, w->tank_hit, w->tank_ang, w->tank_in,
             N_TANKS, (int32_t)w->move_speed, (int32_t)w->collide_scale, w->grid, TANK_R);
  agent_move(w->mite_xy, w->mite_vxy, w->mite_hit, w->mite_ang, w->mite_in,
             N_MITES, (int32_t)w->mite_speed, (int32_t)w->collide_scale, w->grid, MITE_R);
  /* 8. advance the frame counter — it is the record timestamp clock */
  w->frame++;
}

/* ---- mutators: rebuild only the tables a change can affect ---------------- */

void sim_toggle_wall(World* w, uint32_t wcx, uint32_t wcy) {
  wcx = (uint32_t)wrap_wcx((int32_t)wcx); wcy = (uint32_t)wrap_wcy((int32_t)wcy);
  uint32_t screen = (wcy / GRID_H) * SCREENS_X + (wcx / GRID_W);
  uint32_t cx = wcx % GRID_W, cy = wcy % GRID_H;
  w->grid[screen * GRID_H + cy] ^= (1u << cx);

  w->l1_screen_max[screen] = (uint16_t)l1_build_screen(w->l1dist + screen * TRI, w->grid, screen);
  w->l1_maxdist = 0;
  for (uint32_t s = 0; s < N_SCREENS; s++)
    if (w->l1_screen_max[s] > w->l1_maxdist) w->l1_maxdist = w->l1_screen_max[s];
  l2_build(w);
  for (uint32_t t = 0; t < N_TANKS; t++) l2_compute_pg(w, t);
  mites_fold_nests(w);   /* routes changed: re-fold the resident nests + drop the stale cache */
}

void sim_set_dest(World* w, uint32_t tank, uint32_t wcx, uint32_t wcy) {
  if (tank >= N_TANKS) return;
  wcx = (uint32_t)wrap_wcx((int32_t)wcx); wcy = (uint32_t)wrap_wcy((int32_t)wcy);
  w->pdest_screen[tank] = (uint8_t)((wcy / GRID_H) * SCREENS_X + (wcx / GRID_W));
  w->pdest_cell[tank]   = (uint16_t)((wcy % GRID_H) * GRID_W + (wcx % GRID_W));
  w->phas[tank] = 1;
  l2_compute_pg(w, tank);                           /* fold the matrix for this goal (rare) */
}

void sim_cycle_tank(World* w, uint32_t tank) {
  if (tank >= N_TANKS) return;
  if (w->selected != tank) {                        /* select it (into AUTOPATH); drop the old one */
    if (w->selected != SEL_NONE) w->tstate[w->selected] = TS_UNSELECTED;
    w->selected = (uint8_t)tank;
    w->tstate[tank] = TS_AUTOPATH;
  } else if (w->tstate[tank] == TS_AUTOPATH) {       /* taking manual control abandons the auto-path, */
    w->tstate[tank] = TS_MANUAL;                      /* so deselecting from MANUAL leaves it where it is */
    w->phas[tank] = 0; w->pstatus[tank] = PS_IDLE; w->pgoal[tank] = DIR_NONE;
  } else {                                           /* MANUAL -> deselect (no destination to resume) */
    w->tstate[tank] = TS_UNSELECTED;
    w->selected = SEL_NONE;
  }
}

void sim_set_seed(World* w, uint32_t seed) {           /* re-seed and re-scatter the swarm */
  w->mite_seed = (uint16_t)seed;
  mites_spawn(w);
}

void sim_set_nest(World* w, uint32_t nest, uint32_t wcx, uint32_t wcy) {  /* move a nest, re-fold its field */
  if (nest >= NEST_COUNT) return;
  wcx = (uint32_t)wrap_wcx((int32_t)wcx); wcy = (uint32_t)wrap_wcy((int32_t)wcy);
  w->nest_cell[nest] = wc_pack((int32_t)wcx, (int32_t)wcy);
  mites_fold_nest(w, nest);
}
