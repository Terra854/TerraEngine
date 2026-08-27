# Terrain tools

## Create and import

Create a flat terrain or import a true 16-bit PNG/RAW16 heightfield. RAW16
files declare width, height, and little-endian encoding; malformed byte counts
and 8-bit images are rejected before an authored root is created. Terrain
instances use signed, canonical world coordinates and may reference shared
assets only after the editor reports the ownership choice.

Only canonical `TERRAROOT1` terrain can be opened. Older `TERRAINROOT2`, JSON,
or otherwise incompatible roots are not migrated. If the Terrain window shows
`UnsupportedTerrainRoot`, recreate from the original PNG16/RAW16 source or
recook with the current compiler, then replace the old terrain asset. A failed
load leaves the current scene terrain unchanged.

## Sculpt, layers, and holes

Use bounded raise, lower, smooth, flatten, terrace, ramp, stamp, noise, and
height-remap strokes. Preview work is detached; commit enters the typed
transaction/history path at a protected boundary. Surface layers are named,
ordered, and generation-tracked. Keep four local contributors per control page;
the fifth contributor is previewed and displaced deterministically. Hole paint
and erase use the same canonical cell mask as rendering and collision.

For textured surfaces, import textures and create compiled Material assets first.
Select a terrain layer and drag its Material onto **Drop Material Here**. Switch
to **Paint**, choose the layer, configure opacity, size, and spacing, then drag on
the terrain in Scene View. The preview updates while dragging and pointer release
queues one undoable transaction. Fill, Replace, and Normalize remain explicit
whole-area operations. The live proxy resolves four contributors per sample
against the first 16 enabled layers; unavailable authored materials render
magenta so they can be repaired without losing their stable paint IDs.

Roots may retain 64 definitions but only 1–16 layers can be enabled. Stable
layer IDs survive reorder. Enabling a 17th layer, disabling the final enabled
layer, or introducing a duplicate/zero ID or duplicate normalized name rejects
the whole edit and keeps the previous stack.

## Mapping and material channels

Terrain-XZ maps textures in asset-local metres; triplanar projects the same
physical scale across three axes. New layers default to a 4 m by 4 m tile, zero
offset, zero rotation, and triplanar sharpness 4. Adjust tile width/depth,
metre offsets, layer rotation, and sharpness in the layer controls. Translating
the terrain carries the texture with it, while distant render-origin changes do
not move the pattern. Terrain object rotation and scale are not supported.

Each layer uses four slots: Base Color, tangent-space Normal, packed ORM
(`R=AO`, `G=roughness`, `B=metallic`), and Emissive. If fallback is enabled,
missing slots use white, flat normal `(0.5,0.5,1)`, `(1,1,0)`, and black.
Otherwise Apply is rejected and the last-known-good material remains visible.
Changing material, mapping, factors, or filter does not rebuild terrain
geometry; painting weights does not rebuild geometry either.

## Filtering

Choose Nearest for hard texels, Bilinear for linear minification without mip
blending, Trilinear for mip blending, or Anisotropic for angled surfaces. The
Terrain window shows both requested and effective quality. When anisotropy is
unsupported it safely uses Trilinear; a level above the device maximum is
clamped. `DescriptorCapacityExceeded` means the global 1024 combined texture
aliases are full: reduce unique texture/filter combinations or release unused
terrain/material resources.

## Procedural tools

Height, slope, mask, and layer selections are strict about bounds and budgets.
Hydraulic/thermal erosion and spline grading run as cancellable jobs. A
cancelled or stale job discards private output and never publishes a partial
preview. Roads and rivers preserve width, shoulder, bank, depth, falloff, and
cross-area continuity.

## Diagnostics and recovery

The Terrain window distinguishes current, pending, stale, fallback, conflict,
and unavailable states. Locate an area before repairing or rebuilding it;
diagnostics include the affected coordinate, severity, impact, action, source,
generation, and stage timing. Save writes immutable blobs and a root manifest
before replacing `CURRENT`; a failed stage retains the predecessor root.

For a visible area, LOD diagnostics distinguish desired, balanced, resident,
and actually rendered levels. They also show projected error, hysteresis,
stitch mask, hole-boundary pinning, desired/actual page, own-ancestor fallback
depth, payload bytes/status, and missing-halo state. A fallback while a fine page
streams is expected; an unrelated page is never substituted. Missing interior
halo data, an unbalanced/incomplete cut, bad checksum/range, stale publication,
or rollback is actionable and retains the prior valid snapshot.

Automation exposes the same root generation, enabled stable layers, mapping,
requested/effective sampler quality, per-area LOD/page state, and stable
diagnostic codes through `terrain.asset.inspect` and
`terrain.diagnostics.get`. See [Terrain automation](../../../Docs/TerrainAutomation.md)
for bounded requests, generation checks, jobs, and receipts.

## Heightfield limits

Terrain v1 uses heightfields: caves, overhangs, voxels, runtime deformation,
vegetation/biomes, navigation generation, and water simulation are not
supported. Collision rebuilds publish a complete replacement and retain the
last-known-good shape when a candidate is stale or invalid.
