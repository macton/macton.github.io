# 001 — collision is destination-only, not swept

`tanks_move` checks only the destination cell of each axis move, not the path
swept to get there. At the default `move_speed` (20 subcells/tick, with 256
subcells per cell) a tank moves ~0.08 cell per tick, far less than one cell, so
it cannot skip a wall. But `move_speed` is editable in the debug panel; pushed
above ~256 subcells/tick (more than a cell per tick) a tank can tunnel through
a one-cell-thick wall in a single step.

Not fixing now: no real need at the default speed, and swept collision adds
branches/work the current data doesn't require. Revisit if/when fast
projectiles or a dash move make per-tick motion exceed a cell — at which point
a stepped or swept test against the grid is the right machine.
