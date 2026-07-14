# Implementation Plan: Complete Physics System

**Branch**: `006-physics-system` | **Date**: 2026-07-13 | **Spec**: [spec.md](spec.md)  
**Input**: Feature specification from `specs/006-physics-system/spec.md`

## Summary

Add a backend-neutral, multithreaded physics runtime shared by the editor and exported games. The runtime is organized around immutable authoring snapshots, worker-safe preparation and simulation, a typed deterministic physics command queue, coordinated scene/physics transactions, and immutable published results. Jolt Physics v5.6.0 is the conditional provider choice because its job-system interface, deterministic same-build behavior, feature breadth, platform coverage, focused MIT-licensed source tree, performance improvements, and relevant gravity and `LinearCast` fixes best match TerraEngine. Permanent vendoring is gated on an offline selection spike; PhysX 5.8.0 remains a research fallback candidate that requires renewed planning and explicit approval before implementation.

The implementation proceeds through independently compiling and installable slices: provider spike/foundation, authored data and serialization, fixed-step scheduler integration, events and queries, cooked assets, constraints and characters, editor tooling, C#/automation, then export parity and hardening. Advanced ragdoll, vehicle, and soft-body modules remain separate follow-up features.

## Technical Context

**Language/Version**: C++20 native engine/editor; C# managed gameplay surface through the existing NativeAOT-compatible bridge  
**Primary Dependencies**: TerraEngine scheduler/job graph, ThreadPool, EnTT component/reflection layer, SceneMutationQueue and TransactionalMutation, asset database/import pipeline, editor automation registry, TEPAK export/runtime mount; conditional Jolt Physics v5.6.0 (`e77f175595e64cb44218cc9d9d56fc365ad0e36a`) under `External/JoltPhysics`  
**Storage**: Reflected scene/prefab JSON; `ProjectSettings/PhysicsSettings.json`; GUID-addressed physics material and cooked-collider assets; immutable runtime snapshots and bounded in-memory histories; TEPAK export manifests/evidence  
**Testing**: Project-owned CTest harnesses (`TerraEngine-EngineTests`, `TerraEngine-EditorTests`), focused executable filters, scheduler/transaction regressions, headless export baseline, installed-editor loopback validation, provider spike benchmarks, stress/soak and supported sanitizer lanes  
**Target Platform**: Windows x64 first-release acceptance; Linux x64 build compatibility and provider spike coverage; architecture retains a path to other existing TerraEngine targets  
**Project Type**: Native game engine, desktop editor, managed scripting SDK, headless asset compiler/export tools, and exported runtime  
**Performance Goals**: At least 10,000 mixed bodies; 1,000 active dynamic bodies at 60 fixed updates/second on recorded reference hardware; 10,000 batched rays with bounded memory and stable engine-owned ordering; no per-body hot-path heap allocation  
**Constraints**: Same-build/platform repeatable command/event/query order; protected mutation visibility; atomic rejection across scene and physics domains; no provider types in public or serialized contracts; no system/global runtime dependencies; no backend-private worker pool; safe teardown under active work; installed tree required after every code slice  
**Scale/Scope**: One primary gameplay world initially; 32 stable collision layers; rigid bodies, eight collider families, materials, constraints, character foundation, queries/events, editor, C#, automation, cooking, export parity, diagnostics, and lifecycle hardening

## Constitution Check

*GATE: Passed before research and re-checked after Phase 1 design.*

- **Task-based concurrency**: PASS. Physics declares snapshot, worker preparation, protected command commit, worker simulation/query, protected extraction/publication, and barrier workloads. Jolt jobs are lowered through TerraEngine's shared `ThreadPool`; no provider-private recurring pool is permitted.
- **External integrity and approval**: PASS. The plan conditionally adds pristine Jolt v5.6.0 at `External/JoltPhysics`, pinned to commit `e77f175595e64cb44218cc9d9d56fc365ad0e36a`. The user explicitly approved this exact dependency, revision, location, and integration scope on 2026-07-13, so implementation may execute the submodule addition after the selection spike. Any substitution, upgrade, patch, or different dependency requires stopping and returning to planning for approval; no provider source is modified.
- **Shader separation and installed assets**: PASS. Physics debug drawing reuses renderer-owned pipelines and immutable packets; any later physics shader must be a dedicated file below `Engine/Shaders/` or `Editor/Shaders/`. All runtime assets and tools are validated from the install tree.
- **Compile/install completion**: PASS. Each implementation slice ends with the smallest meaningful compile/test, and integrated acceptance requires complete build, applicable CTest/regression suites, install target, installed-editor loopback, and exported-runtime parity.
- **CRTP components**: PASS. Authored physics components use `Component<Derived>`, stable names, and reflection. Runtime handles/provider state are excluded.
- **System lifecycle naming**: PASS. Constitution v1.3.1 and the contributor guide require the actual `SystemsBase` type in `Engine/Core/SystemBase.h`; `PhysicsSystem` inherits it.
- **Transactional mutation**: PASS. Worker paths never mutate live `ObjectManager`; coordinated two-domain preflight/commit/rollback is explicitly designed for scene-plus-physics operations.

