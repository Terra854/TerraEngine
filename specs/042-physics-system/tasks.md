# Tasks: Complete Physics System

**Input**: Design documents from `specs/042-physics-system/`  
**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `contracts/`, `quickstart.md`  
**Tests**: Required by the feature specification. Within each story, create the listed tests first and confirm that they fail for the missing behavior before implementation.

**Organization**: Tasks are grouped by user story. Every code task must follow the repository's C++20, CRTP, `SystemsBase`, task-scheduling, transactional-mutation, single-tab indentation, beginner-friendly comment, self-contained dependency, shader-separation, and install-tree rules.

**Completion rule**: Spec 042 uses `RelWithDebInfo` exclusively. A code-changing task remains unchecked until its applicable phase compile and `cmake --build --preset RelWithDebInfo --target install` gate succeeds. Implementation work may be prepared earlier, but the phase validation task completes the preceding code-task group only after the canonical `out/build/RelWithDebInfo/Build` tree is verified. Debug, Release, and MinSizeRel builds do not satisfy this feature's completion gates.

**Dependency approval**: T004 may add only Jolt Physics v5.6.0 commit `e77f175595e64cb44218cc9d9d56fc365ad0e36a` at `External/JoltPhysics` with the documented core-only integration scope explicitly approved on 2026-07-13. Jolt 5.5 remains comparison evidence only. Any failure requiring a different revision, patch, upgrade, or provider stops implementation and returns to planning for approval.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Safe to perform concurrently because it targets distinct files and does not depend on another incomplete task in the same group.
- **[Story]**: Maps the task to a user story from `spec.md`.
- Every task names the exact file or directory it changes or the evidence file it produces.

---

## Phase 1: Setup and Provider Selection

**Purpose**: Produce provider evidence, finalize the approved dependency pin, and establish hermetic build integration before engine architecture depends on it.

- [X] T001 Create the disposable Jolt 5.5/Jolt 5.6/PhysX 5.8 configure-build-install and measurement harness in `Tools/Physics/BackendSpike/CMakeLists.txt`, `Tools/Physics/BackendSpike/RunBackendSpike.ps1`, and `Tools/Physics/BackendSpike/RunBackendSpike.sh`
- [X] T002 [P] Create the provider comparison evidence schema, required metrics, hard gates, and approved-revision record in `specs/042-physics-system/evidence/backend-selection.md`
- [X] T003 Run the backend spike offline after source bootstrap, capture Windows/Linux build, feature, determinism, lifecycle, large-world, size, memory, performance, and calibrated query-bound evidence in `specs/042-physics-system/evidence/backend-selection.md`; if approved Jolt v5.6.0 fails any hard gate, stop implementation and return to planning rather than selecting the PhysX candidate
- [X] T004 If and only if T003 passes the hard gates, execute the explicitly approved addition of pristine Jolt Physics v5.6.0 commit `e77f175595e64cb44218cc9d9d56fc365ad0e36a` as `External/JoltPhysics` and update `.gitmodules` without modifying provider source, revision, or scope
- [X] T005 Configure the root build for a core-only Jolt target with samples, viewer, provider tests, object stream, and private thread-pool usage disabled in `CMakeLists.txt` and `cmake/TerraEngineDependencies.cmake`
- [X] T006 [P] Record the Jolt MIT license, exact revision, enabled options, redistribution obligations, and upgrade policy in `Docs/Dependencies/JoltPhysics.md`
- [X] T007 Add provider-revision and offline-dependency validation checks to `Tests/Engine/Physics/PhysicsDependencyContractTests.cpp` and register them in `Tests/Engine/CMakeLists.txt`
- [X] T008 Configure, compile, test the dependency contract, run the install target, and record commands and results in `specs/042-physics-system/evidence/setup-validation.md`

**Checkpoint**: The exact provider is evidenced, approved, pinned, hermetic, and available through the project build without exposing it to gameplay-facing code.

---

## Phase 2: Foundational Runtime Contracts

**Purpose**: Establish backend-neutral identities, settings, queues, transaction semantics, adapters, world lifecycle, and diagnostics that block every user story.

**CRITICAL**: No user-story implementation begins until this phase passes its focused build, tests, and install gate.

### Foundational tests first

- [X] T009 [P] Add failing tests for world/body/constraint/character handle allocation, generation reuse, and stale rejection in `Tests/Engine/Physics/PhysicsHandleTests.cpp`
- [X] T010 [P] Add failing tests for settings defaults, 32 stable layer IDs, layer-mask validation, units, world gravity, and the T003-calibrated maximum cast distance, shape extent, hits, 10,000-request batch, and retained-result bounds in `Tests/Engine/Physics/PhysicsSettingsTests.cpp`
- [X] T011 [P] Add failing tests proving public headers contain no Jolt types and provider jobs use the TerraEngine pool in `Tests/Engine/Physics/PhysicsBackendBoundaryTests.cpp` and `Tests/Engine/Physics/PhysicsJobAdapterTests.cpp`
- [X] T012 [P] Add failing tests for deterministic command merge keys, bounded queues, cancellation, and stale world generations in `Tests/Engine/Physics/PhysicsCommandQueueTests.cpp`
- [X] T013 [P] Add failing tests for coordinated scene/physics preflight, staged commit, inverse rollback, and zero partial visibility in `Tests/Engine/Physics/PhysicsTransactionTests.cpp`

### Foundational implementation

- [X] T014 Implement backend-neutral world IDs, generation-validated handles, engine-owned enums, force modes, result statuses, and fixed-width public value types in `Engine/Physics/PhysicsTypes.h` and `Engine/Physics/PhysicsTypes.cpp`
- [X] T015 Implement versioned PhysicsSettings, world gravity, fixed-step limits, 32 stable collision layers, the T003-calibrated query/history bounds, defaults, and validation in `Engine/Physics/PhysicsSettings.h` and `Engine/Physics/PhysicsSettings.cpp`
- [X] T016 Define the coarse provider-neutral lifecycle, shape, body, query, constraint, character, extraction, staging, and inverse contract in `Engine/Physics/Backend/IPhysicsBackend.h`
- [X] T017 Implement the Jolt/TerraEngine job bridge with barrier-safe waits and no provider-private recurring worker pool in `Engine/Physics/Backend/PhysicsJobAdapter.h` and `Engine/Physics/Backend/PhysicsJobAdapter.cpp`
- [X] T018 [P] Implement Jolt allocation, assertion, logging, error translation, and external profiling adapters in `Engine/Physics/Backend/PhysicsAllocatorAdapter.h` and `Engine/Physics/Backend/PhysicsAllocatorAdapter.cpp`
- [X] T019 Implement Jolt initialization, shutdown, world creation/destruction, explicit math conversion, and fatal-failure containment in `Engine/Physics/Backend/JoltPhysicsBackend.h` and `Engine/Physics/Backend/JoltPhysicsBackend.cpp`
- [X] T020 Implement PhysicsWorld lifecycle state, monotonic generation, registries, fixed-step bookkeeping, queue ownership, and ordered teardown in `Engine/Physics/PhysicsWorld.h` and `Engine/Physics/PhysicsWorld.cpp`
- [X] T021 Implement typed command envelopes, producer/global sequencing, bounded multi-producer buffers, cancellation, and idempotency metadata in `Engine/Physics/PhysicsCommands.h` and `Engine/Physics/PhysicsCommands.cpp`
- [X] T022 Implement coordinated scene/physics read sets, dual preflight, staged provider objects, inverse records, protected commit, and rollback in `Engine/Physics/PhysicsTransactions.h` and `Engine/Physics/PhysicsTransactions.cpp`
- [X] T023 [P] Implement immutable authoring, body-state, command-result, debug, and diagnostic snapshot types in `Engine/Physics/PhysicsAuthoringSnapshot.h`, `Engine/Physics/PhysicsAuthoringSnapshot.cpp`, `Engine/Physics/PhysicsDiagnostics.h`, and `Engine/Physics/PhysicsDiagnostics.cpp`
- [X] T024 Add all foundational sources, headers, tests, Jolt link visibility, and install rules to `CMakeLists.txt`, `Tests/Engine/CMakeLists.txt`, and `cmake/TerraEngineInstall.cmake`
- [X] T025 Run the foundational focused tests, compile affected targets, run install, and record exact results in `specs/042-physics-system/evidence/foundation-validation.md`

