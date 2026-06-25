/* view.h — chapter 4's projection is now a TOP-DOWN PERSPECTIVE camera with a slight
 * angled offset, built host-side as a model-view-projection matrix (see app.js) and
 * applied in the vertex shader. It is still PRESENTATION: the simulation never sees
 * it, render.c emits world placements, and the camera is a uniform (the MVP), not a
 * rebuild. This header keeps the one render-side constant the C still needs — the
 * depth KEY that orders the translucent FX pass back-to-front.
 *
 * The opaque pass uses the GPU z-buffer (the perspective matrix writes real depth), so
 * there is no CPU sort. The translucent pass (laser glow, destruction bursts) is
 * painter-sorted by this key. The camera tilts only in the y–z plane (top-down,
 * leaning south), so "nearer the camera" grows with +y (south) and +z (up) and is
 * independent of x — an approximation that is exact enough for the few, mostly
 * ground-level FX that ever overlap. */
#ifndef TANK_VIEW_H
#define TANK_VIEW_H

#include "defs.h"

/* larger == nearer the camera (drawn last). Monotone in +y and +z, flat in x. */
static inline int32_t view_depth_key(int32_t wx, int32_t wy, int32_t wz) {
  (void)wx; return wy + wz;
}

#endif
