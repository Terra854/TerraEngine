# Data Model: Complete Physics System

**Date**: 2026-07-13  
**Scope**: Backend-neutral authored data, runtime identities, commands, transactions, snapshots, assets, events, queries, diagnostics, and lifecycle states.

## Identity Rules

All runtime-addressable physics objects use compound engine-owned identity rather than provider identifiers:

- `PhysicsWorldId`: stable logical world ID.
- `worldGeneration`: increments whenever the runtime world is recreated or atomically replaced.
- `objectId`: authored scene UUID.
- `componentGeneration`: generation of the body, collider set, constraint set, or character component.
- `recordId`: stable UUID for an authored collider or constraint child record.
- `recordGeneration`: increments when a child record is removed/recreated or its runtime representation is replaced.
- `slotIndex` and `slotGeneration`: compact runtime registry identity.

Every command, result, wrapper, and event carries enough of this identity to reject reuse. Provider body or constraint IDs exist only inside adapter registry records.

## Authored Entities

### PhysicsSettings

Project-owned versioned settings included in exports.

| Field group | Fields | Validation |
|-------------|--------|------------|
| Schema | `schemaVersion` | Known version or explicit migration |
| World | `gravity`, `fixedTimeStep`, `maxSubsteps`, `maxAccumulatedTime` | Finite values; positive timestep; bounded substeps |
| Solver | velocity/position iterations, stabilization, sleep thresholds | Safe documented ranges |
| Layers | ordered `CollisionLayer` records and broad-phase mapping | Maximum 32 initial layers; unique stable IDs/names |
| Defaults | default material GUID, query limits, event limits | GUID resolves or documented fallback |
| Debug | bounded history lifetimes and default visualization flags | No unbounded history |
| Provider-safe | allowlisted neutral advanced settings | No provider enum or binary layout serialization |

### CollisionLayer

| Field | Meaning |
|-------|---------|
| `id` | Stable 0–31 identity; not changed by rename/reorder |
| `name` | Unique user-facing name |
| `collisionMask` | 32-bit permitted-layer mask |
| `broadPhaseGroup` | Stable project-defined coarse group |
| `enabled` | Whether new bodies may use the layer |

Transitions: create → active → renamed/reconfigured → retired. Deletion requires reference-impact preflight; referenced layers are remapped atomically or deletion is rejected.

### RigidBodyComponent

One optional CRTP component per object.

| Field group | Fields |
|-------------|--------|
| Identity/schema | component schema version |
| Motion | static/dynamic/kinematic type, enabled state |
| Mass | automatic-from-density or explicit mass |
| Damping/gravity | linear/angular damping and `useGravity` |
| Sleep/motion quality | allow sleeping, start awake, `Discrete` or `LinearCast` motion quality |
| Initial state | linear/angular velocity |
| Limits | maximum velocities, allowed degrees of freedom |
| Presentation | interpolation mode |
| Filtering | stable collision layer ID, optional mask override |

Runtime handles, accumulated forces, sleep state, backend IDs, contact caches, and published transforms are never authored fields.

Gravity resolution is deterministic per tick: `useGravity=false` resolves to no built-in gravity; `useGravity=true` resolves to the immutable project world-gravity vector captured for that tick. Custom gravity is transient gameplay input represented by a mass-independent acceleration command rather than authored component state.

### ColliderSetComponent

One optional CRTP component containing an ordered collection of `ColliderRecord` values. An object with a nonempty valid collider set and no rigid body is static geometry.

#### ColliderRecord

| Field group | Fields | Validation |
|-------------|--------|------------|
| Identity | stable `recordId`, record schema version | Unique within component; preserved through ordinary edits |
| Common transform | local center, local rotation | Finite; hierarchy resolved before runtime |
| Type | box, sphere, capsule, cylinder, tapered capsule, convex hull, mesh, compound | Known tagged payload |
| Shape payload | dimensions or collider/cooking asset GUID | Positive finite dimensions; referenced asset valid |
| Material | material GUID or explicit default | Missing reference uses bounded warning and default |
| Interaction | trigger, layer override, event policy, query flags, enabled | Stable layer IDs; supported combinations |
| Debug | optional color override | Finite normalized color |

Compound records contain an ordered list of child recipe references or inline primitive children, each with stable child identity. Recursion and cycles are rejected.

### ConstraintSetComponent

One optional CRTP component containing ordered `ConstraintRecord` values.

#### ConstraintRecord

| Field group | Fields | Validation |
|-------------|--------|------------|
| Identity/type | stable `recordId`; fixed, point, distance, hinge, slider, cone, swing-twist, 6DOF, optional gear | Known type and unique identity |
| Endpoints | connected object UUID or world anchor | Target exists or world anchor chosen; no stale/self-invalid target |
| Frames | local anchors, axes, orientations for both endpoints | Finite and normalizable |
| Behavior | collision policy, limits, springs, motors, targets, max force | Min/max consistent; finite safe ranges |
| Break policy | break force and torque | Nonnegative; infinity means unbreakable |
| State | enabled, authored reset request | Runtime broken state not serialized as authored state |

