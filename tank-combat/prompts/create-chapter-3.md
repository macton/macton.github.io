# Prompt — Create Chapter 3: The Swarm

You are building **chapter 3** of the interactive book that teaches
*data-oriented design* by growing one small game, one feature at a time. Chapter 1
is a two-tank Atari-*Combat* game; chapter 2 grew the world to a 4×4 toroidal grid
of screens and added self-pathing tanks. Chapter 3 adds an **enemy faction**: a
**thousand small units** that wander, gossip the last place they saw a tank, and
swarm toward it — while the **tanks shoot back**, each turret picking the mite it can
most easily hit and firing a laser, and **every kill feeds the same gossip**, so
thinning the swarm enrages it.

Each chapter is a self-contained web page: the running game on top, a written
walk-through below, and the game's live data shown and editable *in the context of
the text*.

The chapter's lesson is **scale**: one entity is easy, a thousand is a data layout.
The thousand units force the structures that volume needs — a per-cell spatial
index rebuilt every tick, a fixed pool with no allocation, a deterministic integer
RNG, a piece of shared knowledge that is just **timestamped data copied on contact**
with no central manager, and **a handful of shared route fields** standing in for a
thousand per-unit routes. **Combat is the coda that proves the point**: when the tanks
shoot back, the aiming, the line-of-sight, the kills, and the death-cry that re-arms
the swarm all **reuse the structures already built** — the per-cell index, the record
gossip, the route fields — and add **no new system**, only a small transform that
reads them.

## The faction's name

Call them **mites**. Codename used throughout the sources, page, and docs:
`mite` — `N_MITES`, `mite_*` arrays, `mites_step`, `MITE_R`. It is a placeholder
chosen for being short and easy to refer to; rename freely (Motes, Gnats, Scouts,
Roaches, Drones are all fine) — it appears only in the chapter-3 sources, its
`README`/`CONTRACT`, and the page prose, so the rename is a contained sweep.

## Starting point — copy chapter 2, then strip the prose

1. Copy `tank-combat/chapter-2/` to `tank-combat/chapter-3/` verbatim and get it
   building/running first (`build.sh`, `test.sh`) — chapter 3 is an *increment* on
   chapter 2, not a rewrite. Reuse its whole sim core: fixed-point types, the
   screen-major bitset grid, the movement transforms, the baked trig and escape
   tables, the two-level path tables (the tanks keep pathing exactly as before),
   the sim/render/wasm split, the follow camera + screen picker, version stamping,
   and the native test harness.
2. **Strip the page's article text.** Chapter 3 discusses net-new ideas, so remove
   chapter 2's explanatory prose from `index.html` (the `<article>` write-up) but
   keep the page *structure*: the canvas, the controls, the follow camera + picker,
   the minimap, the live debug-widget anchors, the version badge, the styling.
   Write fresh chapter-3 prose for the new concepts as you build them. The tanks
   and their pathing stay; the chapter-2 path widgets can stay as a recap or be
   trimmed to make room — your call, but keep the world and the tanks playable.
3. Add a chapter-3 entry to the book's table of contents (`tank-combat/index.html`)
   and give chapter 3 its own `README.md` and `CONTRACT.md`.

## Vocabulary — "grid segment" is one world cell

