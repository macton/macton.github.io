# AGENTS.md

Working instructions for agents in this repository. Read this before starting
work here, and follow it.

This repository follows Mike Acton's data-oriented design rules in
[data-oriented-design.md](https://github.com/macton/nagent/blob/master/context/data-oriented-design.md).
Every instruction here is meant to be consistent with that document. Precedence,
when instructions conflict: (1) an explicit instruction for the current task,
(2) the data-oriented-design doc, (3) this file, (4) existing convention.

These are defaults, not mandates. Deviate when you can *demonstrate* a benefit —
measured performance, a concrete size/layout/simplicity win — and record that
evidence with the change. A deviation justified only by speculation is not
justified: decide from facts, not dogma (including the instructions here).

When an instruction cites a concrete artifact (a table such as
`pattern_escape`, a function such as `build_instances`), the artifact is an
**illustration of the principle, not a required structure to reproduce** — apply
the principle to the data you actually have and let it produce whatever artifact
fits.

Each instruction ends with a short italic *origin* — the situation that taught
it — so the intent survives. The instruction is the thing to follow; the origin
is just there to explain why.

---

## Maintenance protocol (this file maintains itself)

- **After every user correction or modification, add or refine an instruction
  here that, had it been followed, would have avoided the correction.** Phrase it
  as a concrete rule ("do X"), consistent with data-oriented-design.
- **Generalize from the sample.** A correction is one data point; record the
  pattern it reveals, not just the one-off fix. (Where there is one, there are
  many.)
- **Keep a short origin note, not a transcript.** One line naming what prompted
  the rule is enough; don't paste the conversation. If it is unclear *why* the
  user asked for the change, ask before recording it — a wrong generalization is
  worse than none.
- **Edit, don't duplicate.** When a new correction refines an existing
  instruction, rewrite that instruction instead of adding a near-duplicate.
  Removing redundancy is the work (simplicity is removing work).
- This maintenance protocol is itself one of the instructions; keep it here.
- General corollary, beyond this file: **when a request's intent or rationale is
  unclear, ask before acting or generalizing** — don't guess at the "why."

---

## Platform & deployment

- **Determine the serving/deployment model before declaring work done, and design
  within it.** Reality is the platform, not the toolchain. This repo is served
  live by GitHub Pages straight from a branch with **no build step on the host**,
  so any built artifact the page needs (e.g. compiled `.wasm`) must be committed,
  and user-facing work must reach the live branch. Confirm the target branch for
  anything meant to be seen live rather than leaving it on a side branch.
  *Origin: the page is viewed live and needs the committed wasm.*
- **Put a visible build version on a live page, and cache-bust its assets.**
  Static hosts and browsers cache aggressively, so "I fixed it" and "the user
  sees the fix" are different events — a reported bug may just be a stale build.
  Stamp a version (build timestamp) into the page, show it, append it to asset
  URLs (e.g. `game.wasm?v=…`), and report the version when you publish so it can
  be confirmed visually. *Origin: a "stuck in the corner" bug that was really a
  stale cache.*

## Numeric representation

- **Default to integer / fixed point. Treat floating point as a deliberate choice
  that must be justified by the data**, not the unexamined default. Choose the
  representation from the data's real range and precision (e.g. positions as Q8.8
  subcells in a fixed-size arena), state that choice and its range, and keep it
  consistent end-to-end. Push any unavoidable conversion to a single boundary
  (e.g. the GPU, which is float hardware) rather than scattering floats through
  the simulation. *Origin: "16-bit fixed point is sufficient."*

## C / types & data layout

- **Use the exact-width integer types from `<stdint.h>` (`uint8_t`, `int16_t`, …)
  directly for any data with a defined width or layout.** Widths are part of the
  data contract — packed structs, the wasm↔JS protocol, on-disk/on-wire formats —
  so make them explicit and guaranteed; do not hand-roll typedefs over
  `char`/`short`/`int`, whose widths the language does not guarantee. **Do not
  alias the stdint types to shorter names** (`typedef uint8_t u8;`): the alias is
  an indirection carrying no information stdint hasn't already captured — pure
  cost. `<stdint.h>` is freestanding, available even under `-nostdlib`.
  *Origin: stdint already manages the value; the alias added zero-value
  indirection.*
