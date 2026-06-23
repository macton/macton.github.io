# Prompt — Create Project: Single-Chain IK = Length + Direction

You are building a self-contained sub-project for an existing site
(`macton.github.io`) that already hosts small data-oriented-design demos. This one
**visualizes a method for single-chain inverse kinematics** by example: a spider
walking over rough terrain, where each leg is one kinematic chain placed by that
method. Like the other demos it is a single web page: the running visualization on
top, a written explanation below, and the live data shown next to the prose.

The **goal is to teach the IK method** — the spider is the vehicle, not the point.
Keep it minimal and clean; don't build for features that don't exist yet.

## The method (this is the subject — implement it, and explain it on the page)

After a 2026 thread by Kiaran Ritchie. Single-chain IK (place the end of a jointed
chain — a foot — on a target) is split into two independent problems:

1. **Length.** Configure the chain's internal joints so the straight-line distance
   from the root (hip) to the end (foot) equals the distance from the hip to the
   target. Direction is irrelevant at this stage; the objective is a single scalar.
2. **Direction.** Rotate the whole solved chain about its root so its hip→foot
   vector points at the target. One rigid rotation, applied at the base joint.

If the chain can reach the target *distance* and then that reach is *aimed* at the
target, the end coincides with the target. A coupled positional solve becomes a
scalar reach plus one rotation — and because the gross aiming is a single rotation
at the base, the motion looks limb-like (a shoulder/hip swings the whole leg).

You must also handle what the thread leaves open and state your policy: the
**reachable interval** (a target nearer than the most-folded pose or farther than
full extension is infeasible — clamp to the nearest end and flag it), the
**null-space** (many internal poses give the same reach — pick one policy), the
**pole / bend side**, and **degenerate** cases (target on the root, zero reach).

## Specialize to the plane (a deliberate, stated decision)

The page is a **2-D side elevation** rendered with the site's instanced-quad
pipeline, so specialize the method to the plane — and say so on the page and in the
README. This is the *simplest machine* that shows the two stages clearly, and it
makes both exact:

- The internal pose becomes a **single scalar** (a uniform per-joint "curl":
  **each successive segment turns by the same signed angle from the previous — a
  constant-curvature arc**), so **Length is a 1-D monotone root-find**: more curl
  folds the chain in, so reach decreases from the segment-length sum down to the
  bottom of the fold. **Bisect the curl** against the target distance, comparing
  *squared* lengths so there is no square root in the loop and no derivative.
  Establish the interval `[0, curl_max]` by **measuring** the fold's bottom: scan a
  half-turn and take the **global** reach minimum — reach is **flat and
  rounding-noisy near curl 0**, so a greedy "stop at the first increase" wrongly
  returns `curl_max ≈ 0` (a real trap). Monotonicity on `[0, curl_max]` is the
  bisection's precondition.
- **Direction (FromTo) is exact in the plane** from a dot and a cross: the rotation
  from the solved end vector `E` to the target vector `D` has `cos ∝ E·D` and
  `sin ∝ E×D`. Recover the unit `(cos,sin)` by normalizing the **single vector
  `(E·D, E×D)`**, whose own magnitude is `|E||D|` — one integer reciprocal-length.
  Do **not** divide by two separately-rooted lengths `|E|·|D|`; it loses precision
  badly when the chain is folded. Allow a **small fixed-point tolerance** on the
  aim's unit length (subcell quantization of a folded `E` leaves a few-percent
  residual). No `atan2`, no quaternion antipode care.

Note that the full 3-D method generalizes both (a quaternion FromTo; a bend-plane /
pole vector picks the null-space pose) and list **3-D as deferred** — don't build
it.

## Binding guidance — read first, follow strictly

These are requirements, not suggestions.

1. **`data-oriented-design.md`** — Mike Acton's rules (raw:
   `https://raw.githubusercontent.com/macton/nagent/master/context/data-oriented-design.md`).
   Understand the real data first; write the simplest machine that turns the input
   you have into the output you need; state costs; exploit every constraint
   (including: **you own the terrain — shape it so downstream constraints hold**);
   solve only the problem you have.
