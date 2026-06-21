# AGENTS.md

Working agreements for agents in this repository. This file is built up
empirically: **each entry exists because a correction was needed once, and the
entry is the rule that would have avoided it.** Read it before starting work
here.

This repository follows Mike Acton's data-oriented design rules in
[data-oriented-design.md](https://github.com/macton/nagent/blob/master/context/data-oriented-design.md).
Every rule below is meant to be consistent with that document. Precedence, when
rules conflict: (1) an explicit instruction for the current task, (2) the
data-oriented-design doc, (3) this file, (4) existing convention.

---

## Maintenance protocol (this file maintains itself)

- **After every user correction or modification, add an instruction here that,
  had it been followed, would have avoided the correction.** Phrase it as a
  concrete rule ("do X"), consistent with data-oriented-design.
- **Generalize from the sample.** A correction is one data point; record the
  pattern it reveals, not just the one-off fix. (Where there is one, there are
  many.)
- **If it is unclear *why* the user asked for the change, ask before recording
  it.** A wrong generalization is worse than none. Capture the user's stated
  reason next to the rule so the intent survives.
- **Edit, don't duplicate.** When a new correction refines an existing entry,
  rewrite that entry instead of adding a near-duplicate. Removing redundancy is
  the work (simplicity is removing work).
- This maintenance protocol is itself one of the instructions; keep it here.
- General corollary, beyond this file: **when a request's intent or rationale
  is unclear, ask before acting or generalizing** — don't guess at the "why."

---

## Platform & deployment

- **Determine the serving/deployment model before declaring work done, and
  design within it.** Reality is the platform, not the toolchain. This repo is
  served live by GitHub Pages straight from a branch with **no build step on
  the host** — so any built artifact the page needs (e.g. compiled `.wasm`)
  must be committed, and user-facing work must reach the live branch. Confirm
  the target branch for anything the user means to *see live* rather than
  silently leaving it on a side branch.
  - *From:* "Push to main; include wasm binary since GitHub.io is a live web
    page view." *Reason given:* the page is viewed live.

## Numeric representation

- **Default to integer / fixed point. Treat floating point as a deliberate
  choice that must be justified by the data**, not as the unexamined default —
  most so in data-oriented code. Choose the representation from the data's real
  range and precision (e.g. positions as Q8.8 subcells in a fixed-size arena),
  state that choice and its range, and keep it consistent end-to-end. Push any
  unavoidable conversion to a single boundary (e.g. the GPU, which is float
  hardware) rather than scattering floats through the simulation.
  - *From:* "We don't need floating point. 16 bit fixed point is sufficient."

## C / types & data layout

- **Use the exact-width integer types from `<stdint.h>` (`uint8_t`, `int16_t`,
  …) directly for any data with a defined width or layout.** Widths are part of
  the data contract — the packed structs, the wasm↔JS protocol, the
  on-disk/on-wire format — so make them explicit and guaranteed; do not
  hand-roll typedefs over `char`/`short`/`int`, whose widths are not guaranteed
  by the language. **Do not alias the stdint types to shorter names** (`typedef
  uint8_t u8;`): the alias adds an indirection that carries no information
  stdint hasn't already captured — it is pure cost. `<stdint.h>` is a
  freestanding header, available even under `-nostdlib`.
  - *From:* "Use stdint.h." / "Remove the typedef'd int types, just use stdint
    types directly … indirection and abstraction that is zero value; all of the
    value has already been managed by the existence of stdint."
- **Size every field to the data's real domain, and make the rationale
  explicit.** Be ready to justify each width; say which bits are semantically
  used and which are precision. A heading stored as a `uint16_t` is a Q5.11
  value — 5 bits select 1 of 32 directions, the low 11 are sub-direction turn
  precision; document that so the width isn't a mystery (5 bits alone would
  force whole-step turning). If a width can't be justified against the domain,
  it is probably wrong.
  - *From:* "Does tank angle require 16 bits?"

## Design / abstraction