Cycles are allowed only when every endpoint is valid and the complete batch preflights; invalid or cyclic prefab-reference graphs that cannot be remapped are rejected atomically.

### CharacterControllerComponent

One optional CRTP component per object.

| Field group | Fields |
|-------------|--------|
| Shape | capsule or cylinder; radius, height, center, up direction |
| Traversal | step height, support volume, slope angle, skin/contact padding |
| Interaction | mass, maximum strength, layer, mask |
| Grounding | ground adhesion, stair walking, optional gravity and jump defaults |
| Rotation | authored rotation policy |
| State | enabled and schema version |

Desired velocity and jump are commands. Grounded state, normals, support velocity, and contacts are published runtime state.

### PhysicsMaterialAsset

GUID-addressed project asset.

| Field | Validation |
|-------|------------|
| schema version | Known or migratable |
| static/dynamic friction | Finite and nonnegative |
| restitution | Finite, range documented by policy |
| density | Finite and positive |
| friction/restitution combine modes | Engine-owned enum |
| surface tag | Bounded project string or stable tag ID |

### ColliderRecipe

Canonical backend-neutral input for cooking or runtime primitive creation.

- Shape type and normalized geometry.
- Source asset GUID and source content hash where applicable.
- Import/cook settings and platform profile.
- Scale normalization decision.
- Material slot/submesh selection.
- Stable recipe hash derived from normalized fields, never provider object bytes.

### CookedColliderAsset

Immutable GUID-addressed compiled asset.

| Field group | Fields |
|-------------|--------|
| Identity/version | asset GUID, schema version, cook format version |
| Provenance | source GUID/hash, normalized recipe hash, settings hash |
| Provider context | provider-neutral format family plus selected provider/version used for payload |
| Geometry | bounds, counts, material slots, immutable payload extents |
| Status | valid, stale-last-known-good, invalid/unpublished |
| Diagnostics | bounded warnings/errors and cook timing |

State transitions: requested → validating → cooking → staged → published. Failure before publication becomes failed and preserves prior published version when safe. Cancellation before atomic rename leaves no published partial result.

## Runtime Entities

### PhysicsWorld

One primary gameplay world initially.

| Field group | Fields |
|-------------|--------|
| Identity | logical world ID and monotonic generation |
| Timing | fixed-step accumulator, tick number, dropped/clamped time, pause state, pending explicit-step count, configured step duration, and last completed step request |
| Registries | body, shape, constraint, character, query and asset mappings |
| Queues | incoming commands, prepared batches, pending destruction, query batches |
| Publication | current immutable body/event/query/debug/diagnostic snapshots |
| Provider | private provider world, allocator and job adapter state |

Lifecycle: absent → creating → active/paused → stopping → draining/cancelling → destroyed. A fatal provider error transitions active/paused → failed → stopping. No transition back to active without a new generation.

While paused, one Next request creates one explicit-step token. Consuming the token bypasses normal accumulator catch-up, advances simulation time by exactly the configured fixed timestep, runs one coherent callback/publication/render cycle, records completion, and remains paused. The configured timestep is read at the protected request boundary so the entire step uses one immutable duration.

### PhysicsBodyRecord

- Compound engine identity and provider-private ID.
- Motion type and current runtime generation.
- Associated object UUID and component/collider record generations.
- Shared immutable shape reference.
- Last published body state and dirty flags.
- Pending destruction and active-job reader count.

Transitions: reserved → staged → active ↔ sleeping → pending destruction → destroyed. Any recreation allocates a new generation.

### PhysicsConstraintRecord

- Compound identity and provider-private constraint ID.
- Endpoint body handles and generations.
- Applied impulse/lambda summary used by provider-neutral break policy.
- Active/broken/pending-destruction state.
- One-shot break-event-emitted flag.

Transitions: staged → active → broken or pending destruction → destroyed. Broken remains broken until explicit reset/recreation.

### PhysicsCharacterRecord

- Compound identity and provider-private character state.
- Latest desired movement/jump sequence consumed.
- Current grounded/support/contact state.
- Pending shape resize or destruction.

Transitions: staged → active/disabled → pending destruction → destroyed.

### SharedShapeEntry

- Canonical recipe hash.
- Immutable provider-private shape.
- Reference count and memory estimate.
- Cooked asset GUID/version where applicable.
- Last-use tick and retirement fence.

Identical recipes share entries. Retirement waits for all provider readers/jobs and published snapshots that reference diagnostic metadata.

## Commands and Transactions

### PhysicsCommand

Common envelope:

- Command type.
- Producer ID and monotonically assigned producer sequence.
- Global deterministic merge sequence assigned at enqueue boundary.
- Correlation ID and optional idempotency key/fingerprint.
- World/object/component/record identities and generations.
- Requested effective tick and command payload.
- Cancellation token/job identity where applicable.

