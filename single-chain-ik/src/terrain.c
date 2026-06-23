#include "terrain.h"
#include "defs.h"

/* Four cosine octaves. Frequencies are how many full turns each octave makes per
 * WORLDP (1,2,3,5 — coprime-ish so the silhouette looks rough, not obviously
 * tiled); amplitudes are in subcells and fall off so the low octave rolls and the
 * high ones add the bumps. With WORLDP a power of two, x -> angle is a shift:
 * one turn over WORLDP means angle = x * (ANG_FULL/WORLDP) = x << 1. */
#define OCTAVES 4
static const int32_t FREQ[OCTAVES] = { 1, 2, 3, 5 };
static const int32_t AMP [OCTAVES] = { 110, 60, 38, 22 };   /* sum 230 sub p2p ~1.8 cells */

int32_t terrain_y(int32_t x) {
  int32_t h = 0;                                  /* undulation, Q14-weighted sum */
  for (int o = 0; o < OCTAVES; o++)
    h += AMP[o] * icos((uint32_t)(x * FREQ[o]) << 1);
  /* >>14 turns the Q14-weighted sum back into subcells; subtract so peaks (cos>0)
   * raise the ground (smaller y). */
  return TERRAIN_MID - (h >> 14);
}
