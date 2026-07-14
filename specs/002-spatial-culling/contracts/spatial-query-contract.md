# Spatial Query Contract

## Purpose

Define the behavior expected by engine systems that query spatial objects for rendering, editor selection, diagnostics, and future broadphase work.

## Caller Responsibilities

- Provide finite query geometry.
- Choose filters that match the use case: default active-only filtering for runtime behavior, diagnostic inclusion for editor/debug views.
- Treat returned objects as candidates, not narrow-phase proof of exact intersection.
- Avoid mutating scene object membership while consuming a query result from the same snapshot.

## Provider Responsibilities

- Return deterministic results for identical scene state and query input.
- Preserve correctness by including uncertain objects when bounds are missing or invalid.
- Exclude inactive objects by default.
- Exclude destroyed objects always.
- Keep query results valid for the snapshot/frame in which they were requested.
- Provide optional diagnostics describing candidate count, fallback count, and rejection count.

## Query Types

### Area Query

**Input**: World-space area bounds, category filters, active-state filter, optional result cap.

**Output**: Objects whose conservative bounds overlap the area.

**Required behavior**:

- Boundary-overlapping objects are included.
- Very large objects are included when they overlap the area.
- Invalid query geometry returns a controlled empty/error result without crashing.

### Sphere Query

**Input**: World-space center, non-negative radius, category filters, active-state filter.

**Output**: Objects whose conservative bounds may overlap the sphere.

**Required behavior**:

- Radius zero behaves as a point proximity query.
- Negative or non-finite radius is rejected as invalid input.

### Frustum Query

**Input**: Camera-visible region, category filters, active-state filter.

**Output**: Visible candidate set for render preparation or diagnostics.

**Required behavior**:

- Partially visible objects are included.
- Objects with uncertain bounds are included through fallback handling.
- Result set is safe for render preparation and does not hide visible objects.

### Ray Query

**Input**: Ray origin, ray direction, maximum distance, category filters, active-state filter, sort preference.

**Output**: Candidate objects whose conservative bounds may intersect the ray.

**Required behavior**:

- Results can be sorted nearest-first when requested.
- Ray queries are broad-phase only; callers that need exact mesh hits must perform a narrower follow-up test.
- Non-finite origin, non-finite direction, zero-length direction, or invalid distance is rejected as invalid input.

## Diagnostics Contract

Spatial diagnostics expose the latest snapshot counts:

- Indexed object count.
- Static object count.
- Dynamic object count.
- Visible candidate count.
- Rejected object count.
- Fallback object count.
- Query/update duration.

Diagnostics must be read-only and must not alter spatial membership.
