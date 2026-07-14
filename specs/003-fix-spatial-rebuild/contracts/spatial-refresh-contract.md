# Spatial Refresh Contract

## Purpose

Define the observable behavior expected from the spatial index refresh path after replacing the per-update full rebuild.

## Refresh Inputs

The renderer provides the latest complete set of renderable spatial entries for the current update. Each entry includes identity, active state, category, static classification, bounds, and revision.

## Refresh Outcomes

The spatial system reports one of these modes after each refresh:

- `Unchanged`: The latest input matches the existing spatial state and no index membership changes were required.
- `Incremental`: Added, updated, or removed entries were applied without rebuilding the entire index.
- `Full`: The entire index was rebuilt because lifecycle or safety conditions required it.

## Required Behaviors

- Stable input after warm-up must not report repeated `Full` refreshes.
- Added entries must appear in relevant queries during the same update.
- Removed entries must be absent from all query results after refresh.
- Moved entries must stop appearing in their old spatial region and appear in their new spatial region when queried.
- Fallback-bound entries must remain queryable even when they cannot be placed into normal spatial nodes.
- A full refresh must produce query results equivalent to a freshly built index from the same entries.

## Diagnostics

The latest diagnostics snapshot must expose enough information for tests and developers to distinguish:

- No spatial changes.
- Incremental changes with changed-entry counts.
- Full refresh with total processed-entry count.
- Query candidate, rejected, fallback, and timing values already provided by the spatial diagnostics.

## Compatibility

Existing query functions continue to return query results in deterministic order and continue to honor active-state and category filters.
