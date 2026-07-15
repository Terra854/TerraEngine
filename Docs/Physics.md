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

### Incremental runtime and publication behavior

Collision categories use stable 0-31 layer IDs. A contact is accepted only
when both bodies' effective masks include the opposite layer; disabled layers
are rejected by the provider's broad filter first. Project-layer masks are the
default, while an enabled body mask override replaces that default. Continuous
collision is deliberately two-stage: the project must enable it and the
individual body must request `LinearCast`. Otherwise the runtime resolves the
body to `Discrete`.

The accumulator consumes only configured fixed steps, clamps catch-up to the
project limits, and gives paused `Next` requests exactly one configured step.
Provider callbacks enter an exclusive engine-owned event drain. Persistent
collision/trigger/contact/activation records are then filtered against the
immutable subscription snapshot; filtered and dropped volumes remain visible
in diagnostics. Sleeping/static bodies stay in the paged complete body-state
snapshot but are not read or published every frame. Wake, removal, and explicit
full-state verification make them incremental work again.

Authoring changes are generation journal entries, not file timestamp polls.
Unchanged bodies retain descriptor indices and shared primitive shape recipes;
only exact object, dependency, collider-asset, or material-asset reasons enter
the preparation plan. Large independent plans and provider extraction are split
into deterministic bounded ranges using the project settings. The defaults are
1,024 records for extraction/publication and 512 records before parallel work;
validated ranges are 64-8,192 records.

Extraction converts provider ranges into engine-owned records before the
protected boundary. The state store reuses immutable pages and publishes both a
same-tick complete query view and a changed-body packet. Only changed dynamic
poses become typed transform chunks. Every chunk for a tick preflights and
commits as one atomic publication group; any stale scene, hierarchy, object,
body, transform, or parent generation rejects the whole group. Accepted flush
receipts alone acknowledge poses. A declared later same-frame scene reader uses
After Physics visibility; reduced/headless schedules defer to Frame End, and an
empty changed packet creates no scene transaction.

`physics.diagnostics.get`, the Physics Diagnostics window, and normal profiler
captures expose authoring reasons, preparation path/ranges, extraction volumes,
state-page reuse/copy counts, publication chunks/receipts/failures, and current
health. Aggregate work-volume refreshes use the configured cadence (30 frames
by default), while a current failure bypasses that rate limit.

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

Spec 043 accepts RelWithDebInfo exclusively for compile, test, install, and
live-validation completion gates. The canonical stress workload is 10,000
bodies (1,000 dynamic, 1,000 kinematic, 8,000 static) with 100 exact paused
steps. Max workers use hardware concurrency; explicit 1/2/4 values are reserved
for comparison matrices, and no build command uses `--parallel`.

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
  -ObjectCount 10000 -ActiveDynamicCount 1000 -ExplicitStepCount 100 `
  -AllowLiveStress
```

Add `-RunUs6Loopback` for managed/automation reload and idempotency checks, or
`-RunAuthoringWalkthrough` for the editor authoring workflow. Keep each run in
its own disposable `out/validation` directory and use a free `-McpPort`.

The readiness matrix uses `cold`, `warm`, `stale`, `corrupt`, and
`forced-build` modes. Profiler v2 is the only recording format; v1 remains
load-only compatibility. Detailed, sampled, and aggregate captures stay open
for five seconds and must remain at or below 10 MiB. Use the debug-memory
switches separately so debug primitives and process sampling do not contaminate
the canonical timing run.

For the Spec 043 incremental matrix, retain each run under
`out/validation/043-us2/` and include unchanged, active, stale-generation,
reparent, and conflicting tick-wide publication cases. The emitted profiler and
JSON evidence must record configured batch sizes alongside examined, extracted,
changed, page-reuse, chunk, rejection, and acknowledgement counts.

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
