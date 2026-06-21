#include "sim.h"
#include "tanks_turn.h"
#include "tanks_move.h"
#include "collide.h"

/* initial level, stored directly as row bit-words (no parse step). The ASCII
 * picture in each comment is just documentation; the word is the data.       */
static const uint32_t GRID_INIT[GRID_H] = {  /* bit c == column c */
  0xFFFFF,  /* #################### */
  0x80001,  /* #..................# */
  0x98019,  /* #..##..........##..# */
  0x98619,  /* #..##....##....##..# */
  0x80601,  /* #........##........# */
  0x8C061,  /* #....##.......##...# */
  0x8C061,  /* #....##.......##...# */
  0x80401,  /* #.........#........# */
  0x8C061,  /* #....##.......##...# */
  0x8C061,  /* #....##.......##...# */
  0x80601,  /* #........##........# */
  0x98619,  /* #..##....##....##..# */
  0x98019,  /* #..##..........##..# */
  0x80001,  /* #..................# */
  0xFFFFF,  /* #################### */
};

void sim_init(World* w) {
  for (int32_t r = 0; r < GRID_H; r++) w->grid[r] = GRID_INIT[r];

  /* place the two tanks in known-open cells, facing each other */
  w->tank_xy[0] = xy_pack(2  * SUB + SUB / 2, 7 * SUB + SUB / 2);
  w->tank_ang[0] = 0;        /* east */
  w->tank_xy[1] = xy_pack(17 * SUB + SUB / 2, 7 * SUB + SUB / 2);
  w->tank_ang[1] = 0x8000;   /* west */
  for (uint32_t i = 0; i < N_TANKS; i++) {
    w->tank_in[i] = 0; w->tank_vxy[i] = 0; w->tank_hit[i] = 0;
  }
  w->frame = 0;
  w->move_speed = 20;   /* subcells per tick (~4.7 cells/sec at 60 Hz) */
  w->turn_rate  = 400;  /* angle units per tick                        */

  /* Build the steer's escape table once. Two stages: the 16 local patterns ->
   * per-pattern open-direction masks (collision topology), then masks ->
   * per-(pattern,travel) escape (the steer's scan, precomputed). Both stages are
   * grid-independent, so unlike a per-cell cache there is nothing to rebuild when
   * the grid is edited — the per-tick lookup reads the cell's pattern live. */
  uint32_t pattern_open[N_PATTERNS];
  patterns_build_open(pattern_open);
  tanks_build_escape(w->pattern_escape, pattern_open);
}

void sim_tick(World* w) {
  /* rotate: player turn + auto-steer out of last tick's collision */
  tanks_turn(w->tank_xy, w->tank_ang, w->tank_in, w->tank_hit,
             N_TANKS, w->turn_rate, w->grid, w->pattern_escape);
  tanks_move(w->tank_xy, w->tank_vxy, w->tank_hit,
             w->tank_ang, w->tank_in, N_TANKS, (int32_t)w->move_speed, w->grid);
  w->frame++;
}
