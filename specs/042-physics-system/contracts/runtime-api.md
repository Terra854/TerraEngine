# Runtime API Contract

## Purpose

Define the backend-neutral native physics surface, scheduling boundaries, identities, commands, queries, events, and lifecycle. Provider types and headers are private to `Engine/Physics/Backend`.

## Compatibility Rules

- Public handles contain only engine-owned world, slot, and generation values.
- Public enums have stable symbolic meanings independent of provider values.
- Every asynchronous result is stamped with world generation and physics tick.
- Any stale world, object, component, record, or slot generation returns a typed stale-target result and performs no action.
- Public operations do not return raw pointers or references into provider storage.

## World Service

The runtime service supports:

- Create and destroy a world with explicit `PhysicsWorldId`.
- Build or atomically replace the primary gameplay world from an immutable authoring snapshot.
- Configure fixed timestep, substep cap, accumulator clamp, gravity, solver/sleep settings, layers, and bounds.
- Pause without advancing. A Next request advances exactly one configured fixed physics tick, runs fixed and ordinary simulation callbacks once with that explicit duration, publishes once, permits one resulting render, and remains paused.
- Stop acceptance, cancel/drain by policy, join jobs, stop events, destroy runtime records, and invalidate the generation.

The initial product exposes one primary world but requires world ID on every contract.

## Handle Contracts

### PhysicsBodyHandle

- `worldId`
- `worldGeneration`
- `slotIndex`
- `slotGeneration`

### PhysicsConstraintHandle / PhysicsCharacterHandle

Use the same shape. Handle lookup additionally validates the associated authored object/component/record generations stored in the registry.

### PhysicsQueryHandle

- World ID/generation.
- Result slot/generation.
- Submitted tick and expiry policy.

An invalid handle is distinguishable from a valid handle whose target has been destroyed.

## Command Contract

All mutating operations enqueue a typed `PhysicsCommand`; callers do not directly mutate the active provider world.

Common command metadata:

- World and target identities/generations.
- Producer and deterministic sequence.
- Correlation ID.
- Optional idempotency key and request fingerprint.
- Effective-tick policy.

Required command families:

- Body create, rebuild, destroy, enable, teleport, kinematic target.
- Continuous force/torque, mass-independent acceleration, impulse/point impulse, instantaneous velocity change, explicit velocity, and wake/sleep policy.
- Collider set rebuild and material/filter update.
- Constraint create, update, reset, destroy.
- Character create, move, jump, resize, update, destroy.
- Query schedule/cancel.
- World settings/layer update and world replacement.

Submission returns accepted/rejected plus command ID. Accepted means queued, not yet visible. Completion and rejection diagnostics appear in the published command-result snapshot.

The initial motion-quality contract exposes `Discrete` and `LinearCast` only. Provider-specific motion-quality values never cross the adapter boundary. `LinearCast` must pass the fast-body versus thin-static-geometry tunnelling fixtures before publication.

## Query Contract

Supported query kinds:

- Ray cast: closest, any, or all hits.
- Shape cast: supported primitive or validated shape recipe.
- Overlap: supported shape against world.
- Point query.
- Prepared batch containing any bounded combination of the above.

Filters:

- Stable collision layer/mask.
- Trigger include/exclude/only.
- Static/dynamic/kinematic selection.
- Object/body include and exclude sets.
- Optional gameplay surface/material predicates represented by stable data, not callbacks into user code from provider workers.

Result ordering:

1. Ascending distance/fraction.
2. Stable object UUID.
3. Collider record UUID/subshape key.
4. Stable contact/hit index.

All dimensions, origins, directions, distances, result limits, and batch counts are validated against finite values and `PhysicsSettings` bounds. Over-budget input is rejected. A response may be explicitly marked truncated only when the caller requested a bounded all-hits result.

The provider-selection spike calibrates maximum cast distance, shape extent, hits per query, requests per batch, and retained-result ticks. Those accepted values are recorded in backend-selection evidence and pinned by PhysicsSettings tests before query APIs ship; the scheduled batch limit must support the specified 10,000-request acceptance fixture.

Immediate protected queries execute only at an approved protected boundary. Normal gameplay and automation use the immutable published snapshot or scheduled batches.

## Event Contract

Event types:

- Collision enter/stay/exit.
- Contact point/manifold summary.
- Trigger enter/stay/exit.
- Activation and sleep.
- Constraint break.

Event participants are canonicalized so the lower stable identity is participant A. Events are sorted after thread-local collection; provider callback order is unobservable. Each event includes tick, world generation, participant and collider identities/generations, and relevant contact/material data.

Subscriptions are snapshotted before delivery. Unsubscription or destruction during delivery affects the next packet. Consumer exceptions are isolated and logged; they do not interrupt remaining delivery.

Bounded histories expose explicit dropped/filtered counts and overflow diagnostics.

## Body State and Transform Publication

