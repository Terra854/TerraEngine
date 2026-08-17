# Terrain Renderer

This directory owns terrain-specific render derivation: error pyramids,
meshlets, virtual-geometry page payloads, material-control data, seam/stitch
variants, and terrain shader-facing snapshots.

## Public boundary

- Consume immutable terrain render snapshots and generation-tagged invalidation
  packets from `Engine/Terrain`.
- Publish render candidates through the existing virtual-geometry page
  streaming and protected page-publication path.
- Preserve last-known-good pages while replacement work is pending or fails.

## Private boundary

- Meshlet clustering, page packing, LOD selection, seam transitions, and
  renderer backend details remain local to this module.
- Render derivation must never become authoritative terrain storage.

Terrain shaders live under `Engine/Shaders/Terrain`; editor-only preview
shaders belong under `Editor/Shaders/Terrain`.
