# v3.1.0
- Buffer window now counts physics ticks instead of wall-clock time (stable under lag, pause and TPS changes)
- Settings are cached and updated by listeners instead of being read every frame
- Smart Click decision is recomputed every physics tick, so it is never a frame stale when you click
- Smart Click and the trajectory now support dual mode (both players simulated)
- Platformer: the held left/right direction is carried into the simulation
- Ghosts live in their own hidden node, and global pointers are cleared when the level layer is destroyed

# v3.0.0
- Smart Click: blocks deadly clicks, clicks for you when needed
- Lookahead, practice-only and label settings

# v2.0.0
- Trajectory preview (click / no-click paths, death and orb markers)
- Buffer zone highlight
- Per-tick buffer timing

# v1.0.0
- Initial release
