# Single-Chain IK — Length + Direction

An interactive visualization of a single-chain inverse-kinematics method:
**solve the chain's length, then aim it.** A spider walks over rough terrain; each
leg is one kinematic chain placed by this two-stage solve. WebAssembly (integer
fixed point) + WebGPU. Live page: open `index.html`.

The method is from a June 2026 thread by
[Kiaran Ritchie](https://twitter.com/kiaran_ritchie).

## The method

Place the end of a jointed chain (a foot) on a target by splitting the solve:

1. **Length** — configure the chain's internal joints so the straight-line
   distance from the root (hip) to the end (foot) equals the hip-to-target
   distance. Direction is irrelevant; the objective is one scalar.
2. **Direction** — rotate the whole solved chain about its root so its
   hip→foot vector points at the target. One rigid rotation at the base.

Reach the right distance, then aim that reach, and the foot lands on the target.
A coupled 2-D positional problem becomes a scalar reach plus one rotation, and —
because the gross aiming is one rotation at the hip — the motion looks limb-like
(shoulders/hips swing; distal joints shape the reach).

### How each stage is solved here

- **Length** is a one-dimensional, monotone root-find. Each 3-segment leg folds
  by a single shared *curl* angle (a uniform arc); more curl ⇒ shorter reach,
  monotonically, down to the bottom of the fold (`ik_curl_max`, measured, not
  assumed). So we **bisect the curl** until *squared* reach meets *squared* target
  distance — no derivative, and no square root in the loop. A target outside the
  reachable interval is **clamped** to the nearest end and flagged (the foot turns
  red and leaves the ground).
- **Direction** is exact in the plane: the from-to rotation taking the solved
  end vector `E` onto the target vector `D` has `cos = (E·D)/(|E||D|)` and
  `sin = (E×D)/(|E||D|)` — one dot, one cross, normalised once. No `atan2`, no
  quaternion antipode care.

This is the method **specialised to the plane**, which is what makes the two
stages so clean. The full 3-D method generalises both: the from-to becomes a
quaternion, and a bend-plane / pole vector chooses among the many internal poses
that hit a given reach (here that choice is the fixed bend side per leg).

## The demo

The body walks at constant speed; a distance-based gait steps two alternating
leg groups so the body is always supported. A planted foot stays fixed on the
terrain while the body rides over it (this is what stretches a chain), and a
swinging foot arcs to the next foothold. The terrain is a heightfield (a sum of
cosine octaves, stored as nothing — a pure function of world x) so every foothold
sits at a different height and each leg must reach a different distance.

Tunables on the page: walk speed, segment length (shrink it to force clamping),
stand height, stride, foot lift. Toggle **show the two stages** to see, on the
focused leg, the length-only pose (faint blue) and the same chain after the single
hip rotation (solid).

## Layout

Simulation is fully separate from rendering and wasm, so it builds and runs
natively under test with no browser or GPU.

| file | role |
|------|------|
| `src/ik.c` | **the method** — length bisection + direction from-to (pure functions) |
| `src/gait.c` | the gait: body advance + per-leg footfall timing (the IK's input) |
| `src/sim.c` | per-tick orchestration: gait, then IK per leg, into the `World` |
| `src/terrain.c` | the heightfield (a function of x, read from the trig table) |
| `src/trig.h` / `src/trig_table.c` | one baked Q14 cosine table, read interpolated (sin is the same table) |
| `src/render.c` | reads the `World`, builds the packed 16-byte instance buffer |
| `src/wasm.c` | the only wasm-specific file: the `World`, the buffer, the exports |
| `src/test.c` | native tests of the solver and the walk |
| `tools/gen_trig.c` | bakes `src/trig_table.c` (host, build time) |
| `tools/ascii_dump.c` | a GPU-free terminal view of the scene, for development |

Numbers are integer fixed point: positions in subcells (`256` = one cell),
angles in `65536`/turn, cos/sin in Q14. Float appears only at the GPU.

## Build / test

```sh
./test.sh     # native, GPU-free: solver + walk invariants
./build.sh    # regenerate the trig table, compile ik.wasm, stamp version.js
```

`ik.wasm` is committed because GitHub Pages serves the page live with no build
step. `build.sh` writes a build version into `version.js`, shown on the page and
appended to the wasm URL to defeat caching.

## Deferred

- **Full 3-D** (quaternion from-to; a real pole/bend-plane choice; a depth-buffered
  renderer). The plane is the simplest machine that shows the two stages clearly.
- **Body tilt** to the ground slope, and excluding swing feet from the body-height
  average. The body currently stays level and rides the mean foot height.
- **End-effector orientation, joint limits, contact friction** — the open
  questions the source thread lists; out of scope for a method visualization.
