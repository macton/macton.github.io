/* tanks_fire — the combat transform: the tanks shoot the swarm.
 *
 * Independent transform (own file, per AGENTS.md): it reads the mite index + the
 * grid (line of sight) and the tank/mite positions, and writes the turrets, the
 * fire cooldowns, the killed mites' respawn timers, and the death-cry records. The
 * tank BODY still moves on tank_ang through agent_turn/agent_move; the TURRET
 * (tank_turret) aims independently here.
 *
 * Per tank, each tick: find the nearest mite it has line of sight to — a rough
 * ring scan outward over the per-cell index, stopping at the first ring with a
 * visible mite (lowest index breaks ties) — snap the turret to it (nearest of the
 * 32 headings), tick the cooldown, and when ready fire: one shot kills the target
 * mite. A kill is a "death cry": every mite within 2x sensing range of the dead
 * one learns the firing tank's cell (record stamped now + hunt), so the swarm
 * turns on its attackers by the same gossip that spreads any sighting.
 *
 * `fire_period == 0` disables firing (the turret still aims). Firing is fully
 * deterministic — it uses no RNG — so the swarm stays a pure function of the seed
 * and inputs. */
#ifndef TANKS_FIRE_H
#define TANKS_FIRE_H

typedef struct World World;

void tanks_fire(World* w);      /* aim every turret, tick cooldowns, fire + death cry */
void mites_respawn(World* w);   /* revive timed-out dead mites at their nests (cap-respecting) */

#endif
