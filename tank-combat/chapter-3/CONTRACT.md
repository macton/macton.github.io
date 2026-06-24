# Chapter 3 contract — the swarm

The explicit promises chapter 3 makes, so they can be relied on and tested. The
tests in `src/test.c` enforce them (103 checks). Chapter 3 **inherits chapter 1's
movement contract** and **chapter 2's pathing/viewport contract**
([../chapter-2/CONTRACT.md](../chapter-2/CONTRACT.md)) unchanged — the four tanks
still route themselves exactly as before. The rename of the shared transforms to
`agent_turn`/`agent_move` (with a `radius` parameter) changed names, not behaviour:
the inherited tests still pass and the baked escape table is byte-identical.

## The pool

- **A fixed pool of `N_MITES` (= 1000) mites**, spawned at init and alive for the
  whole chapter — flat structure-of-arrays, statically sized, **no dynamic
  allocation**. *(tested: the index counts sum to `N_MITES`.)*
- **Spawn is deterministic and legal.** From the seed, mites scatter across **open,
  reachable** cells only, **≤ `MITE_CAP` (= 4) per cell**. *(tested: every mite is on
  an open cell reachable from the open ring; no spawn cell exceeds the cap.)*

## The per-cell index

- **Rebuilt every tick from current positions.** `mite_cnt[cell]` is the true
  occupancy and `mite_list[cell]` holds its first `MITE_CAP` occupants; every mite
  appears in exactly its own cell's list. *(tested.)* It is the swarm's `O(N)`
  acceleration structure — "which mites are within one cell of me" is reading a 3×3
  neighbourhood, not a 1000×1000 scan.

## The crowding cap

- **At most `MITE_CAP` (= 4) mite centres per cell, after every tick.** Enforced by a
  deterministic in-order reservation (current occupants + committed inbound), not by
  physics. A blocked mite re-routes to a free neighbour or holds (emits no drive) and
  re-evaluates next tick. *(tested: the cap holds across thousands of natural ticks
  **and** under forced convergence of the whole swarm on one cell; and it is binding —
  cells do reach 4.)*

## The shared record (last-write-wins)

- **One cell + one timestamp per mite** — the last known tank position (a world cell,
  or empty), and the frame it was recorded. **Newer timestamp wins**; ties break on
  lowest mite index. *(tested.)*
- **Empty is a readable value that propagates.** A newer *empty* record overwrites an
  older cell record by the same rule. *(tested.)*
- **Timestamps are monotone.** An older record never overwrites a newer one. *(tested.)*
- **The gossip is order-independent.** Every mite reads the previous tick's records and
  writes the next tick's, then the buffers swap (double-buffered), so a planted record
  spreads exactly **one hop per tick** along a connected in-range chain, regardless of
  iteration order. *(tested.)*

## Behaviour

- **Sense → record + hunt.** A tank within `mite_sense` cells sets the record to the
  tank's cell, stamped this frame, and the mite hunts it. A self-sensed tank is always
  hunted. *(tested.)*
- **Adopt → 80/20 hunt/home.** On adopting a newer peer record, the mite rolls the role
  die: with probability `mite_phunt` (= 80%) it **hunts** the recorded cell, else it
  **paths home** to its nest; either way it keeps and relays the record. The roll fires
  **even while already hunting or homing** — a newer record interrupts and re-rolls.
  *(tested: the boundaries `P_HUNT = 0`/`100` are exact, the default split matches the
  RNG stream statistically, and a newer record interrupts a homing mite.)*
- **Arrive (hunt).** A hunting mite that reaches within one cell of its recorded cell
  refreshes the record (stamp now) if a tank is there, or **erases** it (empty, stamp
  now) and reverts to wander if not — the erase, being newest, propagates "it's gone."
  *(tested: both refresh and erase.)*
- **Arrive (home).** A homing mite that reaches its nest idles there (a small local
  wander) until a newer record interrupts it.
- **An empty record means wander.** A mite with no recorded cell wanders.

## Nests & shared route fields

- **Every mite belongs to one of four nests**, `nest_of(i) = i % 4`, partitioning the
  swarm into four equal groups. *(tested.)*
- **A hunting/homing mite navigates the path tables, not a greedy heuristic.** A homing
  mite reaches its nest cell; a hunting mite reaches within one cell of its recorded
  cell. *(tested.)*
- **The route is a shared field keyed by destination, equal to the tank ground truth.**
  A mite's lookup into the field for a destination returns the same step the tank
  pathing returns for that destination — the field *is* the tank route, keyed by
  destination instead of by tank. *(tested.)*
