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

<!-- Filled in after the first run; see git history for the prompt edits each run
     motivated. -->