**Checkpoint**: Backend-neutral contracts compile; handles, job integration, queues, transactions, world lifecycle, and provider isolation pass before story work begins.

---

## Phase 3: User Story 1 — Author and Simulate Rigid Bodies (Priority: P1) MVP

**Goal**: Author primitive static/dynamic/kinematic bodies and run a safe fixed-step world with gravity, forces, collision filtering, CCD, sleep, transforms, Play Mode restoration, and deterministic paused Next.

**Independent Test**: Create a scene with primitive static, dynamic, kinematic, and trigger bodies; verify world gravity and `useGravity`, custom fixed-tick acceleration, collisions/materials/layers, CCD, sleep/wake, exact Next semantics, After-Physics transform visibility, and exact authored restoration on Play Mode exit.

### Tests for User Story 1

- [X] T026 [P] [US1] Add failing CRTP, reflection, defaults, repeated-collider-record, and component registration tests in `Tests/Engine/Physics/PhysicsComponentTests.cpp`
- [X] T027 [P] [US1] Add failing scene save/reload and runtime-field exclusion tests for rigid bodies and primitive collider sets in `Tests/Engine/Physics/PhysicsSerializationTests.cpp`, plus failing PhysicsMaterial schema, defaults, JSON load/save, migration, and runtime-field exclusion tests in `Tests/Engine/Physics/PhysicsMaterialAssetTests.cpp`
- [X] T028 [P] [US1] Add failing fixed-step accumulator, clamp, pause, and 100-consecutive-token Next tests proving one configured tick, one fixed/ordinary callback pass, one publication, one render request, and remain-paused behavior per token in `Tests/Engine/Physics/PhysicsFixedStepTests.cpp`
- [X] T029 [P] [US1] Add failing scheduler phase/resource/workload, post-scripting input, After-Physics flush, and transform visibility tests in `Tests/Engine/Physics/PhysicsSchedulerTests.cpp`
- [X] T030 [P] [US1] Add failing rollback tests for invalid collider, stale object/body, cancellation, and transform version conflict in `Tests/Engine/Physics/PhysicsRigidBodyTransactionTests.cpp`
- [X] T031 [P] [US1] Add failing simulation fixtures for gravity, `useGravity`, mass-independent custom acceleration, materials, layers, triggers, `Discrete` and `LinearCast` motion quality including fast-body/thin-geometry tunnelling, sleep/wake, and static/dynamic/kinematic interaction in `Tests/Engine/Physics/PhysicsRigidBodySimulationTests.cpp`
- [X] T032 [P] [US1] Add failing editor Play Mode enter/pause/Next/exit and exact authored restoration tests in `Tests/Editor/Physics/PhysicsPlayModeTests.cpp`

### Implementation for User Story 1

- [X] T033 [P] [US1] Implement CRTP RigidBodyComponent fields, `useGravity`, validation, stable name, reflection, and automation declaration in `Engine/Components/Physics/RigidBodyComponent.h` and `Engine/Components/Physics/RigidBodyComponent.cpp`
- [X] T034 [P] [US1] Implement stable tagged Box, Sphere, Capsule, Cylinder, and Tapered Capsule records in `Engine/Components/Physics/ColliderSetComponent.h` and `Engine/Components/Physics/ColliderSetComponent.cpp`
- [X] T035 [P] [US1] Implement the versioned `.tephysmat` PhysicsMaterial schema, runtime fields, combine modes, GUID identity, defaults, JSON load/save, migration entry points, and bounded diagnostics in `Engine/Physics/Assets/PhysicsMaterial.h` and `Engine/Physics/Assets/PhysicsMaterial.cpp`
- [X] T036 [US1] Register physics components, enums, reflected properties, serializers, and component-access metadata in `Engine/Core/Object/ComponentRegistry.cpp` and `Engine/Scene/Serializer.cpp`
- [X] T037 [US1] Implement primitive Jolt shape construction, scale validation, shared immutable recipe caching, material mapping, sensors, layers, masks, and CCD mapping in `Engine/Physics/Backend/JoltPhysicsBackend.cpp` and `Engine/Physics/PhysicsWorld.cpp`
- [X] T038 [US1] Implement immutable hierarchy-resolved PhysicsAuthoringSnapshot capture and dirty/generation tracking in `Engine/Physics/PhysicsAuthoringSnapshot.cpp`
- [X] T039 [US1] Implement PhysicsSystem lifecycle plus PhysicsSnapshot, PhysicsPrepare, PhysicsCommandCommit, PhysicsSimulate, PhysicsExtract, PhysicsPublish, and PhysicsPublicationBarrier workloads in `Engine/Physics/PhysicsSystem.h` and `Engine/Physics/PhysicsSystem.cpp`
- [X] T040 [US1] Add Physics phase, scheduler resources, post-scripting dependency, After-Physics mutation flush, and declaration diagnostics in `Engine/Core/Scheduling/SystemScheduler.h` and `Engine/Core/Scheduling/SystemScheduler.cpp`
- [X] T041 [US1] Implement body create/rebuild/destroy, static-collider fallback, invalid-body diagnostics, force/torque/acceleration/impulse modes, `useGravity`, velocities, kinematic targets, wake/sleep, and deterministic commit ordering in `Engine/Physics/PhysicsWorld.cpp` and `Engine/Physics/Backend/JoltPhysicsBackend.cpp`
- [X] T042 [US1] Convert dynamic body results into typed Transform scene transactions with expected versions, interpolation data, sleeping suppression, and After-Physics visibility in `Engine/Physics/PhysicsSystem.cpp` and `Engine/Scene/SceneMutationQueue.cpp`
- [X] T043 [US1] Register the shared PhysicsSystem after Scripting and before UI for editor and exported runtime lifecycle in `Engine/Core/MainEngine.cpp`
- [X] T044 [US1] Replace scripting-only paused stepping with an explicit engine step token that advances one configured physics tick, fixed and ordinary callbacks, one publication, one render, and remains paused in `Engine/Scripting/ScriptingSystem.h`, `Engine/Scripting/ScriptingSystem.cpp`, and `Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.cpp`
- [X] T045 [US1] Build and destroy a fresh physics world around editor Play Mode capture/restore, scene reload, and project/application shutdown boundaries in `Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.cpp` and `Engine/Physics/PhysicsSystem.cpp`
- [X] T046 [P] [US1] Document rigid-body authoring, units, gravity/custom acceleration, fixed stepping, hierarchy/scale limits, and native usage in `Docs/Physics.md`
- [X] T047 [US1] Run all US1 tests first as failing then passing, compile affected engine/editor targets, run install, and record results in `specs/042-physics-system/evidence/us1-validation.md`
- [X] T048 [US1] Demonstrate the standalone MVP fixture from the installed editor and record gravity, collision, Next, visibility, and restoration evidence in `specs/042-physics-system/evidence/us1-mvp-demo.md`

