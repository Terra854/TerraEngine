# Research: Spatial Partitioning and Object Culling

## Decision: Use Conservative World AABBs as the Shared Bound

**Rationale**: A world-space axis-aligned bounding box gives every caller a simple, conservative shape for overlap, visibility, ray prefiltering, diagnostics, and fallback behavior. It can be derived from mesh/resource data and transformed object state without requiring render-specific knowledge. Conservative inclusion also matches the spec requirement that uncertain objects remain visible rather than incorrectly hidden.

**Alternatives considered**:

- Bounding spheres: cheaper for frustum checks but less precise for elongated meshes and broad area queries.
- Oriented bounding boxes: more precise but more expensive and harder to aggregate for broad queries.
- Meshlet-only bounds: already useful for GPU culling, but too low-level for object visibility, editor picking, and scene query contracts.

## Decision: Use a Loose Octree as the First Spatial Partition

**Rationale**: TerraEngine scenes are 3D worlds, not just 2D maps. A loose octree handles uneven object distribution and large open scenes better than a fixed dense grid while still giving straightforward insertion, removal, update, and frustum/area traversal behavior. Looseness reduces churn for objects near cell boundaries and allows objects to remain in parent nodes when they are too large for child nodes.

**Alternatives considered**:

- Fixed spatial hash: simple and good for uniform dynamic objects, but less efficient for very large worlds with uneven density and large static scenery.
- BVH: strong for static geometry, but dynamic updates and deterministic editor edits add complexity for the first implementation.
- Per-frame linear scan only: simplest, but fails the feature goal of avoiding whole-scene scans.

## Decision: Keep Static and Dynamic Membership in One Query Surface

**Rationale**: Callers should not need separate code paths for static and dynamic objects. The implementation can optimize static objects internally while preserving one query contract and identical result semantics.

**Alternatives considered**:

- Separate static and dynamic query APIs: exposes optimization details and increases caller complexity.
- Static-only initial implementation: useful for early performance, but incomplete for editor and runtime correctness.

## Decision: Update Spatial Membership at Deterministic Scene Snapshot Points

**Rationale**: Current render preparation snapshots transforms, renderables, cameras, and lights before parallel work. Spatial membership should follow the same model: gather authoritative scene state, update changed bounds deterministically, then allow parallel read/query work. This keeps query results stable for a frame and avoids data races against component mutation.

**Alternatives considered**:

- Immediate updates on every component write: more responsive but risks scattered synchronization and harder-to-debug editor behavior.
- Full rebuild every frame: simple and deterministic, but too expensive for the target scale unless used only as a fallback or validation mode.

## Decision: Use Task-Parallel Query Filtering with Serialized Structural Mutation

**Rationale**: The constitution requires task-based concurrency and avoiding single-threaded bottlenecks. Traversing candidate nodes and post-filtering objects can be parallelized. Structural mutation of the index can remain serialized at frame/update boundaries because it is shorter, easier to reason about, and protects correctness.

**Alternatives considered**:

- Fully lock-free spatial index: unnecessary complexity for the initial feature and difficult to validate.
- Fully single-threaded queries: simpler but conflicts with scale and constitution goals.

## Decision: No New Third-Party Dependencies

**Rationale**: The repository requires self-contained dependencies and explicit approval for `External/` changes. The needed math, containers, object identity, and task facilities already exist in the engine.

**Alternatives considered**:

- Import a third-party spatial index library: rejected because it would require dependency review and may not match engine lifecycle needs.
- Use dependency code already present under `External/assimp`: rejected because those utilities are importer-oriented and not engine-runtime scene query infrastructure.

## Decision: Keep Shader Work Out of Initial Scope

**Rationale**: The engine already has GPU meshlet, Hi-Z, and light-cluster culling. This feature adds object-level scene filtering before draw preparation. Shader changes are not required to deliver the specified user value.

**Alternatives considered**:

- Move object culling to GPU immediately: may be useful later, but complicates diagnostics and editor query reuse.
- Modify task shaders for object-level diagnostics: deferred until CPU-side candidate filtering is validated.