After simulation, the runtime publishes an immutable `PhysicsBodyStateSnapshot`. Dynamic body pose changes are converted to typed `PhysicsTransformUpdatePacket` scene transactions. The expected Transform version and target generation are validated at the protected After-Physics boundary.

- Worker code never writes Transform or ObjectManager storage.
- Failed transform transaction publication does not expose a partially updated group.
- Sleeping unchanged bodies do not generate redundant transform mutations.
- Interpolation data is a presentation snapshot; it does not overwrite authored state.

### Gravity resolution

At the immutable authoring snapshot boundary, every active body resolves one built-in gravity state:

- Disabled: `useGravity=false`; no built-in gravity acceleration.
- World: `useGravity=true`; use the captured project world-gravity vector.

Resolution and application occur once per fixed tick using the same settings snapshot, so a concurrent editor/settings change becomes visible only at the next declared boundary. Custom gravity uses the same deterministic command ordering as other gameplay input and selects mass-independent acceleration mode.

## Explicit Paused Step Contract

When Play Mode is paused, the Next command:

1. Captures the current configured fixed timestep at the protected request boundary.
2. Creates one explicit-step token without adding wall-clock time to the accumulator.
3. Commits commands eligible for that tick.
4. Runs exactly one fixed physics update using the captured duration.
5. Runs fixed-step and ordinary simulation callbacks once using the same explicit duration and coherent tick context.
6. Extracts and publishes transforms, events, queries, debug data, and diagnostics once.
7. Renders one frame from the published state.
8. Clears the token and remains paused.

Residual accumulator time cannot cause an extra tick during this operation. Requests are serialized; if future UI supports queued N-step operation, each token still represents exactly one fixed tick. A hard-coded millisecond duration and live editor frame delta are prohibited for Next.

## Coordinated Transaction Contract

A mixed transaction may contain scene writes and physics commands. It follows:

1. Validate scene read set and all target generations.
2. Validate physics world/read set and all commands.
3. Reserve capacity and build staged provider objects or inverse records.
4. Revalidate both domains under the protected commit boundary.
5. Apply provider staging/invertible changes.
6. Commit scene writes.
7. Roll back provider changes if scene commit fails.
8. Publish both generation changes only after success.

Any preflight, conflict, cancellation, allocation, or provider failure returns a structured failure and leaves no partial live state. No event or query result from an uncommitted generation is published.

## Scheduler Declaration Contract

`PhysicsSystem` inherits `SystemsBase` and declares:

| Workload | Kind | Reads | Writes | Boundary |
|----------|------|-------|--------|----------|
| PhysicsSnapshot | protected snapshot | flushed scene, hierarchy, physics components/settings, command ranges | immutable authoring snapshot | after Scripting flush |
| PhysicsPrepare | worker-safe | authoring snapshot, published runtime snapshot | prepared commands/recipes/query batches | none |
| PhysicsCommandCommit | protected commit | prepared work, world registry | staged/active physics world | hard boundary |
| PhysicsSimulate | worker-safe orchestration | active world/settings | provider state and thread-local collectors | provider barrier |
| PhysicsExtract | worker-safe | provider state/collectors | immutable unsorted packets | none |
| PhysicsPublish | protected commit | packets/subscriptions/limits | sorted snapshots and scene transactions | hard boundary |
| PhysicsPublicationBarrier | barrier | pending scene transactions/snapshots | visible scene and snapshot generation | After Physics flush |

Diagnostics expose exact resource names, worker/protected time, barrier/fallback reasons, queue depth, and mutation visibility.

## Backend Adapter Contract

`IPhysicsBackend` exposes coarse operations only:

- Provider lifecycle and world creation/destruction.
- Shape creation/cooking/loading from engine recipes.
- Body/constraint/character staged creation, update, and destruction.
- Typed force/motion commands.
- Fixed-step simulation with engine job and temporary allocator adapters.
- Protected and prepared queries.
- Extraction of engine-neutral body state, event collector data, statistics, and debug primitives.
- Staging/inverse support needed by coordinated transactions.

Adapters must:

- Explicitly convert vectors, quaternions, matrices, units, and angles.
- Route jobs only to TerraEngine's job facilities.
- Route allocation, assertions, errors, profiles, and logs through TerraEngine adapters.
- Never invoke ObjectManager, managed code, editor UI, automation, or renderer APIs from provider callbacks.

## Lifecycle and Failure Contract

Shutdown order:

1. Stop accepting commands and queries.
2. Cancel or drain queued work according to recorded policy.
3. Join provider and engine physics jobs.
4. Stop event delivery and invalidate outstanding results.
5. Destroy characters, constraints, bodies, shapes, world, job adapter, and allocators.
6. Clear registries and advance world generation.

Scene unload, Play Mode stop, script reload, component removal, and fatal provider failure use the same state machine. No callback may outlive its world. Renderer availability is irrelevant to physics cleanup.
