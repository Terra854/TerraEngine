# Feature Specification: Complete Physics System

**Feature Branch**: `042-physics-system`  
**Created**: 2026-07-13  
**Status**: Ready for implementation after prerequisite validation  
**Input**: User description: "Begin implementation of the complete TerraEngine physics system described in `Docs/PhysicsSystemDesign.md`."

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Author and Simulate Rigid Bodies (Priority: P1)

As a game creator, I can add rigid bodies and colliders to scene objects, configure their physical behavior, and run a stable fixed-step simulation in Play Mode so ordinary gameplay objects fall, collide, slide, bounce, sleep, wake, and respond to forces predictably.

**Why this priority**: Rigid-body authoring and simulation are the minimum useful physics experience and establish the safety, timing, and data-ownership foundation for every later story.

**Independent Test**: Create a scene containing static, dynamic, and kinematic objects with primitive colliders and materials; run, pause, and single-step Play Mode; verify gravity, collision filtering, continuous collision detection, sleeping, transform publication, and exact authored-scene restoration on exit.

**Acceptance Scenarios**:

1. **Given** an object with a valid dynamic body and collider, **When** Play Mode advances, **Then** it uses project world gravity when `useGravity` is enabled, receives no built-in gravity when disabled, responds to fixed-tick custom acceleration commands and contacts, and leaves authored state unchanged.
2. **Given** static, dynamic, and kinematic bodies on configured layers, **When** their paths intersect, **Then** only permitted pairs interact and trigger-only pairs produce trigger behavior without physical response.
3. **Given** Play Mode is paused, **When** the user presses Next, **Then** exactly one configured fixed physics tick runs, simulation callbacks receive that explicit tick duration, results are published once, one resulting frame is rendered, and Play Mode remains paused.
4. **Given** a fast-moving body configured for continuous detection, **When** it approaches representative thin geometry, **Then** it does not pass through that geometry in the supported test cases.

---

### User Story 2 - Build Rich Collision Geometry and Assets (Priority: P2)

As a content creator, I can use primitive, convex, compound, and triangle-mesh colliders with reusable materials and cooked assets so complex levels behave consistently without requiring source model data at runtime.

**Why this priority**: Production scenes require more than primitive shapes, and a validated reusable asset path is necessary for large scenes and exports.

**Independent Test**: Author all required shape types, cook representative convex and mesh geometry, move and rename referenced assets, reload the project, and verify identical collider behavior and clear validation for unsupported scale or invalid geometry.

**Acceptance Scenarios**:

1. **Given** supported source geometry, **When** a creator publishes a collider asset, **Then** the asset records its source identity and settings and can be reused by multiple bodies.
2. **Given** invalid, degenerate, oversized, or unsupported geometry, **When** cooking is requested, **Then** publication is rejected with actionable diagnostics and no partial asset replacement.
3. **Given** a previously valid collider is recooked unsuccessfully, **When** retaining the prior collider is safe, **Then** the last known-good result remains active and is visibly identified as stale.

---

### User Story 3 - Drive Gameplay with Queries and Events (Priority: P2)

As a gameplay programmer, I can issue forces and motion commands, perform immediate and batched scene queries, and receive ordered collision, contact, activation, sleep, and trigger events without depending on a particular physics provider.

**Why this priority**: Gameplay needs a stable command, query, and event contract in addition to passive simulation.

**Independent Test**: Run rays, shape casts, overlaps, point queries, and a 10,000-request batch against a fixture while applying commands from concurrent producers; verify filtering, bounded results, stable ordering, event transitions, and safe stale-target rejection.

**Acceptance Scenarios**:

1. **Given** multiple valid hits, **When** a query completes, **Then** hits are returned in stable distance and identity order with world and tick context.
2. **Given** simultaneous command producers, **When** commands reach the same simulation boundary, **Then** they are applied in a deterministic engine-defined order.
3. **Given** an object or world is removed before a queued command or result is published, **When** publication is attempted, **Then** the stale item is rejected and cannot affect a replacement object or world.
4. **Given** a body enters, remains in, and leaves a collision or trigger pair, **When** ticks are published, **Then** exactly ordered enter, stay, and exit events are available according to subscription policy.

