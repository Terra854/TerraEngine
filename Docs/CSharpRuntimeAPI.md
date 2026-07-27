# C# Runtime Configuration API

TerraEngine gameplay assemblies can configure complete physics policy, object
hierarchy, scene lifecycle, scene rendering, time, input, resource streaming,
and animation without borrowing native engine memory. Reads return copied
snapshots. Writes return an accepted/queued status when they have entered a
protected native queue and normally become visible at the next declared frame
boundary.

## Main surfaces

- `Physics.Configuration` reads or assigns the complete physics configuration,
  including all 32 collision layers. `Physics.ApplyConfiguration` returns a
  `PhysicsStatus` when code prefers explicit rejection handling.
- `Object.Parent`, `Object.Children`, `Object.All`, and `Object.FindAllWithTag`
  provide typed hierarchy traversal. `SetParent` is transactional and cannot
  change live hierarchy during component iteration.
- `Object.Instantiate` returns an `ObjectCreationRequest`. Its stable receipt
  resolves to an `Object` only after the protected scene mutation flush.
- `Scene.ActiveScene`, `Scene.LoadedScenes`, `Scene.LoadScene`,
  `Scene.InstantiatePrefab`, and
  `Scene.UnloadAll` cover runtime scene and prefab lifecycle. Prefab publication
  and unloads occur at frame end; unload-all wins over other lifecycle requests
  captured in the same frame.
- `Input.Snapshot()` returns one immutable keyboard, mouse, cursor, and scroll
  snapshot. Transition helpers distinguish pressed, held, and released states.
- `Time` exposes scaled and unscaled delta/elapsed values, fixed delta, frame
  count, and a queued `TimeScale` setting.
- `Scene.RenderSettings` configures scene-wide indirect-lighting and ambient-
  occlusion policy. This is separate from game-wide `Graphics.Settings`.
- `ResourceStreaming.Preload`, `GetStatus`, and `Release` expose asynchronous
  readiness, residency bytes, diagnostics, and protected release.
- `TerraEngine.Animation.Animator` submits typed, generation-validated playback
  and parameter commands and returns stable receipts with observable ticks.
  `ReadState`, `ReadParameters`, `ReadCurves`, `ReadMorphs`, `ReadBones`,
  `ReadSockets`, `ReadEvents`, and `ReadRootMotionResults` return complete
  immutable publications with explicit age, generation, and overflow metadata.
- `TerraEngine.Animation.AnimationEvents` provides bounded disposable event
  subscriptions in hosted and managed-vtable-v3 exported runtimes. See
  `Docs/Animation/CSharpAnimationAPI.md` for filters, callback containment,
  reload handling, provider capability checks, and observation boundaries.

## Boundary example

```csharp
PhysicsConfiguration physics = Physics.Configuration;
physics.Gravity = new Vector3(0.0f, -3.0f, 0.0f);
PhysicsStatus physicsStatus = Physics.ApplyConfiguration(physics);

ObjectCreationRequest rigRequest = Object.Instantiate("Rig");
ObjectCreationRequest cameraRequest = Object.Instantiate("Camera");

// Resolve on a later frame, after the protected scene mutation flush.
Object? rig = rigRequest.Resolve();
Object? camera = cameraRequest.Resolve();
RuntimeStatus hierarchyStatus = rig != null && camera != null
  ? camera.SetParent(rig)
  : RuntimeStatus.NotFound;

SceneRenderSettings lighting = Scene.RenderSettings;
lighting.IndirectStrength = 1.25f;
RuntimeStatus lightingStatus = Scene.ApplyRenderSettings(lighting);

Time.TimeScale = 0.5f;
InputSnapshot input = Input.Snapshot();
```

`Accepted` means native code copied and validated the request; it does not mean
the mutation is already visible. Scripts should observe hierarchy and scene
changes on the next frame. Invalid or conflicting complete-configuration writes
leave the previous native state unchanged.

All object metadata, component membership, reflected component properties,
physics authoring records, script-binding additions, and scene lifecycle writes
issued from managed lifecycle callbacks follow the same rule on main-thread and
worker dispatches. Managed code must not assume that a successful setter changes
same-callback reads.

Animation follows that rule as well. `AnimationStatus.Queued` means the command
was copied into the native queue; observe the receipt's `ObservableTick` and the
snapshot's `PublicationGeneration` rather than assuming same-callback pose or
event visibility. Stale wrappers return `StaleHandle` and are never silently
rebound to a reused object/component slot.

The root CMake configuration discovers `Engine/Managed/Source/Animation/*.cs`
through the managed core source glob. Managed publish and the install target
therefore place the animation surface in the same `TerraEngine.CSharp.dll` used
by the editor and export pipeline; no build-tree-only animation assembly is
required.

## Verification

From a Visual Studio developer shell on Windows:

```powershell
dotnet build Engine/Managed/TerraEngine.csproj -c Debug --no-restore
$env:TERRAENGINE_TEST_FILTER="Scripting.RuntimeApi;Animation.Managed"
.\out\build\RelWithDebInfo\TerraEngine-EngineTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
cmake --build out\build\RelWithDebInfo --target install --config RelWithDebInfo
```
