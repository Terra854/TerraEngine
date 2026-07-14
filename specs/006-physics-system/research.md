# Research: Complete Physics System

**Date**: 2026-07-13  
**Status**: All planning unknowns resolved; provider selection remains conditional on the recorded spike.

## Physics Provider

### Decision

Conditionally select **Jolt Physics v5.6.0**, pinned to commit `e77f175595e64cb44218cc9d9d56fc365ad0e36a`, under `External/JoltPhysics`. Keep **PhysX 5.8.0**, tag `110.0-omni-and-physx-5.8.0`, commit `fc1018a3745664a1db2b95ce03fb5e91eb585f2e`, as a fallback candidate requiring a return to planning and separate explicit approval if Jolt fails. Jolt 5.6.0's 2026-07-11 release has only two days of burn-in, so its CPU-only configuration, new friction behavior, gravity-force and `LinearCast` fixes, determinism, memory, and performance require explicit spike evidence; its GPU-compute and hair features remain disabled and outside initial scope.

The user explicitly approved the planned Jolt addition on 2026-07-13. Under constitution v1.3.1, approval permits implementation to add this exact pristine dependency, revision, location, and integration scope after the spike passes; it does not permit modifying provider source, upgrading Jolt, or substituting PhysX without returning to planning.

### Rationale

Jolt is the closest architectural fit:

- MIT-licensed, focused source tree, standard-library-only core, CMake install support, and documented Windows/Linux support.
- Abstract job-system and barrier interfaces can lower provider work onto TerraEngine's shared pool.
- Direct coverage for required primitives, tapered capsule, convex/compound/mesh shapes, CCD, sensors, queries, constraints, characters, ragdolls, vehicles, and soft bodies.
- Optional double-position mode and unusually explicit determinism documentation.
- Allocator, assertion/log, external profiler, debug renderer, statistics, and recorder hooks.
- Lower offline vendoring and build-maintenance risk than the larger finalists.