---

### User Story 4 - Create Constraints and Playable Characters (Priority: P2)

As a game creator, I can connect bodies with configurable constraints and configure a robust character controller so doors, mechanisms, chains, moving platforms, slopes, stairs, and jumping characters behave reliably.

**Why this priority**: Constraints and characters are part of the complete first release and unlock core gameplay beyond free rigid bodies.

**Independent Test**: Exercise every required constraint family and a character fixture containing slopes, stairs, moving and rotating platforms, jumps, resize requests, and penetration recovery; verify limits, motors, springs, breaks, grounding, and ordered output.

**Acceptance Scenarios**:

1. **Given** a configured constraint, **When** connected bodies move, **Then** limits, springs, and motors enforce the authored behavior within documented tolerances.
2. **Given** a breakable constraint exceeds its threshold, **When** the tick is published, **Then** it produces one ordered break event and remains broken until explicitly reset or recreated.
3. **Given** a character traverses supported stairs, slopes, and moving platforms, **When** movement and jump commands are issued, **Then** grounded state, support motion, and contact information remain stable and usable by gameplay.

---

### User Story 5 - Author, Debug, and Restore Physics in the Editor (Priority: P3)

As an editor user, I can configure physics through inspectors and project settings, use undo/redo and prefabs, visualize runtime state in the Scene View, inspect diagnostics, and leave Play Mode without runtime state dirtying my authored scene.

**Why this priority**: A production system must be approachable and diagnosable, but these tools build on the runtime foundation.

**Independent Test**: Author and edit each physics component, apply prefab overrides, undo and redo changes, enter and exit Play Mode repeatedly, and inspect debug and diagnostic views both with simulation paused and with rendering unavailable.

**Acceptance Scenarios**:

1. **Given** a physics property edit, **When** the user applies, undoes, or redoes it, **Then** the scene, prefab override state, and runtime rebuild remain consistent.
2. **Given** a running or paused world, **When** physics visualization is enabled, **Then** requested colliders, contacts, constraints, character state, and recent queries are shown from a safe published snapshot.
3. **Given** the renderer is unavailable, **When** physics continues or shuts down, **Then** simulation and cleanup remain safe and diagnostics do not require renderer access.
4. **Given** runtime motion has changed objects, **When** Play Mode exits, **Then** the captured authored scene is restored exactly and is not marked dirty by transient physics state.

---

### User Story 6 - Use Physics from C# and Editor Automation (Priority: P3)

As a scripting or automation author, I can discover physics capabilities, inspect state, issue bounded commands, submit durable long-running work, and receive correlated results using the same behavior as native gameplay.

**Why this priority**: Scripting and automation are required product surfaces, but they depend on stable runtime contracts.

**Independent Test**: From C# and an active editor automation session, apply motion commands, query the world, handle events, edit settings and assets, test approvals and idempotency, reload scripts, and verify stale wrappers fail safely.

**Acceptance Scenarios**:

1. **Given** a valid managed body reference, **When** a script issues a force, impulse, teleport, velocity, character, or query request, **Then** it receives the same observable behavior as native gameplay without holding an unsafe runtime pointer.
2. **Given** a script callback throws, **When** physics events are delivered, **Then** the exception is isolated and reported while remaining events and simulation continue safely.
3. **Given** an automation request with destructive reference impact, **When** it is submitted, **Then** impact is reported and approval is required before mutation.
4. **Given** two requests share an idempotency key, **When** both reach the runtime, **Then** the physical action or asset publication occurs at most once.
5. **Given** no project is open in the native editor, **When** a physics automation operation is requested, **Then** it fails safely and does not create, open, or close a project.

---

### User Story 7 - Export Identical Runtime Behavior (Priority: P3)

As a game developer, I can export a headless or graphical game containing all required physics data and obtain behavior matching editor Play Mode without source assets or machine-installed dependencies.

**Why this priority**: The feature has no production value unless shipped games use the same validated physics behavior.

**Independent Test**: Export a representative fixture, validate its packaged settings and cooked data, run it without source assets or globally installed dependencies, and compare invariant results against editor Play Mode through scene transitions and shutdown.

