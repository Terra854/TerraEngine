# Physics constraints and characters

This page describes the provider-neutral gameplay contract for the US4 physics
slice. Include `Engine/Physics/PhysicsTypes.h`, `PhysicsCommands.h`, and
`PhysicsWorld.h`; do not include Jolt headers. Jolt Physics 5.6.0 (the pinned
revision documented in `Docs/Dependencies/JoltPhysics.md`) is an implementation
detail of the backend.

## Constraints

`ConstraintSetComponent` is an ordered collection of stable `ConstraintRecord`
values. A record can connect two authored objects, one object to the immutable
world frame, or (for a gear constraint) two authored hinge records. The
supported kinds are `Fixed`, `Point`, `Distance`, `Hinge`, `Slider`, `Cone`,
`SwingTwist`, `SixDegreesOfFreedom`, and `Gear` (gear requires two live hinge
records). Endpoints use UUIDs, never provider body IDs. A missing, self-
referential, cyclic, or stale endpoint is rejected before the world rebuild.

Frames are local `PhysicsPose` values (`position` in metres and `rotation` as a
quaternion) relative to each endpoint. `firstAxis` and `secondAxis` are finite,
non-zero directions and are normalized during preparation. A `WorldAnchor`
endpoint uses the world frame; its object UUID is ignored. For a SixDOF record,
the six entries are ordered translation X/Y/Z, then rotation X/Y/Z. Other kinds
consume the applicable scalar entry (distance/slider use the first linear axis;
angular joints use the first angular axis).

Each limit has `enabled`, `minimum`, and `maximum`; disabled means unconstrained.
Linear limits are metres; angular limits use the engine's existing angle
convention (the backend packet does not reinterpret the numeric value). Limits
must be finite and `minimum <= maximum`; editor controls should label the
selected joint axis so a gameplay caller does not confuse metres and angles.

Springs have `enabled`, `stiffness`, and `damping`. TerraEngine marks the backend
packet as stiffness/damping mode; the provider must not reinterpret authored
stiffness as a frequency. Non-negative finite values are required. Motors are
`Disabled`, `Position`, or `Velocity`
with a target position/velocity and non-negative `maximumForce`. Motor position
uses the same linear/angular units as its axis. For SixDOF, use the per-axis
`axisSprings` and `axisMotors`; the scalar fields are used by the simpler joint
families.

`breakForce` and `breakTorque` are force (N) and torque (N-m) thresholds.
Defaults are positive infinity, so the constraint is not breakable. Solver
impulses are converted using the fixed tick duration. Crossing either finite
threshold publishes one ordered constraint-break event and disables the joint.
Broken state is runtime-only and remains broken until the record is recreated or
`PhysicsCommand::MakeResetConstraint` is submitted with a strictly newer,
non-zero `resetGeneration`. Increment the authored record's reset token when an
explicit reset is desired; never write transient broken state into the scene.

`collideConnected` is retained in the engine contract and serialized, but Jolt
5.6 constraints do not own collision filtering. In this implementation the
connected pair therefore still follows the bodies' layer/mask policy; setting
`collideConnected` cannot override that policy. This is a known provider
limitation, and changing it would require a future pair-filter design rather
than mutating a body's shared collision group.

## Characters

`CharacterControllerComponent` stores durable configuration for one virtual
character. The supported shapes are `Capsule` and `Cylinder`. Radius and height
are metres, with `height >= 2 * radius`; `localCenter` and `upDirection` are in
character space. Defaults are radius 0.5 m, height 2 m, up `(0,1,0)`, step
height 0.4 m, support radius 0.5 m, support height 0.2 m, maximum slope 50 degrees,
contact padding 0.02 m, mass 75 kg, maximum strength 1000 N, gravity scale 1,
jump speed 5 m/s, and stair walking enabled. The project gravity default is
`(0,-9.81,0)`; `useGravity=false` disables built-in character gravity. Layer and
mask filtering use the same 32-layer project policy as rigid bodies.

The Jolt adapter maps the capsule/cylinder to `CharacterVirtual`. Jolt 5.6
models its supporting volume as a plane, not a finite cylinder: the authored
support radius is preserved in the engine record, while support height becomes
the plane offset. Gameplay should consume the published support packet rather
than assume a provider-specific finite-volume test.

