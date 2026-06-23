/* mites.h — the swarm: spawn, the per-cell index, and the per-tick mite update.
 *
 * A mite is a quarter-cell driven body in a fixed pool (defs.h). These three
 * transforms own everything mite-specific; the mites then flow through the SAME
 * agent_turn/agent_move the tanks use (sim.c wires the order). The lesson is
 * scale: one entity is easy, a thousand is a data layout.
 *
 * mites_step is the mite analogue of tanks_path — it turns each mite's mode into
 * an input byte — but where a tank reads a precomputed route table, a mite owns:
 *   - the shared knowledge: one world cell + one timestamp (the last-known tank
 *     position), copied on contact. Last-write-wins, double-buffered so the
 *     gossip is order-independent (read the previous tick, write the next, swap).
 *   - the crowding cap: <= MITE_CAP centres per cell, enforced by a deterministic
 *     in-order reservation against the per-cell index (no per-unit physics).
 *   - the wander/seek decision: choose the next cell (random open neighbour, or
 *     the one greedily nearest the recorded cell), then steer to it.
 *
 * All randomness is one xorshift32 stream (World.rng), advanced as mites are
 * processed in index order, so the whole swarm is a pure function of the seed and
 * the inputs: same seed + same inputs => identical state. */
#ifndef MITES_H
#define MITES_H

typedef struct World World;

/* Seed the PRNG from World.mite_seed and scatter the pool across the open,
 * reachable cells (<= MITE_CAP per cell), deterministically. Called on init and
 * whenever the seed is edited (re-seed + re-scatter). */
void mites_spawn(World* w);

/* Rebuild the per-cell index (mite_cnt + mite_list) from current positions, and
 * cache each mite's centre cell in mite_cell. O(N), every tick — the swarm's
 * acceleration structure. mite_cnt is the TRUE occupancy (so an over-cap cell is
 * detectable); mite_list holds the first MITE_CAP occupants. */
void mites_build_index(World* w);

/* The per-tick mite transform: update records (sense tanks, read peers 80/20,
 * arrive/erase — double-buffered LWW), pick each mite's mode + next cell
 * (cap-gated, in-order reservations), and write mite_in. Reads the index built
 * this tick; runs before agent_turn/agent_move over the mites. */
void mites_step(World* w);

#endif
