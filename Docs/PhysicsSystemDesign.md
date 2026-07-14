# TerraEngine Full Physics System Design

## Purpose of This Document

This document is a self-contained design brief for implementing a complete,
production-oriented physics system in TerraEngine. It is intended to be copied
to another development machine and supplied to the same Spec Kit workflow as
the authoritative feature description.

The receiving repository is expected to preserve TerraEngine's existing
architecture:

- C++20 engine and editor code.
- All third-party dependencies vendored inside `External/`.
- Engine components use `Component<Derived>`, provide
  `static constexpr std::string_view kComponentName`, and register reflected
  properties through `Derived::reflect` when appropriate.
- Every engine/editor system derives from `SystemsBase` and declares scheduler
  phase, resource reads/writes, worker-safe preparation, protected work, and
  mutation visibility.
- Worker jobs, C# callbacks, editor actions, MCP calls, and other cross-system
  producers never mutate live `ObjectManager` component storage directly.
  They use `SceneMutationQueue`, typed transactions, or a physics-owned deferred
  command queue and publish at declared protected boundaries.
- Rejected transactional operations are atomic and leave no partial live state.
- Editor and exported games must use the same runtime physics implementation.

This document describes the intended final architecture. Spec Kit may split the
work into user stories and implementation phases, but it should not silently
weaken the safety, runtime parity, or testing requirements.

---

## Product Goal

Add a complete physics stack that supports ordinary gameplay, large scenes,
editor authoring, C# scripts, automation, and exported games without coupling
game code to a third-party physics API.

The system must provide:

- Multithreaded fixed-timestep simulation.
- Static, dynamic, and kinematic rigid bodies.
- Box, sphere, capsule, cylinder, tapered capsule, convex hull, compound, and
  triangle-mesh collision shapes.
- Collision layers, masks, broad-phase layers, materials, triggers, continuous
  collision detection, sleeping, and configurable gravity.
- Constraints and joints.
- Ray casts, shape casts, overlap queries, point queries, and batched queries.
- Collision, contact, activation, sleep, and trigger events.
- A production-ready character controller foundation.
- Optional advanced modules for ragdolls, vehicles, and soft bodies.
- Scene/prefab serialization, reflection, Inspector authoring, undo/redo, Play
  Mode restoration, Scene View debug drawing, diagnostics, C# APIs, MCP APIs,
  and exported-runtime parity.

Physics must remain stable during scene load/unload, Play Mode transitions,
script reload, object deletion, component removal, project close, renderer
failure, and application shutdown.

---

## Physics Backend Selection

The physics library is intentionally not selected in this design. Spec Kit must
research the available libraries and record the decision in the implementation
plan before any third-party source is added.

### Required Evaluation Criteria

Evaluate candidates against:

- License compatibility with TerraEngine and redistribution requirements.
- C++20 toolchain compatibility and clean CMake integration.
- Windows and Linux support, with a credible path to other target platforms.
- Multithreaded rigid-body simulation and integration with TerraEngine's job
  system rather than an unavoidable private thread pool.
- Static, dynamic, and kinematic bodies; sensors/triggers; continuous collision
  detection; sleeping; filtering; materials; and stable scene queries.
- Primitive, convex, compound, and triangle-mesh shapes with offline cooking.
- Constraint/joint breadth and motor, spring, limit, and break support.
- Character controller quality.
- Ragdoll, vehicle, and soft-body support or a viable extension path.
- Large-world support and documented numerical limitations.
- Debug drawing, profiling, memory reporting, and error diagnostics.
- Serialization neutrality: the engine must be able to store its own stable
  formats rather than backend object layouts.
- Determinism characteristics and suitability for a future networking layer.
- Maintenance health, release cadence, documentation, test coverage, and the
  risk of adopting or maintaining a fork.
- Build size, runtime memory, performance, and ease of vendoring all required
  code without system packages.

The plan must include a short comparison matrix, benchmark or prototype evidence
for the leading candidates, the selected version/revision, known limitations,
and the reason the selected backend best fits TerraEngine.

### Dependency Rules After Selection

- Vendor the selected source under a clearly named directory in `External/` and
  build it exclusively from TerraEngine's CMake configuration.
- Pin an exact upstream revision and record it in dependency documentation.
- Disable samples, tests, viewers, and unrelated tools in normal TerraEngine
  builds.
- Do not expose third-party physics headers from TerraEngine's public
  gameplay-facing headers.
- Do not serialize third-party enum values, object layouts, pointers, or binary
  runtime objects.
- Wrap backend allocation, logging, assertions, job execution, and temporary
  allocators behind TerraEngine adapters.
- Keep backend replacement possible through `IPhysicsBackend`, but do not build
  multiple backends in the first implementation unless Spec Kit identifies a
  concrete product requirement.

### Determinism Position

The initial system guarantees deterministic ordering of TerraEngine commands,
events, queries, and fixed-step bookkeeping on the same engine build, platform,
CPU architecture, settings, and starting state. It does not promise cross-CPU
or cross-platform bit-identical floating-point simulation.

Deterministic lockstep networking and full simulation rollback are separate
features. The design must avoid preventing them, but they are not prerequisites
for the first complete physics release.

---

## Coordinate, Unit, and Transform Conventions

- One TerraEngine world unit equals one metre.
- Mass is measured in kilograms.
- Time is measured in seconds.
- Forces use newtons and impulses use newton-seconds.
- Angles exposed to users follow existing TerraEngine editor conventions;
  internal physics calculations use radians.