The world is the chapter-2 `BIG_W × BIG_H` (= 80 × 60 = 4800) toroidal grid of
cells, organised as 4×4 screens. A **grid segment** in this prompt is **one world
cell**. The mites' size, their crowding cap, their sensing range, and their arrival
test are all expressed in whole cells. **"Within one cell" means the 3×3 Chebyshev
neighbourhood** (the mite's cell and its 8 neighbours, toroidally) — used identically
for sensing a tank, reading a peer, and the arrival test; resolve it by reading the
per-cell index over those 9 cells, never a radius in subcells.

## What chapter 3 adds — the mites

### What a mite is
- A **mite is a quarter-cell unit**: roughly ¼ the size of a grid cell
  (`MITE_R ≈ SUB/8` subcells, so its diameter is about a quarter cell — tune it).
  It is a moving body on the same grid as the tanks.
- There are **`N_MITES = 1000`**, **spawned at init**, scattered across the open,
  reachable cells of the map, deterministically from a seed, **respecting the
  crowding cap below** (no spawn cell starts with more than 4). A mite is a member
  of a **fixed pool** — flat structure-of-arrays, statically sized, no allocation.
  The pool is fixed for the whole chapter: combat can **kill** a mite (it goes dead,
  is skipped by the index/gossip/render, and **revives at its nest** after a timeout —
  see *Combat*), but no mite is ever allocated or freed. Spawning waves and a
  free-list / generational despawn are deferred.
- A mite **moves with the same turn/move transforms the tanks use** (see *Shared
  movement* below). It collides only with **walls**, by the same far-side rule,
  with its own smaller radius. Mites do **not** physically collide with tanks or
  with each other; crowding is handled by the cap, a *decision-level* rule, not by
  physics.
- A mite **navigates with the path tables** the tanks already build, but it does
  **not** own a route: it reads a **shared route field** for its destination (see
  *Navigation* below). Across 1000 mites there are only a handful of distinct
  destinations — the 15 nests plus the few recent tank-sighting cells the gossip is
  converging on — so a few shared fields serve the whole swarm. While a mite has no
  destination (an empty record) it **wanders** (a random walk over open cells).
- **Every mite belongs to one of `NEST_COUNT = 15` nests** (home cells), assigned by
  index: `nest_of(i) = i % 15`. There is **one nest per screen except the tanks'
  start screen** (the 4×4 world has 16 screens; nest 0's screen is left to the tanks
  so revived mites don't appear under a barrel), each on an open cell near its
  screen's centre. A nest is a destination a mite paths home to (and revives at).
  **Why one per screen, not a few clustered:** spreading the homes across all the
  screens spreads the spawn/revival load — with a single screenful of nests every
  revived mite funnels out through that one screen's two or three border openings (a
  Poisson pile-up at the exits); one nest per screen gives each its own exits and
  ~`N_MITES/15` residents, which is what lets the revival logic stay dead simple
  (revive, wander, re-acquire) with no jam to dissolve.

### The crowding cap (and the spatial index it needs)
- **At most `MITE_CAP = 4` mites per cell — one per sub-segment.** Quarter each cell
  into a **2×2 grid of sub-segments**; assign a mite one by index,
  `seg_of(m) = (m >> 2) & 3`, and **park it at that sub-segment's centre** (a
  quarter-cell off the cell centre), so the swarm spreads out *inside* a cell instead
  of stacking on the centre (and is harder for a laser to mow down in one line). The
  cap is **at most one mite per `(cell, sub-segment)`**: a mite may enter a cell only
  if its own sub-segment there is free. Use the **upper** index bits for `seg_of`,
  deliberately **not** `nest_of`'s low bits — decoupling them so a nest's mites fan out
  over all four sub-segments and **revive in parallel** rather than serialising on one
  slot. If its sub-segment is taken, the mite picks a **different neighbouring cell**
  with that sub-segment free; if none, it **holds** (emits no drive) and re-evaluates
  next tick.
- This needs, cheaply, **which sub-segments of each cell are occupied** and **which
  mites are near a given mite**. Build a **per-cell index every tick**: a count plus a
  **sub-segment-slotted list** — `mite_cnt[N_WORLD_CELLS]` (u8 = occupied sub-segments)
  and `mite_list[N_WORLD_CELLS * MITE_CAP]` (u16 = the mite in each `(cell, sub-seg)`
  slot, or `REC_EMPTY`). This uniform grid is the chapter's **acceleration structure**
  — "which mites are within one cell of me" becomes reading the ≤4 occupants of the
  3×3 neighbourhood, O(N) not O(N²). Rebuild it at the start of each tick from current
  positions; it changes every tick.
