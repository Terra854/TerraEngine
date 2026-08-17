# Terrain tools

## Create and import

Create a flat terrain or import a true 16-bit PNG/RAW16 heightfield. RAW16
files declare width, height, and little-endian encoding; malformed byte counts
and 8-bit images are rejected before an authored root is created. Terrain
instances use signed, canonical world coordinates and may reference shared
assets only after the editor reports the ownership choice.

## Sculpt, layers, and holes

Use bounded raise, lower, smooth, flatten, terrace, ramp, stamp, noise, and
height-remap strokes. Preview work is detached; commit enters the typed
transaction/history path at a protected boundary. Surface layers are named,
ordered, and generation-tracked. Keep four local contributors per control page;
the fifth contributor is previewed and displaced deterministically. Hole paint
and erase use the same canonical cell mask as rendering and collision.

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

## Heightfield limits

Terrain v1 uses heightfields: caves, overhangs, voxels, runtime deformation,
vegetation/biomes, navigation generation, and water simulation are not
supported. Collision rebuilds publish a complete replacement and retain the
last-known-good shape when a candidate is stale or invalid.