- Gravity defaults to `(0, -9.81, 0)`.
- The adapter must explicitly convert TerraEngine vectors, quaternions, and
  matrices to backend types. No `reinterpret_cast` between math libraries.
- World-space positions use the backend's large-world position type if
  available. Local shape geometry remains single precision unless profiling
  proves a need for another representation.
- Negative scale is rejected for simulated collider geometry unless the shape
  cooking path explicitly supports and normalizes it.
- Non-uniform scale is supported only for compatible shapes. Unsupported scale
  combinations produce validation errors rather than silently distorted shapes.
- Parent hierarchy transforms are resolved before physics synchronization.
  Dynamic rigid bodies may not inherit arbitrary moving/non-uniformly-scaled
  parents unless a documented local-to-world conversion path is active.

---

## High-Level Architecture

```text
Authored Components / Project Settings / Assets
                     |
                     v
       Immutable Physics Authoring Snapshot
                     |
        worker-safe validation and cooking
                     |
                     v
          Physics Deferred Command Queue
                     |
          protected world commit boundary
                     |
                     v
      Backend-owned PhysicsWorld / Body Registry
                     |
       fixed-step worker simulation and queries
                     |
                     v
         Immutable Results + Event Packets
                     |
          protected publication boundary
                     |
          +----------+-----------+
          |                      |
          v                      v
 Typed transform updates    C#/MCP/diagnostics
 through scene mutation     read immutable snapshots
```

### Ownership Layers

1. **Authoring layer**
   Components and project settings contain backend-independent user intent.

2. **Asset/cooking layer**
   Produces deterministic, validated collider assets and shared immutable shape
   recipes. Cooking may run on worker threads.

3. **Runtime physics layer**
   Owns worlds, bodies, constraints, character controllers, backend resources,
   fixed-step accumulation, and simulation commands.

4. **Publication layer**
   Converts backend results to backend-independent snapshots, events, query
   results, and typed scene mutations.

5. **Integration layer**
   Provides scripting, editor, MCP, serialization, export, diagnostics, and
   debug visualization without exposing backend handles.

---

## Core Runtime Types

### Stable Handles

Use small, generation-validated TerraEngine handles rather than backend body
IDs in public APIs:

```cpp
struct PhysicsBodyHandle
{
	std::uint32_t index = 0;
	std::uint32_t generation = 0;
};

struct PhysicsConstraintHandle
{
	std::uint32_t index = 0;
	std::uint32_t generation = 0;
};

struct PhysicsCharacterHandle
{
	std::uint32_t index = 0;
	std::uint32_t generation = 0;
};
```

Handle records map to object UUIDs, component generations, world generation,
and backend IDs. Every operation validates all generations. Scene reload,
world recreation, component removal, and object deletion invalidate stale
handles deterministically.

### Physics World

`PhysicsWorld` owns:

- Backend world/system instance.
- Body, constraint, character, and shape registries.
- Collision layer tables.
- Temporary allocator and backend job adapter.
- Deferred commands and pending destruction queues.
- Fixed-step accumulator and simulation tick number.
- Contact/event collectors.
- Query batch queues and completed query snapshots.
- Debug draw snapshot.
- Diagnostics and timing counters.
- A monotonically increasing world generation.

The initial engine needs one primary gameplay world. The API should use an
explicit `PhysicsWorldId` so preview worlds, isolated prefab worlds, server
worlds, or future multi-scene simulation can be added without redesigning
every component.

### Backend Interface

`IPhysicsBackend` should expose lifecycle and coarse operations, not mirror
every class from the selected third-party library:

- Create/destroy world.
- Cook or instantiate shapes from TerraEngine recipes.
- Create/update/destroy bodies, constraints, and characters.
- Apply forces, impulses, velocities, and kinematic targets.
- Step simulation.
- Execute immediate protected queries and prepared query batches.
- Extract body state, events, statistics, and debug primitives.

The selected backend implementation provides this interface. Backend callbacks
publish into TerraEngine-owned collectors and never call `ObjectManager`, C#,
ImGui, MCP, or renderer APIs directly.

---

## Component Model

All components below derive from `Component<Derived>` and define a stable
`kComponentName`. Reflected fields must use backend-independent names and types.

### RigidBodyComponent

Authoring fields:

- `motionType`: `Static`, `Dynamic`, or `Kinematic`.
- `massMode`: `AutomaticFromDensity` or `ExplicitMass`.
- `mass`.
- `linearDamping` and `angularDamping`.
- `gravityFactor`.
- `allowSleeping` and `startAwake`.
- `continuousCollisionMode`: `Discrete`, `LinearCast`, or future extended mode.
- `linearVelocity` and `angularVelocity` initial values.
- `maxLinearVelocity` and `maxAngularVelocity`.
- `allowedDegreesOfFreedom` / freeze position and rotation axes.
- `interpolationMode`: `None`, `Interpolate`, or `Extrapolate`.
- `collisionLayer`.
- `collisionMask` override, when enabled.
- `isEnabled`.

Runtime-only state such as body handles, sleep state, backend IDs, accumulated
forces, and contact caches must not be serialized in authored scene JSON.

### Collider Components

Support multiple collider components per object. Each collider has:

- Local centre/position and local rotation.
- Shape-specific dimensions.
- Physics material reference or inline default material.
- `isTrigger` / sensor flag.
- Collision layer override, if enabled.
- Contact event policy.
- Query participation flags.
- Enabled state.
- Debug visualization colour override, optional.

Component types:

