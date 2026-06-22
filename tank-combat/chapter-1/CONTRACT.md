# Movement contract

This is the explicit promise the simulation makes about tank movement, so it
can be relied on and tested. The tests in `src/test.c` enforce it.

## The promise

**While a tank holds a throttle (forward or backward) and is not manually
turning, it keeps making progress in that throttle direction whenever any escape
exists.** The player can just hold forward (or back) and the tank will not get
permanently stuck — it auto-rotates to find a direction it can move. **Any manual
turn input hands the heading back to the player: auto-steer yields while left or
right is held**, so the player can steer freely (including deliberately into a
wall).

Concretely, each tick the simulation:

1. rotates the heading (`tanks_turn`): the player's turn input, plus — if the
   tank collided last tick while driving *and the player is not turning* — an
   auto-steer that turns it (at `turn_rate`) toward the nearest of the 32
   directions it *can* move,
2. moves the tank along its heading, resolving collision one axis at a time so
   it slides along a wall it meets at an angle (`tanks_move`).

"Travel direction" is the heading when moving forward, and the heading turned
180° when reversing (a reversing tank faces opposite its motion).

## Guaranteed cases (each has a test)

- **Open space** — no auto-rotation; the heading is left exactly as the player
  set it.
- **Flat wall, hit at an angle** — the tank slides along the wall and rotates to
  run parallel with it.
- **Head-on into a wall** (forward or reverse) — the tank rotates off the wall
  and starts moving along it, instead of sticking facing into/away from it.
- **Convex corner** (two walls meeting, including the arena's own corners) — the
  tank turns out of the corner and follows the wall.
- **Concave pocket** (U-shape / dead-end slot) — the tank drives in, rotates
  around (up to ~180°) until it faces the opening, and drives back out, without
  oscillating at the antipode.
- **Wrap-around edges** — the arena is a torus. Driving into a gap in one edge
  carries the tank through to the opposite edge if that side is open, or stops it
  against the wall on that side if not (the collision is with the wrapped far
  wall, and the tank turns away without oscillating).
- **Manual turn overrides auto-steer** — while the player holds left or right,
  the heading turns by exactly the player's input each tick; auto-steer is
  suppressed even when colliding.
- **Not driving** — no auto-rotation; holding no throttle never moves or turns
  the tank.

In the real default map, a tank holding forward wall-follows indefinitely; its
longest stationary stretch is a handful of ticks (the brief in-place turn when
it reaches a corner).

## Boundaries / non-promises

- **A fully enclosed space has no escape.** If there is genuinely no direction
  the tank can move, it stays put (it does not spin pointlessly).
- **While the player is manually turning, the never-stuck guarantee is
  suspended.** The player owns the heading and can deliberately hold the tank
  against a wall; auto-steer resumes the moment the turn keys are released.
- The escape turn happens at `turn_rate`; turning ~180° out of a tight pocket
  takes about `32768 / turn_rate` ticks, during which the tank is stationary.
- Tanks do not yet collide with each other (a later step).
- Collision is destination-only, not swept; see `issues/001-swept-collision.md`.

## How it's tested

`./test.sh` builds the simulation core (no wasm, no renderer, no GPU) with the
host C compiler and runs `src/test.c`, which sets up each case above on a
`World`, steps `sim_tick`, and asserts the tank escapes / slides / stays put as
promised. Because the simulation is separated from rendering, these tests need
no browser or GPU.