- **The field table is sized from a measured peak.** Across representative scenarios the
  distinct active-destination count peaks at ~22 (≈21 with combat on — the death cry's
  goals overlap existing sightings), and `N_FIELDS = 64` holds it with headroom; the live
  count and peak are shown on the page. If the count ever exceeds `N_FIELDS`, the overflow
  mites steer greedily that tick — **no crash, no cap break**. *(tested: the peak stays
  within `N_FIELDS`; a forced overflow holds the cap and stays deterministic.)*

## Movement & collision

- **Mites collide only with walls**, through the shared `agent_move` at radius
  `MITE_R`. A mite's footprint **never overlaps a wall** across a run. *(tested.)*
- **Mites do not collide with tanks or with each other** — crowding is the cap (a
  decision-level rule), not physics. The cap gates the actual step in every mode.

## Combat (the tanks shoot the swarm)

- **Each turret aims independently of the body and turns at a finite rate.**
  `tank_turret` is separate from `tank_ang` (movement heading) and swings toward its aim
  at `turret_rate` per tick — it does not snap. With no target it relaxes toward the body
  heading. *(tested: the turret aims even with firing off; it does not snap; the body is
  unaffected.)*
- **The target is the mite MOST LIKELY TO BE HIT, not the spatially nearest.** Among the
  mites in line of sight (within the search radius), the turret targets the one closest to
  its current direction — the least rotation to bring the barrel on — over the per-cell
  index; lowest index breaks angle ties. *(tested: a farther but aligned mite is chosen
  over a nearer off-axis one.)*
- **Line of sight is required.** A mite is targetable only if an integer Bresenham walk
  from the tank cell to the mite cell crosses no wall. *(tested: a mite behind a wall is
  not targeted.)*
- **Fire is fixed-rate, aimed, and one-shot-kill.** The tank fires only once the turret
  has swung onto the target (within `FIRE_CONE`) and the cooldown is elapsed, every
  `fire_period` ticks (default 30 = 2/sec; `0` disables firing, aim only) — so the turn
  rate gates firing. One shot kills the target mite: it is marked dead and leaves the
  per-cell index on the next rebuild — a corpse is not drawn (on the map either),
  gossiped, or targeted. *(tested: the kill, the cooldown, the corpse leaving the index,
  and an off-axis target not shot until the turret swings on.)*
- **A kill is a death cry.** Every mite within **2× `mite_sense`** cells of the dead one
  has its record set to the firing tank's cell (stamped now) and its mode set to hunt,
  through the ordinary record buffer — the swarm turns on its attacker by the same gossip
  that spreads any sighting. *(tested.)*
- **A dead mite revives at its nest with its memory cleared, after `mite_respawn` ticks**
  (default 300 = 5 s). The record is emptied and the mode reset to wander, so it does not
  resume an old hunt. Revival **respects the crowding cap** (current occupancy + inbound
  reservations); if the nest is full it waits a tick. The four nests sit **off the tanks'
  start screen** so revived mites don't appear under a barrel. *(tested: revival at the
  nest after the timeout with the record cleared; the cap holds across thousands of ticks
  with firing on.)*

## Determinism

- **The swarm is a pure function of the seed and the inputs.** Same seed + same inputs
  ⇒ identical state hash after K ticks; a different seed scatters differently;
  re-seeding to the same seed is reproducible. *(tested.)* All randomness is one
  xorshift32 stream advanced in index order. **Combat draws no randomness** — aiming,
  firing, kills, and respawns are deterministic, so they replay identically too.
  *(tested: the tank combat-state hash matches across runs, with firing on by default.)*

## Boundaries / non-promises

- **The cap is the structural maximum 4** (it sizes the occupant list). The `mite_cap`
  tunable may tighten it within `[1, MITE_CAP]`; it cannot exceed 4.
- **The route table is bounded by `N_FIELDS`.** Beyond the measured peak it is a
  deliberate static size; an overflow degrades to greedy steering for that tick, it does
  not grow the table.
- **Mites route, tanks route — but the cost differs.** Each tank holds its own route;
  the mites share a few fields keyed by destination. A mite stuck where no field route
  exists (overflow, or a sealed goal) falls back to greedy and relies on the
  wander/erase rules and the swarm's collective coverage.
- **The shot is hitscan, and only mites take damage.** A shot is an instant
  line-of-sight kill, not a travelling projectile; tanks fire but are not damaged by the
  swarm (tank health/score is not promised here). A turret parked on a nest legitimately
  suppresses it — a revived mite in a tank's sights is shot again.
- **The viewport is presentation only** (inherited): following, sliding, the picker,
  and which mites are drawn never change the simulation.

## How it's tested

`./test.sh` builds the simulation core (no wasm, no renderer, no GPU) with the host C
compiler and runs `src/test.c`, which builds the world, drives the swarm, and asserts
each promise above. Because the simulation is separated from rendering, these tests
need no browser or GPU.