- **Enforce the cap deterministically.** Process mites in index order against a
  per-cell **bitmask of reserved sub-segments** (current occupants + already-committed
  inbound, each a `1 << seg`), so the *n*-th mite sees earlier mites' commitments this
  tick. The **invariant** — *at most one mite per `(cell, sub-segment)`, every tick* —
  holds inductively because spawn respects it and each tick gates only the
  `(cell, sub-segment)` a mite *enters* (echoing chapter 1's "test only the cell the
  leading edge enters"). Pin it under a forced-convergence stress: the whole swarm
  aimed at one cell still tops out at 4.

### The shared knowledge — a last-known-tank-position record (the heart of the chapter)
Each mite carries one small **record**: the **last known tank position**, as a
world cell, plus the **frame it was recorded** (a timestamp). The record is either
a cell or **empty**, and it always has a timestamp. Newer timestamp wins — it is a
last-write-wins register, and the swarm's behaviour emerges entirely from copying
it on contact.

Per tick, for each mite, the record updates by these rules (define them precisely;
they are tested):

- **Sense a tank.** If any tank is within **`mite_sense` cells** (Chebyshev; a
  tunable, default **2**) of the mite — a handful of tanks, so scan them directly, no
  tank index needed — set the record to **that tank's cell, stamped this frame**, and
  **hunt** it. The nearest sensed tank wins (lowest index breaks ties). A
  directly-sensed tank is always hunted (the mite can see it).
- **Read a peer.** Otherwise, among the mites within one cell (read from the
  per-cell index), take the one whose record is **newer** than this mite's. If it is
  newer, **adopt it** (copy cell + timestamp — including an *empty* record that is
  newer), then roll the role die on the adopted cell: **80% → hunt** (path to the
  recorded tank cell), **20% → go home** (path to this mite's nest). Either way the
  mite **holds the adopted record and keeps relaying it**. This fires even while
  already hunting or homing — a newer record interrupts and re-rolls. `P_HUNT = 80%`
  is a tunable; an adopted *empty* record means nowhere to hunt → **wander**.
- **Arrive and check (hunt).** If a *hunting* mite reaches **within one cell of its
  recorded cell**: if a tank is there, it has found it — **re-record that tank's
  current cell, stamped this frame** (refreshing the source so the converged swarm
  stays converged). If no tank is there, **erase** the record (set it empty, stamped
  this frame) and go back to **wander**. The empty-stamped-now record is *newer*, so
  "it's not here any more" propagates back through the swarm by the same read rule.
- **Arrive (home).** A homing mite that reaches its **nest** has delivered the
  sighting: it **drops its record (empty, stamped now) and reverts to wander**,
  rejoining the swarm. A mite wears the nest tint only while *in transit*; without
  this, homed mites pile up at the nest, stay `MM_HOME` forever (perpetually
  nest-coloured), and keep relaying a stale record.
- **Otherwise** keep the current record, mode, and target.

Make the gossip **order-independent**: read every mite's record from the *previous*
tick and write the new records to a second buffer, then swap (double-buffer the
record arrays). Then the propagation does not depend on iteration order and is
deterministic. Resolve "newest peer" with a canonical tie-break (e.g. lowest mite
index) so equal timestamps are deterministic too.

### Navigation — a few shared route fields for a thousand mites
A **hunting** or **homing** mite has a **destination cell** (its recorded tank cell,
or its nest). It does not compute its own route — it reads a **shared route field**
for that destination: the same Level-1/Level-2 composition the tanks use, with the
remaining-distance vector (`pg`) **keyed by destination cell** instead of by tank.

**What `pg` is (so the table size is reproducible):** it is chapter 2's **Level-2
remaining-distance vector keyed by edge point** — one entry per inter-screen *edge
point*, **not** a per-cell distance map over all 4800 cells. Within a screen, the
**baked Level-1 all-pairs table** gives the next step; `pg` only routes *between*
screens. That two-level reuse is the whole reason a field is cheap and a fixed table
of them fits the budget — if you instead fold a full per-cell BFS per destination
(≈ 9.6 KB each) you have rebuilt chapter 2's job and blown the budget. Reuse the
chapter-2 tables; do not re-derive a flat distance field. (Size `N_EDGE_MAX` from the
**actual edge-point count of the map** — it is map-specific, ≈128 for the default map —
and **detect overflow** rather than silently truncating edges.)

**Read the field as one monotone potential — or it oscillates at screen borders.** The
field must answer, at the mite's current cell, "the next cardinal whose neighbour has a
**strictly smaller remaining distance to the destination**" — and that remaining
distance must be **one global quantity** (Level-1-within-screen composed with
`pg`-between-screens) evaluated **identically at the current cell and at the candidate
neighbour**. The failure mode to avoid: choosing a screen *exit* independently of the
within-screen step, so a mite steps north out of a screen and the next screen's field
sends it straight back — a 2-cycle that never reaches the goal. Compose one potential,
step strictly downhill; the route is a non-oscillating descent to the destination.

Because the swarm shares destinations, the set of *distinct* destinations in flight
is **small and practically bounded** — the 15 nests, plus the handful of recent
tank-sighting cells the gossip is converging on — even though 1000 mites are moving.
So keep a **fixed table of route fields** (`N_FIELDS`), keyed by destination cell:

- The **15 nest fields stay resident** (re-folded only on a wall edit).
- A **tank-goal field is folded the first tick its cell becomes an active
  destination and reused while it stays active**; its slot is freed once no mite
  wants it.
- **Size `N_FIELDS` from a measured peak, not a guess.** Instrument the distinct
  active-destination count, run representative scenarios (idle tanks, fleeing tanks,
  several sightings at once), take the high-water mark, and size `N_FIELDS` to it
  with headroom — the prove/measure/detect discipline `AGENTS.md` applies to the
  Level-1 byte, here applied to a runtime peak you can simulate.
- The **overflow → greedy fallback** is the safety net: if the live count ever
  exceeds `N_FIELDS`, those mites steer greedily that tick (no crash, no cap break).
  Surface the live **distinct-destinations / `N_FIELDS`** count **and its peak** on
  the page — a thousand mites served by a handful of routes is the chapter's payoff,
  and the peak is how you size the table.

Turning a destination into the input byte is the tank path-follow, unchanged: the
field gives the next cardinal from the mite's current cell; turn toward `CARD_DI[dir]`
and **drive only when the discrete heading equals that chosen cardinal** — not merely
"some axis". This matters for the cap: the mite must enter exactly the cell it
reserved, so it advances at most one orthogonal cell-boundary per tick. Driving while
the heading is any axis (a still-rotating mite drifting diagonally) lets it cross into
an unreserved cell and the per-cell cap leaks. So: align to the *reserved* cardinal,
then drive; the mite tracks cells and clears the one-cell gaps.

**Wander** (empty record) needs no field: pick a random **open, non-full adjacent
cell** (deterministic RNG), re-choosing at each cell centre.

**The crowding cap gates the actual step**, in every mode: take the preferred next
cell (from the field, or from wander) only if it is open and not full; else the best
open, non-full neighbour toward the destination, ranked by the field's distance;
else **hold** this tick.

### Combat — the tanks shoot the swarm (a transform that only reads what's here)
Combat is its own small transform (`tanks_fire`) and it adds **no new structure**: it
reads the **per-cell index** to find targets and to kill, and writes a kill through the
**ordinary record buffer** so the swarm turns on its attacker by the same gossip that
spreads any sighting. Per tick (after movement):

- **The turret aims independently of the body, at a finite rate.** Give each tank a
  `tank_turret` heading separate from `tank_ang` (its movement heading); it swings
  toward its chosen aim at `turret_rate` per tick — it **does not snap**, and with no
  target it relaxes back toward the body heading.
- **Target the mite MOST LIKELY TO BE HIT, not the nearest.** Scan the per-cell index
  over a search box around the tank — **at least `LASER_MAX` cells** in each direction:
  the box must cover everything the laser can reach, or a mite the beam *would* kill can
  never be aimed at (a consistency constraint between the aim range and the shot range).
  Among mites in **line of sight**, pick the one whose bearing is **closest to the
  turret's current direction** (the least rotation to bring the barrel on), lowest index
  breaking angle ties. Line of sight is an integer **Bresenham** walk from the tank cell
  to the mite cell that crosses no wall (reuse the wall grid — a mite behind a wall is
  not targetable).
- **Fire is a fixed-rate, exactly-aimed laser.** A tank fires only once the turret has
  swung **exactly** onto the target bearing (a line shot is never a cone, so the turn
  rate gates firing) and the cooldown has elapsed, every `fire_period` ticks (default
  **30 = 2/sec**; `0` disables firing, aim only). Make `turret_rate` **divide the
  angle-per-direction step** so the turret can land *exactly* on a discrete bearing; a
  rate that does not divide evenly overshoots forever and the tank never fires. The beam
  half-width `BEAM_HW` must be **less than the quarter-cell sub-segment offset**, so that
  a mite parked off the line genuinely dodges while the aimed one (on the line) dies. The shot is a **laser**: a thin ray
  marched from the muzzle along the barrel until it meets a wall (capped at `LASER_MAX`),
  destroying every mite whose position lies within a narrow band (`BEAM_HW`) of the beam
  line — again just reading the per-cell index. Because mites sit in sub-segments a
  quarter-cell off centre, the ones **on** the line die and the ones **off** it
  **dodge** (the aimed target always dies). The beam is drawn for `LASER_TICKS` (~0.2 s);
  while it is live the **turret is locked** to its firing direction (a shot commits the
  aim). A killed mite is marked **dead** (a `mite_resp` respawn countdown), leaves the
  index on the next rebuild, is not drawn / gossiped / targeted, and spawns a cosmetic
  **destruction burst** (a small fixed-size FX ring — presentation only).
- **Every kill is a death cry.** Every live mite within **`2 × mite_sense`** cells of a
  destroyed one has its record overwritten with the **firing tank's cell** (stamped now)
  and is flipped to **hunt** — through the ordinary record buffer. So a laser that mows a
  line doesn't thin the swarm quietly; it **broadcasts the shooter's position** and the
  survivors converge on the attacker.
- **Revival.** A dead mite **revives at its nest after `mite_respawn` ticks** (default
  **300 = 5 s**), memory wiped (record empty, mode wander, so it resumes no stale hunt),
  **respecting the cap** (it takes a free sub-segment at its nest cell; if the cell is
  full it waits a tick — with one nest per screen this is rare, so no
  neighbourhood-fallback is needed). Revival runs **right after the index is rebuilt**
  (step 1b below) so the revived mite is an ordinary live mite for the rest of the tick.
- **Combat is deterministic** — aiming, firing, kills, and revival draw **no
  randomness** — so it replays exactly under the same seed + inputs. Only mites take
  damage; the tanks fire but are not damaged by the swarm (tank health/score is not
  promised here), and the laser is **hitscan** (an instant line, not a travelling shot).

### Determinism
All randomness — the wander direction and the hunt/home roll — comes from a **single
integer PRNG** (e.g. xorshift32) seeded in the `World` and advanced as mites are
processed in index order; each adopting mite consumes exactly one role-die draw, so the
state hash is reproducible across builders. **Combat adds no randomness** (aiming,
firing, kills, and revival are deterministic). Expose the seed (editable → re-seed and
re-scatter the swarm). The whole simulation stays **deterministic and replayable**: same
seed + same inputs ⇒ identical state, including the tank combat state. Pin both with a
state-hash test.

## Shared movement — rename the transforms, make size a parameter

The tanks and the mites are both **driven bodies on the grid**: a position, a
heading, an input byte, a collision flag. They share the turn and move transforms.
The transform already takes arrays and a count, so the work is naming and one
parameter:

**Inherited movement contract (you are reusing chapter 2's — do not re-derive it; if
building from scratch, take it from `create-chapter-2.md`, where it is pinned):** the
**position integrator wraps toroidally** (a body crossing an edge reappears on the far
side — *positions* wrap, not just the conceptual grid; clamping instead silently fakes
a cap break); the discrete-direction ↔ heading mapping is fixed with **screen-down y**
(direction 0 = +x = east, advancing through the 32 dirs, so "north" is −y — getting the
sign or the dir-0 anchor backwards inverts navigation); rotate-then-move with the
auto-steer reacting to the *previous* tick's `hit`; and collision tests only the
**leading edge at the body's destination** (far-side rule — test `pos + delta ± radius`,
the cell the edge will occupy *after* the step, **not** the current edge `pos ± radius`;
testing the current edge lets the footprint overshoot into a wall by up to one step's
travel), parameterised by radius below. These are chapter-2 guarantees the swarm leans
on; a fresh build that guesses them differently will pass its own unit tests and still
navigate wrong.

- **Rename `tanks_turn` → `agent_turn` and `tanks_move` → `agent_move`** (files and
  functions; `body_*` or `mover_*` are acceptable alternates). They operate on "a
  set of moving bodies," not on tanks specifically — naming them `tanks_*` while
  mites use them would misdescribe the data. Per `AGENTS.md`: a rename isn't done
  until every reference is updated, and a refactor must prune what it orphans — sweep
  the per-tick order, `wasm.c`, the README/CONTRACT, the tests, and the comments.
- **Make the collision radius a parameter.** `agent_move` takes a `radius`, threaded
  into `blocked_x`/`blocked_y` (which currently hardcode `TANK_R`). Tanks pass
  `TANK_R`; mites pass `MITE_R`. Size is data you pass, not a reason to fork the
  transform. `cell_is_wall`, `cell_pattern`, and the escape table are
  radius-independent and unchanged — the auto-steer keys on the 4-neighbour wall
  pattern, which both sizes share.
- The **input producers stay separate** (split by independence): `tanks_path`
  produces the tanks' routing input; a new **`mites_step`** produces the mites'
  input (it owns the per-cell index, the record gossip, the cap, the shared route
  fields, and the hunt/home/wander decision). Both write an input byte that flows
  through the *same* `agent_turn`/`agent_move`. This extends chapter 2's "the state
  only picks the input source" across two factions.

### Per-tick order
1. **Build the per-cell mite index** from current positions.
   1b. **Revive** timed-out dead mites at their nests, **into the freshly-built index**
   (cap-respecting), so a revived mite is an ordinary live mite for the rest of the tick.
2. **Tank input** (controls for the MANUAL tank; `tanks_path` for routing tanks) —
   unchanged from chapter 2.
3. **Records** — update each mite's record (double-buffered LWW: sense tanks, read
   newer peers + re-roll hunt/home, arrive/erase), then swap the record buffers.