**Acceptance Scenarios**:

1. **Given** a valid physics project, **When** it is exported, **Then** all referenced settings, layers, materials, and cooked colliders required at runtime are packaged and evidenced.
2. **Given** a missing, corrupt, placeholder, or empty physics dependency, **When** export validation runs, **Then** export fails with a clear actionable report.
3. **Given** the same fixture and starting state, **When** it runs in Play Mode and the exported game, **Then** body creation, fixed-step policy, filtering, queries, events, constraints, characters, transitions, and shutdown satisfy the same invariant checks.

### Edge Cases

- A body has no valid collider, a collider has no body, or a referenced material is missing.
- Shape dimensions are zero, negative, non-finite, degenerate, too large, negatively scaled, or incompatibly non-uniformly scaled.
- A dynamic body is placed under a moving or non-uniformly scaled parent that cannot be resolved safely.
- More elapsed time arrives than the configured substep cap permits, including pause and single-step transitions.
- A scene unload, object deletion, component removal, world recreation, script reload, or Play Mode stop races queued commands, queries, cooking, event delivery, or simulation work.
- An object slot, component slot, or world identity is reused while stale work still exists.
- Concurrent commands conflict, a transaction contains one invalid operation, or cancellation occurs before publication.
- Query and event traffic exceeds configured bounds or a consumer retains an expired result.
- A constraint references a deleted body, invalid object, cyclic prefab reference, or incompatible motion arrangement.
- A collider recook, allocation, or physics-provider operation fails after a previously valid result exists.
- Rendering fails or is absent while simulation, debugging data extraction, project close, or application shutdown proceeds.
- Event consumers unsubscribe, are destroyed, or throw exceptions during delivery.
- The application shuts down with active jobs, queued commands, pending queries, events, or deferred destruction.

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: The system MUST simulate static, dynamic, and kinematic rigid bodies using a configurable fixed timestep, bounded catch-up, and pause behavior. While paused, Next MUST advance exactly one configured fixed physics tick rather than a hard-coded duration or wall-clock frame delta, run fixed and ordinary simulation callbacks once with that explicit duration, publish once, render one resulting frame, and remain paused.
- **FR-002**: The system MUST support box, sphere, capsule, cylinder, tapered capsule, convex hull, compound, and triangle-mesh collision shapes, including multiple colliders on one object.
- **FR-003**: An object with valid colliders and no explicit rigid body MUST behave as static geometry, while a rigid body without a valid collider MUST be excluded with an actionable diagnostic.
- **FR-004**: The system MUST support layers, masks, broad-phase grouping, triggers, material friction and restitution policies, sleeping, configurable world gravity, motion limits, axis restrictions, interpolation, and the initial `Discrete` and `LinearCast` collision modes. Every rigid body MUST expose `useGravity`: enabled bodies use project world gravity and disabled bodies receive no built-in gravity. Gameplay MUST be able to implement custom per-object or spatial gravity by disabling built-in gravity and applying a mass-independent acceleration command on fixed ticks.
- **FR-005**: Authored units MUST use metres, kilograms, seconds, newtons, and newton-seconds; gravity MUST default to `(0, -9.81, 0)`; user-facing angles MUST follow existing editor conventions.
- **FR-006**: Unsupported negative scale, non-uniform scale, parent transform, and shape combinations MUST be rejected with validation errors rather than silently producing distorted simulation geometry.
- **FR-007**: The system MUST provide fixed, point, distance, hinge, slider, cone, swing-twist, and six-degree-of-freedom constraints, including applicable anchors, limits, springs, motors, break thresholds, and connected-body collision policy; a gear constraint MAY be included when supported by the selected provider.
- **FR-008**: The system MUST provide a backend-independent character controller supporting configurable shape, step height, slope, support volume, contact padding, strength, grounding, stair behavior, moving platforms, desired movement, jumping, resize, and penetration recovery.
- **FR-009**: The system MUST provide ray, shape-cast, overlap, point, and batched queries with layer, trigger, object, and body-type filters; limits MUST bound dimensions, distance, batch size, retained history, and result count.
- **FR-010**: Query results MUST contain hit identity, position, normal, distance or fraction, material or surface context, world generation, and physics tick, and MUST use stable ordering when multiple results compare equally.
- **FR-011**: The system MUST publish ordered collision, contact, activation, sleep, trigger, and constraint-break events with canonical pair ordering, subscription filtering, bounded storage, and explicit overflow diagnostics.
- **FR-012**: Commands, queries, events, handles, and results MUST validate object, component, and world generations so removed or recreated targets cannot receive stale work.
- **FR-013**: Commands from concurrent native, managed, editor, and automation producers MUST enter a typed deferred path and be merged in a deterministic engine-defined order at declared boundaries.
- **FR-014**: Physics-originated scene changes MUST become visible only through a declared transactional publication boundary; worker-safe work MUST NOT directly mutate live scene, hierarchy, object, or component state.
- **FR-015**: A rejected, conflicting, stale, invalid, or cancelled mutation batch MUST leave both the live scene and physics world unchanged, with no partial publication.
- **FR-016**: Every physics system workload MUST declare its scheduling phase, resources read, resources written, worker-safe preparation, protected boundary work, and mutation visibility; diagnostics MUST expose declarations that affect frame execution.
- **FR-017**: The runtime MUST use engine-owned task scheduling for parallel preparation, simulation, queries, extraction, and cooking where supported, and MUST avoid recurring physics work when no active world exists.
- **FR-018**: The system MUST guarantee stable command, event, query, and fixed-step ordering for the same build, platform, processor architecture, settings, and starting state; cross-platform bit-identical simulation is not promised.
- **FR-019**: The engine MUST own stable authored and runtime contracts so gameplay, serialized scenes, scripts, automation, and exports never depend on provider-specific identifiers, layouts, pointers, enum values, or public headers.
- **FR-020**: Planning MUST compare viable physics providers for licensing, supported platforms and features, task-system integration, large-world behavior, diagnostics, determinism, maintenance, performance, memory, build size, and vendoring risk; it MUST record prototype or benchmark evidence, the selected pinned revision, known limitations, and rationale before third-party source is added.
- **FR-021**: All selected third-party physics source MUST be self-contained within the repository, built from the project configuration, pinned to an exact revision, and stripped of unrelated normal-build samples and tools.
- **FR-022**: Collider cooking MUST validate inputs, support asynchronous and headless use, deduplicate equivalent recipes, publish atomically, record source and settings provenance, and produce actionable warnings and errors.
- **FR-023**: Physics materials and cooked collider assets MUST use stable project identities so moves and renames preserve references; runtime packages MUST not require source model assets.
- **FR-024**: Rigid bodies, colliders, constraints, characters, settings, layers, materials, and references MUST serialize through project-owned versioned schemas, support defaults and migrations, and exclude transient runtime state.
- **FR-025**: Physics components and assets MUST support scene save/reload, duplication, prefab defaults and overrides, connected-object identity remapping, and atomic rejection of cyclic or invalid references.
- **FR-026**: The editor MUST provide physics inspectors and project settings with units, valid ranges, contextual controls, material and layer selection, inline validation, safe multi-editing, shape handles, and fit-to-mesh actions.
- **FR-027**: Physics edits MUST integrate with undo/redo and prefab overrides, and runtime rebuilds MUST publish only at a protected boundary.
- **FR-028**: Entering Play Mode MUST create a fresh runtime world from captured authored state; exiting MUST safely stop work, destroy that world, and restore the exact authored state without runtime transforms dirtying it.
- **FR-029**: Scene View debugging MUST visualize requested shapes, body state, bounds, contacts, constraints, character support, and recent queries from immutable published data and MUST remain safe when paused or rendering is unavailable.
- **FR-030**: An opt-in diagnostics surface and the existing profiler MUST expose world/tick state, timing, body and contact counts, cache use, query and event traffic, queue depth, rejection causes, stale work, memory, and allocator high-water marks.
- **FR-031**: Native and managed gameplay MUST be able to create references, issue body and character commands, perform queries, and receive events with equivalent behavior; managed wrappers MUST fail safely after reload, destruction, or world unload.
- **FR-032**: Exceptions from managed event callbacks MUST be isolated, correlated, and reported without corrupting event delivery or simulation.
- **FR-033**: Editor automation MUST expose discoverable bounded read, query, settings, layer, material, body, and character capabilities only within an already-open native-editor-owned project.
- **FR-034**: Automation MUST require reference-impact preflight and appropriate approval for destructive or broad changes, generation validation for runtime commands, idempotency for repeatable mutations, durable jobs for long operations, and correlated diagnostics.
- **FR-035**: Export MUST package the same runtime physics behavior used by editor Play Mode together with required settings, layers, materials, and cooked assets, and MUST validate missing, corrupt, empty, or placeholder dependencies before launch.
- **FR-036**: Editor Play Mode and exported games MUST pass the same invariant fixtures for initialization, stepping, filtering, queries, events, constraints, characters, scene transitions, and shutdown.
- **FR-037**: Scene activation, unload, object or component removal, script reload, Play Mode stop, project close, and shutdown MUST stop acceptance, drain or cancel queued work by documented policy, join active work, prevent late callbacks, invalidate stale identities, and release resources in a safe order.
- **FR-038**: Invalid authored data SHOULD disable only affected physics objects when safe; fatal allocation or provider failures MUST stop simulation cleanly and emit a controlled critical report rather than continuing corrupted state.
- **FR-039**: Failed collider recooking SHOULD retain the last known-good collider when safe and MUST clearly report that the retained result is stale.
- **FR-040**: Normal per-step processing MUST use bounded memory, reuse identical immutable shapes, avoid per-body heap allocation, avoid unnecessary publication for sleeping bodies, and avoid full-world rebuilds after localized edits.
- **FR-041**: A final verification traceability matrix MUST map FR-001 through FR-040 to reproducible tests or acceptance evidence and report the status of units, conversions, identities, validation, filtering, materials, timing, ordering, events, queries, reflection, serialization, scheduling, visibility, concurrency, rollback, simulation, editor, scripting, automation, export, stress, lifecycle, and failure containment.
- **FR-042**: Every independently delivered implementation phase MUST leave the affected code compiling, tests reproducible, and the installed runtime tree successfully produced and verified.
- **FR-043**: Ragdolls, vehicles, soft bodies, buoyancy, force fields, cross-platform deterministic lockstep, networking rollback, fracture, fluids, GPU rigid bodies, runtime provider switching, and arbitrary user callbacks inside physics workers are outside the initial complete release.

