/* defs.h — shared vocabulary: fixed sizes and the fixed-point conventions the
 * whole module exploits. These are constraints, not parameters to expose.
 *
 * NUMBERS ARE INTEGER FIXED POINT.
 *   lengths / positions   subcells, SUB == 1 "cell" (Q-format, int32_t)
 *   angles                ANG_FULL == 65536 units per turn (see trig.h)
 *   cos / sin             Q14 (TRIG_ONE == 16384)
 * Float appears in exactly one place: the GPU, which is float hardware. The host
 * divides the packed integer instance fields by their scale in the shader; the
 * simulation never sees a float.
 *
 * THE SCENE. A side elevation of an 8-legged walker crossing rough ground that
 * scrolls past as it walks. Each leg is one kinematic chain of N_SEG segments
 * from a hip (on the body) to a foot (planted on the terrain). Per tick the gait
 * decides where each foot should be; the IK (ik.c) places the chain so the foot
 * lands there. That single-chain solve IS the subject of the page. */
#ifndef SCIK_DEFS_H
#define SCIK_DEFS_H

#include <stdint.h>
#include "trig.h"

/* ---- fixed-point scale ---------------------------------------------------- */
#define SUB        256        /* subcells per "cell" (Q8.8 within a cell)      */
#define SUB_SHIFT  8

/* ---- the walker ----------------------------------------------------------- */
#define N_LEGS     8          /* four pairs, read as a spider in side view     */
#define N_SEG      3          /* segments per leg (hip-femur-tibia-foot)       */
#define N_JOINT    (N_SEG + 1)/* 4 points per leg: hip, two knees, foot        */

/* ---- the ground ----------------------------------------------------------- */
/* The terrain is a pure function of world x (a sum of cosine octaves read from
 * the trig table — see terrain.c), so nothing is stored and it repeats every
 * WORLDP subcells. WORLDP is a power of two so x -> table-angle is a shift. */
#define WORLDP     32768      /* terrain period in subcells (== 128 cells)     */
#define TERRAIN_MID (6 * SUB) /* mean ground line, world-y (y increases down)  */

/* ---- defaults for the runtime tunables (see sim.h: World) ----------------- */
/* All are editable on the page; they are the knobs that make the solver's
 * behaviour visible. Shrinking seg_len starves the reach until targets fall
 * outside the reachable interval and the solver has to clamp (watch the flag). */
#define DEF_WALK_SPEED  10    /* body advance, subcells per tick (~2.3 cells/s) */
#define DEF_SEG_LEN    235    /* one segment length, subcells (max reach 3x)    */
#define DEF_STAND_H    320    /* body height above its feet, subcells           */
#define DEF_STEP_LEN   280    /* stride length / gait period, subcells          */
#define DEF_STEP_LIFT  150    /* peak foot lift during a swing, subcells        */

#endif