**Checkpoint**: The installed editor can independently author and simulate primitive rigid bodies with deterministic fixed-step behavior and safe restoration.

---

## Phase 4: User Story 2 — Build Rich Collision Geometry and Assets (Priority: P2)

**Goal**: Author, cook, cache, move, reload, and simulate convex, mesh, and compound collider assets without runtime source geometry.

**Independent Test**: Cook valid convex/mesh/compound fixtures, reject invalid geometry atomically, retain last-known-good output after failed recook, preserve GUID references through move/rename/reload, and reuse identical immutable shapes.

### Tests for User Story 2

- [X] T049 [P] [US2] Add failing recipe normalization, stable hashing, scale, degenerate geometry, recursion, and limit tests in `Tests/Engine/Physics/ColliderRecipeTests.cpp`
- [X] T050 [P] [US2] Add failing cooked-envelope version, checksum, provider revision, provenance, nonempty payload, and last-known-good tests in `Tests/Engine/Physics/CookedColliderAssetTests.cpp`
- [X] T051 [P] [US2] Add failing headless cook, cancellation, atomic publication, move/rename GUID, reimport, and cache reuse tests in `Tests/Editor/Physics/PhysicsColliderCompilerTests.cpp`, plus PhysicsMaterial editor creation and move/rename GUID round-trip integration tests in `Tests/Editor/Physics/PhysicsMaterialAssetTests.cpp`
- [X] T052 [P] [US2] Add failing convex, triangle-mesh, compound, unsupported-scale, and large-mesh simulation tests in `Tests/Engine/Physics/PhysicsCookedShapeSimulationTests.cpp`

### Implementation for User Story 2

- [X] T053 [P] [US2] Implement normalized convex, mesh, and compound ColliderRecipe records, provenance, validation, and stable hashing in `Engine/Physics/Assets/ColliderRecipe.h` and `Engine/Physics/Assets/ColliderRecipe.cpp`
- [X] T054 [P] [US2] Implement the versioned immutable CookedColliderAsset envelope, checksums, provider/cook compatibility, bounds, status, and diagnostics in `Engine/Physics/Assets/CookedColliderAsset.h` and `Engine/Physics/Assets/CookedColliderAsset.cpp`
- [X] T055 [US2] Implement Jolt convex/mesh/compound cooking and loading behind provider-neutral recipes without exposing provider serialization in `Engine/Physics/Backend/JoltPhysicsBackend.cpp`
- [X] T056 [US2] Implement worker-safe validation/cooking, cooperative cancellation, stage validation, and protected stage-then-rename publication in `Editor/AssetCompilers/PhysicsColliderCompiler.h` and `Editor/AssetCompilers/PhysicsColliderCompiler.cpp`
- [X] T057 [US2] Add the self-contained headless physics collider compiler entry point, CLI diagnostics, and install target in `Editor/TerraEnginePhysicsColliderCompiler.cpp` and `CMakeLists.txt`
- [X] T058 [US2] Implement the PhysicsMaterial asset writer/loader in `Editor/AssetCompilers/PhysicsMaterialAssetWriter.h` and `Editor/AssetCompilers/PhysicsMaterialAssetWriter.cpp`, then register `.tephysmat`, `.tecollider`, and `.tepcoll` importer/type/GUID paths with append-only asset IDs in `Editor/TerraEngine-Editor/Assets/AssetDatabase.h`, `Editor/TerraEngine-Editor/Assets/AssetDatabase.cpp`, and `Engine/Resource/ResourceManager.h`
- [X] T059 [US2] Implement shared immutable cooked-shape cache publication, dirty recook replacement, stale-last-known-good state, and reader-safe retirement in `Engine/Physics/PhysicsWorld.cpp`
- [X] T060 [P] [US2] Document collider recipes, supported scale, headless cooking, provenance, failure recovery, and asset movement in `Docs/PhysicsColliderAssets.md`
- [X] T061 [US2] Run all US2 tests first as failing then passing, compile the engine/editor/compiler targets, run install, and record results in `specs/042-physics-system/evidence/us2-validation.md`

**Checkpoint**: Rich collision geometry is independently cookable, reference-stable, atomically publishable, and usable without source assets at runtime.

---

## Phase 5: User Story 3 — Drive Gameplay with Queries and Events (Priority: P2)

**Goal**: Provide deterministic typed motion commands, bounded immediate/batched queries, and ordered collision/contact/activation/sleep/trigger events.

**Independent Test**: Apply commands from concurrent producers, execute all query types including a 10,000-request batch, verify stable hit/event ordering and bounds, and prove stale targets/results cannot affect reused objects or worlds.

### Tests for User Story 3

- [X] T062 [P] [US3] Add failing concurrent producer, force-mode, effective-tick, idempotency metadata, and deterministic merge tests in `Tests/Engine/Physics/PhysicsGameplayCommandTests.cpp`
- [X] T063 [P] [US3] Add failing ray, shape-cast, overlap, point, filter, calibrated-bound, expiry, cancellation, and 10,000-batch tests that repeat the same state 100 times and assert identical hit order and bounded peak memory in `Tests/Engine/Physics/PhysicsQueryTests.cpp`
- [X] T064 [P] [US3] Add failing collision/trigger enter-stay-exit, contact, activation, sleep, canonical pair, sorting, filtering, overflow, and retention tests in `Tests/Engine/Physics/PhysicsEventTests.cpp`
- [X] T065 [P] [US3] Add failing remove/recreate/world-reload races proving stale commands, hits, and events are rejected in `Tests/Engine/Physics/PhysicsStalePublicationTests.cpp`

### Implementation for User Story 3

