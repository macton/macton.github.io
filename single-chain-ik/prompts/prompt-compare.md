# Experiment — clean-room rebuild & instruction-gap check

A repeatable experiment for this sub-project. The goal is **not** to produce a
build to keep — it is to test whether our written guidance
(`data-oriented-design.md` + `AGENTS.md` + `prompts/create-project.md`) is, on its
own, enough to steer a fresh build to the same data-oriented result and the same
product decisions we arrived at by iteration. Where the rebuild diverges, the
divergence is a gap in the instructions; close it.

Run this after the project has stabilized, and any time the guidance changes.

## Inputs

- `prompts/create-project.md` — the task prompt (the spec under test).
- `AGENTS.md` (repo root) — the accumulated working instructions.
- `data-oriented-design.md` — raw:
  `https://raw.githubusercontent.com/macton/nagent/master/context/data-oriented-design.md`
- The existing `single-chain-ik/` — the baseline to compare against (the
  experimenter reads it; the builder must **not**).

## Procedure

1. **Build clean-room.** Spawn a subagent (general-purpose) with these hard
   constraints:
   - Work only in a temp dir (e.g. `/tmp/gen-single-chain-ik`).
   - **Do not read `single-chain-ik/`** — seeing the answer contaminates the result.
   - Read only `create-project.md` + `AGENTS.md`, and fetch the DOD doc.
   - Actually build it: compile the wasm (`clang --target=wasm32 …`) and run the
     native tests on the host; iterate until both pass.
   - Report back: the file tree; the real data structures (paste them — `World`,
     the IK result struct, the instance layout, the leg config); the per-tick
     transforms (gait, the length bisection, the FromTo aim) and the gait/terrain
     behavior; the numeric formats; build size + test count; **and, most important,
     every place the prompt or `AGENTS.md` was ambiguous, underspecified,
     contradictory, or guessed at.** Finding gaps is the point — tell the agent so.
2. **Compare** the rebuild against `single-chain-ik/` on: data layout (sizes,
   packing, SoA, what lives in the sim struct vs render); the two-stage solver
   (length = scalar/bisection on squared reach? direction = dot/cross FromTo? or
   did it reach for analytic / atan2 / a different null-space policy?); the gait
   (distance-phased tetrapod? planted-foot-fixed? swing arc with terrain-clearing
   lift?); the terrain (pure periodic function? terraces + obstacles?); the rig
   (cephalothorax/abdomen, arch-up, taper, foreshortened middle legs); the
   rendering occlusion decision; the no-penetration validation and its tiers; the
   clamp policy; sim/render/wasm separation; tests covered; and completeness.
   Verify rebuild claims by reading its files and running its build/tests.
3. **Answer the two questions** for the user:
   - *Is anything improved?* Anything the rebuild did better (smaller/clearer
     representation, a realization we missed) — adopt it if it's a real win.
   - *Are there gaps in the instructions?* Anywhere the two builds diverged on
     something that should have been determined by the guidance.
4. **Close the gaps.**
   - A product decision the prompt left open but that should be fixed → pin it in
     `create-project.md` (representation stays derived from the docs).
   - A missing or misread principle → add/refine an instruction in `AGENTS.md`
     (per its maintenance protocol: imperative rule + a short origin note).
   - A real improvement the rebuild found → fold it into `single-chain-ik/`.

## What this run found

**Run 1** (general-purpose subagent, clean-room from `create-project.md` + `AGENTS.md`
+ the DOD doc; it did not read `single-chain-ik/`). It built the whole thing, link
+ native tests passing, zero float in the sim — and independently reproduced
**every core decision**: length = bisection on *squared* reach over a single
uniform-curl DOF (no sqrt/derivative in the loop); `curl_max` *measured* as the
global reach minimum; direction = exact planar FromTo normalizing the single
`(E·D, E×D)` vector; distance-phased diagonal tetrapod; world-fixed planted feet;
swing arc with a terrain-clearing lift hard-clamped to `min(arc, terrain)`; body =
average foot height floored by terrain beneath it; terrain a pure periodic function
(quantized terraces + boxcar obstacles); cephalothorax/abdomen rig with arch-by-
folding, tapered segments, foreshortened middle legs, near/far depth; **opaque
ground drawn in front of the legs to occlude leg dips**; the tiered no-penetration
sweep (feet 0, body 0, knees occluded/bounded); Q8.8, full-turn 16-bit angle, one
baked Q14 cosine with sine a quarter-turn over; presentation kept out of the sim.
It even hit the **same `curl_max` early-break bug** the reference did. Nothing in
the rebuild was better enough to adopt back.

So the prompt steers the *architecture* well. The divergences were all in
**unpinned magnitudes and a few underspecified constructions** — closed in
`create-project.md` this run:

- **Segment count was "a few"** → builder chose 3 (matches), but 2/4 would change
  the arch/taper/`curl_max`. → pinned "three segments (femur/tibia/tarsus)."
- **"Uniform curl" was ambiguous** (constant curvature vs one shared interior
  angle vs distributed fold). → defined as "each successive segment turns by the
  same signed angle (a constant-curvature arc)."
- **`curl_max` measurement** — the builder's first early-break ("stop at first
  reach increase") returned `curl_max ≈ 0` on a flat-near-zero chain, the exact
  reference bug. → pinned "reach is flat/noisy near curl 0; take the **global**
  minimum, don't break on first increase."
- **Aim normalization** — the builder first divided by two separate lengths
  `|E|·|D|` (up to 18% off unit when folded) before switching to normalizing the
  `(E·D, E×D)` pair. → pinned that method explicitly + "allow a small fixed-point
  tolerance on the aim's unit length."
- **Foothold placement** was the biggest gap: with no construction, the builder
  first over-fanned rear feet beyond reach → ~31% permanently clamped. → pinned the
  stance/home construction and "choose stance offsets + leg length so the home
  reach (±half-stride) stays inside the reachable interval."
- **Body height** — "average foot height" vs "lift over an obstacle" tensioned; the
  builder's first reading (max terrain under footprint) popped the body per riser.
  → pinned "average foot height − stand, then raised only if the ground under the
  body footprint would poke through."
- **No-pop bound** is intrinsically speed-dependent (foot peak speed is several×
  body speed). → the validation section now says bound it at the default walk speed
  / scale with speed, not as an absolute.
- **Obstacle size vs the pop/no-sink contract** conflict — the builder had to
  reshape its terrain to make the gait cope. → added "you own the terrain; size
  obstacles to what the gait clears" (and to the DOD "exploit constraints" line).
- **Near/far hip offset: sim or render?** The builder (correctly) put the depth
  hip-raise in the sim and only dimming in render. → pinned that split.
- **Scale/encoding were "derive it"** — both builds converged (Q8.8, 16-bit turn,
  1024-entry cosine), but the table must be **interpolated** or curl/foot precision
  is table-step-limited. → pinned the Q8.8 (shader-fixed) scale, the 16-bit angle,
  and "interpolate the cosine table."
