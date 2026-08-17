# Terrain Runtime Authority

This directory owns the provider-neutral authored terrain model and its runtime
publication boundary. Code here defines terrain coordinates, immutable asset
roots, area/channel storage, typed edit transactions, mutation queues,
streaming, diagnostics, and derived invalidation packets.

## Public boundary

- Stable types consumed by scene, resource, editor, renderer, and physics code.
- `TerrainComponent` remains a small scene reference; bulk data stays in the
  terrain asset store.
- Runtime reads use immutable snapshots and `ContentMount`/TEPAK resolution.

## Private boundary

- Area blob layout, compression, copy-on-write roots, and worker preparation
  details are implementation-owned by this module.
- Workers may prepare candidates but may not mutate live scene or component
  storage. Publication occurs only at declared scheduler boundaries.

Terrain data is authored as height, hole, and surface-control channels. Mesh,
virtual-geometry pages, and collision shapes are derived and replaceable.
