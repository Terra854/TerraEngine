# Terrain Physics

This directory owns provider-neutral terrain collision preparation and its
backend publication integration.

## Public boundary

- Convert immutable terrain height/hole snapshots into a provider-neutral
  heightfield recipe.
- Stage collision candidates on worker jobs and publish shape replacement at
  the declared physics boundary.
- Preserve the last valid collision until a complete replacement is ready.

## Private boundary

- Backend-specific heightfield allocation, hole sentinels, and shape cooking
  stay behind the physics backend interface (for example, Jolt).
- Live shapes must not be edited while physics queries can observe them.

Collision coverage is demand-driven by resident terrain areas and uses the same
authoritative hole channel as rendering, picking, and shadows.
