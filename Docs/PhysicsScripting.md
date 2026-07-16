# Physics scripting (C#)

`TerraEngine.Physics` is the engine-owned managed contract (ABI version 1).
Scripts receive fixed-width copies and stable handles; they never receive a
Jolt pointer, provider ID, or borrowed native storage. The same API is used by
the editor host and NativeAOT exports.

## Commands and custom gravity

`Rigidbody`, `CharacterController`, `ColliderSet`, and `ConstraintSet` are
ordinary managed components. Rigidbody commands are `AddForce`,
`AddImpulseAtPoint`, `SetLinearVelocity`, `SetAngularVelocity`, `Teleport`,
`MoveKinematic`, `WakeUp`, and `Sleep`; character commands are
`SetDesiredVelocity`, `Jump`, `Resize`, and `Teleport`. Every command is
deferred to the protected fixed-step command boundary and returns a
`PhysicsCommandReceipt` (`Status`, `CommandId`, `EffectiveTick`,
`CorrelationId`, and bounded `Diagnostic`). `Accepted` means queued, not that
the next snapshot already includes the change.

`UseGravity` uses the project gravity. For local/custom gravity, disable it and
submit a mass-independent acceleration during fixed-step gameplay:

```csharp
// Apply custom gravity without depending on the body's mass.
body.UseGravity = false;
PhysicsCommandReceipt receipt = body.AddForce(
	new Vector3(0.0f, -9.81f, 0.0f), ForceMode.Acceleration,
	effectiveTick: 0, correlationId: 1001);
if (!receipt.Accepted)
	Log.Warn($"Gravity command rejected: {receipt.Status} ({receipt.Diagnostic})");
```

Managed commands use `effectiveTick` (zero means the next eligible tick) and a
correlation ID for diagnostics. Unlike editor automation, the managed ABI does
not expose an idempotency-key field; producer sequencing and protected-boundary
validation prevent stale or duplicate native effects.

## Runtime authoring and object metadata

`Rigidbody` exposes its complete scalar authoring profile: motion and mass
modes, mass, damping, gravity/sleep flags, continuous collision quality,
initial velocities, velocity limits, allowed degrees of freedom, interpolation,
collision layer/mask override, and the physics-specific enabled flag.
`CharacterController` exposes shape dimensions, local center/up direction,
step and support volumes, slope/contact limits, mass/strength, collision filter,
gravity/jump behavior, rotation policy, and its physics-specific enabled flag.

These properties change durable component authoring. A worker callback queues a
copied typed mutation, and the value becomes visible only after the protected
scripting scene flush. Physics then observes it through its next immutable
authoring snapshot; setting a property does not immediately rebuild provider
state inside the callback.

```csharp
Rigidbody body = Object.GetComponent<Rigidbody>()!;
body.MotionType = PhysicsMotionType.Dynamic;
body.MassMode = PhysicsMassMode.ExplicitMass;
body.Mass = 12.5f;
body.ContinuousCollisionMode = PhysicsMotionQuality.LinearCast;
body.AllowedDegreesOfFreedom = PhysicsDegreesOfFreedom.All;
body.CollisionLayer = 2;

// Object metadata follows the same worker-safe publication rule.
Object.Tag = "Player";
Object.Layer = "Gameplay";
Object.IsStatic = false;
Object.Active = true;
bool currentlyEffective = Object.ActiveInHierarchy;
```

`Component.Enabled` is the common component filtering flag. Physics components
also expose `PhysicsEnabled`, which controls whether the physics authoring record
creates runtime provider state. `Object.ActiveInHierarchy` is read-only because
it combines the object's authored `Active` flag with every ancestor.

Collider and constraint collections use stable child-record identities and are
not scalar reflection properties. `Records` returns a copied array, while
`ReplaceRecords` validates and replaces the complete collection atomically.
A direct protected call returns `Success`; a worker callback returns `Accepted`
and publishes only at the scripting flush. If another component edit advances
the object's generation before that flush, the stale replacement conflicts and
the prior collection remains intact.