- **Size every field to the data's real domain, and make the rationale explicit.**
  Say which bits are semantically used and which are precision. A heading stored
  as a `uint16_t` is Q5.11 — 5 bits select 1 of 32 directions, the low 11 are
  sub-direction turn precision; document that (5 bits alone would force whole-step
  turning). If a width can't be justified against the domain, it is probably
  wrong. *Origin: "Does tank angle require 16 bits?"*
  - *Corollary — when the value is produced at build time, size to the range it
    can actually take, not a defensive worst case; you can **prove or measure**
    the bound, and **detect** a violation, because you own the data.* A theoretical
    ceiling (a within-screen path could be up to `N_CELLS-1` cells) is the wrong
    width if the achievable values are far smaller: bound them (a 2×2 block can't
    lie wholly on an induced path, so the longest in-screen distance ≤
    `N_CELLS - (W/2)(H/2) - 1` ⇒ fits a `uint8`), assert that with a
    `_Static_assert`, and have the generator/build record the measured max so an
    overflow is *seen*, not silently truncated — then fix the design (or widen
    only the offending case) if it ever fires. Provisioning the full theoretical
    width "to be safe" on data you compute is paying for a case that can't occur.
    *Origin: sizing the per-screen all-pairs distance — proposed as `uint16` for
    the 0..299 theoretical max, then shown to provably fit a byte and be measured
    at build time.*

## Design / abstraction

- **Do not add an abstraction (typedef, wrapper, layer, indirection) unless it
  carries value the underlying thing does not already provide.** State concretely
  what it organizes and what it costs; if the value is already captured upstream,
  the wrapper is pure cost — omit it. Renaming something already clear is negative
  value. (Simplicity is removing work; an abstraction must earn its place.)
  *Origin: zero-value indirection over stdint.*
- **Solve only the problem you have; don't build for hypothetical future needs.**
  No fields, flags, enum values, parameters, or hooks for features that don't
  exist yet — not even "reserved/unused, for a later step." Add them in the step
  that needs them. A placeholder is dead weight and a lie about what the code does
  (e.g. an `IN_FIRE` bit with no firing). Note deferrals in the README's
  "deferred" list, not in the code. *Origin: "IN_FIRE should not be defined."*

## Data packing & the shape of the input

- **Write down each transform's exact read/write set (its access footprint) as a
  routine first step — before choosing data layout or a table's key, not after a
  bug.** For every transform list precisely which fields and which cells it reads
  and which it writes. This one habit hands you the two biggest layout decisions
  for free: it exposes a lookup's *true* key (the minimal inputs the result
  depends on — next bullet) and which fields are always touched together (so they
  should share a record — SoA-merge below). If the footprint isn't obvious,
  instrument or hand-trace the reads; don't guess. *Origin: the steer's footprint
  — it only ever reads a cell's 4 neighbours — was there to be read all along; we
  found the 16-pattern table only when finally asked to highlight the sampled
  cells. Doing the footprint analysis up front would have found it unprompted.*
- **Find the minimal data the answer actually depends on — it is often a small
  local pattern, not the whole structure.** Before you index a big table by "the
  whole world," check what the computation truly reads. The steer's escape
  direction looked like a function of (which cell, which facing) over the whole
  300-cell grid; in fact the probe only ever reads a cell's **4 orthogonal
  neighbours**, so the answer is a pure function of a **4-bit local pattern** —
  16 possibilities. That collapsed a ~9.6 KB per-cell table to a **512-byte
  16-pattern table that no longer depends on the map at all** (read the cell's
  live pattern, one lookup). Identify the real input; the table shrinks to it,
  and often stops depending on the things you assumed it did. Confirm by grouping
  (cells sharing a pattern must give identical results) — see `analyze.sh`.
  *Origin: highlighting the cells sampled while moving revealed the response was a
  lookup into a small set of local patterns.*