4. **Route fields** — collect the distinct active destinations (15 nests + the
   hunting mites' recorded cells), fold a `pg` field for each new one into the table
   (reuse resident ones), free dropped ones.
5. **`mites_step`** — for each mite, read its destination's field (or wander), pick
   the next cell (cap-gated, in-order reservations), write `mite_in`.
6. **`agent_turn`** over the tanks (`TANK` turn rate) and the mites (mite turn rate).
7. **`agent_move`** over the tanks (`TANK_R`) and the mites (`MITE_R`).
8. **Combat** (`tanks_fire`) — swing each turret toward its best in-sight target; if it
   is exactly on bearing and off cooldown, fire the laser, mark the mites it destroys,
   and broadcast each kill's **death-cry** through the record buffer.
9. Advance the `World` frame counter (the record timestamp clock).

## Binding guidance — read first, follow strictly

These are requirements, not suggestions.

1. **`data-oriented-design.md`** — Mike Acton's rules
   (raw: `https://raw.githubusercontent.com/macton/nagent/master/context/data-oriented-design.md`).
   Understand the real data first; write the simplest machine that turns the input
   you have into the output you need; exploit every constraint; solve only the
   problem you have.
2. **`AGENTS.md`** at the repo root — every rule applies. Chapter 3 is a textbook
   case for several: **where there's one, there's many** (a thousand mites in flat
   SoA, one batch per transform); **the grid you already have is the acceleration
   structure** (bin once per tick, read 3×3 neighbourhoods — O(N), not O(N²));
   **match the work to the frequency of change** (the path tables rebuild on a wall
   edit — rare; the mite index rebuilds every tick — constant; show both ends of the
   frequency spectrum side by side); **find the minimal data the answer depends on**
   (the shared knowledge is one cell + one timestamp, copied on contact; a thousand
   mites resolve to a handful of distinct destinations, so the routes are a few
   shared fields, not a thousand); **size every structure to its real domain**;
   **exploit the invariant** (spawn ≤4/cell,
   gate only the entering edge); **one transform, many input sources, size as a
   parameter**; integer/fixed-point only; no dynamic allocation; sim/render/wasm
   separation with native tests; render scales with what's visible, not with the
   population.
3. **Chapter 2's sources** — the movement model, the screen-major grid, the follow
   camera, the minimap, and `tanks_path` (the template for deriving an input byte
   from a decision). Read them before designing; you are extending them.

## What to deliver

- The chapter-3 sim core (extending chapter 2): the mite pool with its **fifteen
  nests**, the per-cell sub-segment index, the record gossip, the shared route-field
  table (keyed by destination, reusing the Level-1/2 composition), `mites_step`
  (records + cap + hunt/home/wander → input), **`tanks_fire`** (turret aim + LOS +
  laser kill + death-cry + cap-respecting revival), the renamed `agent_turn`/
  `agent_move` with a radius parameter, and the deterministic RNG — all freestanding
  `wasm32`, integer fixed point, no allocation, one-way dependency (render/wasm depend
  on the sim, never the reverse). The tanks keep pathing.
- **Native tests** (no browser/GPU):
  - **Pool & index:** the per-cell index is correct (counts sum to live `N_MITES`;
    each live mite appears in exactly its `(cell, sub-segment)` slot); spawn places
    ≤1 per sub-segment (≤4 per cell) on open reachable cells.
  - **Crowding cap:** after any tick, no `(cell, sub-segment)` holds two mites and no
    cell holds more than 4, even when the whole swarm targets one cell (the in-order
    sub-segment-bitmask reservation holds the cap), and it stays binding (cells reach 4).
  - **Gossip / last-write-wins:** a planted record propagates outward to a connected
    chain of in-range mites over successive ticks; a *newer* record (including an
    empty one) overwrites an older one; an older record never overwrites a newer one
    (timestamps are monotone); the gossip is order-independent (double-buffered).
  - **Behaviour:** sensing a tank records its cell and hunts; reaching the recorded
    cell with no tank there erases the record and reverts to wander; reaching it with
    a tank there refreshes the record; on adopting a newer record the 80/20 split
    sends mites to hunt vs to their nest, matching the RNG stream; a newer record
    interrupts a homing mite and re-rolls.
  - **Nests & shared fields:** `nest_of(i)` partitions the swarm into **fifteen
    balanced** groups; a homing mite reaches its nest cell; a hunting mite reaches its
    recorded cell; a mite's route lookup into a shared field equals the tank pathing
    ground truth for the same destination (the field is the tank route keyed by
    destination); over representative runs the **peak distinct-destination count** is
    measured and stays within `N_FIELDS`, and a forced overflow falls back to greedy
    without breaking the cap or determinism.
  - **Combat:** the turret aims at the most-hittable mite (a farther but aligned mite
    beats a nearer off-axis one) and does not snap; a mite behind a wall is not
    targeted; firing waits until the turret is exactly on bearing and the cooldown is
    elapsed; the laser destroys mites **on** its line and **misses** ones off it, stops
    at a wall (a collinear mite beyond survives), and the corpse leaves the index; the
    **death-cry** flips nearby live mites to hunt the shooter; a dead mite **revives at
    its nest** after the timeout with its record cleared; the cap holds across thousands
    of ticks **with firing on**; the combat state is **deterministic** (its hash matches
    across runs).
  - **Wall safety:** a mite's footprint (`MITE_R`) never overlaps a wall cell across
    a run — proving the radius-parameterised collision serves both sizes.
  - **Determinism:** same seed + same inputs ⇒ identical state hash after K ticks
    (sim **and** combat state).
  - **Regression:** the tanks still pass every chapter-1/2 movement and pathing test
    after the rename — the rename changed names, not behaviour.
- The WebGPU page: the chapter-2 world and tanks, plus the swarm. In the one-screen
  viewport, draw the mites **on the visible screen(s)** (build instances only for
  mites the camera shows — render scales with visibility, not with 1000). On the
  minimap, show the **whole swarm** as downsampled dots. Live debug widgets for the
  new data, embedded next to the prose:
  - the mite pool (live count, a few SoA rows),
  - the **per-cell occupancy heatmap** (0–4 per cell),
  - the **belief field** — each mite's record visualised (a faint marker at its
    believed tank cell, and/or mites tinted by record age) so you can *watch the
    last-known position diffuse out from a sighting and dissolve when it goes
    stale*,
  - the **fifteen nests** drawn on the map (one per screen but the tank start), mites
    tintable by nest, and the live **distinct-destinations / `N_FIELDS`** count **and
    its peak** (the handful of shared routes, and how you size the table),
  - **combat** drawn over the same data: each turret on its own bearing, the **laser**
    beam + **destruction bursts**, the live **alive / dead** split, and the per-tick
    **update** cost (the whole CPU step),
  - the editable **seed** and tunables (`N_MITES` shown, `mite_sense`, cap, `P_HUNT`,
    mite size/speed/turn rate, **fire rate, respawn delay, turret turn rate**) — the
    nests are structural (one per screen), so they are not a position widget.