### Post-Design Re-check

Phase 1 contracts preserve the same gates. The data model uses engine-owned identities and immutable publication packets; contracts keep provider layouts private; cooking and automation publication are atomic; scheduler boundaries are explicit; and export uses the same runtime. No unjustified constitution exception remains.

## Backend Selection and Dependency Plan

### Conditional Selection

- **Selected**: Jolt Physics v5.6.0, commit `e77f175595e64cb44218cc9d9d56fc365ad0e36a`, MIT license.
- **Planned location**: `External/JoltPhysics`, preferably as a pinned git submodule consistent with existing dependencies.
- **Normal build options**: core library only; disable samples, viewer, performance tool, unit tests, object stream, and unrelated tools; disable provider-owned thread pool usage; preserve exceptions/RTTI settings compatible with TerraEngine.
- **Fallback candidate**: PhysX 5.8.0, tag `110.0-omni-and-physx-5.8.0`, commit `fc1018a3745664a1db2b95ce03fb5e91eb585f2e`. It is not pre-approved for implementation. If Jolt fails a hard gate, implementation stops and returns to planning/task generation for a new explicit approval and regenerated task graph.
- **Fresh-release risk**: Jolt 5.6.0 was released two days before this plan, so the spike must explicitly validate its CPU-only configuration, new friction behavior, gravity-force and `LinearCast` fixes, determinism, memory, and performance. GPU compute and hair simulation remain disabled and outside initial scope.

### Selection Spike Gates

1. Prove network-disabled configure/build/install on Windows and Linux without global packages.
2. Integrate temporary allocator, logging/assertion, profiling, and TerraEngine job adapters without a private pool.
3. Run the representative shape, CCD, filter, material, query, constraint, character, event-order, and lifecycle fixtures.
4. Measure 10,000 mixed bodies, 1,000 active bodies, 10,000 rays, dense contacts, and constraint chains with p50/p95/p99 tick cost, allocations, peak memory, binary size, and scheduler contention.
5. Replay identical command streams across process launches and worker counts, hashing backend state and sorted TerraEngine packets.
6. Measure float and double-position large-world behavior and teardown races.

Jolt is accepted if all offline, correctness, lifecycle, and ordering gates pass and representative CPU performance is no worse than 10% behind the best finalist. The spike records reference hardware, build settings, exact revisions, raw results, and limitations in `research.md` or a linked evidence file before permanent vendoring.

The selection spike also calibrates and records initial query bounds for maximum cast distance, shape extent, hits per query, requests per batch, and retained-result ticks. Settings tests pin the accepted values before runtime APIs are implemented; no API ships with unbounded query input or history.

## Scheduler and Mutation Architecture

### Frame placement

Add a `Physics` scheduler phase after `Scripting` and before `UI`. Physics requires the post-scripting scene mutation flush so script-created or edited components are visible. Add an `After Physics` flush because same-frame rendering, editor inspection, managed callbacks, and automation reads need the newly published transforms and immutable packets.

### Paused Next semantics

The editor Next command advances one simulation quantum equal to the configured physics fixed timestep. It does not use wall-clock editor delta and does not hard-code a millisecond duration. One request runs exactly one physics tick, invokes fixed-step and ordinary simulation callbacks once with that explicit duration, commits and publishes once, renders one resulting frame, clears the step request, and leaves Play Mode paused. The step request bypasses normal accumulator catch-up so residual accumulated time cannot produce zero or multiple ticks. A future advanced control may request N ticks or a duration, but the primary Next button remains one fixed tick.

### Declared workloads

1. **PhysicsSnapshot** — protected snapshot of hierarchy-resolved transforms, physics components, settings, dirty generations, and incoming command sequence ranges.
2. **PhysicsPrepare** — worker-safe validation, recipe hashing, dirty-body/constraint/character preparation, and query-batch preparation.
3. **PhysicsCommandCommit** — protected two-domain preflight and deterministic backend command commit.
4. **PhysicsSimulate** — worker-safe fixed-step execution using the shared job adapter; zero work when no world is active or no tick is due.
5. **PhysicsExtract** — worker-safe collection into thread-local body, event, query, debug, and diagnostic packets.
6. **PhysicsPublish** — protected canonical sort, bounded-history publication, and construction of typed transform scene transactions.
7. **PhysicsPublicationBarrier** — protected scene transaction flush and immutable snapshot generation swap.

