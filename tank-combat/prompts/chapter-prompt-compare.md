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
split, native tests, precompute-on-rare-change).

**Run 1** gaps, now closed:

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

**Run 2** (after run-1 fixes + the SoA-merge / host-baked-table / footprint
rules) confirmed those fixes landed: the rebuild kept `color` out of `World`,
merged `x,y`→`tank_xy` and `vx,vy`→`tank_vxy` with the same `xy_pack/xy_lo/xy_hi`
helpers, **generated the escape table on the host and baked it** (`tools/gen_tables.c`
→ `src/escape_table.c`, "pure geometry → baked, not built at launch"), keyed the
steer by the local 4-bit neighbour pattern, and implemented the full
reverse/slide/torus/never-stuck contract (22 checks pass). The remaining gaps
were genuinely-unspecified *behavior details*, now pinned in `create-chapter-1.md`:

- **Tank footprint was undefined** — the rebuild made the tank a *point*, which
  changes collision and breaks the "leading-edge" assumption. → the prompt now
  states a tank is a small square smaller than one cell.
- **Tick order / steer latency was unpinned** — the rebuild chose move→turn
  (opposite of the reference). → the prompt now fixes "rotate then move; auto-steer
  reacts to the previous tick's `hit`."
- **"Nearest direction it can move" was ambiguous** — the rebuild used a fixed
  compass scan order, not angular nearness to the heading. → the prompt now
  defines "nearest" as the smallest turn from the direction being travelled.

Note: the rebuild's 16-entry escape table (one orthogonal direction per pattern)
is smaller than the reference's `[pattern][travel]` table, but it does *not*
honour "nearest to current heading" and steers to a fixed-priority orthogonal
direction — smaller, not better. Nothing from run 2 was worth adopting back.

## What past runs found (chapter 3)

The rebuild (forbidden from reading `chapter-3/`, so it reconstructed chapter 2 from
the prompt + `AGENTS.md`) independently reproduced the core DOD result: flat SoA
`World`, the per-cell index rebuilt every tick, **double-buffered last-write-wins**
gossip propagating one hop/tick order-independently, the cap enforced by an in-order
reservation, nests partitioning the swarm, and a deterministic xorshift32 swarm —
freestanding wasm + native tests passing. The shape was right; the gaps were in the
*details the prompt assumed from chapter 2 but never restated*, three of which the
rebuild got wrong on the first pass and found only by debugging cap/nav failures.

**Gaps, now closed** (all in `create-chapter-3.md`):

- **The route field's representation was unreproducible.** The memory budget cites
  `pg[128]` / ~256 B per field, but the navigation text only said "the same
  Level-1/2 composition keyed by destination." A builder *without* chapter 2's
  two-level tables can't know `pg` is an **edge-point** vector (≈128 entries) backed
  by the baked per-screen Level-1 table — so it folds a full **per-cell BFS** (~9.6 KB
  each, ~600 KB total): correct ground truth, blown budget. → the prompt now states
  what `pg` is and that within-screen steps come from the Level-1 table, not a flat
  field. *(This is the one divergence a faithful rebuild hits for certain.)*
- **The cap's drive-gate was ambiguous.** "Drive only when the discrete heading is
  axis-aligned" reads as *any* axis; the rebuild drove on any cardinal and a
  still-rotating mite drifted diagonally into an **unreserved** cell — a silent cap
  leak. → the prompt now says drive only when the heading equals the **reserved**
  cardinal, and explains why (≤1 boundary crossing/tick).
- **"Within one cell" was undefined** (Chebyshev 3×3 vs Manhattan vs a subcell
  radius). → pinned to the 3×3 Chebyshev neighbourhood, read from the index.
- **Inherited chapter-2 movement was assumed, not stated.** The rebuild had to
  re-derive **toroidal position wrapping** (it clamped first → bodies piled on the
  edge, faking a cap break) and the **screen-down-y direction sign** (inverted N/S →
  navigation ran backwards). → the prompt now restates the inherited contract
  (positions wrap not clamp, −y is north, rotate-then-move, leading-edge collision)
  and points at `create-chapter-2.md`.

