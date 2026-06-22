#include "sim.h"
#include "tanks_turn.h"
#include "tanks_move.h"
#include "escape_table.h"   /* PATTERN_ESCAPE — generated, baked in */

/* initial level, stored directly as row bit-words (no parse step). The ASCII
 * picture in each comment is just documentation; the word is the data. Each
 * outer edge has a one-cell gap at its centre, matched across opposite edges
 * (top/bottom at column 10, left/right at row 7), so the toroidal wrap passes
 * straight through by default.                                                 */
static const uint32_t GRID_INIT[GRID_H] = {  /* bit c == column c */
  0xFFBFF,  /* ##########.######### */
  0x80001,  /* #..................# */
  0x98019,  /* #..##..........##..# */
  0x98619,  /* #..##....##....##..# */
  0x80601,  /* #........##........# */
  0x8C061,  /* #....##.......##...# */
  0x8C061,  /* #....##.......##...# */
  0x00400,  /* ..........#......... */
  0x8C061,  /* #....##.......##...# */
  0x8C061,  /* #....##.......##...# */
  0x80601,  /* #........##........# */
  0x98619,  /* #..##....##....##..# */
  0x98019,  /* #..##..........##..# */
  0x80001,  /* #..................# */
  0xFFBFF,  /* ##########.######### */
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
  w->move_speed = 10;     /* subcells per tick (~2.3 cells/sec at 60 Hz) */
  w->turn_rate  = 200;    /* angle units per tick                        */
  w->collide_scale = 128; /* Q0.8: move at 50% speed while colliding      */
  /* No escape table to build: PATTERN_ESCAPE is generated on the host and baked
   * into the binary (src/escape_table.c). The per-tick steer reads it directly. */
}

void sim_tick(World* w) {
  /* rotate: player turn + auto-steer out of last tick's collision */
  tanks_turn(w->tank_xy, w->tank_ang, w->tank_in, w->tank_hit,
             N_TANKS, w->turn_rate, w->grid, PATTERN_ESCAPE);
  tanks_move(w->tank_xy, w->tank_vxy, w->tank_hit, w->tank_ang, w->tank_in,
             N_TANKS, (int32_t)w->move_speed, (int32_t)w->collide_scale, w->grid);
  w->frame++;
}
