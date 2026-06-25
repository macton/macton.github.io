/* iso.h — the chapter-4 projection. This is the ONLY new math the chapter adds,
 * and it is a PURE FUNCTION on the RENDER side: the simulation never calls it and
 * never sees its output. render.c emits world placements (camera-local subcells);
 * the GPU vertex shader projects them with this same basis (the constants are
 * exported from wasm so the shader reads one source of truth); and the native
 * test exercises the C functions below directly — no browser, no GPU.
 *
 * DIMETRIC 2:1 (the classic game-isometric diamond):
 *
 *     screenX = (wx - wy) * ISO_X
 *     screenY = (wx + wy) * ISO_Y - wz * ISO_Z
 *
 * with ISO_X : ISO_Y == 2 : 1, so a cell's footprint reads twice as wide as it is
 * tall. ISO_Z is the height scale (world height -> screen rise). wx, wy, wz are in
 * SUBCELLS (the sim's Q8.8 unit, SUB == one cell), in the camera-local frame
 * render.c builds (origin at the viewport screen's corner), so they never wrap and
 * stay small. The projection is orthographic — no perspective foreshortening —
 * which is exactly what "isometric" means: an orthographic view down a diagonal.
 *
 * This is presentation: ISO_X/Y/Z, the height, and the camera are render-only and
 * appear nowhere in the World. Panning/zoom/follow live entirely in the shader's
 * view uniform (scale + offset applied AFTER this projection); changing the camera
 * moves the picture without changing one emitted placement. */
#ifndef TANK_ISO_H
#define TANK_ISO_H

#include "defs.h"

/* the dimetric basis, in screen units per subcell (integer: the projection is
 * exact and testable; absolute scale is irrelevant — the camera's zoom maps these
 * units to NDC). ISO_X == 2*ISO_Y pins the 2:1 diamond. */
#define ISO_X 2
#define ISO_Y 1
#define ISO_Z 2
_Static_assert(ISO_X == 2 * ISO_Y, "dimetric projection must be 2:1 (ISO_X : ISO_Y)");

typedef struct { int32_t sx, sy; } IsoPt;

/* project a world placement (camera-local subcells) to screen units. Pure. */
static inline IsoPt iso_project(int32_t wx, int32_t wy, int32_t wz) {
  IsoPt p;
  p.sx = (wx - wy) * ISO_X;
  p.sy = (wx + wy) * ISO_Y - wz * ISO_Z;
  return p;
}

/* the depth / painter's key: LARGER == NEARER (drawn in front). It is monotone
 * along the view diagonal — stepping one cell south (+wy), east (+wx), or up (+wz)
 * strictly increases it — so a thing one cell "north-west" can never sort in front
 * of a thing one cell "south-east". The opaque pass maps this into the z-buffer
 * (per vertex); the translucent FX pass sorts back-to-front by it. */
static inline int32_t iso_depth(int32_t wx, int32_t wy, int32_t wz) { return wx + wy + wz; }

/* the depth-key range, for the shader's map into [0,1] NDC depth. Placements are
 * WORLD positions (anchor = world origin), so wx+wy+wz spans [0, (BIG_W+BIG_H)*SUB +
 * height], and this bound holds every emitted vertex with headroom. */
#define ISO_WZ_MAX     (4 * SUB)
#define ISO_DEPTH_SPAN ((BIG_W + BIG_H) * SUB + ISO_WZ_MAX)

#endif
