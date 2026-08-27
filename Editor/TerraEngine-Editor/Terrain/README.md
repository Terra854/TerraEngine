# Terrain Editor

This directory owns editor-facing terrain creation, import, sculpting,
selection, preview, procedural tools, history integration, diagnostics, and
automation providers.

## Public boundary

- Route editor input and automation through typed terrain edit transactions.
- Keep drag previews detached from committed authored state.
- Report progress, cancellation, conflicts, receipts, and unavailable-target
  diagnostics through the existing editor job/automation services.

## Private boundary

- Brush kernels, masks, selection evaluation, import remapping, and editor UI
  state remain local to this module.
- Editor workers prepare candidates only; protected publication owns the commit.

Visible tools and automation must produce equivalent typed transactions,
history entries, dirty-state changes, and derived invalidation.

## Canonical editor contract

Every create, import, save, inspect, and runtime-open route uses
`TERRAROOT1`. Older `TERRAINROOT2`, JSON, unknown, or non-canonical roots are
rejected with recreate/recook guidance; the editor does not migrate them or
publish a partial replacement. Layer edits preserve 1–64 definitions, 1–16
enabled layers, non-zero unique stable IDs, and unique normalized names as one
atomic root generation.

The layer UI exposes Terrain-XZ and triplanar physical mapping in asset-local
metres: tile width/depth (4 m defaults), metre offsets, layer rotation, and
triplanar sharpness (default 4). It also exposes Base Color, tangent-space
Normal, packed ORM (`R=AO`, `G=roughness`, `B=metallic`), Emissive, factors,
fallback policy, and Nearest/Bilinear/Trilinear/Anisotropic filtering. Requested
and effective quality are both visible; unsupported anisotropy falls back to
trilinear and over-limit values clamp without rejecting the terrain.

## Publication and diagnostics

Editor and automation workers prepare detached candidates. Protected commits
publish accepted authored roots and derived material/control/geometry
generations; VG/RHI publication makes upload-complete pages visible. No worker
mutates live object/component storage, terrain/material registries, descriptor
tables, or page visibility. Material/mapping/filter edits avoid geometry cooks,
weight edits avoid geometry cooks, and height/hole edits advance geometry and
collision together.

The Terrain window and automation snapshots expose the canonical root identity,
enabled stable layers, physical mapping, requested/effective sampler and
fallback reason, descriptor capacity/deduplication, material/control generation
touches, and every visible area's desired/balanced/resident/actual LOD and page.
Projected error, hysteresis, stitch mask, hole pinning, own-ancestor fallback
depth, payload status, missing halo, stale rejection, and rollback are
actionable rather than hidden behind an origin-only proxy.

See the artist-facing [Terrain help](../Help/TerrainHelp.md), the
[automation guide](../../../Docs/TerrainAutomation.md), and the main
[terrain guide](../../../Docs/Terrain.md).
