#include "sim.h"
#include "tanks_turn.h"
#include "tanks_move.h"
#include "tanks_steer.h"

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
  w->tank_x[0] = (int16_t)(2  * SUB + SUB / 2); w->tank_y[0] = (int16_t)(7 * SUB + SUB / 2);
  w->tank_ang[0] = 0;        /* east */
  w->tank_x[1] = (int16_t)(17 * SUB + SUB / 2); w->tank_y[1] = (int16_t)(7 * SUB + SUB / 2);
  w->tank_ang[1] = 0x8000;   /* west */
  for (uint32_t i = 0; i < N_TANKS; i++) {
    w->tank_in[i] = 0; w->tank_vx[i] = 0; w->tank_vy[i] = 0; w->tank_hit[i] = 0;
  }
  w->frame = 0;
  w->move_speed = 20;   /* subcells per tick (~4.7 cells/sec at 60 Hz) */
  w->turn_rate  = 400;  /* angle units per tick                        */
}

void sim_tick(World* w) {
  tanks_turn(w->tank_ang, w->tank_in, N_TANKS, w->turn_rate);
  tanks_move(w->tank_x, w->tank_y, w->tank_vx, w->tank_vy, w->tank_hit,
             w->tank_ang, w->tank_in, N_TANKS, (int32_t)w->move_speed, w->grid);
  /* keep a throttled tank from getting stuck against walls/corners/pockets */
  tanks_steer(w->tank_x, w->tank_y, w->tank_ang, w->tank_in, w->tank_hit,
              N_TANKS, w->turn_rate, w->grid);
  w->frame++;
}
