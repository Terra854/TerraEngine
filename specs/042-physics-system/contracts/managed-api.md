# Managed C# API Contract

## Purpose

Provide native/exported-runtime parity for physics authoring references, runtime commands, queries, state, and events without exposing pointers or provider types.

## Wrapper Identity

Every managed physics wrapper stores:

- Object UUID.
- Component type and component generation.
- Collider/constraint record UUID where applicable.
- Physics world generation.
- Native binding generation.

Wrappers never store provider pointers or IDs. Every native call validates all generations. Script reload increments binding generation; authored components survive but prior transient wrappers fail safely and can be reacquired.

## Managed Types

### Component wrappers

- `Rigidbody`
- `ColliderSet` and typed collider record views
- `ConstraintSet` and typed constraint record views
- `CharacterController`

Generic component property access may expose simple authored fields. Enums, layer masks, GUID assets, variant shape records, constraints, queries, and events use dedicated typed DTOs.

`Rigidbody` exposes authored `UseGravity`. Enabled uses project world gravity; disabled suppresses built-in gravity. Custom gravity uses `AddForce(customGravity, ForceMode.Acceleration)` during fixed-step gameplay rather than a serialized per-body vector.

### Stable value types

- `PhysicsWorldId`
- `PhysicsBodyHandle`
- `PhysicsConstraintHandle`
- `PhysicsCharacterHandle`
- `PhysicsQueryHandle`
- Engine-owned motion, interpolation, CCD, combine, shape, constraint, and event enums
- Query filter, request, hit, result, event, body state, character state, and command receipt DTOs

## Rigidbody Commands

Required operations:

- `AddForce`, `AddTorque`, including continuous-force and mass-independent-acceleration modes
- `AddImpulse`, `AddImpulseAtPoint`
- `SetLinearVelocity`, `SetAngularVelocity`
- `MoveKinematic`
- `Teleport`
- `WakeUp`

Every operation enqueues a typed command and returns a receipt containing accepted/rejected state, command ID, correlation ID, and target generation. Acceptance does not imply immediate visibility.

Calls on stale/destroyed wrappers return a typed failure or documented managed exception without native side effects.

## Character Commands

- Set desired velocity/movement for an effective tick.
- Request jump.
- Request validated resize.
- Teleport/reposition through an explicit command.
- Read immutable grounded state, ground normal/velocity, support identity, and contacts.

Acceleration curves and gameplay locomotion policy remain in game code.

## Static Physics Query Surface

Required operations:

- Synchronous snapshot/protected `Raycast`, `ShapeCast`, `Overlap`, and `PointQuery` where permitted.
- Scheduled single query.
- Scheduled batch query returning `PhysicsQueryHandle`.
- Poll/consume/cancel result.

Results include world generation, tick, correlation ID, stable ordered hits, completion state, truncation/overflow, and stale diagnostics. Inputs are bounded by project settings and reject nonfinite or over-limit data.

## Events

Managed event DTOs are immutable copies/views valid for the documented delivery scope. Required callbacks or subscriptions:

- Collision enter/stay/exit.
- Trigger enter/stay/exit.
- Contact packet.
- Activation/sleep.
- Constraint break.

Delivery follows native canonical ordering. If one callback throws, the runtime records the exception with correlation/world/tick/subscriber context and continues delivering remaining callbacks. Exceptions never escape into provider workers.

## State Reads

Reads use immutable published body/character/diagnostic snapshots. They report the source tick and world generation. A read does not lock or traverse the live provider world from arbitrary managed threads.

## Native ABI

Extend the existing TerraEngine C ABI with typed, versioned functions grouped by:

- Handle resolution/validation.
- Body and character command submission.
- Query submission/result copy/cancellation.
- Body/character snapshot copy.
- Event subscription and packet delivery.
- Physics settings/layer read access required by runtime scripts.

ABI rules:

- Fixed-width integers and explicit struct versions/sizes.
- Caller-provided buffers or two-call size/copy pattern for variable arrays.
- No STL, provider, exception, or owning pointer types cross the boundary.
- UTF-8 strings and GUIDs use existing engine conventions.
- NativeAOT export bridge mirrors editor/runtime behavior.
- Unknown ABI struct versions return a version error.

## Thread and Lifecycle Rules

- Managed mutators may be called from allowed scripting callbacks and enqueue only.
- Event callbacks run at the protected publication/delivery boundary, not provider workers.
- Scene unload, component removal, script reload, Play Mode stop, and runtime shutdown invalidate affected wrappers before native resources are released.
- Calls from destroyed scripts or unloaded worlds return safely.
- Exported games expose the same API and observable ordering as editor Play Mode.

## Error Model

Use typed status categories:

- Success/accepted.
- Invalid argument or bounds.
- Invalid state (for example no active world).
- Stale world/object/component/record/handle.
- Queue full or bounded-resource limit.
- Conflict/preflight rejected.
- Cancelled/expired.
- Provider failed/fatal world unavailable.

Status includes a bounded diagnostic message and correlation ID. Provider error codes are translated and never exposed as the stable public contract.