- **Default to structure-of-arrays; merge fields that are (near-)always accessed
  together into one record.** SoA lets each transform stream only the field it
  needs — the right default. But splitting two fields that every transform
  touching either reads/writes *together* just adds a second potential cache miss
  for no gain: store them in one word (`tank_x`+`tank_y` → packed `uint32_t
  tank_xy`; `vx`+`vy` → `tank_vxy`), through one named pack/unpack owner
  (`xy_pack`/`xy_lo`/`xy_hi`), not ad-hoc shifts at each site. The bar is
  demonstrable independent use: keep them apart only if one is realistically used
  without the other — and a different *writer* or frequency-of-change is itself
  such a reason (heading is written by turn, position by move, so they stay
  separate even though render reads them together). This falls straight out of the
  access-footprint analysis above. *Origin: x and y are never used apart, so they
  belong in one load.*
- **Pack data to the bits its real domain needs; don't spend a byte (or an `int`)
  on a boolean or tiny enum.** A grid of wall/empty cells is a bitset — one word
  per row, one bit per cell — not one byte per cell. Tight packing cuts memory
  and, more importantly, lets one whole-word operation replace a per-element loop
  (test a tank's column span against a row word with a single mask-AND). Size the
  packing to a stated constraint and guard it with a `_Static_assert` (a row ≤ 32
  columns fits one `uint32_t`). Keep the bit layout explicit in the data-protocol
  comment, since JS reads the same words. *Origin: "Grid should be stored as bits
  in integers."*
- **Author build-time data in its target representation; don't keep a source form
  that must be transformed at runtime — and "transform" includes *computing* it,
  not just parsing.** If the values are fully determined when you write the code,
  store what the program actually uses (grid rows as bit-words), not a foreign
  form parsed every init. This holds even when producing the data needs real
  code: **run that generator on the host and emit the result as committed source,
  then compile it in — do not ship the generator and run it at startup.** A value
  with no runtime input is a build artifact (like the trig table, like the
  escape table generated by `tools/gen_escape.c` → `src/escape_table.c`); building
  it at launch only wastes startup work *and* carries the generator code in the
  shipped binary for nothing. Keep the human-readable/generator form as the
  committed source-of-truth; the baked table is the data. *Origin: "MAP does not
  need to exist — values can be set directly," generalized after the steer escape
  table was found being computed in `sim_init` though it is a pure constant.*
- **Replace branchy per-element logic with a small lookup table when the mapping
  is fixed.** Decoding an input bitfield, or a fixed scan order, is a table
  indexed by the input — not a chain of `(x & BIT)` tests. The table is the data;
  the only thing left to compute is what genuinely depends on live state. This
  doesn't conflict with "don't store derivable data": that warns against a second
  *large, runtime-synced* copy, not a tiny const decode table. *Origin: "Replace
  the logic in tanks_turn with lookup table(s)."*
- **A table is also an analysis tool: tabulate to see the structure, then drop the
  table if the data is trivially derivable from the index.** Laying a mapping out
  makes the pattern visible — you can read off which outputs you need and how they
  relate to the input. If each column turns out to be a simple function of the
  index bits (turn = right−left), compute it branch-free and delete the table.
  Reserve a stored table for mappings that are *not* cheaply derivable. *Origin:
  "Putting data into a table … we can see what variables we really need."*
- **Match the work to the frequency of change — and follow it to its endpoint.**
  Hoist a per-tick computation that depends on slow-changing data into a cache,
  and rebuild only when that data changes (make the rebuild trigger explicit at
  every edit point). Push it to the *truly invariant* input: precompute the
  *search*, not just the data, keyed by the minimal pattern. But do not stop at
  "build it once at init": **init-time is still runtime.** Frequency-of-change has
  a zero endpoint — data determined entirely at author/build time (no runtime
  input at all) belongs in the source/binary, generated on the host (see "author
  build-time data"), not computed at launch. The escape table reached that
  endpoint: it depends only on geometry, so it is baked, not built. Reserve
  init/rebuild for data that genuinely depends on runtime state (an editable
  grid). *Origin: "Grid query can be a table … computed when grid data changes,"
  then "the scan order should be a table," then "why build the constant escape
  table at runtime at all?" — once-at-init was one step short of baking it.*
- **Exploit invariants to check only what a change can affect, not the whole
  state.** When a change is incremental and the prior state was valid, re-validate
  only the part it could have touched. A tank steps less than one cell per tick
  from an already-clear cell, so its collision test checks only the leading edge
  it enters (one column or row), not the whole footprint. State the invariant the
  shortcut relies on and confirm equivalence (byte-identical trajectories vs the
  full-footprint test at normal speed). *Origin: "tanks cannot move more than one
  grid element, so just test the one it enters."*
- **Prefer the established conventional format for well-known data — unless you
  can demonstrate a reason to deviate.** Color is a packed `uint32_t` RGBA8888,
  not a 4-`uint8_t` struct: it is the conventional representation, matches the
  GPU's `unorm8x4` byte order on little-endian, and drops a bespoke type. Deviate
  only for a *demonstrable* benefit (measured performance or a concrete
  size/layout win), recorded with the change. *Origin: "rgb(a) can be stored as
  uint32_t — it's a well-established conventional format."*
- **Don't store data you can derive from data you already have.** sin is cos a
  quarter-turn shifted, so one table serves both — index it with an offset instead
  of baking a second table. A derivable copy is redundant: extra memory and a
  second thing to keep in sync. Derive it at the single point that owns the
  relationship (a small inline accessor). *Origin: "You don't need both a sin and
  cos table."*

## Behavior & cases

- **Derive a behavior's target from the persistent constraint that defines it,
  not from a transient that can degenerate.** Collision steering aligns a tank to
  the *wall* (known from which axis was blocked), not to the *resolved velocity*,
  which is zero head-on and would leave the tank stuck facing away. Keying off a
  derived quantity forces per-case patches and still breaks where the quantity
  vanishes. *Origin: reverse/head-on steering bugs from keying off velocity.*
- **Enumerate the degenerate cases, not just the common one.** Zero-length
  vectors, head-on / perpendicular approaches, both-axes-blocked corners: name
  each and check the rule holds (don't let `continue`-on-zero quietly swallow
  one). When a secondary case's correct behavior isn't obvious, surface it and
  ask rather than guessing. *Origin: "if moving backward it's 180°," then "it's
  rotated 90° facing away from the collision face."*
- **To rotate toward a target, pick a consistent handedness — not the shortest
  signed turn — when the target can be ~180° away.** The shortest-signed delta
  flips sign every tick at the antipode, so a half-turn escape jitters forever.
  Turn the way you decided to (the handedness your search used) and clamp so you
  snap on arrival. *Origin: "you will oscillate without escaping."*
- **Probe at the granularity of the decision, not the step.** A "can I move?"
  test that checks only a single sub-cell step reports a dead-end (a niche that
  admits a tiny wiggle) as movable, so a greedy search gets stuck there. To decide
  "is this a way out," probe a whole cell. *Origin: the wiggle-sized test let a
  tank jitter in a pocket instead of turning to leave.*

## Code structure & module boundaries

- **Make independence and boundaries structural, not just commented — put them in
  separate files.** Independent transforms each get their own file (`tanks_turn.c`,
  `tanks_move.c`) so they physically cannot share state; a boundary/data contract
  (the renderer's `Inst` layout + `build_instances`) gets its own file too.
  Transforms take their mutable state and inputs as parameters and reference no
  global *state*. Export only the module's protocol from the wasm; internal
  transforms stay unexported (use the `export_name` attribute, not
  `--export-dynamic`). *Origin: "put independent transforms in their own files to
  structurally reinforce independence."*
  - *Corollary:* "no globals" means no global mutable *state*; a **compile-time
    constant table is not state — reference it directly, don't thread it as a
    parameter.** A parameter says "this can vary per call"; a baked constant
    never varies, so passing it is a lie about the data and invites the reader to
    imagine alternatives that don't exist. The trig table is read directly
    (`dir_cos`); the escape table (`PATTERN_ESCAPE`) is the same kind of thing and
    is read directly too. (A value only earns a parameter once something can
    actually pass a different one — e.g. a tunable in the `World`.) *Origin:
    `PATTERN_ESCAPE` was threaded through `tanks_turn` as a parameter after it had
    been baked into a constant — "why pass it as though it could be different?"*
- **Split by independence, not by activity.** Transforms that write the same field
  and are order-coupled belong together — player-turn and collision-auto-steer
  both rotate the heading, so they are one transform (`tanks_turn`), not two.
  Don't separate what is one concern. *Origin: "Merge steer and turn."*
- **Separate the core simulation from I/O (render, GPU, wasm, network) so the core
  builds and runs on the host under test.** Make the dependency one-way:
  render/wasm depend on the sim; the sim depends on neither. Then tests exercise
  the real logic with no browser, GPU, or device. Keep the sim's data in a struct
  passed to its functions (a `World`), not hidden in the I/O layer. *Origin:
  "separate simulation from render so we can have a simulation-only test build."*
- **Keep presentation-only data out of the sim struct.** A one-way dependency
  (render reads the `World`) still lets render-only data leak *into* it — e.g. a
  per-tank RGBA `color`. No sim transform reads color, so it is dead weight in the
  sim's working set and blurs the boundary; keep it in the render layer (a `const`
  palette beside `build_instances`). Test: if you deleted the renderer, would the
  field still have a reader? If not, it is render data. *Origin: a clean-room
  rebuild put `tank_color` in the `World`.*
- **Pass a result between order-coupled transforms through the shared data — write
  it once, read it next — instead of recomputing it in the consumer.** When
  transform B needs an output of transform A (the auto-steer needs to know the
  move was blocked), have A record it in a `World` field (`hit`) that B reads; do
  not re-run A's computation inside B. Recomputing duplicates the logic and does
  the work twice. The stored field is the explicit contract between the two
  transforms and the single source of truth for that signal. *Origin: a
  clean-room rebuild recomputed the whole move in a `tank_is_boxed` query instead
  of reading the `hit` flag.*
- **Prune what a refactor orphans; a rename isn't done until every reference is
  updated.** A change can leave dead surface (an export nothing calls) and stale
  comments/docs (a field renamed but described by the old name). Both violate
  other instructions, so after a change sweep for newly-unused exports/code and
  references to the old name/shape, and remove or update them. Periodically audit
  the whole codebase. *Origin: "Sweep all the data and code for the rules we've
  established" found dead exports and stale comments.*

## Contracts & testing

- **When behavior the user relies on has emerged implicitly, make it an explicit
  contract and test every case.** Write the promise down (what is guaranteed, and
  the boundaries/non-promises), then enumerate the cases — including the hard ones
  (convex corners, concave U-pockets, head-on, both throttles) and the no-op case
  — and assert each. An implicit contract no one wrote down is one no one can rely
  on or catch regressing. *Origin: the auto-steer "you can hold the throttle and
  never get stuck" promise.*
- **Prefer a general, robust rule over patching cases one at a time.** The
  collision steering went through three per-case patches before becoming one rule
  ("rotate toward the nearest direction you can actually move") covering flat
  walls, corners, and pockets together. When you're on the second patch for the
  same behavior, step back and find the invariant. *Origin: the repeated steering
  patches.*

## UI & presentation

- **Design mobile-first; verify narrow widths before finishing.** A public web
  page is opened on phones. Order the layout by importance for the small screen:
  the **primary subject — the thing the project exists to demonstrate — comes
  first / on top**; secondary panels (debug, controls, help) follow. *Origin:
  "Put the game map above the debug panel for mobile."*
- **Provide input for every target platform.** If it ships to the web, it ships to
  touch devices: supply pointer/touch controls alongside keyboard, don't assume a
  keyboard exists. *Origin: "Support mobile controls (tap)."*
- **Every transient UI state needs an explicit exit.** Model UI state explicitly
  and handle each transition (loading → ready → running); a status/placeholder
  must be cleared by the transition that ends it. Out-of-range and placeholder
  states are resolved deliberately, never left dangling. *Origin: "'Loading…' is
  continuously displayed."*

## Documentation

- **Link the source spec.** When a project is built to follow a referenced
  document or standard, link that document from its README so the contract is
  discoverable. *Origin: "Link the data-oriented design doc in the README."*