- `BoxColliderComponent`
- `SphereColliderComponent`
- `CapsuleColliderComponent`
- `CylinderColliderComponent`
- `TaperedCapsuleColliderComponent`
- `ConvexHullColliderComponent`
- `MeshColliderComponent`
- `CompoundColliderComponent`, when an explicit authored compound root is needed

An object with colliders but no `RigidBodyComponent` behaves as a static body.
An object with a rigid body but no valid collider is rejected from simulation
and reports an actionable diagnostic.

### PhysicsMaterialAsset

Store physics materials as project assets, for example `.tephysmat`, with:

- Static friction.
- Dynamic friction.
- Restitution.
- Density.
- Friction combine mode.
- Restitution combine mode.
- Optional surface tag for gameplay/audio effects.

Materials use asset GUIDs so moves and renames preserve references. Missing
materials use a documented default and emit a bounded warning.

### Constraint Components

Provide:

- `FixedConstraintComponent`
- `PointConstraintComponent`
- `DistanceConstraintComponent`
- `HingeConstraintComponent`
- `SliderConstraintComponent`
- `ConeConstraintComponent`
- `SwingTwistConstraintComponent`
- `SixDOFConstraintComponent`
- `GearConstraintComponent`, if supported by the selected backend version

Common fields:

- Connected object UUID or world anchor.
- Local anchor/axis frames for both bodies.
- Collision between connected bodies.
- Limits, spring settings, motors, target velocity/position, and maximum force.
- Break force and break torque.
- Enabled state.

Broken constraints produce one ordered event and remain broken until explicitly
reset or recreated.

### CharacterControllerComponent

Provide a backend-independent character controller with:

- Capsule or cylinder shape settings.
- Radius, height, centre, and up direction.
- Step height and supporting-volume settings.
- Maximum slope angle.
- Skin/contact padding.
- Mass and maximum strength.
- Ground adhesion and stair walking configuration.
- Collision layer and mask.
- Optional gravity and jump configuration.
- Rotation policy.

The controller exposes desired velocity and jump requests through commands.
Simulation publishes grounded state, ground normal, ground velocity, and
contact information. Gameplay policy such as acceleration curves remains in
game code rather than being hardcoded into the engine component.

### Optional Advanced Components

These belong to later stories within the complete physics program:

- `SoftBodyComponent`
- `VehicleControllerComponent`, wheel/suspension subcomponents, and drivetrain
  settings
- `RagdollComponent` and skeleton-to-body mapping asset
- `BuoyancyVolumeComponent`
- `PhysicsForceFieldComponent`

They must use the same world, command, event, serialization, scripting, editor,
and export contracts rather than separate one-off systems.

---

## Collision Layers and Filtering

Project Settings owns a stable physics layer table:

- Up to a documented maximum, initially 32 object layers.
- Stable numeric ID plus editable display name.
- Symmetric default collision matrix.
- Separate broad-phase category mapping.
- Default query participation.
- Reserved built-in layers only when necessary.

Renaming a layer preserves its ID. Deleting a layer requires reference-impact
analysis and migration to a replacement. Layer table changes rebuild affected
body filter state at a protected boundary.

Filtering order:

1. World/scene compatibility.
2. Object layer pair filter.
3. Per-body mask override.
4. Query filter flags.
5. Ignored object/body set.
6. Optional user callback operating on immutable metadata only.

Callbacks must never invoke arbitrary C# or mutate live scene state from a
backend worker thread.

---

## Shape Cooking and Caching

### Primitive Shapes

Primitive shape recipes are validated and created directly. Identical recipes
share immutable backend shapes through a hash-keyed cache.

### Convex Hulls

- Cook from mesh source positions or an explicit collider mesh asset.
- Weld duplicate points using a documented tolerance.
- Enforce backend point/face limits with clear diagnostics.
- Store source hash, cook settings, bounds, and output version.
- Reject degenerate or effectively planar input unless a documented fallback is
  enabled.

### Triangle Meshes

- Intended primarily for static geometry.
- Dynamic triangle-mesh bodies are rejected by default.
- Cook indexed triangles, material indices, bounds, and acceleration data.
- Remove or report degenerate triangles deterministically.
- Support project-relative cooked output in `Build/Assets` and packaged TEPAK
  content for exports.
- Do not require source model files at runtime.

### Compound Shapes

Multiple collider components on one body become one compound recipe. Child
objects may optionally contribute colliders through an explicit compound-root
policy; implicit hierarchy collection must not surprise authors.

### Cache Identity

Shape cache keys include:

- Source asset GUID and source content hash.
- Collider/cook settings version.
- Scale and local geometry parameters.
- Physics backend cook format version.
- Platform-relevant cook options.

Failed or stale cooks never replace the last known-good published result.

---

## PhysicsSystem and Scheduling

### System Ownership

`PhysicsSystem : public SystemsBase` owns the primary `PhysicsWorld`, adapters,
commands, snapshots, and diagnostics. It is registered by `MainEngine` in both
editor and exported runtime modes.

### New Scheduler Phase

Add a `Physics` phase after the protected flush that makes current-frame scene
and script commands visible, and before render snapshot preparation. The exact
order should be validated against the repository's current phase definitions,
but the intended flow is:

```text
PreFrame -> Input -> Scene -> Scripting -> mutation flush
         -> Physics -> physics publication flush -> UI -> Render -> PostFrame
```

If gameplay requires scripts to consume collision events in the same rendered
frame, event delivery should use a clearly named post-physics callback stage or
the following fixed tick. Do not create an accidental recursive
script-physics-script loop.

### Scheduler Declaration

