# Prompt — Create Chapter 1: Tank Combat

You are building **chapter 1** of an interactive book that teaches
*data-oriented design* by building a small game, one feature at a time. Each
chapter is a self-contained web page: the running game on top, a written
walk-through below, and the game's live data shown and editable *in the context
of the text*.

## The task (chapter 1)

Create a simple 2D game inspired by Atari *Combat*:

- **Two tanks.**
- **Obstacles defined on a grid.**
- Written in **C, compiled to WebAssembly, rendered with WebGPU.**
- **No memory allocations** that are not absolutely required structurally.
- **Absolutely minimal code and data transformations. Compact, tight, explicit
  data.**
- A **debug panel / live elements that demonstrate and give access to ALL the
  data** — and that sit next to the prose that explains them.

The **goal of the project is to demonstrate a data-oriented approach to building
a game** — the game is the vehicle, not the point. This is the *first step*;
later chapters add features one at a time, so keep it minimal and clean, and
don't build for features that don't exist yet.

## Binding guidance — read first, follow strictly

These two documents are requirements, not suggestions. Read them before writing
any code, and let them drive every decision.

1. **`data-oriented-design.md`** — Mike Acton's operating rules:
   <https://github.com/macton/nagent/blob/master/context/data-oriented-design.md>
   (raw: `https://raw.githubusercontent.com/macton/nagent/master/context/data-oriented-design.md`).
   Understand the real data first; write the simplest machine that transforms the
   input you have into the output you need; state costs; exploit every
   constraint; solve only the problem you have.
2. **`AGENTS.md`** at the repository root — the project's accumulated working
   agreements, each one a concrete rule distilled from building this exact
   project (numeric representation, exact-width types, data packing, lookup
   tables vs. derivable data, frequency-of-change precompute, behavior &
   contracts, module boundaries, sim/render separation, testing, UI, deployment).
   Treat every rule as a requirement.

## What to deliver

- **A simulation core in C**: a single flat `World` struct of data plus the
  per-tick transforms over it. Freestanding `wasm32` (no libc, no libm, no
  allocation). **Integer fixed point only — no floating point in the
  simulation.**
- **Separation of simulation from I/O**: the simulation must build and run as a
  **native host program** for testing, depending on neither WebGPU nor
  WebAssembly. The dependency is one-way (render and the wasm boundary depend on
  the sim, never the reverse).
- **Native tests** that set up scenarios and assert the behavior — runnable on
  the host with no browser or GPU.
- **The wasm boundary, a WebGPU renderer, and the interactive page**: game on
  top, write-up below, live debug widgets embedded in the relevant sections.
- **`build.sh` and `test.sh`**, a **visible build version** on the page (and
  cache-bust the wasm), a **README**, and — if your movement behavior makes a
  promise the player relies on — a **CONTRACT** that you also test.

## Movement (chapter 1 behavior — this is the contract, make it explicit and tested)

The two tanks move and rotate on the grid and collide with the walls. The
*representation* of all of this (fixed-point format, angle encoding, how the grid
query and any precompute are shaped) you derive from the data and the rules
above. The *behavior* below is fixed — it is the chapter's contract, so write it
down in `CONTRACT.md` and assert every case in the native tests:

- **Throttle is forward and reverse.** A held forward/back throttle drives the
  tank along/against its heading at a fixed speed per tick; turn input rotates
  the heading at a fixed rate. (Fixed timestep: the per-tick deltas *are* the
  data — no `dt`.)
- **Walls slide, they don't stop dead.** Resolve the step one axis at a time, so
  a tank hitting a wall at an angle keeps the component that is still open.
- **The arena is a torus.** Positions wrap; there is no hard edge. Whether you
  can cross an edge depends only on the wall on the *far* side — two opposite
  open edges let a tank pass through, an opposite wall stops it. (The default map
  has a full border, so it plays closed until a border cell is opened.)
- **A held throttle never leaves a tank permanently stuck.** If a driving tank is
  against a wall, corner, or in a concave pocket, it auto-rotates toward the
  nearest direction it can actually move and drives out — for both forward and
  reverse. This is the part most likely to go wrong; enumerate the hard cases
  (flat wall, convex corner, the arena's own corners, a U-pocket, head-on with
  zero resolved velocity, ~180° escapes) and assert each, including the no-op
  case (no throttle ⇒ no movement, no steering).

## Definition of done

- The sim core compiles freestanding to wasm and natively; `test.sh` passes.
- No floating point in the sim; no dynamic allocation; data is packed and
  explicit; every transform is a batch over the data it needs.
- The page shows the game and explains the data-oriented choices with the live
  data in context.
- You can state, for each data layout and transform, what it costs and why it is
  shaped the way it is.