Types include create/update/destroy body, collider, constraint, and character; apply continuous force, mass-independent acceleration, impulse, or instantaneous velocity change; set velocity; teleport; kinematic target; character move/jump/resize; settings/layer updates; and query scheduling.

Ordering key: effective tick, protected-boundary global sequence, command-type tie policy, target stable identity. Arrival timing from worker callbacks never defines observable order.

### PhysicsTransaction

- Transaction ID/correlation ID.
- Scene read-set and expected generations.
- Physics read-set and expected world/record generations.
- Ordered scene writes.
- Ordered physics commands.
- Staged provider objects or inverse records.
- Approval/preflight fingerprint for automation-originated high-impact work.
- State: queued, preparing, preflight-failed, prepared, committing, committed, rolled-back, cancelled.

Only committed transactions advance visible generations. Preflight failure or cancellation before commit produces no live changes. Commit failure invokes complete inverses before leaving the protected boundary.

## Query Model

### PhysicsQueryRequest

- Query ID and optional durable job ID.
- World generation and requested tick/snapshot policy.
- Type: ray, shape cast, overlap, point.
- Origin/pose, direction, distance, shape recipe/dimensions as applicable.
- Layer/mask, trigger policy, body-motion filter, include/exclude object sets.
- Maximum hits and stable sort policy.
- Correlation ID, requester, deadline/cancellation.

All numeric inputs must be finite and within project bounds.

### PhysicsQueryHit

- Body/object/collider record identity and generations.
- Position, normal, distance/fraction, subshape key.
- Material GUID/surface tag.
- Trigger and motion-type flags.

### PhysicsQueryResult

- Query ID, world generation, physics tick, correlation ID.
- Completion state: success, rejected, cancelled, stale, expired, failed.
- Stably sorted hits.
- Truncation/overflow flags and diagnostics.
- Creation and expiry ticks.

Results are immutable. Expired handles fail safely rather than returning reused storage.

## Event Model

### PhysicsEvent

Common fields:

- Event type: collision enter/stay/exit, contact, trigger enter/stay/exit, activation, sleep, constraint break.
- World generation and tick.
- Canonically ordered participant identities and generations.
- Collider record/subshape identities.
- Contact points, normals, penetration, relative velocity, and impulse where applicable.
- Material/surface context.
- Correlation/causal command sequence where available.

Ordering key: tick, event-class priority, canonical participant pair, collider/subshape IDs, stable contact index. Thread-local provider callback order is never exposed.

### PhysicsEventPacket

- World generation/tick.
- Immutable ordered event array.
- Counts by type, filtered count, dropped count, overflow flag.
- Subscription-generation snapshot.

Packets have bounded retention. Managed callback exceptions are recorded separately and do not alter packet iteration.

## Published Snapshots

### PhysicsBodyStateSnapshot

Immutable array keyed by stable body handle containing pose, velocities, awake state, mass, center of mass, broad-phase layer, contact count, and tick/generation. Sleeping unchanged bodies may reference prior immutable records without republishing scene transforms.

### PhysicsTransformUpdatePacket

Typed scene mutation input containing object identity/generation, hierarchy-resolved world pose, interpolation context, source world/tick, and expected Transform version. It becomes visible only at the After-Physics flush.

### PhysicsDebugDrawPacket

Bounded immutable primitives for collider shapes, body axes, bounds, contacts, constraints, character support, and recent queries. Contains no provider callbacks or live pointers and is safe when rendering is unavailable.

### PhysicsDiagnosticsSnapshot

- World/tick/accumulator/substep state.
- Body/shape/constraint/character counts.
- Pair/contact/query/event counts and bounded latency summaries.
- Queue depths, rejected/stale/idempotent-duplicate counts.
- Prepare/commit/simulate/extract/publish timing.
- Allocator and provider memory estimates/high-water marks.
- Scheduler phase/resource/visibility declarations and fallback reasons.
- Fatal/degraded/stale-collider state.

## Managed Wrapper Model

Managed wrappers store object UUID, relevant component/record ID, object/component/world generations, and a native binding generation. They never store provider or unvalidated native pointers. Script reload increments binding generation; authored components survive, transient wrappers become invalid and may be reacquired.

## Automation Model

Every response uses the existing automation envelope and includes correlation ID. Physics payloads additionally include world generation/tick, target generation diagnostics, bounded/truncated status, approval impact fingerprint where relevant, and durable job identity for long-running work. Runtime mutators require a nonempty idempotency key and fingerprint.

## Serialization and Migration Rules

- Engine-owned enums serialize stable symbolic values, not provider integers.
- New fields receive versioned defaults.
- Collision layer IDs and asset GUIDs survive rename/move.
- Child record IDs survive edits, prefab application, and duplication where identity should remain; object duplication creates new object/record IDs while remapping intentional internal references.
- Runtime handles, generations derived from runtime allocation, provider IDs, caches, forces, events, and snapshots are excluded.
- Asset type numeric values are append-only and accompanied by manifest/schema versions.
- Unsupported future schemas fail with diagnostics rather than partial load.
