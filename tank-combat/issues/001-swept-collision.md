# 001 — collision is destination-only, not swept

`tanks_move` checks only the destination cell of each axis move, not the path
swept to get there. At the default `move_speed` (4 world units/s, ~0.067 units
per 60 fps step) a tank moves far less than one cell per frame, so it cannot
skip a wall. But `move_speed` is editable in the debug panel; cranked high
enough, a tank can tunnel through a one-cell-thick wall in a single step.

Not fixing now: no real need at the default speed, and swept collision adds
branches/work the current data doesn't require. Revisit if/when fast
projectiles or a dash move make sub-step motion exceed a cell — at which point
a stepped or swept test against the grid is the right machine.
