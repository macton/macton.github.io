#include "sim.h"        /* full World */
#include "collide.h"    /* cell_is_wall */

/* fixed-stride accessors into the P<=N_EDGE_MAX matrices (stride is constant so
 * the JS debug widget can read them without knowing P) */
#define D2(w,i,j) ((w)->dist2  [(uint32_t)(i) * N_EDGE_MAX + (uint32_t)(j)])
#define NH(w,i,j) ((w)->nexthop[(uint32_t)(i) * N_EDGE_MAX + (uint32_t)(j)])
#define INF32 0xFFFFFFFFu

uint32_t ep_world_cell(const World* w, uint32_t e) {
  uint32_t s = w->ep_screen[e], cell = w->ep_cell[e];
  uint32_t wcx = (s % SCREENS_X) * GRID_W + (cell % GRID_W);
  uint32_t wcy = (s / SCREENS_X) * GRID_H + (cell / GRID_W);
  return wcy * BIG_W + wcx;
}

/* one (screen,cross) border's cell range, as a start cell + per-step delta */
static void border_span(uint32_t cross, int* cx, int* cy, int* dcx, int* dcy, int* n) {
  switch (cross) {
    case DIR_N: *cx = 0; *cy = 0;          *dcx = 1; *dcy = 0; *n = GRID_W; break;
    case DIR_S: *cx = 0; *cy = GRID_H - 1; *dcx = 1; *dcy = 0; *n = GRID_W; break;
    case DIR_W: *cx = 0; *cy = 0;          *dcx = 0; *dcy = 1; *n = GRID_H; break;
    default:    *cx = GRID_W - 1; *cy = 0; *dcx = 0; *dcy = 1; *n = GRID_H; break; /* E */
  }
}

void l2_build(World* w) {
  /* ---- enumerate connecting edge points -------------------------------- */
  uint32_t P = 0;
  for (uint32_t s = 0; s < N_SCREENS; s++) w->screen_ep_count[s] = 0;

  for (uint32_t s = 0; s < N_SCREENS && P < N_EDGE_MAX; s++) {
    int sox = (int)(s % SCREENS_X) * GRID_W, soy = (int)(s / SCREENS_X) * GRID_H;
    for (uint32_t cross = 0; cross < 4 && P < N_EDGE_MAX; cross++) {
      int cx, cy, dcx, dcy, n; border_span(cross, &cx, &cy, &dcx, &dcy, &n);
      for (int k = 0; k < n && P < N_EDGE_MAX; k++, cx += dcx, cy += dcy) {
        int wcx = sox + cx, wcy = soy + cy;
        if (cell_is_wall(w->grid, wcx, wcy)) continue;                 /* border cell must be open */
        if (cell_is_wall(w->grid, wcx + CARD_DCX[cross], wcy + CARD_DCY[cross])) continue; /* and its neighbour */
        w->ep_screen[P]  = (uint8_t)s;
        w->ep_cell[P]    = (uint16_t)(cy * GRID_W + cx);
        w->ep_cross[P]   = (uint8_t)cross;
        w->ep_partner[P] = 0xFF;
        if (w->screen_ep_count[s] < EP_PER_SCREEN_MAX)
          w->screen_ep[s * EP_PER_SCREEN_MAX + w->screen_ep_count[s]++] = (uint8_t)P;
        P++;
      }
    }
  }
  w->ep_count = P;

  /* ---- match partners (the cell across the matched border) -------------- */
  for (uint32_t i = 0; i < P; i++) {
    uint32_t wc = ep_world_cell(w, i);
    int wcx = (int)(wc % BIG_W), wcy = (int)(wc / BIG_W);
    uint32_t c = w->ep_cross[i];
    int nwx = wrap_wcx(wcx + CARD_DCX[c]), nwy = wrap_wcy(wcy + CARD_DCY[c]);
    uint32_t want_cross = c ^ 2u, want = (uint32_t)(nwy * BIG_W + nwx);
    for (uint32_t j = 0; j < P; j++) {
      if (w->ep_cross[j] != want_cross) continue;
      if (ep_world_cell(w, j) == want) { w->ep_partner[i] = (uint8_t)j; break; }
    }
  }

  /* ---- direct edges of the edge-point graph ----------------------------- */
  for (uint32_t i = 0; i < P; i++) {
    for (uint32_t j = 0; j < P; j++) { D2(w, i, j) = (i == j) ? 0u : (uint16_t)D2_INF; NH(w, i, j) = 0xFF; }
  }
  for (uint32_t i = 0; i < P; i++) {                 /* crossing a matched border costs 1 */
    uint8_t pj = w->ep_partner[i];
    if (pj != 0xFF && 1u < D2(w, i, pj)) { D2(w, i, pj) = 1; NH(w, i, pj) = pj; }
  }
  for (uint32_t s = 0; s < N_SCREENS; s++) {          /* same-screen hops cost the Level-1 distance */
    const uint16_t* L1s = w->l1dist + (uint32_t)s * TRI;
    uint32_t cnt = w->screen_ep_count[s];
    for (uint32_t a = 0; a < cnt; a++)
      for (uint32_t b = 0; b < cnt; b++) {
        if (a == b) continue;
        uint32_t i = w->screen_ep[s * EP_PER_SCREEN_MAX + a];
        uint32_t j = w->screen_ep[s * EP_PER_SCREEN_MAX + b];
        uint32_t ca = w->ep_cell[i], cb = w->ep_cell[j];
        if (!l1_reachable(L1s, ca, cb)) continue;
        uint16_t wt = l1_get(L1s, ca, cb);
        if (wt < D2(w, i, j)) { D2(w, i, j) = wt; NH(w, i, j) = (uint8_t)j; }
      }
  }

  /* ---- Floyd-Warshall: all-pairs next-hop ------------------------------- */
  for (uint32_t k = 0; k < P; k++)
    for (uint32_t i = 0; i < P; i++) {
      uint16_t dik = D2(w, i, k);
      if (dik == (uint16_t)D2_INF) continue;
      for (uint32_t j = 0; j < P; j++) {
        uint16_t dkj = D2(w, k, j);
        if (dkj == (uint16_t)D2_INF) continue;
        uint32_t nd = (uint32_t)dik + dkj;
        if (nd < D2(w, i, j)) { D2(w, i, j) = (uint16_t)nd; NH(w, i, j) = NH(w, i, k); }
      }
    }
}