- `build.sh`/`test.sh` (extended), a visible cache-busted version, `README.md`, and
  a `CONTRACT.md` covering the new behaviours, with the contract tested.

## Behaviour to pin (so it's reproducible)

- **The crowding cap holds every tick** (≤1 mite per `(cell, sub-segment)`, so ≤4 per
  cell), enforced by the deterministic in-order sub-segment-bitmask reservation; a
  blocked mite re-routes to a free neighbour or holds.
- **The record is last-write-wins:** newer timestamp wins; empty is a readable value
  that propagates; self-sighting and erase both stamp the current frame; reads take
  the newest in-range peer with a canonical tie-break.
- **The 80/20 rule fires on adopting a newer record** (from a peer): 80% hunt the
  recorded cell, 20% path home to the mite's nest; it interrupts and re-rolls an
  already-hunting or homing mite. A self-sensed tank is always hunted; an empty
  record means wander; a homing mite that arrives reverts to wander.
- **Mites navigate by shared route fields** keyed by destination (15 resident nests +
  cached tank goals), folded from the same Level-1/2 tables the tanks use; a thousand
  mites resolve to a handful of distinct routes.
- **The tanks shoot back:** each turret aims independently at the most-hittable mite in
  line of sight and fires a fixed-rate **hitscan laser** that kills mites on its line;
  every kill **death-cries** the shooter's cell into the gossip; a killed mite revives
  at its nest after a timeout, cap-respecting. Only mites take damage. Combat is
  deterministic.
