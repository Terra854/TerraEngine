# Data Model: Shadow Memory Management

## Shadow Assignment

Represents the per-frame decision for one shadow-capable retained light.

**Fields**:

- `lightId`: Stable light owner identifier.
- `lightType`: Directional, point, spot, or area.
- `assigned`: Whether this light received shadow storage this frame.
- `priorityScore`: Composite selection score from visibility, authored priority, stability, and shadow cost.
- `logicalShadowIndex`: Stable logical index used by the light payload when assigned.
- `storageRegionId`: Region used by the assigned shadow result.
- `degradationReason`: None, capacity limit, memory budget, unsupported type, allocation failure, or lower priority.
- `wasShadowedPreviousFrame`: Whether the light had an assigned shadow in the previous frame.
- `reusedPreviousStorage`: Whether the same storage owner was retained.

**Validation Rules**:

- Assigned shadows must reference a valid storage region.
- Unassigned shadows must keep direct lighting active and provide a degradation reason when eligible.
- A point-light assignment must reference point-compatible storage.
- A 2D-light assignment must reference 2D-compatible storage.

## Shadow Storage Region

Represents a resource allocation or atlas region used by one reusable shadow result.

**Fields**:

- `regionId`: Stable storage identifier.
- `resourceKind`: Directional 2D, spot 2D, area 2D, shared 2D region, or point cube.
- `ownerLightId`: Light currently using the region.
- `width`: Region width in pixels.
- `height`: Region height in pixels.
- `layerOrFaceCount`: Number of layers or faces covered by this region.
- `estimatedBytes`: Estimated GPU memory consumed by the region.
- `qualityTier`: Full, half, quarter, or degraded.
- `state`: Free, assigned, cached, invalid, pending release.
- `lastUsedFrame`: Last frame in which the region was assigned or sampled.

**Validation Rules**:

- Free regions must not have an owner light.
- Assigned and cached regions must have an owner light.
- Estimated memory must include all faces for point-compatible storage.
- Region dimensions must be positive and fit within the selected quality preset limits.

**State Transitions**:

- Free -> Assigned when a light receives storage.
- Assigned -> Cached when a valid shadow result can be reused.
- Cached -> Invalid when light or caster inputs change.
- Invalid -> Assigned when the region is refreshed.
- Assigned/Cached/Invalid -> Pending Release when the owner no longer needs it.
- Pending Release -> Free after GPU lifetime safety has elapsed.

## Shadow Cache Entry

Represents a reusable shadow result and the signatures that prove it is still valid.

**Fields**:

- `lightId`: Owner light identifier.
- `storageRegionId`: Region containing the cached shadow result.
- `lightSignature`: Shadow-affecting light property signature.
- `casterSignature`: Shadow-affecting caster set signature.
- `viewSignature`: Shadow projection or view-dependent signature where relevant.
- `valid`: Whether the entry can be reused.
- `lastRefreshFrame`: Last frame where the result was rendered.
- `lastReuseFrame`: Last frame where the result was reused.
- `refreshReason`: Initial render, light changed, caster changed, material changed, storage moved, quality changed, forced refresh, or allocation recovery.

**Validation Rules**:

- Valid cache entries must reference an assigned or cached storage region.
- Any unknown signature must force refresh instead of reuse.
- Storage movement or region resizing must invalidate the cache entry.

## Shadow Diagnostics Snapshot

Represents author/debug visibility for one rendered view.

**Fields**:

- `configuredShadowCapacity`: User or preset shadow capacity.
- `activeShadowCount`: Number of shadows actually assigned.
- `allocatedRegionCount`: Number of live storage regions.
- `allocated2DRegionCount`: Number of live 2D-compatible regions.
- `allocatedPointRegionCount`: Number of live point-compatible regions.
- `estimatedShadowMemoryBytes`: Total estimated shadow storage memory.
- `cacheReuseCount`: Number of assigned shadows reused this frame.
- `cacheRefreshCount`: Number of assigned shadows refreshed this frame.
- `cacheInvalidationCount`: Number of cache entries invalidated this frame.
- `degradedShadowCount`: Number of eligible shadows denied or lowered in quality.
- `assignments`: Per-light assignment details.

**Validation Rules**:

- Active shadow count must not exceed configured shadow capacity.
- Estimated memory must equal the sum of live storage region estimates.
- Cache reuse plus refresh count must not exceed active shadow count.
- Degraded shadow count must match per-assignment degradation reasons.
