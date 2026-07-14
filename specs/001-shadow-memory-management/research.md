# Research: Shadow Memory Management

## Decision: Demand-driven typed allocation before cache and atlas work

**Rationale**: The observed memory spike comes from allocating every shadow resource kind for each configured slot. A high global shadow capacity should not reserve directional, spot, area, and point resources for every possible slot. Fixing allocation first directly addresses the largest failure mode and reduces risk for later cache and atlas work.

**Alternatives considered**:

- Increase raw shadow count: rejected because memory and pass count scale with capacity and point cubemap resources dominate.
- Add caching first: useful for frame time, but unchanged allocation still consumes excessive memory.
- Add atlas first: useful for 2D shadows, but does not solve point cubemap over-allocation or allocation by inactive capacity.

## Decision: Preserve existing winner selection and change the resource request shape

**Rationale**: Current light prioritization already accounts for visibility, authored priority, stability, and shadow eligibility. The safer change is to keep that behavior and produce typed resource demand from the selected winners: directional 2D, spot 2D, area 2D, and point cube.

**Alternatives considered**:

- Replace the prioritization model: rejected because it increases behavioral risk and is not required for memory safety.
- Keep a single logical slot index shared across all light types: rejected because it is the root of allocating every resource kind for every slot.

## Decision: Track shadow cache validity explicitly

**Rationale**: Shadow cache reuse must be correct before it is aggressive. Each cached result needs an owner, light signature, caster signature, storage region, last refresh frame, and invalidation reason. Diagnostics must distinguish reused, refreshed, degraded, and unassigned shadows.

**Alternatives considered**:

- Reuse based only on stable slot owner: rejected because current slot reuse does not prove the light or casters are unchanged.
- Cache indefinitely until light movement: rejected because caster movement, material shadow behavior, visibility, and geometry changes can all invalidate shadows.

## Decision: Use conservative invalidation first

**Rationale**: The first cache milestone should favor visual correctness. If caster signatures are incomplete or uncertain, the cache entry should refresh. This satisfies correctness while still allowing static scenes to gain reuse after the needed signatures exist.

**Alternatives considered**:

- Per-triangle or per-meshlet exact dependency tracking in the first milestone: rejected because it is too large for the initial implementation.
- Timed cache reuse without scene signatures: rejected because stale shadows can persist after object changes.

## Decision: Treat 2D atlas management as managed storage regions

**Rationale**: Directional, spot, and area shadows can share 2D managed storage with region metadata, variable resolution, and degradation when full. The plan should not require every 2D light to own one full-resolution image.

**Alternatives considered**:

- One monolithic atlas for every light type including point lights: rejected because point cubemaps have different sampling and face requirements.
- Keep one image per 2D shadow forever: rejected because it fails the memory goals for many lower-priority 2D shadows.

## Decision: Keep point shadows separate and budget them by true cost

**Rationale**: Point shadows require omnidirectional coverage and are materially more expensive than a 2D shadow. They should allocate only when active point shadows are assigned and should have diagnostics that make their memory and refresh cost visible.

**Alternatives considered**:

- Convert point lights to six atlas regions immediately: deferred because it changes shader sampling and complicates the first memory-safety milestone.
- Count point lights as equal to 2D shadows: rejected because it hides their real storage and pass cost.

## Decision: Extend existing diagnostics instead of adding a new external API

**Rationale**: TerraEngine already exposes frame budget and lighting diagnostics. Extending those snapshots gives the editor, validation scenes, and captures one source of truth for shadow assignment, memory, cache reuse, refresh, and degradation.

**Alternatives considered**:

- Add a standalone diagnostics subsystem: rejected as unnecessary for the first implementation.
- Log-only diagnostics: rejected because success criteria require repeatable inspection and validation.
