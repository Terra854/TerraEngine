# Data Model: Spatial Partitioning and Object Culling

## Spatial Object

Represents one scene object that can participate in spatial queries.

**Fields**:

- `objectId`: Stable scene object identity.
- `entityId`: Runtime entity handle for current session lookup.
- `active`: Whether default queries should include the object.
- `staticClassification`: Whether the object is marked as static for optimization.
- `categoryMask`: Caller-facing filter categories such as renderable, editor-selectable, light-affecting, or diagnostic-only.
- `worldBounds`: Conservative world-space bounds used for broad queries.
- `boundsState`: Valid, empty, invalid, or fallback.
- `lastKnownTransformVersion`: Change token used to detect stale membership.

**Validation rules**:

- Active default queries must exclude objects where `active` is false.
- Invalid bounds must not crash query evaluation.
- Fallback bounds must preserve visibility correctness by including uncertain objects.
- Object identity must be stable across index updates within a frame.

**State transitions**:

- `Unindexed` -> `Indexed`: object has valid or fallback bounds and participates in queries.
- `Indexed` -> `Dirty`: transform, hierarchy, active state, renderable content, or category data changed.
- `Dirty` -> `Indexed`: spatial membership refreshed.
- `Indexed` -> `Removed`: object destroyed or no longer spatially queryable.

## World Bounds

Conservative world-space area occupied by a spatial object.

**Fields**:

- `min`: Minimum world coordinate.
- `max`: Maximum world coordinate.
- `center`: Derived midpoint for query sorting and diagnostics.
- `extent`: Derived half-size for broad checks.
- `source`: Mesh-derived, authored override, aggregate child bounds, fallback, or empty.
- `epsilon`: Small expansion value to avoid boundary flicker.

**Validation rules**:

- `min` must be less than or equal to `max` on every axis after sanitization.
- Non-finite values must produce an invalid or fallback state.
- Very small bounds should be expanded by epsilon for stable boundary behavior.
- Very large bounds may be stored at a higher-level partition node or fallback list.

## Spatial Query

Caller request for objects overlapping a spatial shape or visible region.

**Fields**:

- `queryType`: AABB, sphere, frustum, or ray.
- `shape`: Query geometry in world space.
- `includeInactive`: Whether inactive objects should be returned.
- `categoryMask`: Filters object categories.
- `sortMode`: None, stable identity, distance ascending, or caller-defined.
- `maxResults`: Optional result cap.
- `diagnosticMode`: Whether fallback and rejected counts should be collected.

**Validation rules**:

- Query shape values must be finite.
- Ray direction must be non-zero after normalization.
- Result order must be deterministic for identical inputs.
- Result caps must not make rendering visibility incorrect; caps are appropriate only for callers that explicitly request them.

## Visible Candidate Set

Camera-specific collection of objects considered potentially visible.

**Fields**:

- `cameraId`: Object or override camera identity when available.
- `viewTag`: Scene view or game view.
- `queryFrame`: Frame identifier used for diagnostics.
- `objects`: Spatial object identities accepted by the visibility query.
- `fallbackObjects`: Objects included because exact bounds were unavailable.
- `rejectedCount`: Objects excluded by the broad visibility query.

**Validation rules**:

- Must include partially visible and uncertain objects.
- Must not include destroyed objects.
- Must be safe when no camera exists.
- Must preserve deterministic output for identical camera and scene state.

## Spatial Diagnostics Snapshot

Read-only summary for editor, validation, and regression tooling.

**Fields**:

- `indexedObjectCount`
- `staticObjectCount`
- `dynamicObjectCount`
- `dirtyObjectCount`
- `visibleCandidateCount`
- `fallbackObjectCount`
- `lastQueryDuration`
- `lastUpdateDuration`
- `maxNodeDepth`
- `largeObjectCount`

**Validation rules**:

- Snapshot reads must not mutate the index.
- Counts must describe the same frame/update interval.
- Diagnostic collection must be optional or lightweight enough for normal editor use.
