/* tanks_path — the per-tick path-follow transform for the pathed tanks.
 *
 * A pathed tank's "input" is derived from the path tables (the next cardinal
 * toward its goal) and written into the SAME tank_in bitfield the manual tank
 * uses, so it flows through the SAME tanks_turn/tanks_move model — the movement
 * transform is not forked. The rule for turning a cardinal into button bits:
 * turn (LEFT/RIGHT) toward the cardinal's heading; drive (FWD) only once the
 * discrete heading index is exactly that cardinal. Because tanks_move steps
 * along di = angle>>ANGLE_SHIFT (the discrete heading), "exactly aligned" means
 * the motion is exactly axis-aligned — so a pathed tank tracks the grid cells
 * and never clips a wall corner, with no special movement code.
 *
 * Runs before tanks_turn each tick. Reads the path tables + the tank's pose;
 * writes tank_in for the pathed tanks (and their pgoal/pstatus readouts). The
 * manual tank's input is left untouched (it comes from set_input). */
#ifndef TANK_PATH_H
#define TANK_PATH_H

typedef struct World World;

void tanks_path(World* w);

#endif