- **Do not add an abstraction (typedef, wrapper, layer, indirection) unless it
  carries value the underlying thing does not already provide.** Before
  introducing one, state concretely what it organizes and what it costs; if the
  value is already captured upstream, the wrapper is pure cost — omit it.
  Renaming something that is already clear is negative value. (Simplicity is
  removing work; an abstraction must earn its place.)
  - *From:* "You're adding indirection and abstraction that is zero value; all
    of the value has already been managed by the existence of stdint."

## Data packing

- **Pack data to the bits its real domain needs; don't spend a byte (or an
  `int`) on a boolean or tiny enum.** A grid of wall/empty cells is a bitset —
  one machine word per row, one bit per cell — not one byte per cell. Tight
  packing cuts memory and, more importantly, lets one whole-word operation
  replace a per-element loop (test the column span a tank covers against a row
  word with a single mask-AND, instead of looping cells). Size the packing to a
  stated constraint and guard it with a `_Static_assert` (a row ≤ 32 columns
  fits one `uint32_t`). Keep the bit layout explicit in the data-protocol
  comment, since JS reads the same words.
  - *From:* "Grid should be stored as bits in integers. Array of uint32_t where
    each is a row seems reasonable start."
- **Author build-time data in its target representation; don't keep a source
  form that must be transformed at runtime.** If the values are known when you
  write the code, store them as what the program actually uses (grid rows as
  bit-words), not as a convenient foreign form (ASCII strings) parsed every
  init — that parse is an unnecessary step (can we not do this at all?). Keep
  the human-readable form as a comment if it aids legibility; the comment is
  documentation, the stored value is the data.
  - *From:* "MAP does not need to exist. Values can be set in g_grid. This is an
    unnecessary extra step."
- **Use the established conventional format for well-known data.** Color is a
  packed `uint32_t` RGBA8888, not a 4-`uint8_t` struct: it is the conventional
  representation, it matches the GPU's `unorm8x4` byte order on little-endian,
  and it drops a bespoke type. Prefer the convention the consumer already
  speaks over a hand-made layout.
  - *From:* "rgb(a) can be stored as uint32_t - it's a well-established
    conventional format."

## Code structure & module boundaries

- **Make independence and boundaries structural, not just commented — put them
  in separate files.** Independent transforms each get their own file
  (`tanks_turn.c`, `tanks_move.c`) so they physically cannot share state; a
  boundary/data contract (the renderer's `Inst` layout + `build_instances`)
  gets its own file too. Transforms take their data as parameters and reference
  no globals, so the separation is real. Export only the module's protocol from
  the wasm; internal transforms stay unexported (rely on the `export_name`
  attribute, not `--export-dynamic`).
  - *From:* "put independent transforms like tanks_move and tanks_turn in their
    own files, to structurally reinforce independence. similarly, the
    boundary/data contract with the renderer can be structurally reinforced by
    putting those related data/function in their own files e.g. build_instances."

## UI & presentation

- **Design mobile-first; verify narrow widths before finishing.** A public web
  page is opened on phones. Order the layout by importance for the small
  screen: the **primary subject — the thing the project exists to demonstrate —
  comes first / on top**; secondary panels (debug, controls, help) follow.
  - *From:* "Put game map above debug so it's easier to work with on mobile."
    *Reason given:* easier to work with on mobile.
- **Provide input for every target platform.** If it ships to the web, it ships
  to touch devices: supply pointer/touch controls alongside keyboard, don't
  assume a keyboard exists.
  - *From:* "Support mobile controls (tap)."
- **Every transient UI state needs an explicit exit.** Model UI state
  explicitly and handle each transition (e.g. loading → ready → running); a
  status/placeholder indicator must be cleared by the transition that ends it.
  Out-of-range and placeholder states are resolved deliberately, never left
  dangling — the same explicit-state rule applied to the UI.
  - *From:* "'Loading…' is continuously displayed."

## Documentation

- **Link the source spec.** When a project is built to follow a referenced
  document or standard, link that document from its README so the contract is
  discoverable.
  - *From:* "Link data oriented design doc in readme."