- [X] T066 [US3] Complete native body command receipts and deterministic continuous force, acceleration, impulse, point impulse, velocity-change, explicit velocity, teleport, kinematic, wake, and sleep APIs in `Engine/Physics/PhysicsCommands.h` and `Engine/Physics/PhysicsCommands.cpp`
- [X] T067 [P] [US3] Implement backend-neutral query requests, filters, handles, hits, immutable results, bounds, expiry, cancellation, and stable sorting keys in `Engine/Physics/PhysicsQueries.h` and `Engine/Physics/PhysicsQueries.cpp`
- [X] T068 [P] [US3] Implement backend-neutral event records, subscription policies, thread-local collectors, canonical pairs, bounded packets, and ordering keys in `Engine/Physics/PhysicsEvents.h` and `Engine/Physics/PhysicsEvents.cpp`
- [X] T069 [US3] Implement Jolt rays, shape casts, overlaps, point queries, prepared batches, and provider-to-engine hit conversion in `Engine/Physics/Backend/JoltPhysicsBackend.cpp`
- [X] T070 [US3] Implement Jolt contact/activation/sleep callbacks that write only thread-local engine collectors and never call scene, managed, editor, automation, or renderer code in `Engine/Physics/Backend/JoltPhysicsBackend.cpp`
- [X] T071 [US3] Integrate query preparation/execution, event merge/sort/filter, result histories, command receipts, and stale generation checks into frame workloads in `Engine/Physics/PhysicsSystem.cpp`
- [X] T072 [US3] Implement protected immediate-query access and immutable published query/event/body-state access in `Engine/Physics/PhysicsWorld.h` and `Engine/Physics/PhysicsWorld.cpp`
- [X] T073 [P] [US3] Add native usage examples for force modes, custom gravity, queries, batches, subscriptions, ordering, and result lifetime in `Docs/PhysicsGameplay.md`
- [X] T074 [US3] Add physics query/event/command counters, overflow/stale rejection details, and timing to `Engine/Physics/PhysicsDiagnostics.cpp`
- [X] T075 [US3] Run all US3 tests first as failing then passing, compile affected targets, run install, and record results in `specs/042-physics-system/evidence/us3-validation.md`
- [X] T076 [US3] Run the installed-runtime 10,000-query and concurrent-command demonstration and record hashes, bounds, and ordering evidence in `specs/042-physics-system/evidence/us3-query-event-demo.md`

**Checkpoint**: Native gameplay can independently command and inspect physics through stable, bounded, generation-safe contracts.

---

## Phase 6: User Story 4 — Create Constraints and Playable Characters (Priority: P2)

**Goal**: Support the required constraint families and a production-ready character foundation with stable grounding and moving-platform behavior.

**Independent Test**: Exercise every required joint plus break events, then traverse slopes, stairs, jumps, resize, penetration recovery, and moving/rotating platforms with stable published support state.

### Tests for User Story 4

- [X] T077 [P] [US4] Add failing ConstraintSet CRTP/reflection/serialization, stable record ID, UUID endpoint, range, and duplicate-type tests in `Tests/Engine/Physics/PhysicsConstraintComponentTests.cpp`
- [X] T078 [P] [US4] Add failing fixed/point/distance/hinge/slider/cone/swing-twist/6DOF/gear, motor, spring, limit, collision-policy, break, and rollback tests in `Tests/Engine/Physics/PhysicsConstraintSimulationTests.cpp`
- [X] T079 [P] [US4] Add failing CharacterController CRTP/reflection/serialization and command/state tests in `Tests/Engine/Physics/PhysicsCharacterComponentTests.cpp`
- [X] T080 [P] [US4] Add failing slope, stair, jump, resize, penetration recovery, grounding, and moving/rotating-platform tests in `Tests/Engine/Physics/PhysicsCharacterSimulationTests.cpp`

### Implementation for User Story 4

- [X] T081 [P] [US4] Implement stable tagged constraint records, endpoint UUIDs, frames, limits, springs, motors, break policy, reset generation, reflection, and validation in `Engine/Components/Physics/ConstraintSetComponent.h` and `Engine/Components/Physics/ConstraintSetComponent.cpp`
- [X] T082 [P] [US4] Implement CharacterControllerComponent shape, traversal, support, filtering, gravity/jump defaults, rotation policy, reflection, and validation in `Engine/Components/Physics/CharacterControllerComponent.h` and `Engine/Components/Physics/CharacterControllerComponent.cpp`
- [X] T083 [US4] Register constraint and character component schemas, enums, UUID fields, and serializers in `Engine/Core/Object/ComponentRegistry.cpp` and `Engine/Scene/Serializer.cpp`
- [X] T084 [US4] Implement Jolt constraint creation/update/destruction, engine-neutral motors/limits/springs, optional gear support detection, applied impulse extraction, and staged inverse operations in `Engine/Physics/Backend/JoltPhysicsBackend.cpp`
- [X] T085 [US4] Implement provider-neutral threshold evaluation, exactly-once ordered break events, persistent broken state, and explicit reset commands in `Engine/Physics/PhysicsWorld.cpp` and `Engine/Physics/PhysicsEvents.cpp`
- [X] T086 [US4] Implement Jolt virtual-character lifecycle, movement, jump, resize, penetration recovery, support contacts, slopes, stairs, and moving-platform integration in `Engine/Physics/Backend/JoltPhysicsBackend.cpp`
- [X] T087 [US4] Integrate constraint/character snapshot preparation, generation validation, commands, publication, diagnostics, and teardown into `Engine/Physics/PhysicsSystem.cpp`
- [X] T088 [P] [US4] Document constraints, motors, breaks, character policy, moving platforms, and gameplay responsibilities in `Docs/PhysicsConstraintsAndCharacters.md`
- [X] T089 [US4] Run all US4 tests first as failing then passing, compile affected targets, run install, and record results in `specs/042-physics-system/evidence/us4-validation.md`
- [X] T090 [US4] Demonstrate the installed constraint-and-character fixture and record invariant/tolerance evidence in `specs/042-physics-system/evidence/us4-character-demo.md`

**Checkpoint**: Constraints and characters are independently authorable and behave through backend-neutral runtime contracts.

---

## Phase 7: User Story 5 — Author, Debug, and Restore Physics in the Editor (Priority: P3)

**Goal**: Deliver complete editor authoring, undo/prefab behavior, project settings, safe debug visualization, diagnostics, and Play Mode usability.

**Independent Test**: Edit every physics record with undo/redo and prefab overrides, inspect paused/runtime state and debug packets, change project settings safely, and repeat Play Mode lifecycle with and without rendering.

### Tests for User Story 5