Official references: [Jolt repository and license](https://github.com/jrouwe/JoltPhysics/tree/v5.6.0), [Jolt 5.6.0 release](https://github.com/jrouwe/JoltPhysics/releases/tag/v5.6.0), [architecture](https://jrouwe.github.io/JoltPhysics/), [job interface](https://github.com/jrouwe/JoltPhysics/blob/v5.6.0/Jolt/Core/JobSystem.h), and [performance test](https://jrouwe.github.io/JoltPhysics/md__docs_2_performance_test.html).

### Candidate Matrix

| Criterion | Jolt 5.6 | PhysX 5.8 | Bullet 3.25 | Newton 4.x |
|-----------|----------|-----------|-------------|--------------|
| License | MIT | BSD-3-Clause | zlib core; repository audit needed | zlib-style; audit needed |
| Windows/Linux | First-class, broad CPU coverage | Strong x64/aarch64 support | Supported | Supported, less documented |
| Engine job adapter | Excellent abstract job/barrier interface | Excellent application CPU dispatcher | Partial and uneven pipeline coverage | Multithreaded solver, weaker adapter evidence |
| Required shapes/CCD | Direct coverage including tapered capsule | Strong; tapered capsule needs approximation | Broad; tapered capsule indirect | Broad core support |
| Filters/materials/sleep | Direct layers, masks, sensors, combine callbacks, sleep | Strong built-ins | Mature | Broad |
| Queries | Ray, shape cast, overlap/collide-shape, point and filters | Ray, sweep, overlap, point-distance | Ray, convex sweep, contact tests | Less compelling evidence |
| Constraints | Required families including gear and motors | Required families through standard/D6 joints | Required families including gear | Broad |
| Character | Rigid and virtual characters with slope/stair/platform support | Mature kinematic controller | Available but less production-oriented | Weaker evidence |
| Advanced path | Ragdoll, vehicle, soft body in core | Strong articulations/vehicles; GPU deformables | All present | Broad |
| Large world | Optional double positions | Application-visible origin shifting | Compile-time double precision | Double precision |
| Determinism | Strong same-build contract; engine must sort callbacks/results | Limited/enhanced mode, still sensitive | Weak documented contract | Insufficient evidence |
| Diagnostics | Debug renderer/recorder, profiler, allocator and simulation stats | Excellent OmniPVD/profiling | Basic | Smaller ecosystem |
| Maintenance | Active focused releases | Active professional releases | Formal release 3.25 dates to 2022 | Active but smaller and less stable ecosystem |
| Offline vendoring | Lowest risk | Highest risk due size and generation/Packman assumptions | High pruning/license surface | Very large repository/history |

PhysX is technically credible and stronger in GPU deformables, direct break thresholds, and external visualization, but its source/build distribution has a larger offline integration surface. Bullet is permissive and feature-rich but has weaker current release, job-integration, and determinism evidence. Newton does not overcome its smaller documentation/adoption and repository risks. Havok is not a permissive self-contained source candidate; ReactPhysics3D lacks the required breadth.

PhysX official references: [5.8 source](https://github.com/NVIDIA-Omniverse/PhysX/tree/110.0-omni-and-physx-5.8.0), [documentation](https://nvidia-omniverse.github.io/PhysX/), and [threading guide](https://nvidia-omniverse.github.io/PhysX/physx/5.4.0/docs/Threading.html). Bullet references: [repository](https://github.com/bulletphysics/bullet3), [3.25 source](https://github.com/bulletphysics/bullet3/tree/3.25), and [license](https://github.com/bulletphysics/bullet3/blob/3.25/LICENSE.txt).

### Evidence Required Before Final Selection

The provider spike compares the approved Jolt 5.6 candidate and the unapproved PhysX 5.8 research fallback CPU-only on identical hardware and build settings:

1. Network-disabled configure/build/install and dependency audit.
2. Temporary TerraEngine allocator, logger/assertion, external profiler, and shared job adapters.
3. Required shape, CCD, trigger, filtering, material, query, constraint, character, event, and debug primitives.
4. 10,000 mixed bodies, 1,000 active bodies, 10,000 rays, dense contact, stack, and constraint-chain measurements.
5. Repeated command-stream state hashes across process launches and worker counts.
6. Scene/world remove-recreate, in-flight query, shutdown, supported sanitizer, and large-world tests.

Record clean build time, installed/source/object size, warnings, network/file accesses, p50/p95/p99 tick time, allocations, peak temporary memory, worker utilization, barrier stalls, result hashes, and lifecycle outcomes. Accept Jolt if all hard gates pass and it is no worse than 10% behind the best finalist on the representative CPU workload.

## Scheduler Integration

### Decision

Insert `Physics` after `Scripting` and before `UI`, require the existing post-scripting mutation flush, and add an `After Physics` flush. Decompose work into explicit snapshot, worker-prepare, protected-commit, worker-simulate, worker-extract, protected-publish, and barrier workloads.

### Rationale

`Engine/Core/Scheduling/SystemScheduler.cpp` currently defines Startup, PreFrame, Input, Scene, Scripting, UI, Render, and PostFrame. `FrameWorkload` helpers already encode worker-safe preparation, protected commits, and barriers. Script-originated physics changes must be visible before snapshotting, and transform/event/query publication must be visible to same-frame consumers. Whole-system worker execution is not assumed; only declared immutable work runs on workers.

### Alternatives Considered

- **Physics after Scene but before Scripting**: rejected because same-frame script commands would wait a frame.
- **Physics at PostFrame**: rejected because rendering/editor state would use older transforms and diagnostics.
- **Frame-end-only flush**: rejected because it violates declared same-frame consumers.
- **Provider-owned thread pool**: rejected because it competes with the shared scheduler and violates the project threading discipline.

## Scene and Physics Atomicity

### Decision

Use a coordinated two-domain preflight and commit protocol with staged provider objects or complete backend inverse records. Publish generations and immutable results only after both scene and physics commits succeed.

### Rationale

`SceneMutationQueue` and `TransactionalMutation` provide scene read-set validation and rollback, but cannot undo an arbitrary provider mutation. FR-015 requires a rejected mixed batch to leave both domains unchanged. Staging plus revalidation prevents partial visibility; inverse records cover provider operations that cannot use swap-in staging.

### Alternatives Considered

- **Apply provider changes then enqueue scene writes**: rejected because a later scene conflict leaves partial physics state.
- **Commit scene then apply provider changes**: rejected because provider failure leaves authored/live state ahead of physics.
- **Rebuild the entire world after any failure**: rejected due cost, lost transient state, and poor failure containment.

## Paused Next Semantics

### Decision

The editor Next button advances exactly one simulation quantum equal to the project's configured physics fixed timestep. It runs one fixed-step callback pass, one ordinary simulation callback pass using the same explicit duration, one physics commit/simulate/publish cycle, and one resulting rendered frame, then remains paused. It does not use wall-clock editor delta and does not hard-code a fixed millisecond value.

### Rationale

This gives creators deterministic frame-by-frame inspection of forces, contacts, scripts, animation, queries, and published transforms. Following the configured timestep keeps the button correct when a project changes from 60 Hz to 50 Hz or another supported rate. Bypassing accumulator catch-up for the explicit request guarantees one request cannot produce zero or multiple physics ticks.

### Alternatives Considered

- **Advance one variable rendered frame**: rejected because it may contain zero, one, or multiple physics ticks and depends on editor performance.
- **Hard-code 16.667 ms or another duration**: rejected because it conflicts with projects using a different configured fixed timestep.
- **Advance only physics while scripts/animation remain frozen**: rejected because users could not inspect a coherent simulation frame.
- **Advance an arbitrary duration from the primary button**: deferred to an optional advanced stepping control; duration may require multiple ticks and is less predictable for single-step debugging.

## Authored Collider and Constraint Multiplicity

### Decision

Store collider and constraint child records in `ColliderSetComponent` and `ConstraintSetComponent`. Each child has a stable authored UUID and tagged shape/constraint payload. Keep one optional `RigidBodyComponent` and one optional `CharacterControllerComponent` per object.

### Rationale

TerraEngine's EnTT wrapper and reflected serializer support one component of each concrete type per object. Separate Box/Sphere/etc. components cannot represent two boxes of the same type. Stable record identities support undo, prefab overrides, cooking cache keys, diagnostics, and runtime generation mapping without hidden child objects.

### Alternatives Considered

- **One component type per shape**: rejected because repeated same-type shapes are impossible.
- **Hidden collider child objects**: rejected because hierarchy, prefab, duplication, and editor selection semantics become surprising.
- **One monolithic PhysicsComponent**: rejected because rigid body, collider, constraint, and character lifecycles and access declarations need separate ownership.

## Large-World Policy

### Decision

Compile and evaluate Jolt's double-position mode during the spike. Keep authored transforms in TerraEngine conventions, explicitly convert provider math, and choose float or double-position runtime mode from measured range and cost. Record the chosen project-wide mode; do not permit per-world ABI mixtures in the initial release.

### Rationale

Jolt documents double-position support with performance and broad-phase caveats. The feature requires large scenes but does not define a world radius. Measurement is safer than assuming precision needs. Provider math types remain private in either mode.

### Alternatives Considered

- **Always float**: may fail large-scene stability.
- **Always double**: imposes cost without measured need.
- **Runtime provider origin shifting only**: complicates authored coordinates, queries, events, and parity.

## Gravity Policy

### Decision

Each rigid body stores one `useGravity` boolean. Enabled bodies use the project-wide gravity vector; disabled bodies receive no built-in gravity. Custom per-object or spatial gravity is implemented by disabling built-in gravity and submitting a mass-independent acceleration command during each fixed tick. Per-body gravity vectors and scalar factors are not authored Rigidbody fields.

### Rationale

This matches the familiar Unity-style workflow and keeps ordinary Rigidbody authoring small. Dynamic custom gravity is fundamentally gameplay behavior: applying acceleration each fixed tick supports changing directions, gravity volumes, planets, and radial fields without continually rewriting component authoring data or coupling the runtime to a static override.

### Alternatives Considered

- **Scalar gravity factor**: rejected because it adds authoring surface while still being unable to change direction independently.
- **Per-body override boolean and vector**: rejected because it handles only constant overrides and duplicates behavior already expressible through mass-independent acceleration.
- **Dedicated gravity-volume feature in the initial release**: deferred with force fields; gameplay can build it on the typed acceleration command.

## Breakable Constraints

### Decision

Implement provider-neutral break force/torque policy in TerraEngine. The adapter reports applied constraint impulse/lambda; protected publication evaluates the documented threshold, queues disablement, and emits exactly one ordered break event.

### Rationale

Jolt does not expose PhysX's direct break-threshold field, and the public contract must remain provider neutral. Engine-owned policy produces consistent event ordering and persistence semantics.

### Alternatives Considered

- **Expose provider-specific breaking**: leaks provider behavior.
- **Exclude breaking for Jolt**: violates the specification.

## Profiler Integration

### Decision

Initially publish named Physics scopes under the existing CPU engine/task modules plus physics counters and diagnostics snapshots. Add a dedicated profiler module only if the profiler serialization/UI version is deliberately extended with regression coverage.

### Rationale

The current profiler module set and editor UI are hard-coded. Named scopes deliver useful evidence with less cross-cutting risk, while the dedicated Physics Diagnostics window covers detailed counters.

### Alternatives Considered

- **Immediate dedicated Physics profiler module**: possible, but expands capture versioning and UI scope before runtime foundations are stable.
- **Diagnostics window only**: rejected because physics scopes must remain visible without that window open.

## C# and Automation Surfaces

### Decision

Use dedicated typed C ABI/managed DTOs for physics commands, queries, events, handles, enums, GUID assets, and collections. Use generic component automation only for safe reflected authoring. Add a self-registering `PhysicsAutomationProvider` for semantic runtime commands, queries, preflight, approvals, idempotency, durable jobs, and correlation.

### Rationale

The generic managed property bridge handles only a limited set of property kinds, and generic component automation handlers do not receive the complete request context required for generation and approval safety. Dedicated contracts avoid arbitrary central dispatch while preserving existing registry and service infrastructure.

### Alternatives Considered

- **Put everything through generic reflection**: rejected due missing types and request context.
- **Add a central MCP string dispatch chain**: rejected by the feature design and existing provider architecture.

## Export and Cooking

### Decision

Create an immutable, versioned headless collider compiler and stage-then-rename publication path. Export settings, stable layer table, physics materials, and nonempty cooked collider payloads with backend/cook format evidence. Runtime loads only cooked data; source model geometry is not a runtime dependency.

### Rationale

Existing AssetDatabase GUID metadata, atomic compiled publication, ExportPipeline staging, package validation, evidence, and TEPAK runtime mount provide appropriate seams. Extending these paths keeps editor and CI behavior aligned.

### Alternatives Considered

- **Runtime cooking**: rejected due startup cost, failure variability, and source dependencies.
- **Serialize provider runtime objects**: rejected due ABI/version instability and backend leakage.

## Platform and Collision-Layer Defaults

### Decision

Windows x64 is the first-release acceptance platform. Linux x64 must configure and compile during the provider spike and retains a near-term parity path. Use 32 stable collision-layer IDs initially, with a 32-bit runtime mask and names independent of IDs/index order.

### Rationale

This resolves specification assumptions without expanding first-release live validation beyond available ownership. Stable IDs preserve references across rename and reorder.

### Alternatives Considered

- **Require full Linux editor/export acceptance immediately**: deferred unless CI ownership is confirmed during tasks.
- **Name- or index-only layers**: rejected because rename/reorder would break references.
- **Unbounded layers**: rejected because masks, filtering cost, serialization, and provider mapping need a defined first contract.

## Remaining Risks

- Jolt worker callbacks and query results are unordered; all observable packets require engine-owned canonical sort.
- Custom job barriers can deadlock if wait semantics execute unrelated physics-touching jobs; adapter design and shutdown stress are mandatory.
- Double-position mode does not make every broad-phase or velocity calculation double precision.
- Prefab property apply/remapping is currently shallow and must be expanded for stable child records and connected-object UUIDs.
- Inspector reflection currently edits live component storage; physics rebuilds must be deferred and transaction-aware.
- Asset importer dispatch and engine registration are explicit/central seams; changes must remain append-only and tested.
- Export asset type numeric values require append-only compatibility and schema/version evidence.
- Constitution v1.3.1 and the contributor guide both require the repository's actual `SystemsBase` lifecycle type.
