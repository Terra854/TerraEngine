# Data Model: Fix Spatial Rebuild

## Spatial Entry

Represents one renderable object's membership in spatial queries.

**Fields**:

- `objectId`: Stable object identifier used by query results and index membership.
- `entity`: Runtime entity identifier used to map query hits back to renderable data.
- `active`: Whether default renderable queries should include the object.
- `isStatic`: Whether the object is classified as static for diagnostics and refresh policy.
- `categoryMask`: Query categories the object belongs to.
- `worldBounds`: Current query bounds, including fallback or invalid state.
- `revision`: Change version supplied by scene/object state.

**Validation rules**:

- Empty object IDs are not inserted.
- Entries with invalid or fallback bounds remain queryable through fallback handling.
- Entries with unchanged culling-relevant fields must not force spatial node updates.

## Spatial Change

Represents a detected difference between the previous spatial entry set and the latest renderable snapshot.

**Types**:

- `Added`: Object exists in the latest snapshot but not in the previous index state.
- `Updated`: Object exists in both states and at least one culling-relevant field changed.
- `Removed`: Object existed previously but is absent from the latest snapshot.
- `Unchanged`: Object exists in both states and culling-relevant fields are equivalent.
- `FullRefreshRequired`: The change set cannot be applied safely without rebuilding the index structure.

**Validation rules**:

- Added entries must be queryable in the same update they are submitted.
- Removed entries must not appear in query results after refresh.
- Updated entries must leave old spatial regions and appear in new valid regions.
- Full refresh must produce the same visible results as a fresh index built from the latest snapshot.

## Spatial Refresh Summary

Describes the latest spatial refresh for diagnostics, tooling, and tests.

**Fields**:

- `refreshMode`: `Unchanged`, `Incremental`, or `Full`.
- `addedCount`: Number of newly inserted entries.
- `updatedCount`: Number of refreshed existing entries.
- `removedCount`: Number of removed entries.
- `unchangedCount`: Number of entries reused without spatial updates.
- `fullRefreshCount`: Number of entries processed during a complete refresh.
- `lastUpdateDurationMs`: Time spent refreshing spatial data.

**Validation rules**:

- Stable frames report `Unchanged` and zero added, updated, and removed entries.
- Partial-change frames report `Incremental` and bounded changed-entry counts.
- Lifecycle rebuilds report `Full` with the processed entry count.

## Scene Refresh Boundary

Represents a lifecycle point where a full spatial refresh is expected.

**Examples**:

- Scene load.
- Scene unload or clear.
- Scene replacement.
- Loss of reliable previous index state.
- Root bounds no longer safely contain indexable entries.

**Validation rules**:

- A boundary may trigger one full refresh.
- Subsequent stable updates after the boundary must return to unchanged or incremental refresh behavior.