- [X] T091 [P] [US5] Add failing Inspector record add/remove/reorder/duplicate, units, validation, multi-edit, and deferred rebuild tests in `Tests/Editor/Physics/PhysicsInspectorTests.cpp`
- [X] T092 [P] [US5] Add failing grouped undo/redo, stable record ID, prefab apply/revert, connected UUID remap, and atomic invalid-cycle tests in `Tests/Editor/Physics/PhysicsPrefabUndoTests.cpp`
- [X] T093 [P] [US5] Add failing Physics Project Settings, layer rename/reference, impact preflight, serialization, and runtime rebuild tests in `Tests/Editor/Physics/PhysicsProjectSettingsTests.cpp`
- [X] T094 [P] [US5] Add failing immutable debug packet, pause, renderer-unavailable, bounded history, and Scene View display tests in `Tests/Editor/Physics/PhysicsSceneViewTests.cpp`
- [X] T095 [P] [US5] Add failing diagnostics window/menu, scheduler declaration, profiler scope/counter, and zero-extra-work tests in `Tests/Editor/Physics/PhysicsDiagnosticsWindowTests.cpp`

### Implementation for User Story 5

- [X] T096 [US5] Add Rigidbody, ColliderSet, ConstraintSet, CharacterController, and runtime read-only Inspector editors with units, ranges, contextual controls, stable record operations, handles, and deferred transactional rebuilds in `Editor/TerraEngine-Editor/Windows/InspectorWindow.cpp`
- [X] T097 [US5] Add shape edit handles, fit-to-render-mesh actions, local preview, and one final protected authoring transaction in `Editor/TerraEngine-Editor/Windows/SceneViewWindow.cpp` and `Editor/TerraEngine-Editor/Windows/SceneViewOverlayTypes.cpp`
- [X] T098 [US5] Extend grouped component payload undo/redo and prefab record/property overrides with stable child IDs and connected-object UUID remapping in `Editor/TerraEngine-Editor/Subsystems/UndoRedoSystem.cpp` and `Engine/Scene/PrefabComposition.cpp`
- [X] T099 [US5] Add PhysicsSettings metadata, category UI, gravity/timestep/layer/material/solver/query/debug controls, impact preflight, and protected publication in `Editor/TerraEngine-Editor/Project/ProjectSettingsMetadata.h` and `Editor/TerraEngine-Editor/Windows/ProjectSettingsWindow.cpp`
- [X] T100 [P] [US5] Implement immutable physics debug lines/triangles/contacts/constraints/character/query primitives and bounded lifetime selection in `Engine/Physics/PhysicsDiagnostics.cpp` and `Editor/TerraEngine-Editor/Windows/SceneViewOverlayTypes.cpp`
- [X] T101 [US5] Render physics debug packets through existing Scene View overlay paths with renderer-unavailable no-op behavior in `Editor/TerraEngine-Editor/Windows/SceneViewWindow.cpp` and `Engine/Renderer/RenderSystem.cpp`
- [X] T102 [P] [US5] Implement the opt-in Window-menu Physics Diagnostics view over immutable snapshots in `Editor/TerraEngine-Editor/Windows/PhysicsDiagnosticsWindow.h` and `Editor/TerraEngine-Editor/Windows/PhysicsDiagnosticsWindow.cpp`
- [X] T103 [US5] Add named Physics CPU/task scopes, counters, capture serialization, and profiler display integration without requiring the diagnostics window in `Engine/Profiling/ProfilerCapture.cpp` and `Editor/TerraEngine-Editor/Windows/ProfilerWindow.cpp`

### Blocking RelWithDebInfo performance stabilization

**BLOCKING GATE**: The installed editor is too slow to continue safely into US6 or later stories. Complete T104–T111 before US5 acceptance and before any further user-story implementation. Optimize only measured bottlenecks, preserve scheduler and transactional correctness, and use RelWithDebInfo exclusively.

- [X] T104 [P] [US5] Add failing editor/runtime performance regression coverage for 10,000-object creation and load, Play Mode entry, fixed-tick throughput, 100 paused Next requests, scheduler barriers, allocations, diagnostic publication, and renderer-unavailable operation in `Tests/Editor/Physics/PhysicsEditorPerformanceTests.cpp` and `Tests/Engine/Physics/PhysicsPerformanceTests.cpp`
- [X] T105 [US5] Capture a reproducible installed-editor RelWithDebInfo baseline with p50/p95/p99 timings, end-to-end stage timings, worker utilization, protected-boundary stalls, allocation and memory high-water marks, automation latency, frame responsiveness, hardware, and profiler artifacts in `specs/042-physics-system/evidence/editor-performance-baseline.md`
- [X] T106 [US5] Derive and record explicit pass/fail budgets no weaker than SC-001, SC-003, SC-004, and SC-007, rank the measured critical path, and map each selected bottleneck to an owning subsystem in `specs/042-physics-system/evidence/editor-performance-baseline.md`
- [X] T107 [US5] Resolve baseline-proven scene creation, object/component registration, serialization, hierarchy, and Play Mode capture/restore bottlenecks without weakening transactional visibility in `Engine/Core/Object/ObjectManager.cpp`, `Engine/Scene/Serializer.cpp`, and `Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.cpp`
- [X] T108 [US5] Resolve baseline-proven authoring snapshot, body synchronization, simulation, extraction, and localized rebuild bottlenecks without per-body hot-path allocation or full-world rebuilds in `Engine/Physics/PhysicsAuthoringSnapshot.cpp`, `Engine/Physics/PhysicsSystem.cpp`, `Engine/Physics/PhysicsWorld.cpp`, and `Engine/Physics/Backend/JoltPhysicsBackend.cpp`
- [X] T109 [US5] Resolve baseline-proven Inspector, Scene View, diagnostics, profiler, renderer-unavailable, and automation polling overhead without activating extra simulation work in `Editor/TerraEngine-Editor/Windows/InspectorWindow.cpp`, `Editor/TerraEngine-Editor/Windows/SceneViewWindow.cpp`, `Engine/Physics/PhysicsDiagnostics.cpp`, and `Editor/TerraEngine-Editor/Automation/EditorAutomationService.cpp`
- [X] T110 [US5] Resolve baseline-proven scheduler barrier, task granularity, queue contention, temporary allocation, and publication costs while preserving declared resources and protected boundaries in `Engine/Core/Scheduling/SystemScheduler.cpp`, `Engine/Physics/Backend/PhysicsJobAdapter.cpp`, and `Engine/Physics/Backend/PhysicsAllocatorAdapter.cpp`
- [X] T111 [US5] Rerun the performance regressions and installed-editor fixture across supported worker counts, prove the recorded budgets and correctness invariants pass, compile all affected targets, run the RelWithDebInfo install gate, and record commands, before/after results, remaining limits, and profiler artifacts in `specs/042-physics-system/evidence/performance-stabilization.md`
- [X] T112 [US5] Run all US5 tests first as failing then passing, compile affected targets, run the RelWithDebInfo install gate, and record results in `specs/042-physics-system/evidence/us5-validation.md`
- [X] T113 [US5] Complete and time a clean-project installed-editor workflow that creates and runs the required static/dynamic/kinematic falling-body scene in under 10 minutes, then perform the undo/prefab/Next/debug/renderer-unavailable walkthrough and record timing and results in `specs/042-physics-system/evidence/us5-editor-demo.md`