### Coordinated atomic mutation

Scene STM alone cannot atomically cover provider-owned state. Introduce a coordinated protocol:

1. Capture scene read-set versions, world generation, target generations, and command sequence range.
2. Preflight every scene write and physics command without mutating either live domain.
3. Prepare backend objects or inverse records off to the side; reserve required handles and capacity.
4. Under the protected commit boundary, revalidate both read sets.
5. Commit physics changes into a staging generation or apply with complete inverse records.
6. Commit scene transaction; if it fails, apply backend inverse records before releasing the boundary.
7. Publish new generations and immutable results only after both commits succeed.

Irreversible provider operations are deferred until after acceptance or represented by swap-in staged objects. Cancellation before publication discards staging. Diagnostics retain the correlation ID and failing preflight reason.

## Authored Component Decision

EnTT permits one component of each concrete type per object, but the feature requires multiple colliders and potentially repeated same-type constraints. Use collection components with stable child record IDs:

- `RigidBodyComponent` — one optional body definition.
- `ColliderSetComponent` — ordered collection of tagged collider records, allowing repeated boxes or other same-type shapes.
- `ConstraintSetComponent` — ordered collection of tagged constraint records, allowing repeated same-type constraints and UUID targets.
- `CharacterControllerComponent` — one optional character definition.

Each child record has a stable authored UUID used by prefab overrides, undo, cooking cache keys, diagnostics, and runtime handle generations. Inspector UX presents records as familiar Box/Sphere/etc. collider and constraint entries even though storage is collection-based. This avoids hidden child objects and preserves generic scene serialization.

### Gravity authoring

`RigidBodyComponent` follows the simple global-plus-toggle model:

- `useGravity`: when true, the body uses project world gravity; when false, it receives no built-in gravity.
- `PhysicsSettings.gravity`: the project-wide acceleration vector in metres per second squared.

There is no authored per-body gravity vector or scalar factor. Gameplay that needs custom, changing, radial, or spatial gravity disables `useGravity` and submits a mass-independent acceleration command on each fixed tick. The typed force API distinguishes continuous force, mass-independent acceleration, impulse, and instantaneous velocity change so custom gravity does not depend on body mass.

### Collision modes

The initial public motion-quality contract contains `Discrete` and `LinearCast`. `LinearCast` is validated with representative fast-body versus thin-static-geometry tunnelling fixtures. Additional provider modes remain future engine-owned enum additions and cannot leak provider enum values.

### Install completion

TerraEngine's CMake install rules intentionally publish to the absolute build-tree destination `out/build/<Preset>/Build` (for example `out/build/Debug/Build`), even though CMake presets also define a conventional prefix under `out/install/`. The build-tree `Build` directory is the canonical installed runtime tree for this feature. Every code-changing task remains incomplete until its applicable compile and install gate succeeds; when a phase shares one gate, preceding code-task checkboxes remain open and are completed together only after that gate passes.

## Project Structure

### Documentation (this feature)

```text
specs/006-physics-system/
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
│   ├── runtime-api.md
│   ├── serialization-assets.md
│   ├── managed-api.md
│   ├── automation-api.md
│   ├── editor-authoring.md
│   └── export-parity.md
└── tasks.md
```

### Source Code (repository root)

```text
External/
└── JoltPhysics/                         # Conditional, approved, pinned provider

cmake/
├── TerraEngineDependencies.cmake        # TerraEngine-owned dependency options
└── TerraEngineInstall.cmake             # Physics install rules

Engine/
├── Components/Physics/
│   ├── RigidBodyComponent.*
│   ├── ColliderSetComponent.*
│   ├── ConstraintSetComponent.*
│   └── CharacterControllerComponent.*
├── Physics/
│   ├── PhysicsSystem.*
│   ├── PhysicsWorld.*
│   ├── PhysicsTypes.h
│   ├── PhysicsSettings.*
│   ├── PhysicsAuthoringSnapshot.*
│   ├── PhysicsCommands.*
│   ├── PhysicsTransactions.*
│   ├── PhysicsQueries.*
│   ├── PhysicsEvents.*
│   ├── PhysicsDiagnostics.*
│   ├── Assets/
│   │   ├── PhysicsMaterial.*
│   │   ├── ColliderRecipe.*
│   │   └── CookedColliderAsset.*
│   └── Backend/
│       ├── IPhysicsBackend.h
│       ├── JoltPhysicsBackend.*
│       ├── PhysicsJobAdapter.*
│       └── PhysicsAllocatorAdapter.*
├── Managed/Source/Physics/
└── Scripting/                           # Typed C ABI and export bridge extensions

Editor/
├── AssetCompilers/
│   └── PhysicsColliderCompiler.*
├── TerraEnginePhysicsColliderCompiler.cpp
└── TerraEngine-Editor/
    ├── Automation/Providers/PhysicsAutomationProvider.*
    ├── Windows/PhysicsDiagnosticsWindow.*
    ├── Windows/SceneViewWindow.*         # Immutable physics debug packets
    ├── Windows/InspectorWindow.*         # Collection authoring UI
    └── Project/                          # Physics project settings UI

Tests/
├── Engine/Physics/
├── Editor/Physics/
├── Engine/Scene/                         # Cross-domain transaction/visibility cases
├── Editor/Automation/                    # Physics capability contract cases
├── Editor/Export/                        # Physics package/evidence/parity cases
└── Fixtures/Physics/

Docs/
├── Physics.md
└── Testing.md
```

