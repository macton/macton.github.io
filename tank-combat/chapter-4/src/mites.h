/* mites.h — the swarm: spawn, the per-cell index, the gossip, the shared route
 * fields, and the per-tick mite movement.
 *
 * A mite is a quarter-cell driven body in a fixed pool (defs.h). These transforms
 * own everything mite-specific; the mites then flow through the SAME
 * agent_turn/agent_move the tanks use (sim.c wires the order). The lesson is
 * scale: one entity is easy, a thousand is a data layout.
 *
 * A mite owns the minimal shared knowledge — one world cell + one timestamp (the
 * last-known tank position), copied on contact (last-write-wins, double-buffered).
 * It does NOT own a route: when the gossip gives it a destination it NAVIGATES
 * with the tank path tables through a SHARED route field keyed by that destination
 * cell. Across 1000 mites the distinct destinations are few — the NEST_COUNT
 * nests plus the handful of recent sighting cells the gossip converges on — so a
 * handful of fields serve the whole swarm.
 *
 * All randomness is one xorshift32 stream (World.rng), advanced as mites are
 * processed in index order, so the swarm is a pure function of the seed + inputs. */
#ifndef MITES_H
#define MITES_H

#include <stdint.h>

typedef struct World World;

/* Seed the PRNG from World.mite_seed and scatter the pool across the open,
 * reachable cells (<= MITE_CAP per cell), deterministically. */
void mites_spawn(World* w);

/* Fold the resident route field for nest n (n) / all nests (and clear the cache).
 * Called on init, a wall edit, and a nest move — not per tick. */
void mites_fold_nest(World* w, uint32_t n);
void mites_fold_nests(World* w);

/* Rebuild the per-cell index (mite_cnt + mite_list) from current positions and
 * cache each mite's centre cell in mite_cell. O(N), every tick. */
void mites_build_index(World* w);

/* Update each mite's record (double-buffered LWW: sense tanks, adopt newer peers
 * + roll hunt/home, arrive/erase), set its mode + destination, swap the buffers. */
void mites_records(World* w);

/* Collect the distinct active destinations (nests + hunters' cells), fold a route
 * field for each new one (reuse resident/cached, free dropped), track the peak. */
void mites_update_fields(World* w);

/* Per-tick movement: the crowding cap (in-order reservation) + steer each mite
 * along its route field (or greedy on field overflow, or wander). Writes mite_in. */
void mites_step(World* w);

#endif
