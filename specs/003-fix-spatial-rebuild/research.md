# Research: Fix Spatial Rebuild

## Decision: Replace unconditional rebuild with change-set refresh

Use a spatial refresh operation that receives the latest renderable records and applies only meaningful changes: new records, removed records, and records whose spatially relevant fields differ from the cached version.

**Rationale**: The current renderer rebuilds the complete index every update even when scene data is unchanged. A change-set refresh directly satisfies stable-scene performance goals while preserving a full-rebuild path when correctness requires it.

**Alternatives considered**:

- Keep full rebuild and optimize internals: simpler, but still scales every update with total renderable count.
- Track only dirty object IDs: helpful, but insufficient by itself because removals and newly visible renderables must also be detected.
- Move all spatial ownership into the object manager: larger architecture change than needed for this defect.

## Decision: Treat full rebuild as a valid fallback, not a failure

Use a complete rebuild when the scene is loaded, replaced, cleared, when root bounds must be recomputed beyond the current tree's safe region, or when the incoming state cannot be safely reconciled incrementally.

**Rationale**: Correct culling is more important than avoiding every full refresh. A rare full rebuild is acceptable at lifecycle boundaries and when index structure no longer safely contains changed bounds.

**Alternatives considered**:

- Never rebuild after warm-up: risks stale or missing culling results when root bounds no longer contain moved or newly added objects.
- Rebuild on a fixed timer: predictable but wastes work and can still miss immediate correctness needs.

## Decision: Compare spatial records by culling-relevant fields

Record equality should consider object identity, entity, active state, static classification, category mask, world bounds, and revision. Non-spatial renderer data should not force index refresh.

**Rationale**: The feature should update spatial data when culling could change, but it should not pay refresh costs for unrelated per-frame renderer state.

**Alternatives considered**:

- Compare full renderable structures: too broad and couples the spatial index to rendering implementation details.
- Rely only on revision counters: efficient, but unsafe if all relevant mutation paths do not already update a reliable spatial revision.

## Decision: Extend diagnostics with refresh mode and counts

Expose whether the latest refresh was unchanged, incremental, or full, plus counts for added, updated, removed, unchanged, and full-refresh entries.

**Rationale**: The reported issue is performance behavior, so developers need direct evidence that stable scenes avoid repeated full refreshes.

**Alternatives considered**:

- Use logs only: less useful for editor overlays and automated validation.
- Report only dirty object count: ambiguous because it does not distinguish removals, additions, full rebuilds, or unchanged frames.

## Decision: Validate with large stable and small-change workloads

Extend `SpatialValidation` to cover stable refresh reuse, one-object movement, additions, removals, fallback bounds, and forced full rebuild cases.

**Rationale**: The current validation proves query correctness after full rebuilds. This feature needs regression coverage that proves correctness after incremental refreshes and proves stable frames do not rebuild.

**Alternatives considered**:

- Manual editor profiling only: useful for smoke testing but insufficient for repeatable regression coverage.
- Unit-only tests for record comparison: too narrow to catch stale node membership or fallback list issues.