The system declaration should be equivalent to:

```text
name: PhysicsSystem
phase: Physics
reads:
  - PhysicsAuthoringSnapshot
  - RuntimeLayout
  - TransformSnapshots
  - SceneGraph
  - ProjectPhysicsSettings
writes:
  - PhysicsWorldState
  - PhysicsCommandQueue
  - PhysicsResultSnapshots
  - PhysicsEventQueue
  - PhysicsQueryResults
  - PhysicsDiagnostics
  - SceneMutationQueue
phase kind: PartiallyWorkerSafe / worker preparation plus protected publication
```

Worker-safe work includes snapshot validation, shape cooking, broad simulation
jobs, query batches, state extraction, and event sorting into private buffers.

Protected work includes world creation/destruction, backend registration that
requires exclusive world access, command publication, handle-table mutation,
scene transform mutation submission, and C#/editor-facing event publication.

### Frame Workloads

Declare explicit workloads:

1. `Physics.Snapshot`
   Capture immutable authoring, transforms, component generations, settings,
   commands, and fixed-step input.

2. `Physics.Prepare`
   Validate commands, prepare body/shape changes, cook ready assets, build query
   batches, and calculate the number of fixed substeps.

3. `Physics.CommitCommands`
   Under exclusive world ownership, apply validated create/update/destroy and
   force/impulse/target commands atomically where required.

4. `Physics.Simulate`
   Run zero or more fixed steps through the backend job adapter.

5. `Physics.Extract`
   Build immutable body-state, character-state, event, query, debug-draw, and
   diagnostics packets.

6. `Physics.Publish`
   Validate object/component/world generations, enqueue typed transform changes,
   publish events/query results, and retire destroyed resources.

---

## Fixed-Timestep Policy

Project settings:

- `fixedDeltaTime`, default `1/60` second.
- `maximumSubstepsPerFrame`, default 4.
- `maximumAccumulatedTime`, default `fixedDeltaTime * maximumSubstepsPerFrame`.
- Global gravity.
- Solver velocity and position iteration settings where the backend supports
  stable configuration.
- Sleeping thresholds.
- Deterministic command ordering mode, enabled by default.

Algorithm:

1. Clamp pathological render-frame delta according to project settings.
2. Add to a double-precision accumulator.
3. Determine the bounded number of fixed steps.
4. For each step, consume commands assigned to that tick in stable order.
5. Simulate exactly `fixedDeltaTime`.
6. Retain the remainder for render interpolation.
7. Report dropped/clamped simulation time explicitly.

Pause stops accumulator advancement. Single-step advances exactly one physics
tick. Entering or exiting Play Mode resets transient accumulator state so editor
stall time never becomes a simulation spike.

### Interpolation

Keep previous and current simulated transforms. Rendering reads an immutable
interpolated transform snapshot using `accumulator / fixedDeltaTime`; it does
not write interpolated values back into authored `Transform` components.

Authoritative dynamic transforms are published at the protected physics
boundary. Teleports explicitly reset interpolation history.

---

## Transform Synchronization Rules

### Scene to Physics

- Static body transform edits rebuild or move the backend body at a safe
  boundary.
- Kinematic bodies consume explicit kinematic targets and derive velocity over
  the fixed step.
- Dynamic bodies ignore ordinary authored transform changes while simulating
  unless the request is an explicit teleport.
- Collider or scale changes rebuild the affected shape/body after validation.
- Object activation controls body participation without destroying authored
  component state.

### Physics to Scene

- Dynamic and character results are copied into typed physics result packets.
- Publication verifies object UUID, compact handle, component generation,
  physics body generation, and world generation.
- Valid results enqueue typed `Transform` mutations through the established
  deferred mutation path.
- Stale results are discarded and counted; they must never affect a newly
  created object that reused an entity slot.
- Rejected transform batches are atomic.

### Hierarchy

- Static and kinematic bodies may derive their world pose from the resolved
  hierarchy snapshot.
- Dynamic bodies should normally be root objects.
- Parenting a dynamic body beneath a transformed parent requires an explicit
  supported policy; otherwise validation reports the conflict.
- Reparent operations that affect a simulated body are deferred and processed
  with the related body update at one protected boundary.

---

## Deferred Physics Command Model

All mutations use typed commands such as:

- Create/destroy/rebuild body.
- Enable/disable body.
- Change motion type, layer, material, or collision settings.
- Teleport body.
- Set kinematic target.
- Set linear/angular velocity.
- Add force/torque.
- Add impulse/angular impulse/impulse at point.
- Wake/sleep body.
- Create/update/destroy constraint.
- Move character or request jump.
- Change world settings.

Every command contains:

- Stable command ID and source/correlation ID.
- Target object UUID and expected component/body/world generations.
- Intended physics tick or submission sequence.
- Typed payload.
- Source kind: native, C#, editor, MCP, scene load, or internal lifecycle.
- Optional transaction ID and cancellation token.

Commands from multiple workers are merged in deterministic order. A transaction
that contains dependent physics and scene changes must preflight both domains
before either publishes. A rejected transaction leaves the world and scene
unchanged.

High-frequency native gameplay uses typed commands directly. JSON is reserved
for C#, editor automation/MCP compatibility, tests, and diagnostics.

---

## Collision and Trigger Events

### Event Types

- Contact began.
- Contact persisted/stayed.
- Contact ended.
- Trigger entered.
- Trigger stayed, optionally configurable due to volume.
- Trigger exited.
- Body activated/woke.
- Body deactivated/slept.
- Constraint broken.
- Character contact/ground-state changed.

