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
cmake --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine TerraEngine-Editor TerraEnginePhysicsColliderCompiler TerraEngine-Export TerraEngine-EngineTests TerraEngine-EditorTests
cmake --build --preset RelWithDebInfo --target install
```

Spec 042 accepts RelWithDebInfo exclusively for compile, test, install, and live-validation completion gates.

Run the focused engine slice:

```powershell
$env:TERRAENGINE_TEST_FILTER = "Physics;Physics.FixedStep;Physics.RigidBody;Physics.Serialization"
./out/build/RelWithDebInfo/TerraEngine-EngineTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

The canonical installed tree is `out/build/RelWithDebInfo/Build`. No globally installed
physics SDK is required or permitted. See [Docs/Testing.md](Testing.md) for
CTest filters and full regression workflows.

## Spec 042 stress, export, and troubleshooting

Run the committed deterministic and lifecycle fixtures from the RelWithDebInfo
test binaries. These tests create the full 10,000-body/1,000-active workload,
repeat 10,000-ray batches, and exercise stale-world and Play Mode cycles.

```powershell
$env:TERRAENGINE_TEST_FILTER = "Physics.DeterminismStress;Physics.LifecycleStress"
& .\out\build\RelWithDebInfo\TerraEngine-EngineTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER

$env:TERRAENGINE_TEST_FILTER = "Physics.Editor.Soak"
& .\out\build\RelWithDebInfo\TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

The installed-editor loopback stress harness is the live performance and
authoring check. It records body counts, exact paused `Next` ticks, profiler
data, authored-scene hashes, and shutdown status:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Physics/RunInstalledEditorPhysicsStress.ps1 `
  -InstallRoot out/build/RelWithDebInfo/Build `
  -WorkRoot out/validation/installed-editor-physics-stress `
  -ObjectCount 10000 -ActiveDynamicCount 1000 -ExplicitStepCount 100 -WorkerCount max `
  -AllowLiveStress
```

Add `-RunUs6Loopback` for managed/automation reload and idempotency checks, or
`-RunAuthoringWalkthrough` for the editor authoring workflow. Keep each run in
its own disposable `out/validation` directory and use a free `-McpPort`.

The headless export parity gate cooks source-only collider recipes, exports the
package, validates the six evidence files, launches the published runtime, and
checks orderly shutdown:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/ExportRuntimeBaseline/RunBaseline.ps1 `
  -InstallRoot out/build/RelWithDebInfo/Build `
  -WorkRoot out/ci/physics-export-baseline
```

If a focused test fails, preserve its output before rerunning. Common causes
are a stale install tree (rerun the install target), a busy MCP port, a missing
Visual Studio developer environment, or a source-only collider without its
cooked `.tepcoll` envelope. Do not substitute Debug or a globally installed
physics SDK for the RelWithDebInfo gate.

### Sanitizer support on Windows

The Windows/MSVC project does not provide a full-engine ThreadSanitizer lane.
The supported sanitizer evidence is the repository's provider-only Jolt spike:

```powershell
pwsh -NoProfile -File Tools/Physics/BackendSpike/RunBackendSpike.ps1 `
  -Candidate Jolt56 `
  -Jolt56Source out/physics-spike/sources/JoltPhysics-5.6.0 `
  -WorkRoot out/physics-backend-spike `
  -RunId jolt56-windows-asan `
  -Configuration ReleaseASAN `
  -FixtureRepeats 3
```

That lane instruments the provider fixture only; TerraEngine engine/editor,
Vulkan presentation, managed NativeAOT, and full lifecycle CTest binaries are
excluded. The orchestrator must record the exact toolchain, outcome, and any
unsupported lanes in `specs/042-physics-system/evidence/sanitizer-validation.md`.