```csharp
ColliderSet colliders = Object.GetComponent<ColliderSet>()!;
ColliderDefinition bodyShape = ColliderDefinition.Box(new Vector3(0.5f, 1.0f, 0.5f));
bodyShape.LocalCenter = new Vector3(0.0f, 1.0f, 0.0f);
bodyShape.CollisionLayerId = 2;
PhysicsStatus colliderStatus = colliders.ReplaceRecords(new[] { bodyShape });

ConstraintSet constraints = Object.GetComponent<ConstraintSet>()!;
PhysicsUuid bodyShapeOwnerId = Object.GetComponent<Rigidbody>()!.Identity.ObjectId;
ConstraintDefinition hinge = ConstraintDefinition.ToWorld(
	bodyShapeOwnerId, PhysicsConstraintKind.Hinge);
hinge.Limits = new PhysicsConstraintLimits {
	Enabled = true, Minimum = -45.0f, Maximum = 45.0f
};
PhysicsStatus constraintStatus = constraints.ReplaceRecords(new[] { hinge });
```

Use `ColliderDefinition.Box`, `Sphere`, `Capsule`, `Cylinder`,
`TaperedCapsule`, or `FromAsset` to receive valid schema, identity, pose,
filter, and debug defaults. Use `ConstraintDefinition.BetweenObjects` or
`ToWorld` to receive valid frames, axes, break thresholds, and reset generation.
Six-DOF settings are named `TranslationX/Y/Z` and `RotationX/Y/Z`. The original
`GetRecord` methods remain available for generation-safe runtime handle facades.

## Generation-safe identity and immutable reads

Resolved wrappers carry `PhysicsIdentity` (object, component, optional record,
and binding generations) plus a world-generation handle. Native calls
revalidate all generations. Object removal, component/record recreation, Play
Mode stop, script reload, or world rebuild returns `PhysicsStatus.StaleTarget`
and has no side effect.

Use `Physics.BodyStates()`, `Physics.CharacterStates()`, and `Physics.Settings()`
for copied immutable snapshots. Query synchronously with `Query`, or schedule
with `ScheduleQuery`/`ScheduleBatch` and later call `ReadQuery`; cancel with
`CancelQuery`:

```csharp
PhysicsQueryRequest request = new() {
	World = world,
	Kind = PhysicsQueryKind.Raycast,
	HitMode = PhysicsQueryHitMode.All,
	Pose = new PhysicsPose { Position = origin, Rotation = Quaternion.Identity },
	Direction = direction,
	Distance = 200.0f,
	MaximumHits = 8,
	Filter = new PhysicsQueryFilter {
		CollisionMask = uint.MaxValue, BodyTypeMask = uint.MaxValue, IncludeSensors = false
	},
	CorrelationId = 2001
};
PhysicsQueryHandle handle = Physics.ScheduleQuery(request, out PhysicsStatus status);
if (status == PhysicsStatus.Accepted) {
	PhysicsQueryResult result = Physics.ReadQuery(handle);
	// Hits are copied values. Result handles expire at ExpiryTick and may report Expired.
	foreach (PhysicsQueryHit hit in result.Hits) { /* consume hit */ }
}
```

`PhysicsQueryResult` reports status, publication tick, correlation ID,
`Truncated`, diagnostics, and copied hits. The native two-call copy protocol
never exposes provider memory. Invalid bounds, stale worlds, expired handles,
or ABI mismatches are rejected before a native side effect.

## Events and status contract

Subscribe with `PhysicsEvents.Subscribe(...)`; `Published` receives copied
collision, trigger, contact, activation, sleep, and constraint-break events at
the protected post-physics boundary. Subscription filters include category,
collision layer, sensors, and contact details. `ResetManagedSubscriptions()`
clears delegates during script reload/Play Mode stop. Callbacks are snapshot
ordered and isolated: an exception is logged and later callbacks still run.

The complete status enum is `Success`, `Accepted`, `InvalidArgument`,
`InvalidState`, `StaleTarget`, `LimitExceeded`, `Conflict`, `Cancelled`,
`Expired`, `ProviderFailure`, and `VersionMismatch`. Treat
`VersionMismatch` as an engine/managed-assembly deployment mismatch: rebuild
both sides instead of retrying. `InvalidArgument` and `LimitExceeded` indicate
caller data/bounds errors; `Conflict` indicates a generation or protected
boundary race; `Cancelled`/`Expired` are terminal and should be surfaced to
gameplay rather than retried blindly.
