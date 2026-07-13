# Physics scripting

`TerraEngine.Physics` exposes the engine-owned physics contract to C# without
exposing Jolt objects, provider IDs, or borrowed native storage. It works in
the editor host and NativeAOT exports through ABI version 1.

`Rigidbody.UseGravity` selects the project gravity captured for the next fixed
physics tick. To implement local or custom gravity, turn it off and enqueue a
mass-independent acceleration from fixed-step gameplay:

```csharp
body.UseGravity = false;
body.AddForce(new Vector3(0.0f, -9.81f, 0.0f), ForceMode.Acceleration);
```

All body and character commands are deferred. A `PhysicsCommandReceipt` with
`Accepted` means the request entered the bounded queue; it does not mean that
the next published snapshot already contains the result. Receipts and query
results carry correlation IDs for logs and automation.

```csharp
PhysicsQueryHandle pending = Physics.ScheduleQuery(new PhysicsQueryRequest {
    World = world, Kind = PhysicsQueryKind.Raycast, Pose = rayPose,
    Direction = direction, Distance = 200.0f, MaximumHits = 8,
    Filter = new PhysicsQueryFilter { CollisionMask = uint.MaxValue }
}, out PhysicsStatus status);

if (status == PhysicsStatus.Accepted) {
    PhysicsQueryResult result = Physics.ReadQuery(pending);
    // Results are copied immutable values. Retained handles can expire.
}
```

Every wrapper and request contains object, component, record, world, and
binding generations. A removed object, script reload, or rebuilt world returns
`StaleTarget` and makes no native side effect. Variable results use a two-call
native copy protocol internally, so neither scripts nor callbacks borrow
provider memory.

Subscribe to `PhysicsEvents.Published` for collision, trigger, contact,
activation, sleep, and constraint-break copies. Delivery occurs at the
protected post-physics boundary. Each subscriber is isolated: an exception is
logged and later subscribers continue to receive the ordered packet.

Status values are intentionally narrow: `InvalidArgument`, `InvalidState`,
`StaleTarget`, `LimitExceeded`, `Conflict`, `Cancelled`, `Expired`,
`ProviderFailure`, and `VersionMismatch`. Treat `VersionMismatch` as an engine
and managed-assembly deployment mismatch, rebuild both sides, and do not retry
the operation.
