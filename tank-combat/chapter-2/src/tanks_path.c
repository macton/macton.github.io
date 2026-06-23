#include "sim.h"
#include "tanks_path.h"
#include "edge_paths.h"

/* Has the tank reached its current cell's centre on the axis it is travelling?
 * (Driving a cardinal heading keeps the perpendicular axis exactly centred — the
 * velocity component there is exactly zero — so only the travel axis can be off
 * centre, and only momentarily, before a turn.) */
static int reached_centre(uint32_t cur_di, int ox, int oy) {
  switch (cur_di) {
    case 0:  return ox >= 0;   /* E (+x) */
    case 16: return ox <= 0;   /* W (-x) */
    case 8:  return oy >= 0;   /* S (+y) */
    case 24: return oy <= 0;   /* N (-y) */
    default: return 1;         /* mid-turn (already snapped at the centre) */
  }
}

void tanks_path(World* w) {
  for (uint32_t t = 0; t < N_TANKS; t++) {
    /* a manually-driven tank takes its input from set_input; leave it alone */
    if (w->tstate[t] == TS_MANUAL) continue;
    /* every other tank follows its destination if it has one, else idles */
    if (!w->phas[t]) { w->tank_in[t] = 0; w->pgoal[t] = DIR_NONE; w->pstatus[t] = PS_IDLE; continue; }

    int x = xy_lo(w->tank_xy[t]), y = xy_hi(w->tank_xy[t]);
    int wcx = wrap_wcx(x >> SUB_SHIFT), wcy = wrap_wcy(y >> SUB_SHIFT);
    uint32_t screen = ((uint32_t)wcy / GRID_H) * SCREENS_X + ((uint32_t)wcx / GRID_W);
    uint32_t cell   = ((uint32_t)wcy % GRID_H) * GRID_W + ((uint32_t)wcx % GRID_W);

    int cc_x = (x >> SUB_SHIFT) * SUB + SUB / 2;     /* current cell centre */
    int cc_y = (y >> SUB_SHIFT) * SUB + SUB / 2;
    int ox = x - cc_x, oy = y - cc_y;
    uint32_t cur_di = (uint32_t)(w->tank_ang[t] >> ANGLE_SHIFT) & (N_DIRS - 1);

    if (screen == w->pdest_screen[t] && cell == w->pdest_cell[t]) {   /* arrived: roll to centre, stop */
      w->pgoal[t] = DIR_NONE; w->pstatus[t] = PS_ARRIVED;
      if (reached_centre(cur_di, ox, oy)) { w->tank_xy[t] = xy_pack(cc_x, cc_y); w->tank_in[t] = 0; }
      else w->tank_in[t] = IN_FWD;
      continue;
    }

    uint8_t d = route_next_dir(w, t, screen, cell);
    if (d == DIR_NONE) { w->tank_in[t] = 0; w->pgoal[t] = DIR_NONE; w->pstatus[t] = PS_NOPATH; continue; }
    w->pgoal[t] = d; w->pstatus[t] = PS_ROUTING;

    uint32_t target_di = CARD_DI[d];
    if (target_di == cur_di) {
      w->tank_in[t] = IN_FWD;                        /* drive straight: stays axis-aligned, no drift */
    } else if (reached_centre(cur_di, ox, oy)) {
      w->tank_xy[t] = xy_pack(cc_x, cc_y);           /* snap to the centre, then rotate in place */
      uint32_t delta = (target_di - cur_di) & (N_DIRS - 1);
      w->tank_in[t] = (delta <= N_DIRS / 2) ? IN_RIGHT : IN_LEFT;
    } else {
      w->tank_in[t] = IN_FWD;                        /* roll forward to the centre before turning */
    }
  }
}
