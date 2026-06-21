/* dirtab.h — baked trig for the discrete headings, shared by the move and
 * render transforms. cos/sin in Q14 (value == trig * 16384). No libm. */
#ifndef TANK_DIRTAB_H
#define TANK_DIRTAB_H

#include "defs.h"

extern const int16_t DIR_COS[N_DIRS];
extern const int16_t DIR_SIN[N_DIRS];

#endif