void l2_compute_pg(World* w, uint32_t p) {
  uint16_t* pg = w->pg + p * N_EDGE_MAX;
  uint32_t P = w->ep_count;
  for (uint32_t y = 0; y < N_EDGE_MAX; y++) pg[y] = (uint16_t)D2_INF;
  if (!w->phas[p]) return;

  uint32_t ds = w->pdest_screen[p], dc = w->pdest_cell[p];
  const uint16_t* L1d = w->l1dist + ds * TRI;
  uint32_t cnt = w->screen_ep_count[ds];
  for (uint32_t y = 0; y < P; y++) {
    uint32_t best = INF32;
    for (uint32_t a = 0; a < cnt; a++) {                 /* compose: Level-2 to an entry point + Level-1 in */
      uint32_t e = w->screen_ep[ds * EP_PER_SCREEN_MAX + a];
      uint32_t ce = w->ep_cell[e];
      if (!l1_reachable(L1d, ce, dc)) continue;
      uint16_t d2 = D2(w, y, e);
      if (d2 == (uint16_t)D2_INF) continue;
      uint32_t cand = (uint32_t)d2 + l1_get(L1d, ce, dc);
      if (cand < best) best = cand;
    }
    pg[y] = best > 0xFFFFu ? (uint16_t)D2_INF : (uint16_t)best;
  }
}

uint8_t route_next_dir(const World* w, uint32_t p, uint32_t screen, uint32_t cell) {
  if (!w->phas[p]) return DIR_NONE;
  uint32_t ds = w->pdest_screen[p], dc = w->pdest_cell[p];
  const uint16_t* L1s = w->l1dist + screen * TRI;

  if (screen == ds) {
    if (cell == dc) return DIR_NONE;                     /* arrived */
    if (l1_reachable(L1s, cell, dc)) return l1_next_dir(L1s, cell, dc);
    /* same screen but walled off from the goal: fall through and leave */
  }

  const uint16_t* pg = w->pg + p * N_EDGE_MAX;
  uint32_t best = INF32, bestX = INF32;
  uint32_t cnt = w->screen_ep_count[screen];
  for (uint32_t a = 0; a < cnt; a++) {                   /* cheapest exit: L1 to it + cross + remaining */
    uint32_t X = w->screen_ep[screen * EP_PER_SCREEN_MAX + a];
    uint32_t xc = w->ep_cell[X];
    if (!l1_reachable(L1s, cell, xc)) continue;
    uint8_t pj = w->ep_partner[X];
    if (pj == 0xFF) continue;
    uint32_t g = pg[pj];
    if (g == (uint16_t)D2_INF) continue;
    uint32_t cand = (uint32_t)l1_get(L1s, cell, xc) + 1u + g;
    if (cand < best) { best = cand; bestX = X; }
  }
  if (bestX == INF32) return DIR_NONE;                   /* no route */

  uint32_t xc = w->ep_cell[bestX];
  if (cell == xc) return w->ep_cross[bestX];             /* at the exit: step across the border */
  return l1_next_dir(L1s, cell, xc);
}

uint32_t path_trace(const World* w, uint32_t p, uint16_t* out, uint32_t max) {
  if (!w->phas[p] || max == 0) return 0;
  uint32_t t = p + 1;                                    /* pathed idx -> tank idx */
  int wcx = xy_lo(w->tank_xy[t]) >> SUB_SHIFT;
  int wcy = xy_hi(w->tank_xy[t]) >> SUB_SHIFT;
  wcx = wrap_wcx(wcx); wcy = wrap_wcy(wcy);

  uint32_t count = 0;
  uint32_t cap = max < (BIG_W * BIG_H) ? max : (BIG_W * BIG_H);
  for (uint32_t step = 0; step < cap; step++) {
    uint32_t screen = ((uint32_t)wcy / GRID_H) * SCREENS_X + ((uint32_t)wcx / GRID_W);
    uint32_t cell   = ((uint32_t)wcy % GRID_H) * GRID_W + ((uint32_t)wcx % GRID_W);
    out[count++] = (uint16_t)(wcy * BIG_W + wcx);
    if (screen == w->pdest_screen[p] && cell == w->pdest_cell[p]) break;   /* arrived */
    uint8_t d = route_next_dir(w, p, screen, cell);
    if (d == DIR_NONE) break;
    wcx = wrap_wcx(wcx + CARD_DCX[d]);
    wcy = wrap_wcy(wcy + CARD_DCY[d]);
  }
  return count;
}
