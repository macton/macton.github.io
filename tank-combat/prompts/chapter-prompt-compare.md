# Experiment — clean-room chapter rebuild & instruction-gap check

A repeatable experiment for this book. The goal is **not** to produce a chapter
to keep — it is to test whether our written guidance (`data-oriented-design.md` +
`AGENTS.md` + the chapter's own `create-chapter-N.md` prompt) is, on its own,
enough to steer a fresh build to the same data-oriented result we arrived at by
iteration. Where the rebuild diverges, the divergence is a gap in the
instructions; close it.

Run this after a chapter has stabilized, and any time the guidance changes.

## Inputs

- `prompts/create-chapter-N.md` — the chapter's task prompt (the spec under test).
- `AGENTS.md` (repo root) — the accumulated working instructions.
- `data-oriented-design.md` — raw:
  `https://raw.githubusercontent.com/macton/nagent/master/context/data-oriented-design.md`
- The existing `chapter-N/` — the baseline to compare against (the experimenter
  reads it; the builder must **not**).

## Procedure

1. **Build clean-room.** Spawn a subagent (general-purpose) with these hard
   constraints:
   - Work only in a temp dir (e.g. `/tmp/gen-chapter-N`).
   - **Do not read `chapter-N/`** — seeing the answer contaminates the result.
   - Read only `create-chapter-N.md` + `AGENTS.md`, and fetch the DOD doc.
   - Actually build it: compile the wasm (`clang --target=wasm32 …`) and run the
     native tests on the host; iterate until both pass.
   - Report back: the file tree; the real data structures (paste the structs —
     `World`/SoA, grid, angle, color, instance layout); the per-tick transforms
     and collision/steer behavior; the numeric formats; build size + test count;
     **and, most important, every place the prompt or `AGENTS.md` was ambiguous,
     underspecified, contradictory, or guessed at.** Finding gaps is the point —
     tell the agent so.
2. **Compare** the rebuild against `chapter-N/` on: data layout (sizes, packing,
   SoA, what lives in the sim struct vs render), the transforms and their
   separation (sim/render/wasm one-way dependency), what is precomputed vs live
   and keyed by what, tests (cases covered), behavior chosen, and completeness.
   Verify rebuild claims by reading its files and running its build/tests.
3. **Answer the two questions** for the user:
   - *Is anything improved?* Anything the rebuild did better (smaller/clearer
     representation, a realization we missed) — adopt it if it's a real win.
   - *Are there gaps in the instructions?* Anywhere the two builds diverged on
     something that should have been determined by the guidance.
4. **Close the gaps.**
   - Behavior the prompt left open but that should be fixed → pin it in
     `create-chapter-N.md` (representation stays derived from the docs).
   - A missing or misread principle → add/refine an instruction in `AGENTS.md`
     (per its maintenance protocol: imperative rule + a short origin note).
   - A real improvement the rebuild found → fold it into `chapter-N/`.

## What past runs found (chapter 1)

The rebuild independently reproduced every core DOD choice (no float/alloc, flat
`World`, SoA, bitset grid, Q8.8 / Q5.11, single trig table, sim/render/wasm
split, native tests, precompute-on-rare-change). The gaps it exposed, now closed:

- **Behavior was left fully open** ("decide it yourself"), so the builds diverged
  on reverse vs forward-only, toroidal wrap vs solid border, slide vs stop, and
  whether auto-steer existed at all. → `create-chapter-1.md` now pins the
  behavior contract.
- **`AGENTS.md` read like a changelog of artifacts** precise enough to sound
  mandatory but not to reproduce; the builder misread the escape table's key. →
  artifacts are now stated as illustrations; the doc reads as instructions.
- **No rule kept presentation-only data out of the sim struct** (the rebuild put
  `color` in `World`) and **none covered passing results between order-coupled
  transforms** (it recomputed the move instead of reading `hit`). → both added to
  `AGENTS.md`.