- **Mites collide only with walls** (shared transform, mite radius); mite crowding is
  the cap, not physics; mites do not collide with tanks or each other.
- **Determinism:** the swarm **and the combat** are a pure function of the seed and the
  inputs.
- **The viewport is presentation only** (inherited): following, sliding, the picker,
  and which mites are drawn never change the simulation.

## Memory budget (state it; static, no dynamic allocation)

Size every structure and state the total. A starting point (finalise and report the
real numbers):

| data | size |
|------|------|
| mite SoA: `xy`,`vxy`,`ang`,`in`,`hit`,`mode`,`dest`,`cell`,`tgt`,`resp` × 1000 | ~21 KB |
| records (double-buffered): `rec_cell` (u16) + `rec_time` (u32), ×2 | ~12 KB |
| per-cell sub-segment index: `mite_cnt[4800]` (u8) + `mite_list[4800·4]` (u16) | ~43 KB |
| route fields: `N_FIELDS` × (`pg`[≈`N_EDGE_MAX`≈128] u16 + dest + stamp) | sized from peak (64 ⇒ ~17 KB) |
| combat: per-tank turret / cooldown / target / beam state (×4) + a small **FX ring** | < 1 KB |
| 15 nest cells, PRNG state, mite + combat tunables | < 1 KB |