### Event Record

Each event includes:

- Physics tick and stable event sequence.
- Event type.
- Object UUIDs and validated body handles.
- Collider/subshape identifiers.
- Trigger flags.
- Contact normal and representative point.
- Relative velocity.
- Applied impulse when available.
- Surface/material tags.
- World generation.

Backend callbacks only append minimal records to thread-local collectors.
TerraEngine merges, canonicalizes body ordering, deduplicates where required,
sorts deterministically, validates generations, and publishes immutable event
packets after simulation.

C# callbacks run only on the protected managed boundary. If a callback destroys
an object or changes physics state, that action is deferred to the next allowed
commit boundary.

Event history is bounded. Consumers may poll immutable snapshots by physics
tick, but cannot retain backend pointers.

---

## Scene Queries

### Supported Queries

- Ray cast: closest, any, and all hits.
- Sphere/capsule/box/convex shape cast.
- Sphere/capsule/box/convex overlap.
- Point containment query.
- Broad-phase bounds query.
- Body/collider inspection by stable object identity.

### Query Contract

Input:

- World-space origin/shape/pose and direction.
- Maximum distance.
- Collision layer/mask filter.
- Trigger inclusion policy.
- Back-face policy where meaningful.
- Maximum results and sort mode.
- Ignored object UUIDs.

Hit result:

- Object UUID and body handle.
- Collider/subshape ID.
- Point, normal, fraction, and distance.
- Material/surface tag.
- Triangle/face index when meaningful and safe to expose.
- Physics tick and world generation.

Queries return stable distance-then-identity ordering. Result counts and memory
are bounded.

### Immediate vs Batched Queries

- Main-thread/protected immediate queries are available for rare synchronous
  gameplay needs.
- Worker-safe and C# high-volume use submits query batches against an immutable
  world snapshot or the backend's safe read interface and receives a durable
  result handle.
- MCP queries are rate-limited, bounded, and read-only. They never expose an
  arbitrary callback or unbounded all-hit response.

---

## Character Controller

The controller is a dedicated physics object rather than a dynamic rigid body
with ad hoc forces.

Required behaviour:

- Stable grounding on slopes and moving platforms.
- Step up/down handling.
- Slope rejection above the configured limit.
- Penetration recovery.
- Ground velocity inheritance.
- Trigger and contact participation.
- Crouch/resize preflight that rejects blocked expansion atomically.
- Explicit teleport.
- Deterministic command and event ordering.
- Debug visualization of shape, support plane, ground normal, and contacts.

The engine API exposes `Move`, `Jump`, `SetVelocity`, `Teleport`,
`TryResize`, `IsGrounded`, `GroundNormal`, and `GroundVelocity`. Input mapping,
camera control, and gameplay acceleration policy remain outside PhysicsSystem.

---

## Constraints, Ragdolls, Vehicles, and Soft Bodies

### Constraints

Constraints are part of the main rigid-body world and must be fully supported
in serialization, prefab overrides, scripting, editor tools, diagnostics, and
exports.

### Ragdolls

Use a ragdoll asset that maps skeleton bones to body shapes and constraints.
Runtime pose transfer operates in two directions:

- Animation pose initializes or drives kinematic ragdoll bodies.
- Simulated bodies publish a pose snapshot consumed by animation blending.

Do not mutate skeleton state from physics workers.

### Vehicles

Use a TerraEngine vehicle abstraction for chassis, wheels, suspension,
steering, brakes, engine, transmission, and differentials. Vehicle input is a
typed command per physics tick. Wheel contacts and telemetry are immutable
results.

### Soft Bodies

Soft bodies are an optional advanced module. Their topology/cook data is an
asset; runtime vertex output is published through a bounded snapshot suitable
for renderer integration. Soft-body implementation must not block the rigid
body foundation or force renderer ownership into PhysicsSystem.

---

## C# Scripting API

Expose backend-independent managed types:

- `RigidBody`
- Collider component classes.
- Constraint component classes.
- `CharacterController`
- `PhysicsMaterial`
- `PhysicsScene` / static `Physics` facade for the primary world.
- `RaycastHit`, `ShapeCastHit`, `ContactEvent`, and query filter types.

Representative APIs:

```csharp
body.AddForce(force, ForceMode.Force);
body.AddImpulse(impulse);
body.SetVelocity(linear, angular);
body.Teleport(position, rotation);
body.WakeUp();

bool hit = Physics.Raycast(origin, direction, maxDistance, filter, out RaycastHit result);
PhysicsQueryHandle handle = Physics.ScheduleRaycastBatch(requests);

void OnCollisionEnter(CollisionEvent collision) { }
void OnTriggerExit(TriggerEvent trigger) { }
```

Requirements:

- Managed wrappers store object UUIDs/generation-checked handles, never native
  pointers.
- Mutating APIs enqueue typed commands.
- Synchronous reads use immutable published state or protected query calls.
- Calls from destroyed scripts, stale objects, or unloaded worlds fail safely.
- Script reload preserves authored components but invalidates transient managed
  wrappers.
- Exported games expose the same API and behaviour.
- Exceptions in event callbacks are isolated and reported without corrupting
  the event queue or simulation world.

---

## Editor Integration

### Inspector

Provide beginner-friendly editors for all physics components:

- Units and valid ranges displayed beside fields.
- Motion-type-specific controls.
- Layer/mask selectors.
- Material asset fields.
- Shape edit handles and fit-to-render-mesh actions.
- Validation messages inline.
- Read-only runtime state during Play Mode: awake state, velocity, mass,
  contacts, body handle generation, and broad-phase layer.