**Nothing was worth adopting back.** The rebuild's full per-cell BFS field is simpler
to write but strictly larger and is the chapter-2 work re-done; its 240 checks are
finer granularity, not extra coverage. The reference's edge-point fields and
sub-segment cap are more refined.

**Scope note (not an instruction gap):** `create-chapter-3.md` specifies the *original*
chapter-3 increment — 4 nests, untyped records, ≤4-per-cell cap, **no combat**. The
shipped `chapter-3/` has since grown tank combat (laser/turret/death-cry/respawn), one
nest per screen (15), the 2×2 sub-segment cap, and settled on the simpler gossip model.
The rebuild correctly matched the *prompt*, so those are prompt-vs-shipped drift, not
guidance gaps. Reconciling the prompt with the shipped feature set is a separate pass if
desired.

**Run 2** (after reconciling `create-chapter-3.md` with the shipped chapter — combat,
15 nests, sub-segment cap, simple gossip — and re-running clean-room) confirmed the
reconciliation landed: the rebuild **independently reproduced the full feature set** —
the sub-segment cap (≤1 per `(cell,sub-seg)`, `seg_of` decoupled from `nest_of`), the
15 nests, the death-cry, cap-respecting revival, and all of combat (turret aim / LOS /
hitscan laser) — freestanding wasm + native tests passing, and its **budget matched**
(Level-1 705.5 KB vs the stated ~706). The two run-1 focus fixes held: it sized the
route field correctly as an edge-point vector (no 600 KB per-cell BFS) and got the
sub-segment cap right first try off the drive-gate wording.

The remaining divergences were **new, deeper details**, now closed:

- **The route-field *composition* (the big one).** Run 1 fixed *what* `pg` is; run 2
  showed the prompt never said *how to compose* the two-level field into a
  non-oscillating walk. The rebuild's first composition picked a screen exit
  independently of the within-screen step and **oscillated at borders** (a 2-cycle that
  never reached the goal), failing 5/16 reach tests; it had to switch to one global
  potential stepped strictly downhill. → the prompt now requires the field to be read as
  a single monotone remaining-distance, evaluated identically at the current cell and
  the neighbour, and names the border-oscillation failure mode.
- **Combat consistency constraints the prompt left implicit:** the aim search box must
  be **≥ `LASER_MAX`** (else a mite the beam can reach can't be aimed at — found by
  probe); **`turret_rate` must divide the angle-per-direction step** or "exactly on
  bearing" is never true and the tank never fires; **`BEAM_HW` < the quarter-cell
  sub-segment offset** so off-line mites dodge. → all three pinned.
- **Far-side collision precision:** "the leading edge the body enters" was ambiguous
  between the current edge and the destination edge; the rebuild tested the current edge
  and footprints **overshot into walls** (caught by the wall-safety test). → the
  inherited-contract note now says test `pos + delta ± radius` (the destination edge),
  and pins the dir-0 = +x anchor alongside the screen-down-y sign.

**Inherent clean-room limits (not closable without handing over chapter-2 source):** the
default **map** is unknown, so every spatial result (nest cells, spawn scatter, the
edge-point count `N_EDGE_MAX`, path lengths) is downstream of an invented map and not
comparable in absolutes; and the cross-builder **state hash** can't match because the
xorshift32 variant is "e.g.", not pinned. The prompt now flags `N_EDGE_MAX` as
map-specific with overflow detection; pinning an exact RNG triplet is the obvious next
lever if a cross-builder golden hash is ever wanted. **Nothing from run 2 was worth
adopting back** — the reference's edge-point next-hop already is the non-oscillating
composition the rebuild re-derived, and its `N_FIELDS = 64` (vs the rebuild's 32) is the
safer size once the corner-roaming stress peak (~52) is counted, which the rebuild's
normal-play peak (19) missed.
