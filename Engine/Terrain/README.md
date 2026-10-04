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

## Canonical root

`TERRAROOT1` is the only supported root. The codec is shared by create, import,
save, asset inspection, and runtime loading; it validates the complete bounded
document before returning a value. `TERRAINROOT2`, JSON roots, unknown magic,
and non-canonical input produce `UnsupportedTerrainRoot` with recreate/recook
guidance. Compatibility decoding and partial migration are deliberately absent.

The canonical root persists 1–64 ordered layer definitions and permits 1–16
enabled layers. Stable IDs and normalized names are unique. Invalid layer
counts, identity, mapping, area references, stale generations, and publication
conflicts reject atomically, preserving the preceding descriptor, root hash,
layer bindings, and runtime snapshot.

## Physical layers and invalidation

Terrain-XZ and triplanar mapping are expressed in asset-local metres using
`(area * 256 + localSample) * sampleSpacing`. The defaults are a 4 m by 4 m
tile, zero offset, zero layer rotation, and triplanar sharpness 4. Translation
carries the appearance; render-origin changes do not alter phase, and terrain
instance rotation/scale are unsupported.

Every enabled layer publishes Base Color, tangent-space Normal, packed ORM
(`R=AO`, `G=roughness`, `B=metallic`), and Emissive. Neutral fallbacks are
white, encoded flat normal `(0.5,0.5,1)`, `(1,1,0)`, and black. Material,
factor, sampler, or mapping edits invalidate material bindings only; weight/ID
paint invalidates control textures only; height/hole edits invalidate geometry
and collision together.

## Scheduler contract

Terrain streaming workers resolve roots and shards into immutable candidates.
Terrain-derived workers prepare material, control, geometry, and collision
candidates. Workers write only prepared data; they never mutate live scene or
component storage, terrain registries, descriptor tables, or page visibility.
Protected scheduler boundaries validate expected generations and publish each
accepted root/area or derived generation in one operation. Rejection retains
the last-known-good generation.

Diagnostics expose stable root/layer codes, requested and effective sampler
quality, material/control/geometry generations, page and residency state,
stale-candidate rejection, and rollback. See the root-format and surface-runtime
guidance in [Terrain](../../Docs/Terrain.md).

## Focused verification

From the repository root, use the checked-in preset and finish with the install
gate. `clean` removes generated outputs from the configured build tree; rebuild
and reinstall before using that tree again.

```powershell
cmake --fresh --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine TerraEngine-EngineTests
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
	-R "^Engine\.Terrain|SystemScheduler|TransactionalMutation" --output-on-failure
cmake --build --preset RelWithDebInfo --target install
cmake --build --preset RelWithDebInfo --target clean
```

The complete installed-tree and live-render procedure is in
[`Docs/Terrain.md`](../../Docs/Terrain.md).