### Key Entities

- **Physics World**: One isolated simulation context identified by a stable world identity and generation; owns tick state, bodies, constraints, characters, pending work, results, and diagnostics.
- **Physics Body**: Runtime representation of an authored object and its static, dynamic, or kinematic motion state, addressed through a generation-validated engine identity.
- **Collider Recipe**: Backend-neutral description of one primitive or cooked shape, local transform, material, trigger behavior, filters, and query participation.
- **Cooked Collider Asset**: Immutable validated collision data associated with a stable asset identity, source provenance, settings, format version, bounds, and diagnostics.
- **Physics Material**: Reusable asset defining friction, restitution, density, combine policies, and an optional surface tag.
- **Collision Layer**: Stable project-defined identity with display name, collision matrix membership, and broad-phase grouping.
- **Constraint**: Authored relationship between two bodies or a body and the world, including frames, limits, drive behavior, breaking policy, and runtime state.
- **Character Controller**: Authored character shape and locomotion constraints plus published ground, support, and contact state.
- **Physics Command**: Deferred generation-checked request to create, update, destroy, or move a physics entity or apply force, impulse, velocity, or character intent.
- **Physics Event Packet**: Immutable tick-scoped collection of ordered collision, trigger, activation, sleep, and constraint events.
- **Physics Query Request/Result**: Bounded filtered spatial question and its immutable, tick- and world-stamped answer.
- **Physics Settings**: Project-owned gravity, timestep, layer, solver, sleep, query, diagnostics, and safe advanced settings included in exports.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Creators can configure and run a falling-body scene with static, dynamic, and kinematic interaction in under 10 minutes using editor controls alone.
- **SC-002**: All required body, collider, material, layer, constraint, and character properties survive scene save/reload and prefab apply/revert with 100% parity in the acceptance fixture.
- **SC-003**: A reference stress scene supports at least 10,000 mixed static and dynamic bodies, including at least 1,000 concurrently active dynamic bodies at 60 fixed updates per second, without an engine-side single-thread bottleneck; reference hardware and build configuration are recorded with the result.
- **SC-004**: A batch of at least 10,000 ray queries completes with bounded memory and identical result ordering across 100 repeated runs from the same state.
- **SC-005**: In 100 repeated race tests covering remove/recreate, scene reload, script reload, and shutdown, zero stale commands, results, or events affect a replacement object or world.
- **SC-006**: Every rejected transaction in the rollback suite leaves zero partial scene or physics changes, including invalid collider, stale identity, deleted constraint target, cancellation, and duplicate-idempotency cases.
- **SC-007**: Pause produces zero simulation ticks, and each of 100 consecutive Next requests produces exactly one configured fixed physics tick, one fixed and ordinary simulation callback pass using that tick duration, one publication, one resulting rendered frame, and a return to paused state.
- **SC-008**: The final verification traceability matrix reports passing invariant and tolerance evidence for 100% of required collision, trigger, query, constraint, character, serialization, editor, scripting, automation, export, lifecycle, and failure fixture categories.
- **SC-009**: Editor Play Mode and exported-runtime parity fixtures produce matching invariant outcomes for 100% of covered startup, simulation, query, event, scene-transition, and shutdown cases.
- **SC-010**: Export validation detects 100% of seeded missing, corrupt, empty, and placeholder physics dependencies before the game starts.
- **SC-011**: Repeated scene load/unload and Play Mode enter/exit soak tests complete 1,000 cycles with zero leaked active jobs, late callbacks, stale publications, or authored-scene changes.
- **SC-012**: Physics debug and diagnostic information remains available while paused and all renderer-unavailable lifecycle fixtures complete without physics failure.
- **SC-013**: A clean checkout can follow documented configure, build, test, and install instructions and produce a working installed runtime without globally installed physics dependencies.