Movement and state are deliberately split:

* `MakeCharacterMove` submits desired velocity (m/s).
* `MakeCharacterJump` submits a jump speed (m/s); it is applied only at the
  deferred command boundary.
* `MakeCharacterResize` requests a new height. The request is rejected when it
  would be shorter than the diameter or when the provider cannot recover safely.
* `MakeCharacterTeleport` requests a pose change; it refreshes contacts before
  the next movement update.
* `MakeCharacterConfigurationUpdate` rereads durable component settings at the
  protected boundary.

`groundAdhesion` controls stick-to-floor distance. When stair walking is
enabled, `stepHeight` is the maximum step-up distance; a slope steeper than
`maximumSlopeAngleDegrees` is reported as steep/not supported. The backend
performs penetration recovery during resize and movement (the update packet's
`recoverPenetration` flag is true for normal commands). A teleport should be
used for authoritative repositioning, not to push through geometry every tick.
Characters inherit moving-platform velocity from their current support body,
including rotating-platform support as represented by the published velocity.
Gameplay remains responsible for camera-relative input, acceleration/air control,
stamina or jump permissions, and choosing when to issue commands.

After each completed fixed tick, `PhysicsWorld` publishes an immutable
`PhysicsCharacterStatePacket`. Each state contains the generation-safe character
handle and object/component generations, pose, linear velocity, `groundState`
(`OnGround`, `OnSteepGround`, `NotSupported`, or `InAir`), ground position and
normal, support velocity/body, contact count, and `supported`. Read this packet
for gameplay and presentation; do not inspect provider objects or mutate scene
components from a worker callback.

## Commands, generations, and visibility

Build command targets from the current `PhysicsWorldHandle`, authored UUID, and
object/component generations. Runtime constraint and character handles are
copied into deferred commands but are never serialized. Submit through
`PhysicsSystem::SubmitCommand`; acceptance means bounded-queue admission, not
immediate visibility. Commands are merged in deterministic producer/sequence
order at `PhysicsCommandCommit`, validated against world/object/component/
record generations, and applied before the fixed simulation tick. Stale,
cancelled, conflicting, or invalid commands have no partial effect.

Worker preparation and provider callbacks only produce typed packets. Character
states, break events, and transforms become observable at the protected
publication boundary (and scene transforms at the declared transactional flush).
Packets are immutable and tick-scoped, so retain a shared packet only for its
documented expiry. A recreated world or reused slot changes generation and
invalidates all old commands, handles, and packets.

## Diagnostics

Use `PhysicsSystem::LatestDiagnostics()` for the immutable per-publication
snapshot. It includes body, constraint, and character counts; tick/world state;
command and publication queue depth; stale-generation and rejected-command
counts; provider failures; dropped/filtered event counts; and allocator/cache
high-water values. Constraint break packets include the stable record UUID,
reset generation, measured force/torque, and the configured thresholds. A
resize, endpoint, limit, motor, spring, or shape rejection should be surfaced
with its validation message; do not retry a stale command without rebuilding
its generation-safe target.

## Small examples

```cpp
using namespace TerraEngine::Physics;

PhysicsCommandMetadata meta;
meta.producerId = gameplayProducer;
meta.producerSequence = ++sequence;
// Leave effectiveTick at zero for the next eligible fixed tick, or set an
// explicit tick obtained from the caller's fixed-step bookkeeping.

// Desired movement is deferred and generation checked at PhysicsCommandCommit.
system.SubmitCommand(PhysicsCommand::MakeCharacterMove(
    characterTarget, {5.0F, 0.0F, 0.0F}, meta));
system.SubmitCommand(PhysicsCommand::MakeCharacterJump(characterTarget, 6.0F, meta));

// A break reset must advance the authored token; an old token is rejected.
system.SubmitCommand(PhysicsCommand::MakeResetConstraint(
    constraintTarget, record.resetGeneration + 1U, meta));
```

Use metres, kilograms, seconds, newtons, newton-seconds, and N-m in authored
data. Keep gameplay logic on the engine contracts above; provider diagnostics,
solver objects, and provider-specific IDs belong exclusively to the backend.