The swarm + combat state is small next to chapter 2's inherited tables (~706 KB for
Level 1). Chapter 3 **raises the wasm linear memory to 2 MiB** (from chapter 2's 1 MiB,
in the first increment) — confirm the high-water mark (`__heap_base`) against it — and
**raises the instance-buffer cap** to cover one/two visible screens of walls + up to
`MITE_CAP·N_CELLS` mites + the tanks + the nests + the FX; state the new cap. The route
field is the chapter-2 **edge-point** `pg` (≈128 entries), *not* a per-cell distance map
— see *Navigation* — which is what keeps the table cheap.

## Definition of done

- Chapter 3 builds freestanding to wasm and natively; `test.sh` passes, including the
  new swarm/gossip/cap/**combat**/determinism tests **and** the inherited chapter-1/2
  tests after the rename.
- No floating point in the sim; no dynamic allocation; the per-cell index is rebuilt
  each tick (matched to its frequency of change); the record is one cell + one
  timestamp; data is packed and sized to its real domain (memory budget stated).
- The page shows the world, the tanks (still pathing **and firing**), and the thousand
  mites — wandering, recording a sighted tank, gossiping that record on contact, then
  **80% hunting the sighting while 20% carry it home to their nest**, dissolving back to
  wander when the position goes stale, and **converging on a tank that fires** as its
  kills death-cry through the swarm — with the per-cell occupancy, the diffusing belief
  field, the fifteen nests, the laser + bursts, the alive/dead split, the live
  distinct-destinations / `N_FIELDS` count, and the tunables/seed (`P_HUNT`,
  `mite_sense`, fire rate, respawn, turret turn) live in context.
- You can state, for each structure and transform, what it costs, how often it is
  built vs read, and why it is shaped the way it is — and the page makes the
  chapter's lesson legible: **a thousand agents, a grid used as an index, shared
  knowledge that is just timestamped data copied on contact, and a thousand movers
  served by a handful of shared route fields.**

## Deferred (not built — no speculative generality)

Spawning waves and a free-list / generational despawn over the pool (combat kills and
revives in place, but the pool stays fixed at `N_MITES`); **tanks taking damage from
the swarm** (tank health / score — combat is one-directional, only mites die);
mite-vs-mite or mite-vs-tank *physical* collision (crowding is the cap, not physics);
travelling projectiles (the laser is hitscan); more than one enemy faction;
flocking/alignment beyond the cap; per-mite route fields (the chapter shares a few
instead). Each is its own future step; note it here, not in the code.
