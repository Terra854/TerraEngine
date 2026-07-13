# TerraEngine Physics

TerraEngine physics is exposed through engine-owned components and typed command
packets. Gameplay never depends on provider classes, IDs, pointers, or enum
integers.

## Authoring contract

Rigid bodies use metres, kilograms, seconds, newtons, and newton-seconds. The
default project gravity is `(0, -9.81, 0)`. A `RigidBodyComponent` may be
Static, Dynamic, or Kinematic. A `ColliderSetComponent` stores an ordered list
of stable-ID records, so one object can contain repeated boxes, spheres,
capsules, or compound recipes.

`useGravity=true` applies the project gravity during each fixed tick.
`useGravity=false` disables built-in gravity; custom or radial gravity should be
submitted as a mass-independent `ApplyAcceleration` command each fixed tick.
Use `ApplyForce` for Newton forces, `ApplyImpulse` for an instantaneous
mass-scaled impulse, and `ApplyVelocityChange` for an instantaneous velocity
change. Commands are typed, deferred, generation-checked, and committed at the
declared physics boundary.

Dynamic bodies require a valid collider. Colliders without a rigid body behave
as static geometry. Negative scale, unsupported non-uniform scale, and unsafe
parent transform combinations are rejected rather than silently distorting
simulation geometry. Keep authored transforms in metres and use the existing
editor angle conventions.

## Simulation behavior

Physics advances on the configured fixed timestep with bounded accumulator and
catch-up limits. Excess elapsed time is clamped and reported diagnostically.
When paused, `Next` queues exactly one configured fixed tick: one fixed callback
pass, one publication, and one resulting render; the world remains paused.

Collision layers and masks control broad-phase pairing. Sensor/trigger bodies
report overlap behavior without physical response. Friction, restitution,
combine modes, damping, sleeping, wake-up, and motion limits are authored in
engine-neutral fields. `Discrete` and `LinearCast` are the supported initial
collision-quality modes; use `LinearCast` for validated fast-body/thin-geometry
cases.

## Provider baseline

The runtime provider baseline is pristine Jolt Physics 5.6.0 at commit
`e77f175595e64cb44218cc9d9d56fc365ad0e36a`, vendored under `External/` and
built by the repository configuration. Jolt 5.5 is comparison evidence only;
it must never be used as the runtime baseline.

## Build and focused validation

From a clean checkout with the repository-bundled dependencies:

```powershell
cmake --preset Debug
cmake --build --preset Debug --target TerraEngine TerraEngine-Editor TerraEngine-EngineTests
cmake --build --preset Debug --target install
```

Run the focused engine slice:

```powershell
$env:TERRAENGINE_TEST_FILTER = "Physics;Physics.FixedStep;Physics.RigidBody;Physics.Serialization"
./out/build/Debug/TerraEngine-EngineTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

The canonical installed tree is `out/build/Debug/Build`. No globally installed
physics SDK is required or permitted. See [Docs/Testing.md](Testing.md) for
CTest filters and full regression workflows.