**Checkpoint**: Creators can independently author and diagnose physics without unsafe provider access or authored-state corruption.

---

## Phase 8: User Story 6 — Use Physics from C# and Editor Automation (Priority: P3)

**Goal**: Expose generation-safe C# parity and self-registering bounded editor automation with approvals, idempotency, durable jobs, and structured diagnostics.

**Independent Test**: From C# and loopback automation, issue commands and queries, receive events, reload scripts, exercise stale wrappers, approvals, duplicate idempotency keys, durable jobs, and confirm project lifecycle operations remain absent.

### Tests for User Story 6

- [X] T114 [P] [US6] Add failing C ABI version/size, handle generation, command receipt, query buffer, snapshot, event packet, and NativeAOT parity tests in `Tests/Engine/Physics/PhysicsManagedAbiTests.cpp`
- [X] T115 [P] [US6] Add failing managed Rigidbody `UseGravity`, force modes/custom gravity, query, event ordering, callback exception, and stale wrapper tests in `Tests/Engine/Physics/PhysicsManagedIntegrationTests.cpp`
- [X] T116 [P] [US6] Add failing capability discovery, project/scene/Play state, bounds, generation, and absence-of-project-lifecycle tests in `Tests/Editor/Automation/EditorMcpPhysicsContractTests.cpp`
- [X] T117 [P] [US6] Add failing approval fingerprint, permission denial, idempotent impulse, durable cook/query job, cancellation, reconnect, and correlated log tests in `Tests/Editor/Automation/EditorMcpPhysicsSafetyTests.cpp`

### Implementation for User Story 6

- [X] T118 [US6] Add versioned fixed-width native physics ABI functions for handle validation, commands, queries, snapshots, subscriptions, and packet copying in `Engine/Scripting/ScriptGlue.h` and `Engine/Scripting/ScriptGlue.cpp`
- [X] T119 [US6] Mirror the physics ABI through exported NativeAOT vtables and startup validation in `Engine/Scripting/ExportVTable.h`, `Engine/Managed/Source/Export/ExportVTable.cs`, and `Engine/Managed/Source/Export/ExportBridge.cs`
- [X] T120 [P] [US6] Implement managed physics handles, force modes, query/event/body/character DTOs, and typed status values in `Engine/Managed/Source/Physics/PhysicsTypes.cs`
- [X] T121 [P] [US6] Implement managed Rigidbody/ColliderSet/ConstraintSet/CharacterController wrappers with UUID and object/component/record/world/binding generation validation in `Engine/Managed/Source/Physics/Rigidbody.cs`, `Engine/Managed/Source/Physics/ColliderSet.cs`, `Engine/Managed/Source/Physics/ConstraintSet.cs`, and `Engine/Managed/Source/Physics/CharacterController.cs`
- [X] T122 [US6] Implement managed Physics commands, `AddForce(..., ForceMode.Acceleration)` custom gravity, immediate/scheduled/batch queries, result lifetime, and immutable state reads in `Engine/Managed/Source/Physics/Physics.cs`
- [X] T123 [US6] Implement protected ordered managed event delivery, subscription snapshots, reload invalidation, and per-callback exception isolation in `Engine/Scripting/ScriptingSystem.cpp` and `Engine/Managed/Source/Physics/PhysicsEvents.cs`
- [X] T124 [P] [US6] Implement self-registering read/query/body/character/settings/layer/material/cook capabilities and schemas in `Editor/TerraEngine-Editor/Automation/Providers/PhysicsAutomationProvider.h` and `Editor/TerraEngine-Editor/Automation/Providers/PhysicsAutomationProvider.cpp`
- [X] T125 [US6] Register PhysicsAutomationProvider without central string dispatch and bind immutable-snapshot, command-commit, and authoring-flush boundaries in `Editor/TerraEngine-Editor/Automation/EditorAutomationService.cpp`
- [X] T126 [US6] Enforce reference-impact fingerprints, approval binding, Play Mode/generation checks, explicit idempotency Begin/Complete, durable jobs, bounds, cancellation, and structured logs in `Editor/TerraEngine-Editor/Automation/Providers/PhysicsAutomationProvider.cpp`
- [X] T127 [P] [US6] Document C# and automation APIs, custom gravity, status/error contracts, permissions, approvals, idempotency, and job examples in `Docs/PhysicsScripting.md` and `Docs/EditorMcpPhysics.md`
- [X] T128 [US6] Run all US6 tests first as failing then passing, compile managed/native/editor/export targets, run the RelWithDebInfo install gate, and record results in `specs/042-physics-system/evidence/us6-validation.md`
- [X] T129 [US6] Complete installed-editor loopback plus managed reload/event/custom-gravity scenarios and record results in `specs/042-physics-system/evidence/us6-loopback-demo.md`

**Checkpoint**: Managed gameplay and editor automation independently use the same safe runtime behavior without provider pointers or arbitrary project/process access.

---

## Phase 9: User Story 7 — Export Identical Runtime Behavior (Priority: P3)

**Goal**: Package the same physics runtime, settings, layers, materials, cooked assets, managed bindings, and invariant behavior into headless and graphical exports.

**Independent Test**: Export a complete fixture without source assets or global SDKs, reject seeded missing/corrupt/placeholder physics data, and match editor Play Mode invariants through simulation, transitions, and shutdown.

### Tests for User Story 7

- [X] T130 [P] [US7] Add failing physics manifest, append-only asset type, settings/layer hash, provider/cook revision, inventory, and six-evidence-file tests in `Tests/Editor/Export/PhysicsExportManifestTests.cpp`
- [X] T131 [P] [US7] Add failing missing/corrupt/empty/placeholder/provider-mismatch/source-only collider validation tests in `Tests/Editor/Export/PhysicsExportValidationTests.cpp`
- [X] T132 [P] [US7] Add failing Play Mode/export invariant comparison for bodies, stepping, filtering, queries, events, constraints, characters, transitions, and shutdown in `Tests/Editor/Export/PhysicsExportParityTests.cpp`
- [X] T133 [P] [US7] Add failing exported runtime startup, cooked asset load, managed binding, headless operation, and zero-active-job shutdown tests in `Tests/Engine/Runtime/PhysicsExportRuntimeTests.cpp`

### Implementation for User Story 7

