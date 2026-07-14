# Contract: Shadow Diagnostics Snapshot

This contract defines the observable diagnostics expected from the shadow memory management feature. It extends the renderer's existing frame budget and lighting diagnostics rather than adding an external service boundary.

## Snapshot Fields

Each rendered view must expose:

- `configuredShadowCapacity`: Maximum shadow-capable lights allowed by the active settings.
- `shadowEligibleLightCount`: Retained lights that requested shadows.
- `activeShadowCount`: Lights that received usable shadow storage.
- `allocatedRegionCount`: Live shadow storage regions.
- `allocated2DRegionCount`: Live 2D-compatible shadow regions.
- `allocatedPointRegionCount`: Live point-compatible shadow regions.
- `estimatedShadowMemoryBytes`: Estimated GPU memory currently held by shadow resources.
- `cacheReuseCount`: Shadows reused without redraw this frame.
- `cacheRefreshCount`: Shadows redrawn this frame.
- `cacheInvalidationCount`: Cached shadows invalidated this frame.
- `degradedShadowCount`: Eligible lights that were denied or reduced in quality.
- `allocationFailureCount`: Shadow storage requests that could not allocate memory.

## Per-Light Assignment Fields

Each shadow-eligible light must expose:

- `lightId`
- `resourceKind`
- `assigned`
- `storageRegionId`
- `estimatedBytes`
- `qualityTier`
- `reusedCachedResult`
- `refreshedThisFrame`
- `invalidatedThisFrame`
- `degradationReason`
- `refreshReason`

## Acceptance Rules

- A high configured shadow capacity with low active demand must show low `allocatedRegionCount` and low `estimatedShadowMemoryBytes`.
- Scenes without point shadow assignments must report `allocatedPointRegionCount = 0`.
- Any denied eligible light must report a non-empty `degradationReason`.
- Cache reuse must be visible as `reusedCachedResult = true` and must not also report `refreshedThisFrame = true` for the same light in the same frame.
- Allocation failure must degrade shadows and keep the view renderable.

## Validation Scenarios

1. Capacity 32, 4 active 2D shadows: diagnostics show 4 active shadows, no point regions, and memory proportional to 4 active 2D regions.
2. Capacity 32, 0 active shadows: diagnostics show zero active shadows and zero allocated regions after unused resources are released.
3. Static scene after warm-up: diagnostics show cache reuse for most eligible stable shadows.
4. Moving caster: diagnostics show invalidation and refresh for affected shadows.
5. Too many eligible shadows: diagnostics show deterministic degradation reasons for lower-priority shadows.