2. **`AGENTS.md`** at the repo root — every rule applies and this project is a
   textbook case for many: **integer fixed point only** (float only at the GPU
   boundary); **no dynamic allocation**; **sim/render/wasm separation** with a
   one-way dependency and **native tests** with no browser/GPU; **don't store
   derivable data** (one cosine table, sine read a quarter-turn over); **author
   build-time data on the host and bake it** (the trig table); **keep
   presentation-only state out of the sim struct**; **size every field to its real
   domain**; **mobile-first, primary subject on top, cache-busted version**.

The IK solve, the gait, the terrain, and the renderer are independent transforms —
give each its own file (the solver is a **pure function over a flat result**, no
`World`, so the tests exercise it directly).

## What to deliver

- **A simulation core in C** over a single flat `World`: the per-tick update is
  *gait → IK per leg*. Freestanding `wasm32` (no libc/libm/allocation), integer
  fixed point. The IK output (the joint positions and per-leg solve read-outs) is
  the project's actual product, so it is core sim output the renderer, the page,
  and the tests read — not presentation decoration. Positions are **Q8.8
  subcells** (the shared instance shader divides centres/extents by 256); angles
  are a **full-turn 16-bit value**; trig is **one baked cosine table, interpolated**
  so solver precision isn't limited to a table step (sine read a quarter-turn over).
  The world wraps at a fixed width; legs are a few cells long.
- **A pure IK module** implementing length-then-direction (the function under
  test), plus a **gait** module (the IK's input generator), a **terrain** module,
  a **renderer** (reads `World`, builds the packed instance buffer), and the
  **wasm boundary** (the only wasm-specific file: owns the `World` + instance
  buffer, exports the protocol).
- **Native tests** (no browser/GPU) — see Validation below.
- **The WebGPU page**: the scene on top, the method written up below, live per-leg
  solve read-outs and tunables embedded next to the prose, a teaching overlay.
- **`build.sh`/`test.sh`/`gen.sh`** (gen bakes the trig table), the committed
  `.wasm`, a visible **cache-busted version**, a **README** (link the DOD doc and
  the method source; list deferrals), and a **CONTRACT** that you test.

## The spider rig & gait — pin these (so the result is reproducible)

The *representation* (fixed-point format, angle encoding, table sizes, exact
constants) you derive from the data and the docs. The decisions below are the
product contract — make them so:

- **Eight legs, each a chain of three segments** (femur, tibia, tarsus:
  hip → knee → knee → foot). Solve each leg with the same length+direction
  function — a *single-chain* primitive applied 8×.
- **Anatomy (match a real spider):** legs mount on the **cephalothorax** (the
  front body section), clustered there; the larger **abdomen** trails behind and
  carries **no** legs. Feet **fan out** wide, front legs forward and rear legs
  sweeping back past the abdomen.
- **Legs arch up** to a high knee, the iconic spider profile. Get this for free
  from the method: make the legs **longer than the hip-to-foot reach** so the
  length stage folds them into an arch; choose the **bend side so the arch is up**,
  and keep every foot clearly to one side of its hip so the bend side never has to
  flip mid-stride (which would pop). Segments **taper** (long femur → thin foot).
- **2.5-D depth read in a flat side view:** four near legs and four far legs. The
  far legs' **hip mount sits a little higher/behind — that is leg geometry, a sim
  input that changes the solve** — while their **dimmer colour and thinner width
  are render-only**. **Foreshorten the middle legs** (a per-leg length scale) — in
  3-D they point sideways and read shorter in profile — which also stops a long leg
  from coiling when its foot is nearly under the hip.
- **Gait:** the body walks at constant speed; a **distance-based** phase (so cadence
  tracks speed) steps two **alternating groups** (a diagonal tetrapod) half a cycle
  apart, with swing **under half** the cycle so a support set is always down. Each
  leg has a **stance offset** (its fan): the foot's home is `hip + stance`; it
  **plants there and stays fixed in the world** while the body advances — *this is
  what stretches the chain and gives the IK something to solve* — then on its phase
  it **swings forward ~one stride** to the next foothold and re-plants. **Choose the
  stance offsets and the leg length together so the home reach (and the ±half-stride
  of swing) stays inside the reachable interval** — or feet clamp constantly (a real
  trap: an over-fanned rear foot is simply unreachable). A **swinging foot arcs
  between its two footholds** (interpolate the foothold heights + a lift **sized to
  clear the terrain between them**, then hard-clamp to `min(arc, terrain)` so it
  never sinks). **Body height = average foot height − stand height** (smooth over
  steps), **then raised only if the ground under the body's own footprint would
  otherwise poke through** (so it clears an obstacle beneath it without popping).
- **Tunables on the page** (walk speed, leg/segment length, stand height, stride,
  foot lift), all integer subcells; shrinking the leg length should drive targets
  out of reach so you can watch the **clamp** flag. (Changing stride live re-phases
  the gait — a one-frame adjustment; acceptable for a tunable.)

## The terrain — pin this

The ground is **rough on purpose**: **stair steps** (flat treads with vertical
risers) **plus a few sharp flat-topped obstacle blocks**. Keep it a **pure
function of world x that stores nothing** and **loops seamlessly** (periodic over
the world width), so the walk can run forever with no seam and world-x can grow
without one. (A quantized low-frequency wave gives terraces for free; a small table
of `{x0,x1,height}` boxcars gives obstacles.) Steps and obstacles must be visible
and the steps adjustable in size. **You own this terrain — size the obstacles
(height, and width ≥ a few cells) so the gait's swing can step onto and over them
within its foot-speed budget; a very tall or thin obstacle forces a foot to flick
across it in one swing.** State the max obstacle height the gait clears.

## Rendering — pin the occlusion decision

- Reuse the site's **packed 16-byte instance quad** (centre, half-extent, a
  `(cos,sin)` rotation, packed RGBA), all integer, divided by their scale in the
  shader. Emit instances **relative to a follow camera** (the body horizontally; a
  steady ground line vertically) so the walker stays framed and values stay in
  `int16` though world-x grows. Rebuild the whole view each frame.