- [X] T134 [US7] Extend staged asset and resolved manifest models with physics settings, layers hash, provider revision, cook version, materials, colliders, checksums, and invariant version in `Editor/Export/ExportTypes.h` and `Editor/Export/ExportPipeline.cpp`
- [X] T135 [US7] Stage only validated runtime physics settings/materials/cooked colliders and exclude source-only collider dependencies in `Editor/Export/ExportPipeline.cpp` and `Editor/Export/TepakPackager.cpp`
- [X] T136 [US7] Add physics dependency, payload, version, checksum, placeholder, and startup-scene validation with actionable diagnostics in `Editor/Export/ExportValidator.cpp` and `Editor/Export/ExportPackageValidator.cpp`
- [X] T137 [US7] Extend inventory classification and all six durable evidence outputs with physics provenance and validation data in `Editor/Export/ExportEvidence.cpp`
- [X] T138 [US7] Load packaged PhysicsSettings, layers, materials, cooked colliders, provider runtime, and managed physics bindings from the runtime content mount in `Engine/Runtime/ExportStartupCoordinator.cpp` and `Engine/Runtime/ContentMount/ContentMount.cpp`
- [X] T139 [US7] Add the source-only editor/export parity fixture, expected invariants, and corruption seeds in `Tests/Fixtures/PhysicsExportRuntime/README.md`, `Tests/Fixtures/PhysicsExportRuntime/project.json`, and `Tests/Fixtures/PhysicsExportRuntime/Scenes/Startup.tescene`
- [X] T140 [US7] Extend the installed headless baseline runner to cook, export, launch, compare invariants, and verify shutdown for the physics fixture in `Tools/ExportRuntimeBaseline/RunBaseline.ps1`
- [X] T141 [US7] Run all US7 tests first as failing then passing, compile runtime/editor/export/compiler targets, run the RelWithDebInfo install gate, and record results in `specs/042-physics-system/evidence/us7-validation.md`
- [X] T142 [US7] Run the installed editor/exported-runtime parity workflow without source assets or global physics dependencies and archive the six evidence files under `specs/042-physics-system/evidence/export-parity/`

**Checkpoint**: Exported games independently load and run the same physics behavior and fail safely before launch when packaged physics data is invalid.

---

## Phase 10: Polish, Stress, and Cross-Cutting Acceptance

**Purpose**: Close full-scope performance, lifecycle, documentation, platform, regression, install, and live acceptance gates after all desired stories are integrated.

- [X] T143 [P] Add the committed 10,000-body, 1,000-active, 10,000-ray, dense-contact, constraint-chain, character-platform, and bounded-history fixture manifest in `Tests/Fixtures/Physics/stress-fixtures.json` and `Tests/Fixtures/Physics/README.md`
- [X] T144 [P] Add deterministic replay across process launches and 1/2/4/max worker counts with backend-state and sorted-packet hashes, including 100 identical 10,000-ray repetitions with bounded peak memory, in `Tests/Engine/Physics/PhysicsDeterminismStressTests.cpp`
- [X] T145 [P] Add 100 stale-race repetitions and 1,000 scene/Play Mode lifecycle cycles covering unload, remove/recreate, script reload, renderer failure, project close, and shutdown in `Tests/Engine/Physics/PhysicsLifecycleStressTests.cpp` and `Tests/Editor/Physics/PhysicsPlayModeSoakTests.cpp`
- [X] T146 Run supported ASan/TSan lifecycle and callback-collector lanes and record toolchain, exclusions, and results in `specs/042-physics-system/evidence/sanitizer-validation.md`
- [X] T147 Re-measure the fully integrated system against the T106 budgets, including p50/p95/p99 fixed-tick and editor-stage time, worker utilization, barrier stalls, allocations, temporary/provider memory, cache behavior, binary size, and hardware/build context in `specs/042-physics-system/evidence/performance-validation.md`
- [X] T148 Resolve performance regressions introduced after the T111 stabilization gate without weakening correctness or reintroducing per-body hot-path allocation, main-thread bottlenecks, or full-world rebuilds in `Engine/Physics/PhysicsSystem.cpp`, `Engine/Physics/PhysicsWorld.cpp`, `Engine/Physics/Backend/JoltPhysicsBackend.cpp`, and `Editor/TerraEngine-Editor/Subsystems/MainEditorSubsystem.cpp`
- [X] T149 Update clean-checkout RelWithDebInfo configure/build/test/install, focused filters, stress, installed-editor, automation, cooking, export, performance-budget, and troubleshooting instructions in `Docs/Testing.md`, `Docs/Physics.md`, and `specs/042-physics-system/quickstart.md`
- [X] T150 Run the complete RelWithDebInfo integrated compile of all affected engine/editor/managed/compiler/export/test targets and fix introduced failures in `CMakeLists.txt`, `Tests/Engine/CMakeLists.txt`, and `Tests/Editor/CMakeLists.txt`
- [X] T151 Run the complete applicable RelWithDebInfo CTest and regression suites and record exact commands/results in `specs/042-physics-system/evidence/full-regression.md`
- [X] T152 Run `cmake --build --preset RelWithDebInfo --target install`, verify canonical `out/build/RelWithDebInfo/Build` contains required physics binaries/assets/docs and excludes provider samples/source dependencies, and record results in `specs/042-physics-system/evidence/install-validation.md`
- [X] T153 Execute the full installed-editor loopback and exported-runtime quickstart acceptance, map FR-001 through FR-042 and SC-001 through SC-013 to passing evidence, explicitly confirm FR-043 remains out of scope, and record results in `specs/042-physics-system/evidence/final-acceptance.md`

---

## Dependencies and Execution Order

### Phase dependencies

- **Phase 1 — Setup**: Starts immediately; T004 is conditional on the T003 spike decision.
- **Phase 2 — Foundation**: Depends on Phase 1 and blocks every user story.
- **US1 — Rigid Bodies**: Depends on Foundation and establishes the MVP runtime/scheduler/Play Mode path.
- **US2 — Rich Geometry**: Can begin after Foundation for asset contracts, but cooked-shape simulation and its checkpoint depend on US1 body/world integration.
- **US3 — Queries and Events**: Depends on US1 body simulation and publication boundaries.
- **US4 — Constraints and Characters**: Depends on US1; break events and character query/support integration depend on the relevant US3 contracts.
- **US5 — Editor**: Can begin its isolated UI/test work after Foundation, but its checkpoint depends on US1, US2, and US4 runtime authoring surfaces. T104–T111 form a blocking measured-performance gate before T112–T113 may close US5.
- **US6 — C#/Automation**: No US6 task may begin until the open T048 installed MVP demonstration and all of US5, including the T104–T111 performance stabilization and T112–T113 acceptance, are complete.
- **US7 — Export**: Manifest/validator tests can begin after Foundation; full parity depends on US1–US6.
- **Phase 10 — Acceptance**: Depends on all selected user stories.

### User-story dependency graph

```text
Setup -> Foundation -> US1 (MVP)
                       |---> US2 ---------|
                       |---> US3 ---> US4 |---> US5 implementation
                       |-------------------|              |
                                              Performance stabilization
                                                        |
                                                US5 acceptance -> US6 -> US7
All completed stories -------------------------------------------------> Acceptance
```

US2 and US3 are the main parallel branches after US1. US4 may start its component/test work alongside them but needs US3 event/query seams for completion. US5 editor work can overlap late US2/US4 work in non-overlapping files. The T104–T111 performance gate is serialized after the US5 implementation exists because profiling must select the actual critical path; its bounded fixes may be delegated only after file ownership is partitioned from baseline evidence. US6 managed and automation halves can run concurrently only after the performance gate and US5 acceptance pass. US7 validator/evidence work can overlap late US6 work but parity waits for all surfaces.

