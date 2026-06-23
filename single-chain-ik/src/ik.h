/* ik.h — single-chain inverse kinematics as LENGTH then DIRECTION.
 *
 * This is the method the page is about (after Kiaran Ritchie's decomposition):
 * split a chain's reach-the-target solve into two independent problems.
 *   1. LENGTH    — configure the chain's internal joints so the straight-line
 *                  distance from root to end equals the root-to-target distance.
 *                  Direction is irrelevant here; the objective is one scalar.
 *   2. DIRECTION — rotate the whole solved chain about its root so its
 *                  root-to-end vector points at the target. One rigid rotation.
 * If the chain can reach the target distance and then points that reach at the
 * target, the end coincides with the target.
 *
 * This is a pure function over a flat result — no World, no globals, no I/O — so
 * the solver is exercised directly by the native tests (test.c).
 *
 * SPECIALISATION TO THE PLANE. The page draws a side elevation, so a leg bends
 * in one known plane. Two things the 3D method must wrestle with then become
 * trivial and exact, which is what makes the two stages so legible here:
 *   - the internal pose is a single scalar (a uniform joint "curl"), so LENGTH is
 *     a 1-D monotone root-find solved by bisection on SQUARED length — no
 *     derivative, no sqrt, and no choice of which of many poses to take beyond a
 *     fixed bend side (the "pole");
 *   - in the plane the FromTo rotation of DIRECTION is a single angle recovered
 *     exactly from a dot and a cross, with none of the quaternion antipode care.
 * The full 3-D method generalises both (quaternion FromTo; a bend-plane / pole
 * vector picks the null-space pose). */
#ifndef SCIK_IK_H
#define SCIK_IK_H

#include <stdint.h>
#include "defs.h"

typedef struct {
  int32_t  jx[N_JOINT], jy[N_JOINT]; /* world joint positions, hip[0] .. foot[N_SEG] */
  uint16_t curl;     /* LENGTH answer: the solved per-joint bend (ANG units)         */
  int32_t  reach;    /* achieved |end - root|, subcells                              */
  int32_t  goal_d;   /* the (clamped) target distance aimed for, subcells            */
  uint8_t  clamped;  /* 1 if the target was outside the reachable interval           */
  int16_t  dcos, dsin; /* DIRECTION answer: the hip aim rotation, Q14 unit           */
} IkResult;

/* Solve one chain: root at (sx,sy), target at (tx,ty), N_SEG segments each
 * seg_len long, internal bend limited to [0, curl_max] (from ik_curl_max),
 * bending to the side given by bend_sign (+1 / -1 — the pole choice). */
IkResult ik_solve(int32_t sx, int32_t sy, int32_t tx, int32_t ty,
                  int32_t seg_len, uint16_t curl_max, int8_t bend_sign);

/* Squared root-to-end distance at a given curl — the LENGTH objective, exposed so
 * the tests can confirm it is monotone on the bisection interval. */
int64_t ik_reach2(uint32_t curl, int32_t seg_len, int8_t bend_sign);

/* The largest curl for which reach is still monotonically decreasing (the bottom
 * of the fold). Bisection runs on [0, curl_max]; below it the squared-length
 * objective is strictly monotone, which is the bisection's precondition. */
uint16_t ik_curl_max(int32_t seg_len, int8_t bend_sign);

#endif