**Structure Decision**: Physics owns a backend-neutral engine module and provider adapter. Authoring components remain under the existing component registry, editor-only authoring and automation remain under the editor, and cooking is a headless editor tool. Cross-cutting integration files are extended only at their existing registration seams. Root and test CMake source lists remain explicit.

## Delivery Sequence

1. **Provider spike and foundation**: conditional vendor pin, CMake, provider-neutral contracts, allocator/job/log/profile adapters, stable handles, minimal world, spike evidence.
2. **Authoring and persistence**: collection components, settings, layers, materials, reflection, serialization, prefab/undo foundations.
3. **Fixed-step runtime**: scheduler phase/workloads/resources, snapshots, typed commands, coordinated transaction protocol, body registry, stepping, transform publication, and deterministic paused Next behavior using exactly one configured fixed tick.
4. **Events and queries**: thread-local collectors, sorting, bounded histories, native query APIs, batch jobs.
5. **Cooked geometry**: convex/mesh/compound recipes, headless compiler, GUID cache, atomic publication, export staging.
6. **Constraints and characters**: required joint records, break policy, character movement/support behavior.
7. **Editor and diagnostics**: inspectors, settings, debug drawing, diagnostics window, profiler scopes/counters, Play Mode lifecycle.
8. **Managed and automation**: typed C ABI, managed wrappers/events, dedicated provider, approvals/idempotency/jobs/logging.
9. **Export parity and hardening**: manifests/validation/evidence, installed runtime fixture, stress/soak, lifecycle/failure containment, full validation.

Every slice has a demonstrable checkpoint and ends with affected-target compilation, focused tests, and install verification. Full builds and complete suites remain orchestrator-owned during implementation.

## Execution-Mode Warning

This feature is unusually large, cross-cutting, and high-risk. It combines a new vendored dependency, job-system adaptation, deterministic concurrency, two-domain transactional atomicity, complex lifecycle teardown, versioned assets, editor authoring, managed ABI, automation safety, and export parity. It also has several independent file/subsystem boundaries after foundation contracts stabilize.

**Provisional recommendation for `/speckit.implement`**: Mixed GPT-5.6 delegation with a Sol orchestrator and bounded Terra/Luna workers, using higher Sol reasoning effort (`xhigh`). Parallel work should begin only after provider, data model, scheduler resources, and transaction contracts land. Keep provider foundation, cross-domain atomic commit, scheduler integration, and final integration in Sol; delegate non-overlapping component/assets, editor surface, managed/automation, documentation, and focused test slices. `/speckit.tasks` must reassess the completed task graph and record the final recommendation, costs, accuracy benefits, and coordination risks in `tasks.md`.

## Complexity Tracking

| Design pressure | Why needed | Simpler alternative rejected because |
|-----------------|------------|--------------------------------------|
| Backend abstraction plus one implementation | Prevents provider identifiers and layouts from leaking into public/serialized contracts | Direct provider use would make scripting, assets, exports, and replacement dependent on third-party ABI |
| Collection components for colliders/constraints | Supports repeated same-type authored records within EnTT's one-component-per-type model | Separate shape components cannot represent two boxes or two hinges on one object |
| Coordinated two-domain transaction protocol | FR-015 requires atomic rejection across scene and provider-owned world state | SceneMutationQueue alone cannot roll back an already-mutated provider world |
| After-Physics publication flush | Rendering, scripts, editor diagnostics, and automation need declared same-frame results | Frame-end-only visibility would add a frame of latency and break explicit same-frame acceptance cases |
| Dedicated automation provider | Runtime commands need context, generations, approval fingerprints, idempotency, and durable jobs | Generic reflected component handlers do not receive sufficient request context |
| Headless collider compiler | Exported games must use immutable cooked data without source models | Runtime cooking would add startup cost, nondeterministic failure points, and source-asset dependencies |