- Multi-selection edits where safe.

All edits integrate with undo/redo and prefab overrides. Drag edits may preview
locally, but final physics rebuilds publish at a protected boundary.

### Project Settings

Add a Physics category containing:

- Gravity.
- Fixed timestep and substep limits.
- Collision layer matrix and broad-phase mapping.
- Default physics material.
- Solver/sleep settings.
- Default query limits.
- Debug and diagnostic defaults.
- Advanced backend settings with safe ranges.

Settings serialize into project-owned data and are included in exports.

### Scene View

Add physics debug visualization to the existing Scene View overlay system:

- Collider wireframes and solid translucent shapes.
- Body centre of mass and principal axes.
- Awake/sleep/motion-type colours.
- Broad-phase bounds.
- Contact points, normals, and impulses.
- Constraint frames, limits, and error.
- Character support plane and ground state.
- Query rays/shapes and hits for a bounded lifetime.
- Optional soft-body and vehicle diagnostics.

Debug drawing consumes immutable physics debug packets. It must never call the
backend while renderer work is recording and must remain safe when rendering is
unavailable.

### Physics Diagnostics Window

Create an opt-in editor window, declaring its own location under the `Window`
menu, with:

- World generation and simulation tick.
- Accumulator, substeps, clamped/dropped time.
- Active/static/dynamic/kinematic/sleeping body counts.
- Shape and cooked-asset cache statistics.
- Constraint, character, vehicle, soft-body counts.
- Broad-phase pair and narrow-phase contact counts.
- Query batch counts and latency.
- Event counts and dropped/filtered events.
- Stale handle/result rejection counters.
- Command queue depth and failed preflight reasons.
- Timing breakdown for prepare, commit, simulate, extract, and publish.
- Backend memory usage and temporary allocator high-water marks.

The editor Profiler should receive physics CPU scopes and counters without
requiring the dedicated diagnostics window to be open.

### Play Mode

- Entering Play Mode builds a fresh runtime physics world from the captured
  scene state.
- Runtime body transforms and transient physics state do not dirty the authored
  scene.
- Exiting Play Mode destroys the runtime world before restoring the captured
  scene snapshot.
- Pause freezes fixed-step advancement.
- Single-step advances exactly one fixed physics tick and publishes results.
- Script reload and Play Mode stop cannot race physics jobs; shutdown joins or
  cancels them at a declared boundary.

---

## MCP and Automation Surface

MCP does not create, open, or close projects. Physics tools operate only inside
the native-editor-owned active project/session.

Recommended capabilities:

### Read-only

- `physics.state`
- `physics.settings.get`
- `physics.layers.list`
- `physics.body.inspect`
- `physics.contacts.list`
- `physics.diagnostics.get`
- `physics.raycast`
- `physics.shape_cast`
- `physics.overlap`
- `physics.query_job.get`

### Project/scene mutations

- `physics.settings.set`
- `physics.layer.create`
- `physics.layer.update`
- `physics.material.create`
- `physics.material.update`
- `physics.body.apply_force`
- `physics.body.apply_impulse`
- `physics.body.teleport`
- `physics.body.set_velocity`
- `physics.character.move`
- `physics.character.jump`

Component creation and property editing should primarily use existing generic
component/reflection automation after physics components self-register their
declarations. Do not add a central MCP name dispatch chain.

Safety requirements:

- Layer deletion, bulk body changes, global physics setting changes, and
  material deletion require reference-impact preflight and appropriate approval.
- Runtime force/impulse commands require Play Mode and target generation checks.
- Query dimensions, distances, result counts, and batch sizes are bounded.
- Long-running cook and batch operations return durable jobs.
- Idempotency keys prevent duplicate impulses or duplicate asset publication.
- Results include physics tick, world generation, correlation ID, and stale
  target diagnostics.

---

## Serialization, Prefabs, and Assets

- All authored component fields serialize through existing reflection paths.
- Runtime handles and backend state are excluded.
- Missing or obsolete fields load with versioned defaults.
- Component schema migrations are explicit and tested.
- Prefab defaults and instance overrides work for rigid bodies, colliders,
  constraints, and character controllers.
- Connected-object references use UUIDs and survive save/reload, prefab
  instantiate, duplicate, asset move, and export.
- Cyclic or invalid constraint/prefab references fail atomically.
- Physics material and cooked collider assets use GUID-based references.
- Scene duplication and object duplication generate correct new identities while
  retaining intentional internal references.

---

## Exported Runtime

The export pipeline must:

- Include the same native PhysicsSystem and vendored backend used by the editor.
- Package project physics settings, layer tables, material assets, and cooked
  collider payloads.
- Validate startup scenes for missing/invalid physics dependencies.
- Reject placeholder or empty cooked assets.
- Avoid source model dependencies at runtime.
- Support headless export and CI.
- Produce evidence describing backend version, cook format version, physics
  settings, included assets, and validation results.

Exported games must match editor Play Mode for:

- Body creation and initial state.
- Fixed-step policy.
- Collision filtering.
- Queries and stable result ordering.
- Events and C# callbacks.
- Constraints and characters.
- Scene transitions and shutdown.

Editor-only debug UI and MCP transport are excluded unless a future explicit
runtime diagnostics feature enables them.

---

## Lifecycle and Failure Safety

### Scene Load/Unload

- Scene activation creates a new world generation or atomically replaces world
  contents.
- Old jobs/results/events cannot publish into the new scene.
- Body and shape destruction waits for backend readers/jobs to finish.