### Within each story

1. Write the listed tests and confirm the intended missing behavior fails.
2. Implement data/component models before runtime services.
3. Implement provider/runtime services before editor, managed, automation, or export integrations.
4. For US5, capture the RelWithDebInfo baseline, fix only measured bottlenecks, and pass T104–T111 before acceptance or later stories.
5. Run the story's focused tests and smallest affected builds.
6. Run the RelWithDebInfo install target and demonstrate the story through installed outputs.
7. Keep every code-changing checkbox in the story open until step 6 passes, then complete the validated task group together.
8. Treat worker-local validation as supporting evidence; integrated acceptance remains orchestrator-owned.

---

## Parallel Execution Examples

### User Story 1

- T033 RigidBodyComponent, T034 ColliderSetComponent, and T035 PhysicsMaterial target separate files and can proceed in parallel after their tests exist.
- T046 documentation can proceed while T039–T045 integrate runtime and Play Mode behavior.

### User Story 2

- T053 ColliderRecipe and T054 CookedColliderAsset can proceed in parallel.
- T060 documentation can proceed alongside the compiler/cache implementation.

### User Story 3

- T067 query contracts and T068 event contracts can proceed in parallel before backend/system integration.
- T073 documentation can proceed alongside diagnostics and system integration.

### User Story 4

- T081 constraint components and T082 character components can proceed in parallel.
- Constraint and character test files T077–T080 can be authored concurrently before implementation.

### User Story 5

- Inspector/prefab/settings tests T091–T093 and Scene View/diagnostic tests T094–T095 target independent files.
- T100 debug packet production and T102 diagnostics window can proceed in parallel before final editor integration.
- T104 performance regression coverage can be prepared independently, but T105–T111 follow the measured critical path. Do not parallelize performance fixes until T106 assigns non-overlapping subsystem ownership from evidence.

### User Story 6

- T120 managed value types, T121 wrappers, and T124 automation provider scaffolding target separate files.
- Managed tests T114–T115 and automation tests T116–T117 can be authored concurrently after US5 acceptance.

### User Story 7

- Manifest, validation, parity, and runtime tests T130–T133 can be authored concurrently.
- Fixture construction T139 can overlap manifest/evidence implementation after contract fields are fixed.

---

## Implementation Strategy

### MVP first

1. Complete Setup and provider selection.
2. Complete Foundational runtime contracts.
3. Complete US1 only.
4. Stop and validate the installed MVP fixture: primitive bodies, gravity/`useGravity`, custom acceleration, collisions, fixed tick, Next, transactional transforms, and restoration.
5. Do not begin broad editor/API/export work until this runtime checkpoint passes.

### Incremental delivery

1. **MVP**: US1 primitive authoring and safe fixed-step simulation.
2. **Production geometry**: US2 cooking/assets.
3. **Gameplay API**: US3 commands/queries/events.
4. **Mechanics**: US4 constraints/characters.
5. **Creator experience implementation**: US5 editor/diagnostics.
6. **Blocking performance stabilization**: T104–T111 baseline, budgets, measured fixes, regressions, RelWithDebInfo install, and installed-editor proof.
7. **Creator experience acceptance**: T112–T113 focused validation and installed-editor demonstration.
8. **Integration surfaces**: US6 managed/automation.
9. **Shipping**: US7 export parity.
10. **Acceptance**: stress, regression, install, live loopback, and export evidence.

Every increment must compile and install; no story may defer its rollback, visibility, stale-generation, or lifecycle tests to the final phase.

---

## Execution Recommendation

**Recommended mode**: Selective mixed GPT-5.6 delegation with an `xhigh` Sol orchestrator and a small number of persistent Terra/Luna workers.

Use no more than three workers concurrently by default. Group related tasks by subsystem rather than spawning one worker per task. Reuse workers for follow-up tasks within the same subsystem. Increase concurrency only when assignments are substantial, independent, and have clearly non-overlapping ownership.

**Why it fits**: The graph contains 153 tasks across provider integration, runtime concurrency, transactions, assets, editor, a new blocking performance-stabilization gate, managed ABI, automation, export, tests, and documentation. After the shared foundation and public contracts stabilize, US2 asset work, US3 query/event records, US4 component models, US5 editor surfaces, US6 managed/automation halves, US7 validators/fixtures, documentation, and focused tests offer substantial non-overlapping parallelism. The critical path remains tightly coupled around the provider adapter, scheduler workloads, coordinated two-domain commit, fixed-step/Next semantics, measured editor performance, lifecycle, and final integration; these stay with Sol.

**Tradeoffs**:

- **Cost**: Mixed delegation uses more total model calls and local validation than Sol-only, but avoids spending highest-cost orchestration context on repetitive test, DTO, documentation, and isolated editor work.
- **Accuracy**: `xhigh` Sol reasoning is justified by concurrency, transactional rollback, stale-generation safety, ABI stability, performance critical-path analysis, and teardown correctness. Luna is appropriate for baseline evidence and profile/log extraction; Terra is appropriate for bounded hotspot fixes after the orchestrator partitions ownership from T106 evidence.
- **Coordination**: Parallelism must wait for stable interfaces, assignments must name non-overlapping files/task IDs, and workers must not modify planning artifacts or run full builds/suites. Overlapping central files such as `CMakeLists.txt`, `ComponentRegistry.cpp`, `MainEngine.cpp`, `PhysicsSystem.cpp`, `JoltPhysicsBackend.cpp`, and export registries require serialized ownership and orchestrator integration.
- **Alternative**: Sol-only would reduce coordination risk but lengthen the implementation and consume more expensive context on 153 tasks. Ultra is not recommended by default because the work is large but already decomposed; use it only if maximum quality is explicitly preferred over cost or repeated integration failures make broader proactive delegation worthwhile.

---

## Notes

- The exact Jolt v5.6.0 revision and `External/JoltPhysics` addition were explicitly approved by the user on 2026-07-13 and may be executed by T004 under constitution v1.3.1 after T003 passes. Jolt 5.5 is comparison evidence only.
- If Jolt fails T003, stop and return to planning; PhysX is a research candidate, not an implementation-approved substitution.
- Code-changing task checkboxes remain open until their applicable RelWithDebInfo phase compile/install gate verifies `out/build/RelWithDebInfo/Build`; no other configuration satisfies spec 042 completion.
- Never modify provider source inside `External/JoltPhysics`; adapters and build options live in TerraEngine-owned files.
- All provider callbacks publish only to thread-local collectors.
- All scene/object/component writes from worker, managed, editor, or automation producers use typed deferred/transactional paths.
- Tests use tolerances and invariants rather than fragile exact floating-point snapshots.
- Full builds, complete CTest/regression suites, installed-editor loopback, export parity, and final completion decisions remain orchestrator-owned.