## Assumptions

- The initial complete release includes rigid bodies, all listed collider types, materials, filtering, queries, events, constraints, the character foundation, editor authoring and diagnostics, C# access, editor automation, export parity, and hardening.
- Ragdolls, vehicles, soft bodies, buoyancy, and force fields will be separate follow-up features built on the same foundation.
- The initial runtime provides one primary gameplay world while all public concepts retain an explicit world identity for future preview, prefab, server, or multi-scene worlds.
- Windows is the first fully validated platform; provider selection and architecture must support Linux with a credible near-term path, but first-release Linux parity is not an acceptance gate unless planning establishes available CI and ownership.
- Up to 32 collision layers is the initial compatibility baseline; layer identities remain stable across rename, and planning may raise the limit without changing authored references.
- Hardware-dependent gates are finalized during planning after baseline measurements; the design's 10,000-body, 1,000-active-body, 60 Hz, and 10,000-query targets remain the minimum intended stress fixtures.
- Existing scene, reflection, asset, prefab, undo/redo, scheduler, mutation, scripting, automation, profiler, export, and task-system facilities will be extended rather than replaced.
- The physics provider is intentionally undecided until planning completes the required comparison and prototype evidence; no provider source is added during specification.
- Numerical assertions use tolerances and invariants rather than exact floating-point snapshots.

## Dependencies

- Existing engine scheduler and worker task facilities.
- Existing transactional scene mutation, object identity, reflection, serialization, prefab, asset, undo/redo, and Play Mode lifecycle facilities.
- Existing C# host, automation registration, diagnostics/profiler, rendering debug-overlay, and export packaging facilities.
- A physics provider selected and approved during planning, with all required source vendored and pinned inside the repository.

## Scope Boundaries

- The feature does not grant automation control over project creation, opening, or closing.
- The feature does not expose provider-specific runtime objects through gameplay, serialization, scripting, automation, or export contracts.
- Editor-only debug interfaces and automation transport are excluded from exported games unless a later explicit runtime-diagnostics feature enables them.
- Cross-platform bit-identical simulation, deterministic networking lockstep, and full rollback are future work; this release preserves a viable path without claiming those guarantees.