### Object and Component Removal

- Removal enqueues body/constraint/character destruction.
- Pending forces, queries, events, and results for the old generation are
  rejected.
- Entity slot reuse cannot redirect a stale physics result.

### Shutdown

Order:

1. Stop accepting commands and queries.
2. Cancel or drain queued work according to documented policy.
3. Join backend jobs.
4. Stop event delivery.
5. Destroy characters, constraints, bodies, shapes, worlds, job adapter, and
   allocators.
6. Clear handles and advance generation.

No physics callback may outlive the world or engine.

### Failure Handling

- Invalid authored data disables only affected bodies when possible and reports
  diagnostics.
- Out-of-memory or backend-fatal errors stop physics simulation cleanly and
  produce a controlled critical report rather than continuing corrupted state.
- Physics failure must not call Vulkan or depend on renderer availability.
- A failed collider recook retains the last known-good published collider when
  safe and clearly labels the stale result.

---

## Performance and Memory Requirements

Target gates should be finalized by Spec Kit, but the initial design expects:

- At least 10,000 mixed static/dynamic bodies in stress tests.
- At least 1,000 concurrently active dynamic bodies at 60 Hz on the reference
  development machine without a single-threaded engine bottleneck.
- At least 10,000 batched ray queries with bounded memory and stable ordering.
- No per-body heap allocation in the normal per-step hot path.
- Thread-local event and command buffers merged once per boundary.
- Shared immutable shape instances for identical collider recipes.
- Bounded event, query, debug-draw, and diagnostics histories.
- No recurring physics jobs when no world is active.
- Sleeping bodies avoid unnecessary transform publication.
- Dirty/generation tracking prevents full-world body rebuilds after localized
  component edits.

Collect measurements before setting pass/fail numbers that depend on hardware.
Record the reference hardware and build type with every performance result.

---

## Testing Strategy

### Unit Tests

- Unit and coordinate conversion.
- Handle allocation, generation, and stale rejection.
- Shape validation and recipe hashing.
- Collision layer/mask filtering.
- Material combine modes.
- Fixed-step accumulator, clamping, pause, and single-step.
- Deterministic command ordering.
- Event canonicalization and stable sorting.
- Query filtering and hit ordering.
- Component reflection and serialization defaults.

### Scheduler and Concurrency Tests

- PhysicsSystem scheduler declaration and registered resources.
- Snapshot/prepare/commit/simulate/extract/publish workload declarations.
- Worker jobs never mutate `ObjectManager` directly.
- Physics transform results become visible only at the declared flush boundary.
- Same-frame visibility only where explicitly required.
- Concurrent command producers merge deterministically.
- Remove/recreate/reload races reject stale commands and results.
- Shutdown, scene unload, script reload, and Play Mode stop wait for or cancel
  active work safely.
- Thread Sanitizer coverage on supported platforms where practical.

### Transaction Tests

- Mixed scene and physics transaction success.
- Invalid collider causes complete rollback.
- Stale body or object generation causes complete rollback.
- Constraint referencing a deleted body causes complete rollback.
- Cancellation before commit leaves no partial world or scene state.
- Duplicate idempotency key does not apply an impulse twice.
- Visibility timing is proven before and after the physics publication flush.

### Simulation Tests

- Falling body and gravity.
- Resting stack stability.
- Friction and restitution.
- Static/dynamic/kinematic interaction.
- CCD prevents representative tunnelling.
- Sleeping/waking.
- Trigger enter/stay/exit.
- Constraint limits, motors, springs, and break thresholds.
- Moving platforms and character grounding.
- Character slopes, stairs, jump, resize, and penetration recovery.
- Large mesh collider and compound collider correctness.

Tests should use tolerances and invariant checks rather than fragile exact
floating-point snapshots.

### Serialization and Editor Tests

- Save/reload parity for every physics component.
- Prefab default/override/apply/revert.
- Undo/redo for component add/remove and property changes.
- Layer rename retains IDs and references.
- Material/collider asset move retains GUID references.
- Play Mode exit restores exact authored state.
- Debug visualization remains available when simulation is paused and safe when
  renderer output is unavailable.

### C# and MCP Tests

- Managed force, impulse, teleport, velocity, queries, events, and stale wrapper
  handling.
- Script exception isolation during contact callbacks.
- MCP discovery/handler parity and absence of arbitrary command/filesystem APIs.
- Approval and denial paths.
- Query/result limits.
- Idempotent impulse and durable cooking/query jobs.
- Correlated structured logs and diagnostics.

### Export Tests

- Headless export packages all required physics data.
- Exported runtime loads and simulates a physics fixture.
- Editor Play Mode and export produce matching invariant results.
- Missing/corrupt cooked collider assets fail validation clearly.
- Export shutdown leaves no active jobs or callbacks.

### Stress and Soak Tests

- 10,000-body mixed scene.
- Repeated scene load/unload.
- Repeated Play Mode enter/exit.
- Randomized object/component create/remove while queries are in flight.
- Long-running stacking/contact simulation.
- High-volume trigger and query traffic with bounded histories.
- Character controller against moving/rotating geometry.
- Constraint chains and ragdoll stress.

---

## Suggested Implementation Phases

### Phase 1: Backend Research and Foundation

- Compare viable libraries against the evaluation criteria, prototype the
  highest-risk integrations, document the decision, and vendor the selected
  pinned source.
- Add CMake configuration and TerraEngine adapters for allocation, jobs, logging,
  assertions, math conversion, and backend lifecycle.
