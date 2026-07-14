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