- **Draw the opaque ground IN FRONT of the legs**, and the **feet on top of the
  ground**. The length+direction solve places the *foot*, not the whole chain, so a
  lower leg can dip into a step; drawing the ground over the legs **occludes** that
  dip exactly like a side-scroller's foreground, so a leg is never seen under the
  ground while feet and body stay visible. (This is what lets the terrain be
  genuinely rough without a collision-aware solver — list collision-aware legs as
  deferred.)
- **Contrast:** light the scene so the dark legs read against the background.
- A **teaching overlay** for one focused leg, **off by default**, toggled on the
  page: show the length-only pose (the chain folded to the right reach but *not yet
  aimed*, recovered by un-rotating the solved chain) faintly, alongside the solid
  aimed leg and the target marker, so the two stages are visible on the live spider.

## Validation — the sweep (native, no GPU)

Beyond the solver's own tests (a reachable target ends under the foot to a few
subcells across the reachable band and across segment lengths; the aim is a unit;
an out-of-reach target is clamped and aimed straight at the target; the length
objective is usable across the band), **sweep the whole terrain period × every gait
phase** and validate that **nothing falls under/into the terrain**, in tiers:

- **Feet** (the contacts) are never below the surface — the spider never sinks into
  or falls through the ground. This is exact (to solver rounding); guarantee it by
  construction (a swinging foot clamped to `min(arc, terrain)`, a planted foot on
  the terrain at its foothold). Test the foot **target** against the terrain at its
  own x (the terrain is discontinuous, so sampling at the *solved* x right at a
  riser is meaningless).
- **Body** never dips below the highest ground under its footprint (the gait clears
  it).
- **Knees** may dip into a step (the solve isn't collision-aware) but are
  **occluded** by the foreground ground — never visible. Bound them anyway to catch
  a gross regression, and report the real worst.

Also test the walk invariants: a planted foot sits exactly on the terrain; a foot
never **pops** (moves more than a small bound per tick), including over a step —
note a foot is stationary in stance and sweeps in swing, so its peak per-tick speed
is several× the body speed, so **bound the pop at the default walk speed (or scale
the bound with speed)**, not as an absolute; the body stays above its feet; the
**first frame is already settled** (seat each leg consistent with its phase so tick
0 doesn't jump).

## Definition of done

- Builds freestanding to wasm and natively; `test.sh` passes (solver + walk +
  sweep). No floating point in the sim; no allocation; one cosine table baked on
  the host; data packed and sized to its domain.
- The page shows the spider on rough terrain, the live per-leg solve (curl, reach,
  goal, aim, clamp), the tunables, and the optional two-stage overlay, and explains
  the length+direction method with the live data in context.
- You can state, for each data layout and transform, what it costs and why it is
  shaped that way — and the two-stage method is legible both in the prose and in
  the moving picture.