- Add stable handles, world generation, and a minimal PhysicsWorld.

### Phase 2: Components, Settings, and Serialization

- Rigid body, primitive colliders, materials, project settings, layers, and
  reflection.
- Scene and prefab round trips.

### Phase 3: Fixed-Step Runtime and Scheduler Integration

- PhysicsSystem, workloads, command queue, body registry, simulation, result
  snapshots, transform synchronization, interpolation, pause, and single-step.

### Phase 4: Contacts, Triggers, and Queries

- Event collectors and publication.
- Ray/shape/overlap query APIs and batched results.
- Native and managed consumers.

### Phase 5: Cooked Geometry and Asset Pipeline

- Convex, triangle mesh, compound shapes, caching, asset GUIDs, cook reports,
  headless cooking, and export packaging.

### Phase 6: Constraints and Character Controller

- Full constraint family, motors/limits/breaking.
- Production character behaviour and tests.

### Phase 7: Editor and Diagnostics

- Inspector, Project Settings, Scene View handles/debug drawing, diagnostics
  window, profiler counters, undo/redo, and Play Mode lifecycle.

### Phase 8: C# and MCP Completeness

- Managed APIs/events/queries.
- Self-registering physics automation capabilities, approvals, durable jobs,
  logs, and diagnostics.

### Phase 9: Export Parity and Hardening

- Installed editor/exported runtime validation.
- Stress, concurrency, lifecycle, performance, and failure-containment fixes.

### Phase 10: Advanced Modules

- Ragdolls and animation integration.
- Vehicles.
- Soft bodies.
- Optional buoyancy and force fields.

Each phase must leave the build compiling and installable. Story checkpoints
should be independently demonstrable rather than landing a large untestable
backend integration at the end.

---

## Explicit Non-Goals for the Initial Complete Release

These are compatible future features, not hidden requirements for the first
usable production physics release:

- Cross-platform bit-identical deterministic lockstep.
- Network prediction and full physics rollback.
- Destructible fracture generation.
- Fluid simulation.
- GPU rigid-body simulation.
- Runtime backend switching.
- Author-controlled arbitrary code inside backend worker callbacks.

Ragdolls, vehicles, and soft bodies may be delivered after the rigid body,
constraint, character, editor, scripting, MCP, and export foundation is stable.

---

## Acceptance Criteria

The feature is complete when:

1. A user can author rigid bodies, all required colliders, materials, layers,
   constraints, and characters in the editor with undo/redo and prefab support.
2. Play Mode runs a multithreaded fixed-step world with stable body, trigger,
   event, query, constraint, and character behaviour.
3. Physics jobs do not mutate live scene/component storage, and all declarations
   and visibility boundaries pass scheduler validation.
4. Stale handles, scene reloads, object reuse, component removal, and shutdown
   cannot publish results into the wrong object or world.
5. Native and C# gameplay can issue commands, run queries, and receive events.
6. MCP can inspect and safely operate physics inside an already-open project,
   with correct bounds, approvals, idempotency, and diagnostics.
7. The Scene View and diagnostics surfaces expose colliders, contacts,
   constraints, characters, timing, memory, queue, and rejection information.
8. Exported games use the same runtime system and pass editor/export parity
   fixtures without source assets or system-installed dependencies.
9. All unit, scheduler, transaction, concurrency, serialization, scripting,
   editor, MCP, export, stress, and lifecycle tests pass.
10. The final codebase compiles and the CMake install target succeeds.

---

## Spec Kit Handoff Prompt

Copy this document into the receiving TerraEngine workspace, then invoke the
Spec Kit specification workflow with a prompt equivalent to:

> Implement the complete TerraEngine physics system described in
> `Docs/PhysicsSystemDesign.md`. Preserve every architectural, scheduler,
> transactional mutation, component, scripting, editor, MCP, export, testing,
> dependency, and lifecycle requirement in that document. Research and select
> the physics backend during planning, then keep it behind TerraEngine-owned
> abstractions. Project creation, opening, and closing must remain
> native-editor-only. Split delivery into
> independently testable user stories without weakening the final acceptance
> criteria.

Recommended workflow:

1. Run `speckit-specify` using this document as the feature source.
2. Run `speckit-clarify` for unresolved product decisions such as the initial
   advanced-module scope and target performance hardware.
3. Run `speckit-plan` and require explicit scheduler/resource/visibility
   declarations, a backend comparison/selection record, and dependency details.
4. Run `speckit-tasks`, then `speckit-analyze` before implementation.
5. Run `speckit-implement` in phases, compiling and testing after every story.
6. Perform installed-editor and exported-runtime live validation before marking
   the final tasks complete.

### Clarifications Worth Asking During Spec Kit

- Whether ragdolls, vehicles, and soft bodies must ship in the same feature or
  may be follow-up specifications after the shared foundation lands.
- Which physics backend best satisfies the documented evaluation criteria and
  what prototype evidence is needed before committing to it.
- Reference hardware and performance budgets.
- Whether Linux support is required in the first physics release or immediately
  after Windows parity.
- Whether cross-platform deterministic networking is planned soon enough to
  impose stricter numerical constraints now.
- Whether the existing animation system is ready for ragdoll pose integration.
- Desired maximum collision layer count and compatibility/migration policy.

Unless clarified otherwise, the safe default is to ship the complete rigid
body, collider, material, filtering, query, event, constraint, character,
editor, C#, MCP, export, and hardening scope first, then deliver advanced
ragdoll/vehicle/soft-body modules on the same architecture.
