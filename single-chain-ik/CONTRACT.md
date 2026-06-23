# Contract

What the solver and the walk guarantee, and the boundaries. Each item is checked
in `src/test.c` (native, no browser/GPU).

## `ik_solve` — the single-chain solve

Inputs: root `S`, target `T`, segment length, `curl_max`, bend side. Output: world
joint positions (hip..foot), the solved curl, the achieved reach, the clamped goal
distance, a `clamped` flag, and the aim rotation `(cos,sin)`.

- **Reachable target** (target distance within `[min_reach, max_reach]`): the foot
  lands on the target. Measured error ≤ 8 subcells (≈ 0.03 cell) across the
  reachable band and across segment lengths 80–400; typically ≤ 2.
  - `max_reach = sum of segment lengths` (straight, curl 0).
  - `min_reach = reach at curl_max`, the measured bottom of the fold.
- **Unreachable target** (outside the interval): `clamped = 1`, the reach is set to
  the nearest interval end, and the chain is aimed straight at the target (the foot
  is at maximum/minimum extension *toward* it, not on it). Policy is **clamp**.
- **Aim** is a unit rotation: `cos² + sin² == TRIG_ONE²` within rounding.
- **Degenerate** target on the root (zero distance) or zero reach: the aim is left
  at identity (no direction is defined); no crash.
- The root joint never moves: `joint[0] == S`.

### Length-stage premise

`reach` is non-increasing in `curl` on `[0, curl_max]`, so bisection converges.
Near `curl = 0` the reach sits at its flat maximum, where integer rounding wobbles
it by a few units; `curl_max` is taken as the **global** argmin of reach over a
half-turn (not the first dip) so that wobble is never mistaken for the fold bottom,
and a goal within that wobble band of the maximum simply returns ~maximum reach.

## The walk (`sim_tick`)

Over thousands of ticks on the rough terrain:

- **Foot on target**: each leg's solved foot joint is on its gait target (≤ 8
  subcells) unless that leg is clamped.
- **Planted feet are on the ground**: a non-swinging foot's `y` equals
  `terrain_y(foot_x)` exactly.
- **No popping**: a foot moves a bounded amount per tick (≤ 80 subcells), including
  across lift-off, touch-down, gait re-phasing, and stepping over an obstacle.
- **Body above feet**: `body_y` is above the average foot height every tick.
- **First frame is settled**: `gait_init` seats every leg consistent with its
  phase, so tick 0 does not jump.

## Nothing falls under/in the terrain (`test_no_penetration`)

Swept over a full terrain period (every gait phase × every kind of ground):

- **Feet** never sit below the surface at their contact x — the spider never
  sinks into or falls through the ground. Exact to solver rounding (≤ 3 subcells).
  Guaranteed by construction: a swinging foot is clamped to `min(arc, terrain)`,
  and a planted foot is `terrain_y` at its foothold.
- **Body** never dips below the ground beneath its footprint (`body_y + BODY_LOW`
  stays above `terrain_max_y_up` over `[BODY_X0, BODY_X1]`); the gait raises the
  body to clear obstacles directly under it.
- **Interior knees** may dip into a step (the length+direction solve is not
  collision-aware). The renderer draws the **opaque ground in front of the legs**,
  so any such dip is occluded — never visible. The sweep still bounds it
  (< `SUB/2`) to catch a gross regression and reports the real worst.

## Terrain

- `terrain_y` is a pure function of `x` (stepped terraces + sharp obstacle blocks
  + a little roll), **periodic** with period `WORLDP`
  (`terrain_y(x) == terrain_y(x + WORLDP)`), so the walk loops seamlessly and world
  x can grow without a seam. It stores nothing.

## Non-promises

Collision-aware leg posing, end-effector orientation, joint limits, ground
friction/contact, body tilt, and true 3-D posing are out of scope (see README
"Deferred"). This is a visualization of the length+direction method, not a
production IK rig.
