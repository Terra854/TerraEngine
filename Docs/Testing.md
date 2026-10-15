# TerraEngine Testing

## Engine correctness and failure safety

The seven focused correctness entries are implemented: `Engine.SceneMutationFailureSafety`,
`Engine.ResourceAsyncLifecycle`, `Engine.SceneSaveFailureSafety`,
`Engine.AudioWorldCellCapture`, `Engine.FrameFailureLifecycle`, `Engine.OwnedProcess`
and `Editor.ExportLifecycle`. Their independent process-tree watchdogs reject
missing/zero selections and enforce registered minimum counts. Current floors
are respectively 42, 28, 10, 5, 7, 6 and 6. Retain native and live acceptance
results with their installed artifact hashes and the naming-policy audit.

Run in an x64 Visual Studio developer shell with supported SDK headers/libraries
and pinned repository dependencies. Complete the build before using native
harnesses so executable and engine DLL layouts match.

```powershell
cmake --preset RelWithDebInfo -DBUILD_TESTING=ON
cmake --build --preset RelWithDebInfo --target install --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Compile/install failed.' }
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R '^(Engine\.(SceneMutationFailureSafety|ResourceAsyncLifecycle|SceneSaveFailureSafety|AudioWorldCellCapture|FrameFailureLifecycle|OwnedProcess)|Editor\.ExportLifecycle)$' --output-on-failure
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo --output-on-failure
```

Native build-harness checks are distinct from installed runtime acceptance.
The actual maintained install tree is `out/build/RelWithDebInfo/Build`, beneath
ENGINE_INSTALL_DESTINATION. Installation includes native DLL closures, managed
runtime/test dependencies, shaders, export tooling, child fixture and runner.
Its final `correctness-artifacts.json` compares installed binaries against built
targets and records SHA-256/lengths for the maintained runtime/support inputs.
The installed runner rejects missing/hash-mismatched artifacts, an invocation
from its source copy, and reused work directories.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File out/build/RelWithDebInfo/Build/Tests/Tools/Correctness/RunInstalledCorrectnessAcceptance.ps1 -InstallRoot out/build/RelWithDebInfo/Build -WorkRoot out/validation/correctness-acceptance-fresh -AllowLiveAcceptance
```

Live checks require Windows, a usable desktop/renderer and the installed bundled
NativeAOT tools. Export requests retain the existing high-impact approval policy:
approve each of the four export requests under Edit → Preferences → MCP Bridge
→ Approvals. Fresh correctness/export fixtures copy both installed default layout
files, matching normal project creation. Managed callback fault evidence requires
the exact fixture receipt and logged exception; native dispatch counts alone do
not report exceptions caught inside the managed callback bridge.
The runner uses isolated write-enabled preferences but never manufactures approval
tokens. Optional timing/port arguments and the maintained managed-reload/export
regressions are documented by the installed runner's parameter help.
The runner preserves correlated native/live logs and package/managed/process
observations; watchdog termination is a failed check, not an orderly shutdown.

## Runtime memory safety and renderer scratch

Runtime memory safety separates native regression checks, installed runtime
observations, and performance qualification. The renderer integration, installed
runner/editor helper scripts, and protocol CTests are integrated. The commands
below do not establish acceptance by themselves. Retain final build/install,
complete applicable CTest, CPU, Vulkan, renderer/editor, and performance receipts
separately.

Use an x64 Visual Studio developer shell with the repository's bundled
dependencies. From a clean checkout, configure tests, compile, and install the
same bytes before running installed tools:

```powershell
cmake --fresh --preset RelWithDebInfo -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw 'Configure failed.' }
cmake --build --preset RelWithDebInfo --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Compile failed.' }
cmake --build --preset RelWithDebInfo --target install --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Compile/install failed.' }
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
	-R '^(Engine\.RuntimeMemorySafety(\.|$)|Editor\.RuntimeMemorySafety\.Diagnostics|Runtime\.MemorySafety\.)' --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Focused CTest failed.' }
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Complete CTest failed.' }
```

The maintained install is `out/build/RelWithDebInfo/Build`, regardless of the
preset's nominal install prefix. Runtime-memory tools live under its
`Tests/Tools/RuntimeMemorySafety` directory. The installed
`runtime-memory-safety-artifacts.json` pins their actual bytes and the required
DLL, shader, managed, fixture, and watchdog closure. Close any installed editor
before replacing its loaded binaries. Keep work and evidence outside install
trees; every run needs a fresh work directory.

The frozen native registration floors are:

| Native filter / CTest entry | Minimum registered cases |
| --- | ---: |
| `Engine.RuntimeMemorySafety.TaskCache` | 8 |
| `Engine.RuntimeMemorySafety.DependencyAdmission` | 10 |
| `Engine.RuntimeMemorySafety.Allocators.FixedPool` | 4 |
| `Engine.RuntimeMemorySafety.Allocators.Alignment` | 4 |
| `Engine.RuntimeMemorySafety.Allocators.Overflow` | 6 |
| `Engine.RuntimeMemorySafety.Allocators` (union) | 14 |
| `Engine.RuntimeMemorySafety.RendererScratch` | 10 |
| `Engine.RuntimeMemorySafety.EmptyFramePolicy` | 8 |
| `Editor.RuntimeMemorySafety.Diagnostics` | 4 |

These represent at least 54 unique cases: 50 engine and four editor. The
allocator union and its subgroups overlap; repeated executions add no
registrations. Empty or misspelled selections fail. Native `RMS_OBSERVATION`
lines retain actual addresses, construction/destruction counts, ownership,
worker counts, and lifetime facts alongside exact PASS names and watchdog logs.
The parser accepts logger interleaving between the count and `selected tests`,
including a newline, while preserving the count floor and original log bytes.

The separate `Engine.VulkanPresentationReservationRecovery` regression injects
three host retirement-reservation failures through the production reservation
helper and recreation coordinator. It checks one retry per protected frame
boundary at an unchanged extent, atomic retention of the active bundle, and
frame admission after a complete replacement. It needs no GPU or resize event.
After the build/install commands above, run it with:

```powershell
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
	-R '^Engine\.VulkanPresentationReservationRecovery$' --output-on-failure
```

For an installed allocator slice, after compilation and installation:

```powershell
$installRoot = (Resolve-Path 'out/build/RelWithDebInfo/Build').Path
$slice = Join-Path $installRoot 'Tests/Tools/Correctness/RunNativeCorrectnessSlice.ps1'
$engineTests = Join-Path $installRoot 'Tests/Engine/TerraEngine-EngineTests.exe'
& $slice -Executable $engineTests -Filter 'Engine.RuntimeMemorySafety.Allocators' `
	-MinimumCases 14 -TimeoutSeconds 10 -LogDirectory './out/validation/runtime-memory-safety/allocators-fresh'
if ($LASTEXITCODE -ne 0) { throw 'Allocator slice failed.' }
```

CPU acceptance additionally requires 100 independently watched completed-cache
and 100 never-dispatched-cache runs. After the complete ten-case admission
selection, repeat `DependencyAdmission.MultipleAncestors` 100 times at each
actual worker count 1, 2, and 8. The native 10-second process-tree watchdog
deadline is independent of in-test waits. Record original exceptions, terminal
ancestors, held input lifetime, balanced ownership, and successful shutdown;
requesting a worker count is insufficient without observing it.

Vulkan acceptance requires actually enabled validation, 100 fresh first-image
and 100 recreated first-image operations, valid acquired-image submission and
presentation, and two actual healthy follow-up frames. Check recording/reset/
submit failure checkpoints, consumed acquire waits, rollback, recovery, and
terminal cleanup using their real receipts. A planner test, requested validation
flag, or device preflight cannot establish GPU success. Renderer acceptance
separately requires repeated completed/failed/cancelled/scene-clear/shutdown/
restart episodes and installed editor loopback for copied read-only diagnostics.

The central integration uses a stable renderer-owned `atomic_flag` to
reject recursive/overlapping execution before mutation. Test direct execution,
each coroutine resume, and lifecycle cancellation with the same exclusive gate;
do not substitute the small continuation-handoff mutex for execution ownership.
Camera/light vectors are coroutine locals declared after their strong episode
lease. Borrowing jobs settle before vectors, then lease, retire; the next
protected capture or lifecycle boundary owns the actual reset. Existing Render
phase declarations cover scratch capture writes, worker/backend reads, and
copied publication. No dependencies, shaders, phases, or mutation flushes are
added by this scope.

Installed editor loopback uses the existing `diagnostics.get_summary` operation
and observes `data.rendererScratch.latest`, `.previous`, and `.reclamation`.
Latest/previous terminal copies must retain completed/failed/cancelled outcome,
identities, pre-reset use/peak/high-water, reuse/fallback deltas, and reset count
across protected reclamation and renderer restart. Reclamation has a separate
lifetime ID, cumulative actual fallback allocation/deallocation counts, actual
outstanding count/bytes, and reset count. Compare release facts with actual
counting-upstream deallocations and preserved fallback sentinels in native tests;
an empty reset increasing reset count cannot prove that a fallback was freed.
Automation reads published copies and offers no capacity override, arena pointer,
or allocator control.

The final installed campaign records these results for its content-addressed
runtime manifest:

- Complete integrated CTest: 183/183 groups, zero failures, skips, disabled tests
  or exclusions; [suite gate](../out/evidence/runtime-memory-safety/final-ctest.json).
- CPU: 54 unique scoped native cases and 500 repetitions: 100 completed-cache,
  100 never-dispatched-cache, and 100 admission failures at each of 1/2/8 workers;
  [actual CPU evidence](../out/evidence/runtime-memory-safety/final-cpu.json).
- Vulkan: 10 lanes times 100 repetitions, 2000 healthy follow-up frames, zero
  validation errors and actual context/window cleanup;
  [actual Vulkan evidence](../out/evidence/runtime-memory-safety/final-vulkan.json).
- Renderer: 100 repetitions in each of completion, failure, cancellation, scene
  clear, shutdown and restart, plus installed read-only editor diagnostics;
  [actual renderer/editor evidence](../out/evidence/runtime-memory-safety/final-renderer.json).
- Small/normal/stress parity: exact camera/light identity and ordering,
  equivalent published outputs and zero observed fallbacks;
  [three-scale parity](../out/evidence/runtime-memory-safety/final-parity.json).
- Preparation timing: all 18 separate median/p95 comparisons passed against the
  original six sealed noise limits, using 60 warm-up and 300 measured frames;
  [actual performance evidence](../out/evidence/runtime-memory-safety/final-performance.json).

The [build/install gate](../out/evidence/runtime-memory-safety/final-build-install.json)
pins the same installation. The final acceptance ledger joins these receipts
with source, documentation and graph review. No speedup is inferred from a
passing no-regression gate.

An initial nine-capture timing attempt was rejected before metric comparison
because its MCP port 43189 differed from the sealed reference port 43186. All
nine captures were repeated with matching controls and the original frozen
limits. [The invalid attempt and complete corrected run](../out/evidence/runtime-memory-safety/performance-port-control-correction.json)
remain recorded; no old candidate samples were reused.

Two earlier GPU attempts exited with access violations in NVIDIA App's Game
Proxy, `nvspcap64.dll`, at the same module offset; their root cause remains
unproven. [Failure records and host constraint](../out/evidence/runtime-memory-safety/gpu-host-environment-constraint.json)
are retained. The final matrix kept the overlay enabled and used one hidden
window per lane while constructing and destroying 100 fresh native Vulkan
contexts, devices, surfaces and swapchains. Every iteration still observes
context/thread-pool cleanup, and the root receipt observes final window cleanup.
No driver module was pinned and no global capture setting was changed.

The archived RHI does not expose its private camera-array capture. Parity uses
common GameView/GPU output, authored command identity/order, actual candidate
arrays and the recorded source-flow proof. Post-close fixture hashes establish
on-disk byte identity; they do not establish the bytes held in memory during a
timed run. Keep these limits alongside the raw evidence.

Preserve the unchanged reference runtime and a separate immutable validation-tool
install before production fixes. Reference and candidate timing both use that
same `ToolInstallRoot`; `RuntimeInstallRoot` selects the runtime being measured.
Decode archived journals as data rather than loading a new fixture into old
runtime DLLs. Never overwrite the original reference or its sealed calibration.

Preparation timing is the sum of the existing `RenderSystem: Protected
Publication Capture`, `RenderSystem: Geometry Route Preparation`, and
`RenderSystem: Virtual Geometry Preparation` inclusive callback scopes. Use
Detailed capture at cadence one, select ordinals 0 through 359, discard exactly
60 warm-up frames, and use all remaining 300 frames. V1 journal metadata proves
detail/cadence; the actual capture-start API admission proves `maxFrames=4096`.
The median averages sorted positions 150/151 and nearest-rank p95 uses 285.
Freeze three A/A pairs per small/normal/stress scale and six separate
`max(2%, max_i abs(Aprime_i / A_i - 1))` limits before candidate measurements.
All 18 candidate run/metric comparisons must pass independently, together with
exact camera/light identity/order and equivalent published outputs. Whole-frame
CPU duration, pooled summaries, selective retries, or missing samples cannot
replace this evidence. Report every fallback and retain raw journals and hashes.

The installed `RunInstalledRuntimeMemorySafety.ps1` supports Cpu, Vulkan,
Renderer, Performance, and All. Use PowerShell 7 and invoke its installed copy.
Each WorkRoot must be fresh and outside candidate, reference, and frozen-tool
installs. Source invocation, redirected/overlapping paths, missing artifacts,
and installed hash mismatches are rejected. Cpu requires at least 100 iterations
and exactly workers 1, 2, and 8; Vulkan and Renderer require at least 100
iterations per lane. Vulkan covers ten native lanes and Renderer covers six
lifecycle lanes plus the existing editor diagnostics loopback.

```powershell
$runner = Join-Path $installRoot 'Tests/Tools/RuntimeMemorySafety/RunInstalledRuntimeMemorySafety.ps1'
$evidenceRoot = Join-Path (Get-Location).Path 'out/validation/runtime-memory-safety'
function New-RmsEvidenceRoot([string]$Lane) {
	Join-Path $evidenceRoot ($Lane + '-' + [guid]::NewGuid().ToString('N'))
}
& $runner -InstallRoot $installRoot -WorkRoot (New-RmsEvidenceRoot 'cpu') `
	-Mode Cpu -Iterations 100 -WorkerCounts @(1,2,8)
if ($LASTEXITCODE -ne 0) { throw 'CPU acceptance was nonpassing.' }
& $runner -InstallRoot $installRoot -WorkRoot (New-RmsEvidenceRoot 'vulkan') `
	-Mode Vulkan -Iterations 100
if ($LASTEXITCODE -ne 0) { throw 'Vulkan acceptance was nonpassing.' }
& $runner -InstallRoot $installRoot -WorkRoot (New-RmsEvidenceRoot 'renderer') `
	-Mode Renderer -Iterations 100 -McpPort 43186
if ($LASTEXITCODE -ne 0) { throw 'Renderer/editor acceptance was nonpassing.' }
```

Performance and All additionally require the original immutable reference,
sealed calibration, frozen timing-tool install, and independently collected
pinned parity evidence. The example paths below must identify those real
artifacts; these instructions do not create or certify a parity receipt.

```powershell
$referenceRoot = (Resolve-Path './out/reference/runtime-memory-safety-prechange/Build').Path
$timingTools = (Resolve-Path './out/reference/runtime-memory-safety-validation-tools-v2/Build').Path
$calibration = (Resolve-Path './out/evidence/runtime-memory-safety/reference-calibration.json').Path
$parity = (Resolve-Path './out/evidence/runtime-memory-safety/parity.json').Path
& $runner -InstallRoot $installRoot -WorkRoot (New-RmsEvidenceRoot 'performance') `
	-Mode Performance -ReferenceInstallRoot $referenceRoot `
	-ReferenceCalibrationPath $calibration -ToolInstallRoot $timingTools `
	-ParityEvidencePath $parity -McpPort 43186
if ($LASTEXITCODE -ne 0) { throw 'Performance/parity acceptance was nonpassing.' }
& $runner -InstallRoot $installRoot -WorkRoot (New-RmsEvidenceRoot 'all') `
	-Mode All -Iterations 100 -WorkerCounts @(1,2,8) `
	-ReferenceInstallRoot $referenceRoot -ReferenceCalibrationPath $calibration `
	-ToolInstallRoot $timingTools -ParityEvidencePath $parity -McpPort 43186
if ($LASTEXITCODE -ne 0) { throw 'A required acceptance lane was nonpassing.' }
```

All runs Cpu, Vulkan, Renderer/editor, then Performance and retains lane failures.
Exit 0 means every requested lane passed, 1 means Failed, and 2 means Unverified;
both nonzero outcomes are nonpassing. Inspect retained summary and raw child
receipts; setup rejection may occur before a summary exists. Three installed
protocol CTests cover runner, renderer, and editor evidence contracts:
`Runtime.MemorySafety.RunnerProtocolContracts`,
`Runtime.MemorySafety.RendererProtocolContracts`, and
`Runtime.MemorySafety.EditorProtocolContracts`. Synthetic files and mocked
transport in these checks cannot establish native/live/performance acceptance.
See the maintained
[runtime-memory validation notes](../Tests/Tools/RuntimeMemorySafety/README.md)
for the current supported entry points. Run the complete applicable CTest and
separately documented live/long-running lanes before final acceptance; retain
inherited failures or unavailable lanes explicitly.

## Parented world edits and character scale

Build the editor and both native test targets with the RelWithDebInfo workflow
below. For a focused regression pass:

```powershell
$env:TERRAENGINE_TEST_FILTER = 'WorldHierarchy,Engine.WorldSpatialTransaction'
& ./out/build/RelWithDebInfo/TerraEngine-EngineTests.exe
$env:TERRAENGINE_TEST_FILTER = 'Editor.TransactionalMutation'
& ./out/build/RelWithDebInfo/TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
cmake --install out/build/RelWithDebInfo --config RelWithDebInfo
```

These tests check world-to-local conversion under rotated/nonuniform parents,
moved ancestors, cross-cell positions, singular-parent rejection, child-first
publication batches, stale-ancestor conflicts, exact pose rollback, protected
undo/redo visibility, repeated propagation, and save/load. World edits update
both the stored world pose and the parent-relative pose consumed by hierarchy
propagation. Exact inverse records retain both poses; existing dirty revisions
remain monotonic through rollback. No new scheduler phase or flush is added.

In the installed navigation demo, retain unit scale on the character-controller
root. The original knight mesh is approximately 5.598825 m tall; use visual-child
scale `1.8 / 5.598825` (approximately `0.321496`) for a 1.8 m character. Apply this
through `world.set_transform`, not by changing importer units or skinning math.
Check the child local scale, rendered size against the physical capsule, and
foot alignment in Idle/Walk/Run. Exercise undo/redo and save/reload; merely reading
the Inspector's stored world scale does not prove the rendered size is correct.

Verified on 2026-09-27: the three new hierarchy regressions failed before the fix
and passed afterward. All 17 focused engine checks (including 512 children across
four worker-propagated frames) and seven editor transaction checks passed.
The aggregate CTest entries `Engine.SpatialUnitTests` and `Editor.ExportUnitTests`
passed: 2,480 engine tests and 1,016 editor tests, with three editor checks skipped
because external validation projects were absent. The normal aggregate engine
exclusion for the separately registered 30,000-frame profiler gate was retained.
Build and install succeeded; logs are `out/alignment-{final-build,history-build,
install,ctest}.log` and `out/alignment-focused-{engine,editor}.log`.

The installed navigation-demo scene was corrected and saved. Live Vulkan
off-screen readbacks (not fallback images) verified the size change, Idle, Walk
at 1.49998 m/s, Run at 3.999996 m/s, and stopped speed zero, with grounding retained.
World/local visual scale both saved as 0.321496; the root stayed unit-scaled and
the capsule stayed 1.8 m tall. MCP undo/redo succeeded, and exiting Play restored
the authored state. Windows was locked, so native Inspector/overlay inspection
was unavailable; off-screen captures exclude editor overlays. A small Run-pose
foot dip remains animation/foot-placement polish, not a reason to raise the
entire visual and misalign Idle. An explicit scene-file reopen was not performed:
MCP requested approval; automated serialization round-trip coverage passed.

## Movement inspector usability

Build `TerraEngine-Editor` and `TerraEngine-EditorTests` using the RelWithDebInfo
workflow below, then run `ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo
-R 'Editor.Locomotion|Editor.Navigation' --output-on-failure` and install with
`cmake --install out/build/RelWithDebInfo --config RelWithDebInfo` (close the editor first).

In the installed editor, select the navigation-demo knight. Both movement
inspectors should show Essentials, with Advanced and read-only Runtime Diagnostics
collapsed initially. Navigation dimensions describe route clearance, not a capsule
resize; edit the physical capsule in Character Controller. Hover labels and inputs
for units and usage guidance. Check labels and inputs in a narrow Inspector.
Character Controller, Navigation Agent, and Character Locomotion properties use
single-line rows with the label on the left and the value/control on the right.
Long labels clip rather than wrap; widen the Inspector or hover for guidance.
Expand Advanced to check profile sampling/replanning and animation connections.
Keep Current Direction disables turn rate; Animator None disables parameter names
without clearing them. Select Descendant UUID, enter a child UUID and press Enter;
invalid input must not replace the saved binding. Confirm a valid edit and undo/redo,
including multiple selected characters. Opening sections alone must not dirty the scene.

The source-wiring test `Editor.Locomotion.Authoring.InspectorProgressiveDisclosure`
guards default-collapsed sections and conditional controls; existing authoring tests
cover atomic multi-edits and undo/redo. It does not replace the live layout check.

Verified on 2026-09-23: editor/test targets compiled, install succeeded, all six
Navigation/Locomotion CTest groups and 12 `Editor.Inspector`-filtered native tests
passed. In the installed navigation demo, both sections began collapsed, navigation
advanced fields and locomotion connections expanded correctly, and wrapped help
displayed at the default narrow width. Selecting Keep Current Direction disabled
turn rate; undo restored Face Movement and its enabled turn-rate control. No scene
save was performed. Animator-None/UUID entry and multi-selection interactions were
not exercised live in this pass. Logs: `out/inspector-usability-{build,install,tests}.log`
and `out/inspector-usability-inspector-tests.log`.

## FBX absolute animation poses

Build `TerraEngine-Editor` and `TerraEngine-EditorTests`, then run:

```powershell
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R 'Editor.Animation' --output-on-failure
cmake --install out/build/RelWithDebInfo --config RelWithDebInfo
```

`Editor.Animation.FbxImport.PreservesAbsoluteAnimationRotations` compares
imported rotations against an independent Assimp read of the vendored animated
FBX fixture. Its exported node pose differs from the skin bind pose. Absolute
animation keys must remain absolute; multiplying them by a bind/node-pose
correction incorrectly turns resting poses toward the mesh's T-pose. The
existing authored-bind regression independently preserves inverse bind data.
Importer recipe 7 invalidates clips cooked with that incorrect correction.

For live verification, restart the installed editor and reimport the original
Quaternius knight FBX in the navigation demo. Check lowered arms in Idle and
arm swings in Walking/Run, along with the measured-speed controller state log.
Do not equate successful state changes with correct skin poses. The separate
parent-relative visual-scale issue is not covered by this animation fix.

## Live mesh component edits

After the normal RelWithDebInfo build and install below, run:

```powershell
$env:TERRAENGINE_TEST_FILTER = 'Editor.TransactionalMutation,Editor.Inspector'
& ./out/build/RelWithDebInfo/TerraEngine-EditorTests.exe
$env:TERRAENGINE_TEST_FILTER = 'Engine.SceneChangePacket'
& ./out/build/RelWithDebInfo/TerraEngine-EngineTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

`MeshInspectorLifecyclePublishesRenderChanges` exercises the same commands as
the Inspector and checks the renderer-consumed scene journal after add, mesh and
material assignment, removal, undo and redo. The Inspector wiring regression
prevents raw registry edits from bypassing those notifications. In a disposable
editor scene, add Mesh Render to an object, assign valid mesh/material assets,
and verify it appears without reopening. Remove the component and undo/redo:
visibility must follow the component, and undo removal must restore both asset
references. Saving the scene is not required to refresh rendering.

`LiveCacheRejectsEditsAfterPublication` covers edits made after the frame packet
was published but before protected render capture. Consecutive packet numbers
alone do not prove that a packet includes the live component revision. The
renderer must rebuild its membership cache when unpublished changes remain,
while preserving the incremental path for a complete, consecutive publication.

Verified on 2026-09-23 in the installed editor with
`ProjectCreationRetest-20260916`: Inspector removal hid the red obstacle, undo
restored it, and redo hid it again. A fresh Inspector-added component rendered
after separate MCP mesh/material assignments. Ten MCP removal/undo cycles each
changed Scene-view visible command counts from 4 to 3 and back to 4. All scene
edits were undone without saving. Evidence: `out/mesh-packet-live-cycles.log`,
`out/mesh-packet-native-tests.log` (345 tests), and
`out/mesh-packet-editor-tests.log` (complete editor suite). Build and install
logs are `out/mesh-packet-build.log` and `out/mesh-packet-install.log`.

## Scene View click selection

Build and install the editor and `TerraEngine-EditorTests` with the preset below.
Run the focused picking/overlay regressions from the build directory:

```powershell
$env:TERRAENGINE_TEST_FILTER = 'SceneViewOverlay'
& ./out/build/RelWithDebInfo/TerraEngine-EditorTests.exe
$env:TERRAENGINE_TEST_FILTER = 'Renderer.VulkanRecording'
& ./out/build/RelWithDebInfo/TerraEngine-EngineTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

In the installed editor, single-click visible meshes near both the top and bottom
of Scene View. Each click must select the clicked object after its GPU readback,
without a second click. Click empty space to clear selection. While a readback is
pending, a newer click, Hierarchy selection, camera move, viewport resize, or scene
replacement must invalidate it rather than selecting an outdated target. Repeat
after Play/Stop. The renderer's qualified GPU identity remains authoritative;
projected bounding boxes are not substituted for visible-surface selection.
Include a small/distant virtual-geometry object using compute raster as well as
a close-up mesh: both routes must publish the object's nonzero identity key.

Verified on 2026-09-18 in `ProjectCreationRetest-20260916`: single viewport clicks
selected both ducks and the red obstacle, empty-space selection cleared, and the
distant duck remained selectable after Play/Stop. Build/install evidence is in
`out/scene-picking-build.log`; all 30 overlay tests and 27 Vulkan recording tests
passed (`out/scene-picking-tests.log`, `out/scene-picking-renderer-tests.log`).

## Live navigation regression checks

After building and installing with the preset below, run the focused native
navigation matrix and editor authoring/logging checks:

```powershell
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R 'Navigation|Editor.Animation' --output-on-failure
$env:TERRAENGINE_TEST_FILTER = 'Editor.Automation.Service,Editor.MCP.StructuredLogs'
& ./out/build/RelWithDebInfo/TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

For the interactive check, create terrain with published collision, place an
ordinary scripted object and a target on opposite sides of a static box collider,
and request a route using an explicit non-default profile. Verify copied route
points avoid the box and the script reaches its target. Stop and repeat Play
without restarting the editor; terrain availability must recover after physics
republishes its retained recipe. Confirm script messages appear in the MCP log
query as well as the editor console. Open Scheduler Diagnostics at both a small
and maximized editor size: its title bar must remain on-screen and long table
columns must remain readable via horizontal scrolling.

The focused editor checks also cover an externally rewritten scene reopened at
the same path, unsupported component-property writes rejected without mutation,
and managed native-export logs reaching the editor's engine sink.

### Collision-safe route following

Use `Tests/Fixtures/Navigation/NavigationCollisionLiveTest.cs` as the reproducible
live script (save it as `NavigationLiveTest.cs` in the test project's Assets).
Its header specifies the terrain, controller, target, and kinematic box setup.
Disable the duck's separate collider: the character controller supplies its
collision shape. The target is a destination marker, not a blocking collider.
The box must be nonstatic so it is not baked into the planned route.

Press Play. The box enters the already-planned path, the duck must remain on the
near side for at least 0.8 seconds, and after the box clears the duck must resume.
A second controller-only duck then blocks the path until it moves away; only
then should the follower reach the destination. Require `NAVCOLL PASS held before moving blocker`,
`NAVCOLL PASS held before character`, and `NAVCOLL PASS resumed and arrived`,
with no `NAVCOLL FAIL`. Repeat Play without
restarting. Movement must use `NavigationRouteFollower`/`CharacterController`,
never direct actor Transform writes. The fixture's kinematic-body commands and
controller commands are resolved at protected physics boundaries.

The fixture also logs scene and physics positions every five seconds. They must
track one another (apart from the declared publication boundary), and repeated
render-frame velocity commands must not multiply movement speed: characters
advance on fixed physics ticks, not once per command.

Also compare the rendered ducks before Play and after controller initialization.
Both imported meshes must remain visible at their authored `.01` scale while
moving. The fixture checks that scale on every update: position-only diagnostics
cannot detect an accidentally reset mesh scale or prove correct rendering.

```powershell
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R 'Navigation|Engine.PhysicsConstraintsAndCharacters|Engine.PhysicsManaged' --output-on-failure
```

## Installed navigation acceptance

Navigation acceptance is an installed-only gate. From a clean checkout,
configure, build the engine/editor/export and their tests, and install before
running the matrix. The runner is copied into the install tree; its manifest,
editor/export executables, and acceptance executable are required inputs.

```powershell
git submodule update --init --recursive
cmake --fresh --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target `
    TerraEngine TerraEngine-Editor TerraEngine-EngineTests TerraEngine-EditorTests TerraEngine-Export
cmake --build --preset RelWithDebInfo --target install

$installRoot = (Resolve-Path 'out/build/RelWithDebInfo/Build').Path
$navigationTool = Join-Path $installRoot 'Tests/Tools/Navigation/RunInstalledNavigationAcceptance.ps1'
$navigationEvidence = [IO.Path]::GetFullPath('out/evidence/navigation')
```

### Optional navigation-agent application qualification

Use PowerShell 7 and the installed optional-agent runner. Unlike the
older standalone runner below, this starts the actual editor in Play Mode and
the exported project's launcher. Rendered lanes must prove renderer execution;
the headless lane executes the normal engine scheduler without a render device.
Run it from a Visual Studio x64 developer environment with MSVC, the Windows
SDK, and Windows system tools on PATH (`where.exe` and `link.exe` must resolve).
The export uses the installed bundled .NET toolchain and offline package feed.
The editor must be closed before complete editor tests or installation, because
its MCP listener and loaded DLLs otherwise conflict with those operations.

```powershell
$agentRunner = Join-Path $installRoot 'Tests/Tools/Navigation/RunInstalledNavigationAgentAcceptance.ps1'
$agentEvidence = [IO.Path]::GetFullPath('out/evidence/navigation/059-agent-components')
& $agentRunner -InstallRoot $installRoot -OutputDirectory $agentEvidence `
    -Lanes editor,export-rendered,export-headless -Cohorts direct,agent `
    -Population 1000 -WorkerCount 8 -WarmupSeconds 30 -MeasurementSeconds 600 -Qualification
```

The two headless cohorts each measure 600 real wall-clock seconds after warmup.
Editor/rendered lanes run the same generated project for semantic checks, not
performance comparisons. Runs are sequential. Short `-MeasurementSeconds`
values without `-Qualification` are smoke tests and cannot qualify performance or soak acceptance.
Five-second (or similarly short) diagnostic runs are useful for startup,
publication, cache-status, and wiring evidence only; they do not substitute for
the required 600-second wall-clock qualification or prove moving-blocker
coverage churn. Keep their reports separate from qualification evidence.
The margin-safe `navigation-agent-workload-v2` manifest is
`Tests/Fixtures/Navigation/AgentWorkload/manifest.json`, SHA-256
`CBC042C1FF2231AC252123611B5620590D886D98B472AFCA5E7E75887FE37995`.
It pins seed 59001, terrain/obstacles, endpoint pairs, 100 changes/s and one
moving blocker update/s. Evidence records actual CPU, RAM, OS, power scheme,
worker count, installed engine hash, raw child-process logs and reports.
The process runner also samples OS-reported working-set and paged-memory peaks;
these are separate from the native navigation-owned allocation estimate.
The `dynamicTileReuse` report separates incremental attempts, reused tiles,
rebuilt tiles, and full-shard fallbacks. These count prepared work observed at
publication, not successful root commits. A fallback is a correctness safeguard
when immutable tile evidence is insufficient; inspect its recorded reason rather
than treating every fallback as an error. Root-transition and async-build
counters help distinguish search CPU time from coverage-recovery waiting.
`asyncBuildObservations.timings` records baseline and last-observed cumulative
ground/dynamic completion counts, worker execution time, admission-to-worker wait,
and worker-completion-to-protected-poll wait. Totals include failed and retired
packets; subtract total baselines to isolate the measured interval. Maxima are
cumulative (often dominated by startup), so do not subtract them or label them
dynamic-only. Protected polling is not necessarily the root-publication instant.
Provider-build wait is not route-request wait. `routeStageTimingObservations`
separately records ready-admission-to-capture, worker search,
worker-completion-to-publication, and deferred-coverage wait. Their populations
differ: captured requests, executed searches, successfully stored routes, and
requests promoted from deferred coverage respectively. Count/sum deltas include
measurement and drain; maxima remain cumulative and must not be subtracted.
These stage diagnostics do not change the end-to-end latency denominator or gate.
The reference target is Windows x64 RelWithDebInfo on i9-14900HX with eight
workers; results must not infer mains power or other unobserved metadata.

Do not equate a successful archive/codec test with cached runtime acceleration.
The installed agent runner first verifies each generated scene's sky/light
ownership, camera, terrain material, and cohort population. This authored-data
check is not a pixel test: inspect the actual exported window separately.
`-Qualification` also requests the cache parity matrix; `-CacheParity` requests
that matrix without a performance claim. It captures genuine published coverage
before blocker motion, then exports current, stale and truncated cache variants
through `Assets/Navigation/ground.tenav`. Every requested application lane must
prove current-cache use, explicit stale/malformed rejection with fresh fallback,
and identical per-query warmup reachability. Cache fingerprints are checked
against runtime-captured geometry and registered profiles, not trusted manifest
claims. Authoring tests separately prove that caches never restore agent intent.

`-Scenarios clearance-parity` selects a separate low-roof semantic fixture without
changing the pinned performance scene. It runs small and large profiles against
absent, current, stale, and malformed coverage, using each profile's own generated
cache and comparing exact outcome and reason across the requested editor/export
lanes. Stale/malformed input must be explicitly rejected. Qualification includes this
matrix automatically. Small-profile completion and large-profile rejection must
both be observed; an identical failure for both profiles is not a parity pass.

The completed 2026-09-19 run `navigation-20260919-101315-261` records 48 passing
application/cache/profile combinations and 240 semantic checks. Its two 600-second
headless cohorts each admitted/finalized 60,000 requests: direct p95 31.2581 ms,
agent p95 36.8347 ms, with zero residual runtime records. This is measured evidence
on the recorded machine, not a guarantee for arbitrary scenes or hardware.
Retain raw logs, hashes, final build gates, and actual UI/MCP managed-outcome
verification with each qualification run.

### Navigation hard-break qualification (historical standalone runner)

Run the focused native navigation tests, then the complete suite and mandatory
install gate. Use a Visual Studio developer environment with MSVC and the
Windows SDK available.

```powershell
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R 'Navigation|BulkScenePublication' --output-on-failure
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -j 1 --output-on-failure
cmake --build --preset RelWithDebInfo --target install
$audit = Join-Path $installRoot 'Tests/Tools/Navigation/VerifyNavigationArchitecture.ps1'
& $audit -Root $installRoot
if ($LASTEXITCODE -ne 0) { throw 'Installed navigation architecture audit failed.' }
```

Capture five baseline and five candidate runs with the installed acceptance
runner. Keep evidence outside both install trees. Every run within one cohort
must share its build hash; the baseline and candidate build hashes must differ.
The two cohorts must match on source/settings/workload hashes, renderer, power
mode, worker count, lane, and workload so the code revision is the only intended
comparison variable.

```powershell
$baselineInstallRoot = (Resolve-Path 'out/prechange-baseline-worktree/out/build/RelWithDebInfo/Build').Path
$candidateInstallRoot = $installRoot
$baselineTool = Join-Path $baselineInstallRoot 'Tests/Tools/Navigation/RunInstalledNavigationAcceptance.ps1'
$candidateTool = Join-Path $candidateInstallRoot 'Tests/Tools/Navigation/RunInstalledNavigationAcceptance.ps1'
$baseline = Join-Path $navigationEvidence '058-remove-navigation-components/baseline'
$candidate = Join-Path $navigationEvidence '058-remove-navigation-components/candidate'
1..5 | ForEach-Object {
    & $baselineTool -InstallRoot $baselineInstallRoot -OutputDirectory (Join-Path $baseline ("qualification-{0:D2}" -f $_)) `
        -Lanes export-rendered -Matrices performance -DurationMinutes 5 -Qualification
    if ($LASTEXITCODE -ne 0) { throw "Baseline run $_ failed." }
}
1..5 | ForEach-Object {
    & $candidateTool -InstallRoot $candidateInstallRoot -OutputDirectory (Join-Path $candidate ("qualification-{0:D2}" -f $_)) `
        -Lanes export-rendered -Matrices performance -DurationMinutes 5 -Qualification
    if ($LASTEXITCODE -ne 0) { throw "Candidate run $_ failed." }
}
$baselineResults = 1..5 | ForEach-Object { Join-Path $baseline ("qualification-{0:D2}/work/export-rendered/performance/result.json" -f $_) }
$candidateResults = 1..5 | ForEach-Object { Join-Path $candidate ("qualification-{0:D2}/work/export-rendered/performance/result.json" -f $_) }
& (Join-Path $candidateInstallRoot 'Tests/Tools/Navigation/CompareNavigationEvidence.ps1') `
    -BaselineFiles $baselineResults -CandidateFiles $candidateResults -MinimumRuns 5 `
    -OutputPath (Join-Path $navigationEvidence '058-remove-navigation-components/comparison.json')
```

The comparator fails closed when either side has fewer than five valid runs, a
metric or identity is missing, or identity matching fails. Candidate median
route latency and dynamic-publication responsiveness must each be no greater
than `baseline median * 1.05`. Missing dynamic timing is unavailable evidence,
never zero.

Use `-DryRun` first to validate installed discovery without launching a lane.
Run the editor, rendered-export, and headless-export semantic flows together so
the comparator can enforce parity; a source checkout or build-tree executable
must not be substituted.

```powershell
& $navigationTool -InstallRoot $installRoot -OutputDirectory (Join-Path $navigationEvidence 'semantic') `
    -Lanes @('editor', 'export-rendered', 'export-headless') -Matrices semantic
```

The installed-only audit can validate all three lane packages together without
launching runtime work:

```powershell
& $navigationTool -InstallRoot $installRoot -OutputDirectory (Join-Path $navigationEvidence 'installed-audit') `
    -Lanes @('editor', 'export-rendered', 'export-headless') -Matrices @('semantic') -DryRun
```

Run the remaining installed matrices as separate evidence sets. Runtime
navigation generation is mandatory; caches are optional accelerators and their
absence must not invalidate a valid generation path.

```powershell
& $navigationTool -InstallRoot $installRoot -OutputDirectory (Join-Path $navigationEvidence 'performance') -Lanes export-rendered -Matrices performance -DurationMinutes 5 -Qualification
& $navigationTool -InstallRoot $installRoot -OutputDirectory (Join-Path $navigationEvidence 'idle-overload') -Lanes export-rendered -Matrices idle-overload -DurationMinutes 5 -IdlePairs 5 -Qualification
& $navigationTool -InstallRoot $installRoot -OutputDirectory (Join-Path $navigationEvidence 'soak-30m') -Lanes @('export-rendered', 'export-headless') -Matrices soak -DurationMinutes 30 -Qualification
& $navigationTool -InstallRoot $installRoot -OutputDirectory (Join-Path $navigationEvidence 'lifecycle-100') -Lanes editor -Matrices lifecycle -Cycles 100 -Qualification
& $navigationTool -InstallRoot $installRoot -OutputDirectory (Join-Path $navigationEvidence 'usability') -Lanes editor -Matrices usability -UsabilityInvalidCases 5 -Qualification
```

`-DurationSecondsOverride` is only for bounded smoke checks and cannot satisfy
the 30-minute soak requirement. Each invocation preserves
`navigation-acceptance-result.json` plus per-lane raw stdout, stderr, and
result JSON beneath its contained work directory. Any missing artifact,
nonzero lane, missing result, or unavailable measurement is a failed gate.
Retain the complete `out/evidence/navigation` directory and audit the exact
installed bytes used by the run.

## External memory benchmark

The memory qualification work uses a standalone benchmark workspace;
it is not part of the engine CMake
graph and is not installed with TerraEngine. Rejected custom memory tests are
removed with their production candidates; the normal engine/editor suites remain
the correctness gate for the candidate-free production tree.

From a clean checkout, build and run the production tests, then produce the
mandatory install tree:

```powershell
cmake --fresh --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target `
	TerraEngine TerraEngine-Editor TerraEngine-EngineTests TerraEngine-EditorTests
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo --output-on-failure
cmake --build --preset RelWithDebInfo --target install
```

This repository installs to `<build directory>/Build`; `--prefix` does not
redirect that production tree. Build and self-test the independent benchmark
against the exact installed bytes. Set `TERRAENGINE_MEMORY_BENCHMARK_ROOT` to
the standalone benchmark directory containing its `CMakeLists.txt` and README:

```powershell
$benchmarkRoot = (Resolve-Path $env:TERRAENGINE_MEMORY_BENCHMARK_ROOT).Path
$benchmarkBuild = Join-Path $benchmarkRoot 'out/build/RelWithDebInfo'
$installRoot = (Resolve-Path 'out/build/RelWithDebInfo/Build').Path
cmake -S $benchmarkRoot -B $benchmarkBuild -DTERRAENGINE_INSTALL_ROOT=$installRoot
cmake --build $benchmarkBuild --config RelWithDebInfo
ctest --test-dir $benchmarkBuild -C RelWithDebInfo --output-on-failure
```

Capture commands, identity checks, A/A and A/B minimums, exit classifications,
and soak gates are documented in the standalone benchmark README. Duration or
warmup overrides create a non-admissible tooling probe and cannot freeze a
baseline. An unavailable production metric is recorded with a reason and is
never interpreted as zero.

## Shared utilities and TEPAK validation

The engine utility tests cover the fixed-capacity ring container and the shared
TEPAK binary/worker helpers. Configure from a clean checkout, build the native
engine test target, run the focused registrations, and install the same build:

```powershell
cmake --fresh --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine-EngineTests TerraEngine-Export
$env:TERRAENGINE_TEST_FILTER = "Engine.Utilities;Engine.BinaryIO;Engine.WorkerPool"
& .\out\build\RelWithDebInfo\TerraEngine-EngineTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
cmake --build --preset RelWithDebInfo --target install
```

The filter is an iteration aid. The complete CTest run remains the final native
regression gate because export/runtime archive code is linked into several tools.

## Physics optimization evidence

The physics performance workflow is install-tree based. Configure and install a
`RelWithDebInfo` build, then run capture against the repository-owned workload
manifest. `-DryRun` validates discovery without claiming a baseline.

```powershell
cmake --fresh --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target install
& .\Tests\Tools\Physics\CapturePhysicsOptimizationBenchmark.ps1 -InstallRoot .\out\build\RelWithDebInfo\Build `
	-Manifest .\Tests\Fixtures\Performance\Physics\workloads.json `
	-OutputDirectory .\out\evidence\physics-optimization\candidate -Runs 5 -DryRun
```

Use the comparator for baseline/candidate evidence; unavailable or incomparable
results must remain failures rather than being interpreted as zero regressions.

The complete native and installed acceptance sequence is:

```powershell
cmake --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine TerraEngine-Editor `
	TerraEnginePhysicsColliderCompiler TerraEngineModelPackageCompiler `
	TerraEngine-EngineTests TerraEngine-EditorTests TerraEngine-Export
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
	-R "Physics|TransactionalMutation|SystemScheduler|Editor\.Automation|Editor\.Export|SceneRuntimeParity" --output-on-failure
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo --output-on-failure
cmake --build --preset RelWithDebInfo --target install
& .\out\build\RelWithDebInfo\Build\Tests\Tools\Physics\TerraEnginePhysicsGameplayStress.exe
& .\out\build\RelWithDebInfo\Build\Tests\Tools\Physics\TerraEnginePhysicsConstraintCharacterStress.exe
& .\Tests\Tools\Physics\RunInstalledEditorPhysicsStress.ps1 `
	-InstallRoot .\out\build\RelWithDebInfo\Build `
	-WorkRoot .\out\validation\physics-optimization\installed `
	-RunWorkerScalingMatrix -RunProfilerCaptureChecks -CaptureLevel detailed `
	-CaptureDurationSeconds 5 -AllowLiveStress
```

The worker matrix is an acceptance gate: the median engine-measured simulation
stage at four workers must be at least 1.5x faster than one worker on the
10,000-body/8,000-active large leg, and the maximum-worker leg must be no more
than 10% slower than four. Other fixed-step stages remain separate mixed-workload
regression evidence. The script exits nonzero when either limit, deterministic
hashes, stage-sample coverage, or any child run fails.

## Production virtual-geometry gates

From a clean checkout, compile and run the focused contract registrations before
installing. The install target compiles the Vulkan SPIR-V set and runs a final
audit of required tools, scenarios, fixtures, shaders, and production virtual-geometry docs.

```powershell
git submodule update --init --recursive
cmake --fresh --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target `
	TerraEngine TerraEngine-Editor TerraEngine-EngineTests TerraEngine-EditorTests `
	TerraEngineVirtualGeometryAssetTool TerraEngineVirtualGeometryBenchmark `
	TerraEngineVirtualGeometryCodecExperiment
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
	-R "^(Engine|Editor)\.VirtualGeometryProductionContracts$" `
	--output-on-failure
cmake --build --preset RelWithDebInfo --target install
```

Resolve every dry-run input from the installed tree:

```powershell
$installRoot = (Resolve-Path 'out/build/RelWithDebInfo/Build').Path
$vgTools = Join-Path $installRoot 'Tests/Tools/VirtualGeometry'
$vgRunner = Join-Path $installRoot 'Tools/TerraEngineVirtualGeometryBenchmark.exe'
$vgAdapter = Join-Path $vgTools 'VirtualGeometryRuntimeAdapter.cmd'
$vgWorkloads = Join-Path $vgTools 'workloads.json'
$vgMatrix = Join-Path $vgTools 'hardware-matrix.json'
$vgQuality = Join-Path $vgTools 'quality-profiles.json'
$vgEvidence = [IO.Path]::GetFullPath('out/evidence/virtual-geometry/install-dry-run')

& (Join-Path $vgTools 'RunCodecExperiment.ps1') `
	-InputMesh (Join-Path $installRoot 'Tests/Fixtures/Animation/ImportValidation/Assets/Models/ImportHero.gltf') `
	-ExperimentExecutable (Join-Path $installRoot 'Tools/TerraEngineVirtualGeometryCodecExperiment.exe') `
	-OutputDirectory (Join-Path $vgEvidence 'codec') -DryRun
& (Join-Path $vgTools 'CaptureVirtualGeometryPerformance.ps1') `
	-BenchmarkExecutable $vgRunner -Workloads $vgWorkloads -Matrix $vgMatrix `
	-Quality $vgQuality -Output (Join-Path $vgEvidence 'capture.json') -DryRun
& (Join-Path $vgTools 'RunVirtualGeometryCapabilityGate.ps1') `
	-ScenarioDirectory (Join-Path $vgTools 'CapabilityScenarios') `
	-OutputDirectory (Join-Path $vgEvidence 'production-gate') -DryRun
& (Join-Path $vgTools 'RunBackendParityMatrix.ps1') `
	-Runner $vgRunner -Workloads $vgWorkloads -Matrix $vgMatrix `
	-Quality $vgQuality -Output (Join-Path $vgEvidence 'backend-parity.json') -DryRun
& (Join-Path $vgTools 'RunAnimatedVirtualGeometryBenchmark.ps1') `
	-Runner $vgRunner -RuntimeAdapter $vgAdapter -Workloads $vgWorkloads `
	-Matrix $vgMatrix -Quality $vgQuality `
	-Output (Join-Path $vgEvidence 'animated.json') -DryRun
& (Join-Path $vgTools 'RunVirtualGeometryStreamingSoak.ps1') `
	-Runner $vgRunner -RuntimeAdapter $vgAdapter -Workloads $vgWorkloads `
	-Matrix $vgMatrix -Quality $vgQuality `
	-Output (Join-Path $vgEvidence 'streaming-soak.json') -DryRun
& (Join-Path $vgTools 'RunVirtualGeometryProductionSoak.ps1') `
	-Runner $vgRunner -RuntimeAdapter $vgAdapter -Workloads $vgWorkloads `
	-Matrix $vgMatrix -Quality $vgQuality `
	-Output (Join-Path $vgEvidence 'production-soak.json') -DryRun
```

Non-dry runs fail for missing executables, manifests, images, or raw captures.
Performance comparison requires at least five comparable measured runs; image
comparison records both hashes. Unattached AMD/NVIDIA/Intel Vulkan hardware
remains `unavailable`, never a pass. The production virtual-geometry installed matrix is
Vulkan-only; D3D12 and DXIL are out of scope. Soak acceptance requires bounded
duration, raw counters, high-water/bounded-growth data, command/environment,
and artifact hashes; missing data is not zero. Installed automation commands
(`asset.virtualGeometry.inspect`, `.settings.get`, `.rebuild`, and
`renderer.virtualGeometry.stats`) are read-only/reporting contracts unless an
explicit mutation command is requested, and canceled/failed repairs retain
last-known-good output. Packaging/compatibility claims require installed
artifacts, source/license provenance, and explicit fallback diagnostics.
For individual capability scenarios, use the installed-tree `CapabilityScenarios/`
manifests and the production gate command above.

This document is the reproducible CTest workflow for the current unit test
layout. Engine tests live under `Tests/Engine`; editor tests live under
`Tests/Editor`. Launcher tests are intentionally out of scope for the current
test feature. The engine suite includes a headless exported-runtime smoke
fixture that writes a temporary TEPAK archive and verifies manifest, scene, and
resource reads through the mounted content path.

The export runtime baseline checks also validate
`Tests/Fixtures/ExportRuntimeBaseline` as a stable sample surface. These tests assert
that the sample project keeps its startup scene, project render settings, UI
signal, and script signal wired for manual and CI export coverage.

## Repository-contained managed restore

TerraEngine's Windows managed targets restore only from the audited package
archives in `.nuget/feed`. CMake regenerates missing or hash-mismatched
archives from the HTTPS locations and SHA-256 values in the tracked
`cmake/TerraEngineNuGet.lock.json` lock. Root `NuGet.Config` clears user, machine,
fallback, network, and audit sources; CMake also supplies the absolute feed path
and a build-tree package cache. After the single managed restore target runs,
the engine assembly, source generator, and editor assembly build with
`--no-restore`.

The current closure is pinned to .NET SDK `10.0.303`, runtime pack `10.0.11`,
and RID `win-x64`. CMake downloads the SDK archive selected by
`cmake/TerraEngineDotNetSdk.lock.json`, verifies its SHA-512, and extracts it to
`External/dotnet/sdk/<rid>/<version>`. Build commands always use that absolute
executable; an SDK installed on PATH is ignored. Confirm the repository-local
SDK and verify every package archive, license, and manifest entry after
configuring:

```powershell
cmake --fresh --preset RelWithDebInfo
$dotnet = "External/dotnet/sdk/win-x64/10.0.303/dotnet.exe"
& $dotnet --version
$archives = @(
	Get-ChildItem .nuget/feed/*.nupkg |
	Select-Object -ExpandProperty FullName
)
$env:NUGET_CERT_REVOCATION_MODE = "offline"
& $dotnet nuget verify @archives --all --verbosity minimal
Remove-Item Env:NUGET_CERT_REVOCATION_MODE
cmake -DTERRAENGINE_RUNTIME_ID=win-x64 `
	-P cmake/TerraEngineOfflineNuGet.cmake
```

The SDK version command must print `10.0.303`. NuGet verifies each archive's
available author and repository signatures without an online revocation
lookup. The CMake verifier rejects changed or unmanifested packages, mismatched
SDK metadata, and changed legal files. Set
`-DTERRAENGINE_DOTNET_SDK_MODE=OFF` together with
`-DTERRAENGINE_NUGET_CACHE_MODE=OFF` when network access is prohibited; this
keeps both dependency boundaries fail-closed instead of downloading a missing
SDK or regenerating a missing package archive. `global.json` and
`cmake/TerraEngineDotNetSdk.lock.json` must always select the same SDK version.
`cmake/TerraEngineNuGet.lock.json` records the complete project and NativeAOT
export dependency closure with original package provenance. The fourth project is the
`netstandard2.0` target of the vendored Hexa.NET.ImGui project referenced by the
editor.

CMake treats both `.nuget` and `<build>/NuGetPackages` as generated caches.
Configure repairs missing or damaged pinned feed archives and recreates the
generated manifest, restore config, notices, and legal files from the tracked
lock. The first managed build regenerates its isolated extracted-package cache
from that verified feed. Later builds skip restore while its stamp, all 25
package metadata files, the feed archives, restore configuration, SDK pin, and
participating project files remain valid. Deleting either complete cache or
any package metadata file causes CMake to regenerate it. CMake verifies the
SHA-512 of every restored archive before publishing with `--no-restore`; NuGet
itself never falls back to the user's package cache or a network source.

For a normal clean Windows build, use the existing preset. The managed restore
target runs automatically before any managed publish, but it can also be
exercised independently:

```powershell
cmake --fresh --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo `
	--target TerraEngine.Managed.Restore
cmake --build --preset RelWithDebInfo
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
	--output-on-failure
cmake --build --preset RelWithDebInfo --target install
```

## Virtual Geometry production evidence

Validate the setup manifests without launching a renderer:

```powershell
$installRoot = (Resolve-Path 'out/build/RelWithDebInfo/Build').Path
$vgTools = Join-Path $installRoot 'Tests/Tools/VirtualGeometry'
$vgRunner = Join-Path $installRoot 'Tools/TerraEngineVirtualGeometryBenchmark.exe'
& (Join-Path $vgTools 'CaptureVirtualGeometryPerformance.ps1') `
	-BenchmarkExecutable $vgRunner `
	-Workloads (Join-Path $vgTools 'workloads.json') `
	-Matrix (Join-Path $vgTools 'hardware-matrix.json') `
	-Quality (Join-Path $vgTools 'quality-profiles.json') -DryRun
```

After the installed benchmark target and a live adapter/readback path are
available, capture the processor-expanded baseline and a candidate into
separate directories. The runner preserves unavailable/pending states and does
not synthesize timing values. Repeat the command at least five times per
directory with distinct output names, then compare the directories:

```powershell
$adapter = Join-Path $vgTools 'VirtualGeometryRuntimeAdapter.cmd'
& $vgRunner `
	--workloads (Join-Path $vgTools 'workloads.json') `
	--matrix (Join-Path $vgTools 'hardware-matrix.json') `
	--quality (Join-Path $vgTools 'quality-profiles.json') `
	--workload 'baseline-processor-expanded' --runtime-adapter $adapter `
	--duration-seconds 30 `
	--output ([IO.Path]::GetFullPath('out/evidence/virtual-geometry/baseline/run-01.json'))
& (Join-Path $vgTools 'CompareVirtualGeometryPerformance.ps1') `
	-BaselineDirectory ([IO.Path]::GetFullPath('out/evidence/virtual-geometry/baseline')) `
	-CandidateDirectory ([IO.Path]::GetFullPath('out/evidence/virtual-geometry/candidate')) `
	-Output ([IO.Path]::GetFullPath('out/evidence/virtual-geometry/comparison.json'))
```

The comparison script exits non-zero for insufficient measured evidence or a
median regression above the configured ratio. A successful script run is not a
substitute for the required build, focused CTest, install, image-parity, and
hardware-matrix gates; link those results from the capability evidence directory.

To prove that no ambient global package cache is involved, use a unique empty
cache beneath `out/` and run the four restores directly. Do not clear or alter
the user's normal NuGet cache:

```powershell
$repo = (Get-Location).Path
$run = Get-Date -Format "yyyyMMdd-HHmmss"
$coldRoot = Join-Path $repo "out/validation/nuget-offline/$run"
$packageCache = Join-Path $coldRoot "packages"
$env:DOTNET_CLI_HOME = Join-Path $coldRoot "dotnet-home"
$env:NUGET_PACKAGES = $packageCache
$env:DOTNET_CLI_TELEMETRY_OPTOUT = "1"
$env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE = "1"
$env:DOTNET_ADD_GLOBAL_TOOLS_TO_PATH = "0"
$env:DOTNET_NOLOGO = "1"
$env:NUGET_XMLDOC_MODE = "skip"
$env:NUGET_CERT_REVOCATION_MODE = "offline"
$feed = Join-Path $repo ".nuget/feed"
$config = Join-Path $repo "NuGet.Config"
$common = @(
	"--configfile", $config,
	"--packages", $packageCache,
	"--no-http-cache",
	"--disable-parallel",
	"--verbosity", "minimal",
	"-p:RestoreSources=$feed",
	"-p:RestoreFallbackFolders=",
	"-p:RestoreAdditionalProjectSources=",
	"-p:RestoreIgnoreFailedSources=false",
	"-p:NuGetAudit=false"
)
$hexaCompatibility = @(
	"-p:IsAotCompatible=false",
	"-p:EnableTrimAnalyzer=false",
	"-p:EnableSingleFileAnalyzer=false",
	"-p:EnableAotAnalyzer=false"
)
dotnet restore Engine/Managed/TerraEngine.csproj `
	--runtime win-x64 -p:PublishAot=true @common
dotnet restore `
	Editor/Managed/TerraEngine.Export.Generator/TerraEngine.Export.Generator.csproj `
	@common
dotnet restore `
	External/Hexa.NET.ImGui/Hexa.NET.ImGui/Hexa.NET.ImGui.csproj `
	-p:TargetFramework=netstandard2.0 --runtime win-x64 `
	@hexaCompatibility @common
dotnet restore `
	Editor/Managed/TerraEngine.Editor/TerraEngine.Editor.csproj `
	--runtime win-x64 --no-dependencies @hexaCompatibility @common
Remove-Item Env:NUGET_PACKAGES
Remove-Item Env:DOTNET_CLI_HOME
Remove-Item Env:DOTNET_CLI_TELEMETRY_OPTOUT
Remove-Item Env:DOTNET_SKIP_FIRST_TIME_EXPERIENCE
Remove-Item Env:DOTNET_ADD_GLOBAL_TOOLS_TO_PATH
Remove-Item Env:DOTNET_NOLOGO
Remove-Item Env:NUGET_XMLDOC_MODE
Remove-Item Env:NUGET_CERT_REVOCATION_MODE
```

Retain the cold cache, restore output, and generated `project.assets.json` files
as evidence. Their package folders must resolve only beneath the selected cold
cache, and their restore sources must resolve only to `.nuget/feed`.

Installed editor export has the same requirement. The install target places a
copy of the verified feed, a fail-closed `NuGet.Config`, and the pinned
`global.json` below `Build/Editor/NativeAotNuGet`. The export pipeline restores
`Assembly-CSharp.csproj` with `PublishAot=true` into
`<project>/Library/NativeAot/NuGetPackages`, then publishes with `--no-restore`.
For a source-free proof, copy the installed `Build` directory outside the
repository and run the Export Runtime Baseline against that copy; no user NuGet
cache or repository feed may be used.

The explicit Hexa restore is part of the dependency boundary, not an
optimization. Auditing only the three TerraEngine assets files reported 21
archives because the editor's existing assets did not prove a cold traversal
of the multi-targeted project reference. An empty-cache restore additionally
requires System.Buffers 4.6.0, System.Memory 4.6.0, System.Numerics.Vectors
4.6.0, and System.Runtime.CompilerServices.Unsafe 6.1.0. It also requires the
pinned Microsoft.NETCore.App, Microsoft.WindowsDesktop.App,
Microsoft.AspNetCore.App, and Microsoft.NETCore.App.NativeAOT `win-x64`
runtime packs that the SDK records as download dependencies. The 25-package
manifest records that empirically verified closure. The compatibility
properties prevent unrelated Hexa target frameworks and trim/AOT analyzer
packages from widening the repository contract.

The committed feed does not yet contain `linux-x64` runtime packs. Linux
configure reports that limitation and managed restore fails locally instead of
falling back to an online feed. Audit and vendor the exact pinned-SDK Linux
closure before treating Linux as repository-contained.

## Animation Registry and Filters

Animation coverage uses the existing engine and editor `TestRegistry` harnesses.
The initial scaffold entries are `Animation.RegistrationAvailable` and
`Editor.Animation.RegistrationAvailable`; later animation cases should keep the
`Animation.` or `Editor.Animation.` prefix so the substring filter remains a
stable focused workflow.

CTest exposes focused `Engine.Animation` and `Editor.Animation` registrations.
Run both from an already configured build tree with:

```powershell
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
	-R "^(Engine\.Animation|Editor\.Animation)$" --output-on-failure
```

For direct harness diagnosis, set the shared substring filter before launching
both binaries and remove it afterward so unrelated test commands are not
silently filtered:

```powershell
$env:TERRAENGINE_TEST_FILTER = "Animation"
& .\out\build\RelWithDebInfo\TerraEngine-EngineTests.exe
& .\out\build\RelWithDebInfo\TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

The focused registrations are iteration aids. Feature and release gates still
run the broader scheduler, transactional mutation, renderer, physics,
scripting, automation, export, and complete CTest surfaces described below.

### Animation validation tools

Use the focused CTest registrations for correctness and the reusable commands in
`Tests/Tools/Animation/` for fixture generation, installed-model checks, performance
capture, comparison, and soak testing. The commands and their reproducible
inputs are documented in `Docs/Animation/README.md` and
`Docs/Animation/Performance.md`.

The scene graph runtime checks validate deterministic traversal, hierarchy
diagnostics, prefab composition, queued mutation safety, and editor/runtime
parity. Manual validation fixtures live under `Tests/Fixtures/SceneGraphRuntime`.
Sky component migration checks live under `Tests/Engine/Scene` with the editor
metadata check under `Tests/Editor/Project`. They validate that sky data saves
through `SkyComponent`, obsolete `sceneSettings.sky` input is ignored, and the
scene-level Project Settings sky category stays removed. For a focused local run
on Windows:

```powershell
$env:TERRAENGINE_TEST_FILTER="Scene.SkyComponent"
.\out\build\Debug\TerraEngine-EngineTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
.\out\build\Debug\TerraEngine-EditorTests.exe
```

The editor harness currently runs the full editor test binary; the sky migration
coverage is reported by the Project Settings tests inside that run.

Native and managed editor windows opt into the `Windows` menu from their own
window class. A native constructor supplies an optional category path, such as
`EditorWindow("Inspector", true, "General")`; managed windows pass the same kind
of path to their base constructor. Slash-separated values create deeper
subcategories, so a future window may choose `Audio/Mixing` without adding that
category to the main menu renderer. Omitting the category keeps the window
registered and drawable but removes it from the menu. The focused
`Editor.WindowMenu` tests verify these declarations, arbitrary path splitting,
and the absence of the old centralized title blacklist.

The scheduler checks cover the phase scheduler, general job graph, and ECS query
task graph layers. The engine suite validates component read/write declarations,
deterministic chunking, deferred scene mutation flushes, transform dependency
levels, and exported runtime scheduler smoke coverage. The editor suite validates
that Play Mode can consume the scheduler summaries while C# authors keep normal
script workflows without adding job or query annotations.

Architecture-hardening coverage extends those scheduler checks with
`FrameWorkload` migration summaries, active membership filtering, compact object
reference invalidation, hierarchy snapshot reuse, and component access grants.
Large-scene regression coverage currently includes 10,000-object active queries,
10,000-object hierarchy snapshot preparation, and 10,000 grant validations. Use
`TERRAENGINE_TEST_FILTER=ArchitectureMigrationSummary;ActiveMembership;CompactObjectReference;HierarchySnapshot;ComponentAccessGrant;SystemScheduler`
for a focused local diagnosis pass before running the complete CTest surface.

Hot runtime data layout coverage lives across `Tests/Engine/Core`,
`Tests/Engine/Scene`, `Tests/Engine/Renderer`, and `Tests/Editor/Scene`. These
checks validate compact active membership snapshots, retained flat hierarchy
layout reuse, transform runtime record publication, render runtime layout
diagnostics, compatibility mismatch fallback reporting, and editor-visible
authored-versus-runtime counts. Use this focused pass while iterating on layout
or cache behavior:

```powershell
$env:TERRAENGINE_TEST_FILTER="RuntimeLayout;ActiveMembership;HierarchyLayout;RenderRuntimeLayout;LayoutCompatibility"
.\out\build\CodexDebug\TerraEngine-EngineTests.exe
.\out\build\CodexDebug\TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

The editor diagnostics window should show the runtime layout section with
authored object count, active runtime count, compact render record count,
rejections, fallbacks, and small-scene guard state. This section deliberately
does not include lighting controls; lighting remains in renderer-specific
diagnostic surfaces.

Active object handle coverage extends the
same pass with compact reference footprint helpers, stale-reference rejection,
runtime membership reuse flags, active authored-state serialization, and
automation fallback counters. Use this tighter filter when changing
`ObjectManager`, `RuntimeLayout`, compact references, or ECS query snapshots:

```powershell
$env:TERRAENGINE_TEST_FILTER="ActiveMembership;RuntimeLayout.ActiveMembership;CompactObjectReference;RuntimeLayout;EcsQuery;Scene.Serialization"
.\out\build\CodexDebug\TerraEngine-EngineTests.exe
.\out\build\CodexDebug\TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

Broad worker conversion coverage lives beside the existing scheduler tests. Use
`TERRAENGINE_TEST_FILTER=WorkerConversion;WorkerCoverage;WorkerHardBoundary;ScriptingWorkerBoundary;LiveGpuSubmissionBoundary;WindowInputWorkerBoundary;Editor.ReflectionWorkerBoundary`
to validate the worker coverage report, hard-boundary classifications, managed
callback split, live GPU submission split, protected window/input polling, and
protected editor reflection mutation. The expected classification is:
managed gameplay callbacks and live GPU submission are partially worker-safe,
while window/input polling and editor reflection mutation stay protected with
worker-readable snapshots or preflight validation around them.

Full parallel frame coverage validates the default job-graph-backed frame path,
the explicit compatibility baseline, ordered access handoffs, small-work inline
thresholds, worker-safety scopes, deterministic mutation buffers, and editor
diagnostic surfacing. Deferred scene mutations are intentionally captured into
per-worker buffers and applied only at scheduler flush points. A rejected batch
must leave earlier requests unapplied; direct ObjectManager/component mutation
inside a worker safety scope should report a safety violation instead of
silently changing live state. For a focused Windows run:

```powershell
$env:TERRAENGINE_TEST_FILTER="FullParallelFrame;FrameWorkload;SystemScheduler;JobGraph;SceneMutationQueue;ParallelMutationBuffer;WorkerHardBoundary;Scripting.Mutation"
.\out\build\CodexDebug\TerraEngine-EngineTests.exe
.\out\build\CodexDebug\TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

Open `Windows > Scheduler Diagnostics` in the installed editor to inspect the
executed frame plan. The worker coverage section should show frame id, execution
mode, parallel flag, worker/protected/inline/rejected/fallback counts, barrier
count, worker/protected timings, worker utilization, fallback reasons, and
hard-boundary classifications. Small scenes may intentionally show inline work;
that is an overhead guard, not a protected fallback. Continue to treat managed
callback invocation, final GPU submit/present, platform event polling, and
editor reflected mutation as protected unless a later test proves a narrower
worker-safe phase.

Heavy subsystem worker conversion coverage is split by boundary. Managed script
batch preparation, immutable render/resource/UI planning, and post-poll
window/input snapshots may run as worker-safe preparation. Managed callback
invocation, swapchain acquire/queue submit/present/fence ownership, platform
event polling, and reflected editor mutation application remain protected
commits. Use this focused pass when changing those paths:

```powershell
$env:TERRAENGINE_TEST_FILTER="Scripting.Workload;RenderWorkerSafety;Renderer.Diagnostics;Resource.Streaming.Policy;UISystemPreparation;WindowInputWorkerBoundary;WorkerHardBoundary;Editor.FullParallelFrame;Editor.Automation.Service"
.\out\build\CodexDebug\TerraEngine-EngineTests.exe
.\out\build\CodexDebug\TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

MCP camera image coverage lives in the editor automation and MCP bridge tests.
Use this focused pass when changing `game_view.capture`,
`camera.capture_object`, `camera.capture_custom`, mixed MCP image content, or
`terraengine://capture/availability`:

```powershell
$env:TERRAENGINE_TEST_FILTER="Capture"
.\out\build\Debug\TerraEngine-EditorTests.exe
$env:TERRAENGINE_TEST_FILTER="Editor.MCP.Contract"
.\out\build\Debug\TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

The Vulkan editor backend now advertises native image readback. It submits
render-target copies to host-visible staging buffers, polls completion fences
without blocking normal frame updates, converts RGBA8/BGRA8/HDR half-float
pixels, and publishes the resulting PNG through the bounded MCP capture queue.
Headless tests still exercise the deterministic fallback because they do not
create a Vulkan device. Run the focused CPU/lifecycle regression slice with:

```powershell
ctest --test-dir out/build/Debug -C Debug -R "Engine.VulkanImageReadback|Engine.RendererRecovery" --output-on-failure
```

Live validation must use the installed editor and a real Vulkan adapter. Enable
Vulkan validation layers and the local MCP bridge in isolated editor
preferences, open a project containing an active camera, call
`game_view.capture` and `camera.capture_custom`, and verify each response carries
non-empty `image/png` content with `backendReadback=true` and
`deterministicFallback=false`. Resize a view between requests and confirm the
editor log has no Vulkan lifetime/layout validation errors.

After code changes, rebuild the Visual Studio-compatible targets and run the
install target so native, managed, editor, launcher, and shader-tool outputs are
verified together:

```powershell
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"" >nul && cmake --build out\build\CodexDebug --target TerraEngine-EngineTests TerraEngine-EditorTests --config Debug"
ctest --test-dir out\build\CodexDebug --output-on-failure -C Debug
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"" >nul && cmake --build out\build\CodexDebug --target install --config Debug"
```

Scene View overlay coverage lives in the
editor harness. It validates the toolbar tool state, snap fallback, marker
projection and deterministic overlap selection, orientation widget labels,
renderer-unavailable diagnostics, preference JSON fallback, icon accessibility
metadata, and Game View independence. Use this focused pass while iterating on
Scene View-only overlay behavior:

```powershell
$env:TERRAENGINE_TEST_FILTER="SceneViewOverlay;SceneViewMarker;SceneViewOrientation;SceneViewPreference;SceneViewInteraction;GameViewIndependence"
.\out\build\CodexDebug\TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

The scripting scheduler checks live under `Tests/Engine/Scripting`. They verify
workload snapshots, worker batch planning, thread-local dispatch context,
diagnostics gating, and deferred script-origin scene mutations. Export scripting
tests also cover the NativeAOT path so editor and exported runtimes keep the same
batching and lifecycle contract.
The scripting runtime performance checks also validate managed lookup cache
generations, typed deferred component writes, JSON fallback compatibility,
same-key mutation coalescing, and diagnostics-only bridge/write counters.
The engine harness also supports an opt-in `TERRAENGINE_TEST_FILTER` environment
variable for local diagnosis, for example `TERRAENGINE_TEST_FILTER=Scripting`
to run only scripting tests. CTest still runs the complete engine and editor
suites by default.

The renderer graph checks live under `Tests/Engine/Renderer`. They validate
immutable frame snapshots, backend-neutral pass/resource contracts, packet
scheduling thresholds, submission-plan rejection for stale or unsafe work,
mock-backend portability, opt-in render diagnostics, backend frame breakdown
categories, render worker task group planning, Vulkan recording-batch planning,
thread-local recording context ownership, and stale backend batch rejection. The
complete renderer-threading checks also cover worker-owned render preparation,

Renderer device-recovery coverage lives in `Tests/Engine/Renderer`,
`Tests/Engine/Platform`, `Tests/Engine/Runtime`, and the editor automation,
profiler, project, and renderer-window suites. These tests validate the
CPU-only recovery snapshot model, scheduler resource declarations, one-incident
device-loss latching, duplicate suppression, no normal GPU submission after the
latch, renderer-independent editor status panels, export recovery manifest
validation, diagnostics/crash-dump paths outside SaveData, and deterministic
device-loss injection helpers. Use this focused pass when changing renderer
failure handling or diagnostics:

```powershell
ctest --test-dir out/build/Debug -C Debug -R "RendererRecovery|DeviceLoss|ExportRuntime|RenderDiagnostics|SystemScheduler|PlatformUtils" --output-on-failure
```

This pass proves fail-closed containment and diagnostics. TerraEngine does not
attempt in-process Vulkan device recreation after `VK_ERROR_DEVICE_LOST`;
rendering remains unavailable and the user must restart the editor or exported
application.

Visible-command reuse coverage validates static-view cache-hit telemetry,
deduplicated streaming touch publication, scheduler resource declarations,
automation stats, profiler rows, and repeated-resource performance gates. Use
this focused pass while iterating on visible-command reuse:

```powershell
$env:TERRAENGINE_TEST_FILTER="Renderer.VisibleCommandReuse;Renderer.VisibleStreamingTouches;Engine.SystemScheduler.DefaultResourcesAcceptVirtualGeometryRenderDeclaration"
.\out\build\Debug\TerraEngine-EngineTests.exe
.\out\build\Debug\TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

For CTest-based validation, run:

```powershell
ctest --test-dir out/build/Debug -R "Engine.Renderer|Engine.VirtualGeometry|Engine.Resource|Engine.SystemScheduler" --output-on-failure -C Debug
ctest --test-dir out/build/Debug -R "Editor.VirtualGeometry|Editor.Automation|Editor.Renderer" --output-on-failure -C Debug
```
small-scene inline fallbacks, resource readiness fallbacks, upload budgets,
deferred retirement safety, multi-queue readiness reports, non-fatal failure
containment, randomized pass validation, and visible-frame versus queued
background cost accounting. The backend submit-path checks split RHI begin,
per-view submit, and RHI end work into named backend stages so acquire, fence,
recording, descriptor, transition, submit, present, and forced-idle stalls can
be diagnosed directly. The editor suite also checks that closing Scheduler
Diagnostics disables render graph diagnostic collection with the ECS and
scripting diagnostic toggles.

The resource residency checks live under `Tests/Engine/Resource` with an editor
round-trip check under `Tests/Editor/Export`. They validate loaded RAM/VRAM
cleanup policy defaults, idle release candidates, pin and recent-use protection,
memory-budget ordering, diagnostics gating, and exported build-profile policy
serialization. These tests intentionally do not inspect persistent Vulkan or
pipeline disk caches because those files are backend cache implementation
details, not loaded resource residency.

Profiler stats and render-diagnostic relocation coverage for
GPU profiler statistics coverage lives in the existing engine/editor harnesses.
Use this pass when changing the Game View stats overlay, Profiler timing stream
presentation, Profiler toolbar, Resource Residency virtual geometry diagnostic
tables, or automation diagnostics contracts:

```powershell
ctest --test-dir out/build/Debug -R "Profiler|RenderDiagnostics|VulkanFrameDiagnostics|ResourceResidencyWindow|GameViewStatsOverlay|ProfilerDiagnosticsAutomation|VirtualGeometryDiagnostics" --output-on-failure
ctest --test-dir out/build/Debug -R "Engine\.SpatialUnitTests|Editor\.ExportUnitTests" --output-on-failure
```

Expected coverage includes compact Game View stats, explicit unavailable GPU
timing status instead of fake zeroes, profiler timing aggregates, grouped
Resource Residency virtual geometry runtime/page diagnostics, and structured
automation diagnostics that do not scrape viewport overlay text.

For virtual geometry page streaming, manual resource verification should confirm
that fallback pages stay `Pinned` or `Resident`, high-detail pages can move
through `Requested`, `PendingIO`, `PendingUpload`, `Resident`, `Evicting`, and
`Evicted`, and failed detail pages keep a visible fallback page. The Resource
Residency window and automation diagnostics should report resident payload bytes,
pinned fallback bytes, requests, retirements, evictions, and failure reasons
without showing missing coverage or embedded full-mesh fallback as the normal
path.

The visibility streaming checks extend the resource residency surface with
deferred scene payload registration, streamable state transitions, preload rule
evaluation, active render-view needs, editor workflow protection, active UI
image texture needs, and export manifest policy round trips. For a focused local
run on Windows:

```powershell
$env:TERRAENGINE_TEST_FILTER="Resource.Streaming;Scene.Streaming;Renderer.Streaming"
.\out\build\Debug\TerraEngine-EngineTests.exe
.\out\build\Debug\TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

Virtual geometry coverage will build on the renderer, resource residency,
streaming, asset pipeline, automation, and export suites. Use this focused pass
while implementing virtual geometry changes:

```powershell
$env:TERRAENGINE_TEST_FILTER="VirtualGeometry;MeshCompiler;HiZ;ResourceStreaming;ResourceResidency;Renderer;AssetPipeline;Editor.Automation;Export"
.\out\build\Debug\TerraEngine-EngineTests.exe
.\out\build\Debug\TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

The CTest labels also expose stable virtual-geometry slices:

```powershell
ctest --test-dir out/build/Debug -R "Engine.VirtualGeometryRenderer|Engine.VirtualGeometryResource|Editor.VirtualGeometryAssetPipeline|Editor.VirtualGeometryAutomation|Editor.VirtualGeometryExport" --output-on-failure -C Debug
```

The adaptive virtual geometry milestone adds focused adaptive slices for
configurable cook ratios, Inspector-only per-mesh overrides, real page
streaming, stronger Hi-Z validation, mixed-detail cluster selection, and runtime
visualization. Use this pass while implementing or reviewing that feature:

```powershell
ctest --test-dir out/build/Debug -R "Engine.AdaptiveVirtualGeometry|Editor.AdaptiveVirtualGeometry" --output-on-failure -C Debug
```

The virtual geometry hardening milestone adds the staged hardening pass
for Hi-Z correctness, explainable Hi-Z diagnostics, real page streaming,
counter source truth, transition stability, multi-view independence, visualizer
cleanup, and broad asset coverage:

```powershell
ctest --test-dir out/build/Debug -R "Engine.VirtualGeometryHardening|Editor.VirtualGeometryHardening" --output-on-failure -C Debug
```

The GPU-driven batching milestone adds the staged virtual geometry
scaling pass for cluster packet batching, runtime compaction, real page
residency, performance gates, shadow-view selection, cone-safety metadata, and
Hi-Z baseline preservation:

```powershell
ctest --test-dir out/build/Debug -R "Engine.VirtualGeometryBatching|Engine.VirtualGeometryCompaction|Engine.VirtualGeometryPerformanceGate|Engine.VirtualGeometryShadow|Engine.VirtualGeometryCulling|Engine.VirtualGeometryRenderer|Engine.VirtualGeometryResource|Editor.VirtualGeometryAssetPipeline|Editor.VirtualGeometryAutomation|Editor.VirtualGeometryExport|Editor.VirtualGeometryDiagnostics" --output-on-failure -C Debug
```

When implementing GPU-driven batching, run each capability slice before advancing to
the next one. The Hi-Z slice is a preservation pass: it must prove the current
moving-wall, camera-cut, resize, thin-geometry, and multi-view behavior still
matches the accepted baseline rather than adding new Hi-Z runtime tiers.

The performance-gate slice publishes typed virtual-geometry gate records with
fixture id, metric name, threshold, actual value, source, status, and diagnostic
text. Dense fixtures fail when submitted batches exceed `10%` of selected
cluster packets, when estimated prepared work exceeds `5 ms`, when dispatched
meshlets exceed selected meshlets, when page pressure exceeds the configured
budget, or when the Hi-Z false-occlusion warning count is non-zero. Missing
fixture prerequisites are reported as `Unsupported`, not as passing zeroes. The
cone-culling slice is conservative by default: it validates the required
`coneSafety` TEMF metadata and proves shader cone culling only arms when every
selected cluster is one-sided, opaque, consistent-winding, low thin-shell risk,
and cone-valid.

The GPU packet compaction milestone advances the already-selected virtual
geometry cluster cut into GPU-visible packet buffers, GPU packet compaction,
generated submission records, and source-labeled per-view counter readbacks.
The current CPU-selected cluster cut remains the correctness oracle, and the
current Hi-Z behavior remains locked as the accepted baseline:

```powershell
ctest --test-dir out/build/Debug -R "Engine.VirtualGeometryBatching|Engine.VirtualGeometryCompaction|Engine.VirtualGeometryPerformanceGate|Engine.VirtualGeometryRenderer|Engine.RendererRecovery" --output-on-failure -C Debug
```

When implementing GPU packet compaction, run each capability slice before advancing to
the next one. The GPU packet buffer slice must validate packet and batch layout
before shader consumption is enabled. The compaction slice must validate
candidate, compacted, rejected, fail-open, generated submission, and overflow
counters. The Hi-Z slice is preservation-only: it must prove the existing
moving-wall, camera-cut, resize, thin-geometry, and multi-view behavior still
matches the accepted baseline without adding quality tiers or retuning
occlusion.

The GPU packet compaction performance gates fail when dense selected packet work does
not collapse to submitted batches at or below `10%`, when CPU visible-command
preparation regresses beyond the configured budget, when compaction counters
miss fixture truth by more than `1%` or one packet, when generated submissions
do not match frame/view ownership, when Scene View and Game View share state
across 300 frames, or when the Hi-Z false-occlusion count is non-zero.
Unsupported prerequisites are reported as `Unsupported`, not as passing
zero-valued metrics.

The page-sourced geometry streaming milestone turns virtual geometry residency into a
true page-sourced render contract. Use the page-streaming slices while
implementing that feature:

```powershell
ctest --test-dir out/build/Debug -R "Engine.VirtualGeometryPageStreaming|Engine.VirtualGeometryPageResidency|Engine.VirtualGeometryResource|Engine.VirtualGeometryRenderer|Engine.VirtualGeometryPerformanceGate" --output-on-failure -C Debug
```

For the page request, budget pressure, eviction, and re-request lifecycle, run
the resource-labeled slice after the engine test target builds:

```powershell
ctest --test-dir out/build/Debug -C Debug -L Resource --output-on-failure
```

For capability validation page-source diagnostics, build both native test targets and run the
registry filters that match the editor-facing contract names:

```powershell
cmd /c "set TERRAENGINE_TEST_FILTER=VirtualGeometry.Diagnostics.capability validation&& out\build\Debug\TerraEngine-EngineTests.exe"
cmd /c "set TERRAENGINE_TEST_FILTER=VirtualGeometry.Editor.Diagnostics.capability validation&& out\build\Debug\TerraEngine-EditorTests.exe"
cmd /c "set TERRAENGINE_TEST_FILTER=VirtualGeometry.Editor.Automation.capability validation&& out\build\Debug\TerraEngine-EditorTests.exe"
```

The literal task checkpoint
`ctest --test-dir out/build/Debug -C Debug -R VirtualGeometry.Editor --output-on-failure`
currently matches no top-level CTest target. Treat the registry filters above
as the authoritative capability validation validation until CTest target names are expanded.

For capability validation asset cook/runtime integrity, run the inspector, runtime loader, and
automation slices before the asset-pipeline CTest checkpoint:

```powershell
cmd /c "set TERRAENGINE_TEST_FILTER=VirtualGeometry.Editor.Inspector.capability validation&& out\build\Debug\TerraEngine-EditorTests.exe"
cmd /c "set TERRAENGINE_TEST_FILTER=Resource.Manager.Mesh.capability validation&& out\build\Debug\TerraEngine-EngineTests.exe"
cmd /c "set TERRAENGINE_TEST_FILTER=VirtualGeometry.Editor.Automation.capability validation&& out\build\Debug\TerraEngine-EditorTests.exe"
ctest --test-dir out/build/Debug -C Debug -R AssetPipeline --output-on-failure
```

These checks protect the TEMF v1 page-integrity contract, tiny/simple mesh skip
reasons, mesh-only Inspector controls, and automation fields for page readiness
or integrity failure diagnostics.

For capability validation page-streaming performance and memory gates, run the page-gate filters
before the broad VirtualGeometry label checkpoint:

```powershell
cmd /c "set TERRAENGINE_TEST_FILTER=VirtualGeometry.PerformanceGate.capability validation&& out\build\Debug\TerraEngine-EngineTests.exe"
cmd /c "set TERRAENGINE_TEST_FILTER=VirtualGeometry.Render.MultiView.capability validation&& out\build\Debug\TerraEngine-EngineTests.exe"
ctest --test-dir out/build/Debug -C Debug -L VirtualGeometry --output-on-failure
```

The capability validation gates fail when resident page payload bytes exceed the configured page
budget, resident page payloads approach full owning-mesh residency, page-source
coverage is missing, embedded full-mesh fallback is used as the normal path, or
Scene View/Game View page-source counters lose their view/counter-source
identity.

The clean-checkout fixture names are `page-sourced-resident-payload`,
`page-fallback-only-coverage`, `page-budget-churn`, and
`page-integrity-malformed`; `dense-page-budget-streaming-gate` adds the capability validation
resident-byte, full-owning-mesh, missing-coverage, and embedded-fallback gate
coverage. These fixtures prove that normal rendering emits resident page
payloads, fallback pages stay pinned and visible, resident bytes respect the
configured budget, malformed page metadata fails before rendering, and Hi-Z
behavior remains the accepted baseline.

The broader virtual-geometry regression set covers renderer, resource,
editor asset pipeline, automation, export, and debug visualizer behavior:

```powershell
ctest --test-dir out/build/Debug -R "Engine.VirtualGeometry|Editor.VirtualGeometry|Engine.Renderer|Engine.Resource|Editor.AssetPipeline.VirtualGeometry|Editor.Automation|Editor.Export" --output-on-failure -C Debug
```

capability validation broad asset coverage is represented by
`Tests/Fixtures/VirtualGeometry/hardening-fixtures.json`. The clean-checkout
tests validate tiny, dense, thin, curved, hard-surface, multi-material package,
streaming-pressure, multi-view, and export categories through procedural data,
small glTF fixtures, vendored FBX fixtures, and synthetic TEPAK/TEMF packages.
Generated package sidecars expose `virtualGeometry.coverageTags`; export
manifests expose `virtualGeometry.coverage` with the fixture manifest and
covered categories.

When diagnosing just the adaptive path through the harness binaries, use the
same filters that the CTest entries apply:

```powershell
$env:TERRAENGINE_TEST_FILTER="AdaptiveVirtualGeometry;VirtualGeometry.Render;VirtualGeometry.Resource;VirtualGeometry.Diagnostics;VirtualGeometry.HiZ;VirtualGeometry.Stress"
.\out\build\Debug\TerraEngine-EngineTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
$env:TERRAENGINE_TEST_FILTER="AdaptiveVirtualGeometry;Editor.AssetPipeline.VirtualGeometry;VirtualGeometry.Editor.Inspector;VirtualGeometry.Editor.Automation;VirtualGeometry.Editor.Export;VirtualGeometry.Editor.DebugView"
.\out\build\Debug\TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

Fixture guidance for clean-checkout synthetic meshes, dense-scene thresholds,
and residency churn invariants lives in
`Tests/Fixtures/VirtualGeometry/README.md`. The install target copies
`Docs/Renderer/VirtualGeometry.md`, `Docs/TerraEngineMeshFile.md`,
`Docs/Testing.md`, and that fixture README into the Debug install tree for QA.

For true page residency, run exact Resource Residency checks
before renderer payload checks:

```powershell
ctest --test-dir out/build/Debug -C Debug -R "ResourceResidency|ResourceStreaming|ResourceManagerMesh|ResourceResidencyWindow" --output-on-failure
ctest --test-dir out/build/Debug -C Debug -R "VirtualGeometry\\.(Render|PageStreaming|ShaderLayout|Batching|HiZ|Hardening|MultiView|Diagnostics|PageResidency)|VirtualGeometry.*DebugView" --output-on-failure
```

These tests must prove that `Stream CPU Bytes` and `Render GPU Bytes` are exact
current live bytes for the row's asset, package parent rows do not borrow child
bytes, unavailable accounting stays non-numeric, and page-source diagnostics do
not silently treat owning mesh buffers as true page-owned rendering. The current
implementation publishes shader-decodable page-local vertex, primitive, remap,
meshlet, bounds, decode-info, and material streams for resident high-detail and
fallback packets. The shader-layout regression slice also proves that these
branches select page-local records before any owner-stream read. Owning-mesh
fallback remains valid only for a diagnosed page-policy failure, where the
no-full-owner performance gate must expose the exact owner bytes and reason.

After virtual geometry code changes, always finish with the normal configure,
compile, CTest, and install verification flow so the installed editor/runtime
tree has the matching TEMF loader, shaders, diagnostics, and asset pipeline
behavior:

```powershell
cmake --preset Debug
cmd.exe /s /c '"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 && cmake --build --preset Debug --target TerraEngine TerraEngine-Editor TerraEngineMeshCompiler TerraEngineModelPackageCompiler TerraEngineShaderCompiler TerraEngine-EngineTests TerraEngine-EditorTests'
ctest --test-dir out/build/Debug -R "Engine.VirtualGeometryBatching|Engine.VirtualGeometryCompaction|Engine.VirtualGeometryPerformanceGate|Engine.VirtualGeometryShadow|Engine.VirtualGeometryCulling|Engine.VirtualGeometryRenderer|Engine.VirtualGeometryResource|Editor.VirtualGeometryAssetPipeline|Editor.VirtualGeometryAutomation|Editor.VirtualGeometryExport|Editor.VirtualGeometryDiagnostics" --output-on-failure -C Debug
ctest --test-dir out/build/Debug -R "Engine.VirtualGeometry|Editor.VirtualGeometry|Engine.Renderer|Engine.Resource|Editor.AssetPipeline.VirtualGeometry|Editor.Automation|Editor.Export" --output-on-failure -C Debug
cmd.exe /s /c '"C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 && cmake --build --preset Debug --target install'
```

During virtual geometry hardening, run the hardening slice after each capability slice,
then run the broader virtual-geometry regression set before the install target.
The hardening fixture manifest lives at
`Tests/Fixtures/VirtualGeometry/hardening-fixtures.json`.

During GPU-driven batching work, rebuild the compiler tools before CTest whenever
`MeshCompiler.cpp`, package importers, TEMF virtual geometry structs, or sidecar
metadata changes. Several editor tests launch `TerraEngineMeshCompiler.exe` or
`TerraEngineModelPackageCompiler.exe`, so stale tool binaries can make newly
linked tests cook old payload contracts.

Optional manual large-asset stress is intentionally outside the CTest path. Use
a local scratch project with one dense scan mesh, one hard-surface
multi-material mesh, and one thin occlusion-heavy mesh. Rebuild virtual
geometry, inspect the generated mesh sidecar coverage tags, then validate LOD,
Cluster, Residency, Hi-Z, and Error visualizer modes while watching selected
meshlets, dispatched meshlets, pinned fallback bytes, requested pages, missing
pages, and budget pressure. Keep downloaded or licensed stress assets out of
the repository unless a later feature explicitly approves them.

The editor MCP bridge checks live under `Tests/Editor/Automation`. They cover
automation envelopes, capability metadata, local-only transport checks, bridge
state transitions, bounded activity history, approval prompts, and resource/tool
routing. The glTF and FBX package import checks live under both
`Tests/Editor/AssetPipeline` and `Tests/Editor/Automation`; they validate
fastgltf and Assimp package parsing, generated material/texture/assignment
metadata, diagnostics, stable reimport identities, MCP import responses, and MCP
path boundary rejection. For a focused Windows run, build through the Visual
Studio developer environment and run the editor harness with the MCP/Automation
or model package filters:

```powershell
cmd /c "C:\PROGRA~1\MIB055~1\18\COMMUN~1\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake --build --preset Debug --target TerraEngine-EditorTests TerraEngine-Editor"
$env:TERRAENGINE_TEST_FILTER="Editor.MCP;Editor.Automation;Editor.AssetPipeline.Gltf;Editor.AssetPipeline.Fbx;Editor.AssetPipeline.ModelPackage"
.\out\build\Debug\TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

After installation, open the installed editor, load a project, and use
`Edit > Preferences... > MCP Bridge` to confirm disabled/read-only startup,
local-only connection behavior, capability/resource listing, write-mode
blocking, approval prompts, activity history, and fixed-port persistence. Change
the listen port while disabled, restart the editor, and confirm the endpoint
uses the same port instead of an ephemeral one.

For Linux-specific prerequisites and troubleshooting, see
`Docs/LinuxSupport.md`.

## Linux Workflow

### Configure

```bash
cmake --preset Debug
```

The checked-in preset uses Ninja. On Linux hosts without Ninja, configure an
equivalent Makefiles tree:

```bash
cmake -S . -B out/build/LinuxDebugMake -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_INSTALL_PREFIX=out/install/LinuxDebugMake
```

### Build Unit Tests

```bash
cmake --build out/build/Debug --target TerraEngine-EngineTests
cmake --build out/build/Debug --target TerraEngine-EditorTests
```

### Run CTest

Vendored dependency tests are disabled in the TerraEngine CMake configuration,
so this command runs only the project-owned CTest entries.

```bash
ctest --test-dir out/build/Debug --output-on-failure
```

### Transactional Mutation Checks

Transactional scene mutation tests cover the engine-scoped STM path used by
worker-prepared scene/object/component edits. The feature intentionally keeps
live GPU submission, platform polling, arbitrary managed callback execution,
and editor UI mutation outside the transaction write set; those operations
remain protected boundaries.

```powershell
$env:TERRAENGINE_TEST_FILTER="TransactionalMutation,SystemScheduler.ComponentAccessDeferredWrite"
.\out\build\CodexDebug\TerraEngine-EngineTests.exe
.\out\build\CodexDebug\TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

## Export Runtime Baseline

The source-only sample lives at `Tests/Fixtures/ExportRuntimeBaseline`. Its
textual glTF model is the source of truth for the mesh, material, and texture;
the installed model package compiler creates real TEMF, TMAT, and TETF child
assets only inside a disposable copy below `out/`. Placeholder compiled assets
are rejected by both the helper and export evidence inventory.

Run the authoritative local gate from a Windows x64 Visual Studio developer
environment:

```powershell
cmake --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine-EngineTests TerraEngine-EditorTests TerraEngineModelPackageCompiler TerraEnginePhysicsColliderCompiler TerraEngine-Export TerraEngine-Editor
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R "Editor.ExportUnitTests|Editor.HeadlessExport|Editor.PhysicsExport|Engine.PhysicsExport" --output-on-failure
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo --output-on-failure
cmake --build --preset RelWithDebInfo --target install
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tests/Tools/ExportRuntimeBaseline/RunBaseline.ps1 -InstallRoot out/build/RelWithDebInfo/Build -WorkRoot out/ci/export-runtime-baseline
```

Run the same installed gate beyond 360 characters whenever editor scripting,
NativeAOT, package staging, or output publication changes:

```powershell
$deepWorkRoot = "out/ci/export-runtime-baseline/" + (
  (1..8 | ForEach-Object { "segment-abcdefghijklmnopqrstuvwxyz-{0:d2}" -f $_ }) -join "/"
)
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File Tests/Tools/ExportRuntimeBaseline/RunBaseline.ps1 `
  -InstallRoot out/build/RelWithDebInfo/Build `
  -WorkRoot $deepWorkRoot `
  -RunId export-runtime-long-path
```

This lane fails unless the installed editor's ordinary Assembly-CSharp build
reaches `Ready`, the NativeAOT export succeeds from its private short workspace,
and no `Library/NativeAot` restore or publish state leaks into the project.

The installed tools are read from `out/build/RelWithDebInfo/Build`; callers do
not run an executable from the intermediate target directory. Verify these
installed dependencies before running the baseline:

```text
out/build/RelWithDebInfo/Build/Editor/TerraEngine-Export.exe
out/build/RelWithDebInfo/Build/Editor/TerraEngineModelPackageCompiler.exe
out/build/RelWithDebInfo/Build/Editor/ExportRuntime/TerraEngine-Launcher.exe
out/build/RelWithDebInfo/Build/Editor/ExportRuntime/TerraEngine.dll
out/build/RelWithDebInfo/Build/Editor/ExportRuntime/glfw3.dll
out/build/RelWithDebInfo/Build/Editor/Managed/TerraEngine.CSharp.dll
out/build/RelWithDebInfo/Build/Editor/Managed/Analyzers/TerraEngine.Export.Generator.dll
```

Only files copied into exported games live below `Editor/ExportRuntime`.
Exporter/compiler tools and shared managed references remain editor-side.

The direct headless CLI contract is:

```powershell
& out/build/RelWithDebInfo/Build/Editor/TerraEngine-Export.exe `
  --project out/ci/export-runtime-baseline/project `
  --profile out/ci/export-runtime-baseline/project/BuildProfiles/baseline.json `
  --output out/ci/export-runtime-baseline/project/Builds/Baseline `
  --evidence out/ci/export-runtime-baseline/evidence
```

Publication is an in-place overlay after validation. A staged file overwrites a
matching destination file, but the exporter does not rename the destination
directory and does not delete destination paths absent from staging. This keeps
open terminals and unrelated user files intact. Use a dedicated or disposable
output directory when a completely clean export tree is required.

A successful invocation exits with code 0 and writes exactly one final result
record plus these six durable files: `export-result.json`,
`resolved-manifest.json`, `content-inventory.json`,
`validation-report.json`, `progress.jsonl`, and `package-validation.txt`.
Required inventory entries must be nonempty and set
`placeholderDetected=false`. The archived `game.json` must match
`resolved-manifest.json`.

Failure exit codes are stable: 2 covers `invalid_arguments` or
`invalid_runtime_layout`, 3 `output_conflict`, 4 `validation_failed`, 5
`managed_compilation_failed`, 6 `asset_staging_failed`, 7 `packaging_failed` or
`package_validation_failed`, 8 `runtime_staging_failed` or
`evidence_write_failed`, 9 `publication_failed`, and 10
`unexpected_failure`. Inspect the final result's `stage`, `failureCategory`,
and ordered diagnostics before using the associated validation, inventory, or
package report. See `Docs/ExportRuntimeBaseline.md` for the complete diagnostic
table.

The local command sequence is the supported validation gate. Keep the
`out/ci/export-runtime-baseline` work root until diagnosis is complete because
it retains the disposable project and any partial or complete export evidence
after failure.

For the animation production validation canonical comparison, use 10,000 bodies (1,000 dynamic,
1,000 kinematic, 8,000 static) and 100 exact paused steps. Scene View defaults
are a 2,000-object large-scene threshold, 2,048 visible-marker budget, 512-item
presentation batches, 18-pixel density radius, density threshold 4, clustered
physics markers, and automatic large-scene mode. The explicit dense/selected/all
marker modes are comparison controls, not altered project defaults. Max workers
use hardware concurrency with no environment cap; only the 1/2/4 matrix sets
`TERRAENGINE_THREADPOOL_WORKERS` deliberately, and build commands do not pass
`--parallel`.

## physics foundation validation physics hardening workflow

The complete physics gate uses RelWithDebInfo on Windows x64. From a Visual
Studio developer PowerShell, run the following in order:

```powershell
cmake --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine TerraEngine-Editor TerraEnginePhysicsColliderCompiler TerraEngineModelPackageCompiler TerraEngine-EngineTests TerraEngine-EditorTests TerraEngine-Export
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R "Physics|TransactionalMutation|SystemScheduler|Editor\.Automation|Editor\.Export|SceneRuntimeParity" --output-on-failure
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo --output-on-failure
cmake --build --preset RelWithDebInfo --target install
```

Run the focused stress and soak slices before live validation:

```powershell
$env:TERRAENGINE_TEST_FILTER = "Physics.DeterminismStress;Physics.LifecycleStress"
& .\out\build\RelWithDebInfo\TerraEngine-EngineTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
$env:TERRAENGINE_TEST_FILTER = "Physics.Editor.Soak"
& .\out\build\RelWithDebInfo\TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

Then use the installed-editor harness and export baseline:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tests/Tools/Physics/RunInstalledEditorPhysicsStress.ps1 -InstallRoot out/build/RelWithDebInfo/Build -WorkRoot out/validation/physics-live -ObjectCount 10000 -ActiveDynamicCount 1000 -ExplicitStepCount 100 -AllowLiveStress
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tests/Tools/ExportRuntimeBaseline/RunBaseline.ps1 -InstallRoot out/build/RelWithDebInfo/Build -WorkRoot out/ci/physics-export-baseline
```

The live harness accepts `-RunManagedReloadAutomationLoopback -RunColliderCookIncrementalMatrix`
for managed/automation checks with same-run rollback and visibility evidence, and
`-RunAuthoringWalkthrough` for authoring checks. The export baseline must retain
the six evidence files and must not copy `.tecollider` source recipes into the
published package. Troubleshoot stale binaries by reinstalling; troubleshoot a
failed export by reading `validation-report.json`, `content-inventory.json`,
and `package-validation.txt` before changing the fixture.

There is no supported full-engine Windows/MSVC sanitizer lane. Do not treat a
third-party provider build as coverage for engine, editor, Vulkan, managed, or
runtime lifecycle behavior. Any future sanitizer lane must be registered and
documented as a maintained CMake/CTest configuration before its results are
used as release evidence.

## MCP Editor Completeness

The loopback-only editor bridge exposes typed capability
providers, atomic automation transactions, durable session jobs, structured
logs, project and scene lifecycle, Play Mode, asset and prefab workflows,
scripting, build profiles, Scene View commands, and profiler captures. The
bridge remains disabled and read-only by default, and clients cannot enable it,
elevate write access, execute arbitrary processes, or access paths outside
approved project roots.

Build and run the focused native coverage from an x64 Visual Studio developer
environment:

```powershell
cmake --preset Debug
cmake --build --preset Debug --target TerraEngine-EngineTests TerraEngine-EditorTests TerraEngine-Editor TerraEngine-Export
$env:TERRAENGINE_TEST_FILTER="Editor.MCP;Editor.Automation;AutomationTransaction;AutomationJob;AutomationLog;PlayMode;Prefab;ScriptCompilation;BuildProfile;SceneView;Profiler"
.\out\build\Debug\TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
ctest --test-dir out/build/Debug -C Debug -R "Editor\.Automation|Editor\.RendererRecovery|Editor\.Export|Engine\.TransactionalMutation|Engine\.SystemScheduler" --output-on-failure
ctest --test-dir out/build/Debug -C Debug --output-on-failure
cmake --build --preset Debug --target install
```

Installed-editor validation uses
`out/build/Debug/Build/Editor/TerraEngine-Editor.exe`. Start at the project hub
with isolated preferences, enable the bridge and writes from
`Edit > Preferences... > MCP Bridge`, and run the end-to-end scenario in
the editor automation validation workflow. Validation must cover one
successful atomic batch and one rejected batch, undo/redo, disconnect and job
reconnect, Play Mode single-step, image capture, correlated logs, script
compilation, export evidence, profiler capture, approval binding, project
reload, and final bridge disablement.

Disposable MCP projects must not enter recent-project history. Remove them only
after the installed editor exits so file watchers and background jobs have
released their handles.

## Renderer Performance Baseline

Renderer threading fixtures live at `Tests/Fixtures/RendererPerformanceBaseline`.
The downloaded dense animated validation model uses a separate validation scene
and must never modify, reimport, recook, or rewrite
`C:/Users/benti/Desktop/TerraEngine-Projects/New Animated Project/Assets/Scenes/Scene.tescene`.
The `workloads.json` manifest describes the small, medium, large, streaming,
UI-heavy, shadow-heavy, scene-game-submit, and randomized-stress scenes used by
the renderer graph tests and by manual profiling runs. Keep this fixture
lightweight and committed:
it is the shared reference for validating that small scenes stay inline, large
stable scenes reuse prepared packets, streaming resources fall back instead of
blocking the frame, and diagnostics stay opt-in.

Use the Windows or Linux workflow below, then run three local editor captures
with diagnostics closed and one capture with Scheduler Diagnostics open. Compare
the visible-frame timings against queued background work before accepting a
renderer-threading change. A regression should be fixed or recorded in the
feature quickstart with the machine, scene, and backend context that reproduced
it.

For the installed dense animated-VG visual soak, use a disposable scene and a
fixed MCP port. The tool samples presented frames while orbiting and dollying a
custom camera; it never writes the authored benchmark camera. It records PNG
hashes, sampled luminance/non-black ratios, VG statistics, and errors beneath
the requested result directory, then removes the disposable scene.

```powershell
& .\Tests\Tools\VirtualGeometry\CaptureVirtualGeometryLiveVisual.ps1 `
	-InstalledEditor 'out/build/RelWithDebInfo/Build/Editor/TerraEngine-Editor.exe' `
	-Project 'C:\Users\benti\Desktop\TerraEngine-Projects\New Animated Project' `
	-Asset 'Assets/ValidationModels/DenseAnimatedVG/CesiumMan.glb' `
	-Prefab 'Assets/ValidationModels/DenseAnimatedVG/CesiumMan_Generated/CesiumMan.teprefab' `
	-CreateValidationScene -CleanupValidationScene `
	-ValidationScenePath 'Assets/Scenes/.terra-validation/DenseAnimatedVGSoak5m.tescene' `
	-OutputDirectory 'Tests/Fixtures/RendererPerformanceBaseline/results/soak/dense-animated-vg-5m' `
	-ValidationCameraPosition 0.0,0.5,1.0 -ValidationCameraTarget 0.0,0.5,0.0 `
	-SoakDurationSeconds 300 -SoakSampleIntervalSeconds 10 -McpPort 43155 `
	-ValidationRoute hardware -RepresentationRoute virtual-hardware `
	-PrefabRequestedAnimationMode 1 -EnterPlayMode -ExpectAnimatedChange
```

The Profiler idle-overhead gate uses the same installed editor, project,
disposable dense-animation scene, and fixed port for both sides of every pair.
It alternates closed/open-idle order, records unlimited frame history, and
reports every pair for diagnosis, then requires the median matched-pair Engine
CPU median and p95 deltas to remain within two percent. Run five 60-second pairs
and retain the generated summaries and manifest:

```powershell
& .\Tests\Tools\Profiling\MeasureProfilerIdleOverhead.ps1 `
	-InstalledEditor 'out/build/RelWithDebInfo/Build/Editor/TerraEngine-Editor.exe' `
	-Project 'C:\Users\benti\Desktop\TerraEngine-Projects\New Animated Project' `
	-OutputDirectory 'Tests/Fixtures/RendererPerformanceBaseline/results/profiler' `
	-PairCount 5 -CaptureDurationSeconds 30 -McpPort 43155
```

For a standalone long-capture coherence check, run the visual harness with
`-ProfilerMaxFrames 0 -ProfilerCaptureDurationSeconds 60`. A zero maximum means
unlimited history. The saved capture, reopened summary, timing distribution,
frame list, hierarchy, and typed evidence must report the same completed-frame
count; unavailable evidence remains distinct from measured zero.

### Install After Code Changes

Run the install target before finishing any task that changed code so the
install tree stays verified with the compiled outputs.

```bash
cmake --build out/build/Debug --target install
```

The install target also runs managed publish/build targets through the
repository-local SDK and copies that SDK to `Build/Editor/DotNet`. Installed
editor script builds and NativeAOT exports invoke the copied executable by
absolute path, so neither building nor using TerraEngine requires `dotnet` on
PATH.

### Renderer Stability Hardening

scene runtime validation adds deterministic filters for `VulkanPresentationLifecycle`,
`VulkanUiSubmissionFailure`, `EditorFrameCompletionTracker`,
`Editor.StartupRenderer`, `Editor.PostFrameBoundary`,
`Editor.CheckedImGuiVulkanBridge`, and `Editor.ViewportTextureRetirement`.
Run both test executables with those filters before live GPU work.

Live resize/minimize validation is deliberately opt-in and paced. Use
`Tests/Tools/RendererStability/RunLiveSmoke.ps1` with `-AllowLiveGpuStress`, at most
50 operations per batch, and inspect its process/driver evidence after each
batch. The harness snapshots the latest Windows System-log RecordId/time before
launch and polls after every resize and throughout minimize/restore waits. A
qualifying newer display-driver event records `driver-event-failure.json`, stops
the launched editor process tree, and never advances to the next operation; an
older event cannot invalidate the run. Run the CPU-only seam regression before
live work with `powershell.exe -NoProfile -ExecutionPolicy Bypass -File
Tests/Tools/RendererStability/RunLiveSmoke.Tests.ps1`. The harness also waits for the
backend's first-successful-present marker and stores typed generation/extent
evidence in `presentation-readiness.json`; a window handle alone is never used
as a Vulkan readiness proxy. Do not inject device loss on the primary display
adapter; the engine's policy for a genuine driver/hardware loss remains terminal
containment.

### Linux Editor Script Smoke Checks

After installing, start the editor from the installed/staged Linux editor
directory and validate the scripting paths:

- Open or create a project, create or edit a C# script, and trigger the
  Assembly-CSharp refresh/build command. The editor should run `dotnet build`
  from the project workspace, capture stdout/stderr and the process exit code,
  then reload the managed Assembly-CSharp output or report normal compiler
  diagnostics.
- Open a script or scripting workspace from the editor. Linux should use a
  configured command such as `code {script}` or `rider {solution}`, or the
  desktop default opener, without requiring Visual Studio automation.

The live editor script output may remain `Assembly-CSharp.dll` on Linux because
it is a managed `dotnet build` assembly. That is distinct from exported
NativeAOT payloads such as `Assembly-CSharp.so`.

## animation production validation project readiness and Play Mode restoration

Run the project-readiness slice after an integrated RelWithDebInfo build. Keep
the native worker override unset so the tests and later installed run use the
machine's full hardware-concurrency default; the build command intentionally
does not pass a `--parallel` argument.

```powershell
Remove-Item Env:TERRAENGINE_THREADPOOL_WORKERS -ErrorAction SilentlyContinue
cmake --build out/build/RelWithDebInfo --target TerraEngine-EngineTests TerraEngine-EditorTests TerraEngine-Editor
$env:TERRAENGINE_TEST_FILTER = "DerivedSceneCache;BulkScenePublication"
& .\out\build\RelWithDebInfo\TerraEngine-EngineTests.exe
$env:TERRAENGINE_TEST_FILTER = "ManagedReadiness;ProjectReadiness;ArchitectureHardening.PlayModeBulkRestore;MCP.ProjectLifecycle;MCP.Scripts;MCP.PlayMode;PhysicsExport"
& .\out\build\RelWithDebInfo\TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER -ErrorAction SilentlyContinue
cmake --build out/build/RelWithDebInfo --target install
```

The focused tests cover missing/cold cache creation, unchanged warm reuse,
source-stale and corrupt-envelope fallback, project-close cancellation, the
10,000-object readiness gate, managed input/output fingerprints, conditional
reference copying, explicit compile bypass, one-boundary scene publication,
Play Mode bulk restoration, and export exclusion. A cache failure is successful
recovery only when authored source loads completely, no partial live state is
visible, and best-effort regeneration is atomically published.

After install, run the harness's capability validation matrix under a disposable work root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tests/Tools/Physics/RunInstalledEditorPhysicsStress.ps1 `
	-InstallRoot out/build/RelWithDebInfo/Build `
	-WorkRoot out/validation/readiness `
	-RunReadinessMatrix -AllowLiveStress
```

Retain every scenario summary, MCP log, stage timing, source hash, cache
decision, readiness result, artifact inventory, and shutdown result. Cold must
regenerate from source; warm must report a compatible derived hit and unchanged
managed readiness no slower than 2.5 seconds; stale and corrupt must report
source fallback plus regeneration; forced compile must report a real forced
build even after a warm hit. The authored scene hash must remain authoritative
and normal shutdown must succeed in every accepted scenario.

## Windows Workflow

The Windows workflow uses the same project-owned CTest surface and install
requirement. Run these commands from a Visual Studio Developer PowerShell or
after calling `VsDevCmd.bat`; a plain shell may fail before compiling project
code because MSVC standard library include paths are not initialized.

```powershell
cmake --preset Debug
cmake --build out/build/Debug --target TerraEngine-EngineTests
cmake --build out/build/Debug --target TerraEngine-EditorTests
ctest --test-dir out/build/Debug --output-on-failure
cmake --build out/build/Debug --target install
```

When using a non-preset build directory created by Codex or local automation,
replace `out/build/Debug` with that directory. For example, the current Codex
debug tree uses:

```powershell
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"" && cmake --build out\build\CodexDebug --target TerraEngine-EngineTests TerraEngine-EditorTests --config Debug"
ctest --test-dir out\build\CodexDebug --output-on-failure -C Debug
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"" && cmake --build out\build\CodexDebug --target install --config Debug"
```

For the RelWithDebInfo residency workflow used by current performance/debug
work, run the same commands through `VsDevCmd.bat` when the shell is not already
a Visual Studio Developer shell:

```powershell
cmd /c "C:\PROGRA~1\MIB055~1\18\COMMUN~1\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake --build --preset RelWithDebInfo --target TerraEngine-EngineTests TerraEngine-EditorTests"
$env:TERRAENGINE_TEST_FILTER="Resource.Residency"
.\out\build\RelWithDebInfo\TerraEngine-EngineTests.exe
.\out\build\RelWithDebInfo\TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
cmd /c "C:\PROGRA~1\MIB055~1\18\COMMUN~1\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake --build --preset RelWithDebInfo --target install"
```

## profiler rework V1 evidence and fixed-identity benchmark

The profiler rework starts a clean V1 journal lineage (`TerraEngineProfilerJournalV1`). Pre-feature profiler
artifacts are unsupported and are never migrated or partially interpreted. The
installed benchmark records a fixed identity before launching the editor; a
comparison is invalid if project, scene, workload, camera, viewport, build,
capture settings, or duration differs. Validate source and install contents
first:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tests/Tools/Profiling/CheckProfilerSource.ps1 `
    -RepositoryRoot . -InstallRoot out/install/RelWithDebInfo
```

Run a clean-input dry run, then the canonical five-run installed-editor
comparison (use the five-minute canonical duration for release evidence):

```powershell
& .\Tests\Tools\Profiling\CaptureProfilerReworkBenchmark.ps1 `
    -Project <project> -Scene <scene> -Manifest Tests/Tools/FramePublication/workloads.json `
    -InstallRoot out/install/RelWithDebInfo -EditorExecutable <installed-editor> `
    -CameraHash <read-only-camera-hash> -OutputDirectory out/evidence/profiler-rework `
    -DurationSeconds 60 -DryRun
& .\Tests\Tools\Profiling\CaptureProfilerReworkBenchmark.ps1 `
    -Project <project> -Scene <scene> -Manifest Tests/Tools/FramePublication/workloads.json `
    -InstallRoot out/install/RelWithDebInfo -EditorExecutable <installed-editor> `
    -CameraHash <read-only-camera-hash> -OutputDirectory out/evidence/profiler-rework `
    -DurationSeconds 300
```

Evidence must retain committed counts, resident/high-water bytes, persistence
lag, pending work, detail-loss ranges, finalization status, and explicit
unavailable values. For interruption testing, stop the installed editor after
at least one committed segment, reopen the artifact, and verify that only the
committed prefix is exposed and the incomplete tail is labeled recovered.
Recording rows also preserve JournalV1 profiler-self/batching counters and
logical/encoded/stored byte totals when the installed editor publishes them;
missing counters remain `null` and are never interpreted as zero.
The completed JournalV1 summary's `batching` table is authoritative for logical,
encoded, and stored byte totals when the live health snapshot omits them;
maximum pending bytes remain sourced only from health/settings.
Finish latency is taken from the authoritative `finishCaptureSubmissionMilliseconds`
field returned by `profiler.capture.stop`; the MCP request round-trip is retained
separately as `finishMcpRoundTripMilliseconds` and is not used to evaluate the
50 ms interactive Finish gate. Missing, non-finite, or negative values fail closed.
The disabled control leg does not start a capture and records both finish fields
as explicit `null` not-applicable values.

The remaining profiler-rework acceptance runs use the same authored scene and
the same explicit `CameraHash`; each command fails closed when identity or
installed V1 support is missing. Keep each output directory separate so a
partial run cannot be mistaken for complete evidence:

```powershell
# Run focused suites, then install and audit the installed tree.
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo --output-on-failure
cmake --build out/build/RelWithDebInfo --target install --parallel 2

# Run the exact-scene one-minute JournalV1 smoke and reopen/count check.
& .\Tests\Tools\Profiling\CaptureProfilerReworkBenchmark.ps1 `
  -Project <project> -Scene <project>/Assets/Scenes/Scene.tescene `
  -InstallRoot out/install/RelWithDebInfo -EditorExecutable <installed-editor> `
  -CameraHash <read-only-camera-hash> -Mode DetailedRecording -Runs 5 `
  -DurationSeconds 60 -OutputDirectory out/evidence/profiler-rework/smoke

# Run five AB/BA closed/open-idle pairs (the runner enforces the 2% median/P95 gate).
& .\Tests\Tools\Profiling\MeasureProfilerIdleOverhead.ps1 `
  -InstalledEditor <installed-editor> -Project <project> -PairCount 5 `
  -CaptureDurationSeconds 30 -OutputDirectory out/evidence/profiler-rework/open-idle

Each paired condition is interleaved in short closed/open blocks and then
aggregated by condition, preserving 30 seconds of valid samples per state (300
seconds total) while
canceling slow animation or power drift. Visibility transitions settle before
their timing samples. Evidence includes bounded per-block medians/P95s and
paired-block percentage deltas before the pair aggregate; unequal/missing blocks
fail closed. Open-idle acceptance uses signed one-sided deltas: median and P95
overhead must each be at most 2%; negative values indicate an improvement and
remain passing.

# Run a closed/open recording pair; the window opens only in the final minute.
& .\Tests\Tools\Profiling\CaptureProfilerReworkBenchmark.ps1 `
  -Project <project> -Scene <project>/Assets/Scenes/Scene.tescene `
  -InstallRoot out/install/RelWithDebInfo -EditorExecutable <installed-editor> `
  -CameraHash <read-only-camera-hash> -Mode MatchedWindowOpen -Runs 1 `
  -DurationSeconds 300 -OpenProfilerAtSeconds 240 `
  -OutputDirectory out/evidence/profiler-rework/late-window

Late-window evidence contains five same-run closed/open pairs. Each condition
sets visibility, waits one second for the completed UI state, then collects 30
valid Engine CPU timing samples at 100 ms intervals. The first pair remains
available as `windowMeasurement.preOpen` and `windowMeasurement.postOpen` for
compatibility; every pair is also published in `windowMeasurement.pairedWindows`.
The gate uses the median of the five median deltas and the median of the five
P95 deltas with a one-sided 2% overhead cap (an improvement remains a pass),
and requires the maximum visibility request latency to be at most 1000 ms.
The runner does not fetch the historical frame index while the recording is
active; the separate open-idle experiment retains the same one-sided 2% cap.

If only aggregation logic changes, regenerate an existing completed bundle
without launching the editor by adding `-RegenerateEvidenceFromRuns` to the
same benchmark command. The switch requires the saved identity, every expected
run metric, matching raw-summary hashes, and both comparison modes; any missing
or mismatched artifact fails closed.
For timed-window regeneration it additionally requires exactly five complete
closed/open paired windows with finite signed deltas and latency before treating
the retained measurement as measured.
The benchmark wrapper validates the final `benchmark-evidence.json` directly
against its public V1 envelope invariants before returning; a non-window leg is
emitted as `windowMeasurement.status`
`unavailable` (the raw run JSON is never rewritten), while a timed-open leg must
remain `measured` with all five validated pairs.
For matched recording overhead, the recording leg also publishes a closed-window
100 ms Engine CPU stream (`matchedOverheadEvidence`); the recording-overhead
criterion compares its median
and P95 with the disabled leg, while full Journal terminal windows remain the
source for duration and frame-count gates.
To avoid periodic 100 ms response-tail aliasing, each overhead stream is split
into six equal 10-second blocks. Every block must contain at least 30 valid
samples; the recording-overhead criterion compares the median block median/P95 values and fails closed on
missing or unequal blocks. Raw samples remain available for diagnosis.

# Run one five-minute profiler capture that also serves as the changing-animation
# visual soak, without adding a second sequential soak or saving Scene.tescene.
& .\Tests\Tools\VirtualGeometry\CaptureVirtualGeometryLiveVisual.ps1 `
  -InstalledEditor <installed-editor> -Project <project> `
  -ScenePath Assets/Scenes/Scene.tescene -PrefabInstanceCount 0 `
	-ValidationCameraPosition @(0.0, 1.0, 9.0) `
	-ValidationCameraTarget @(0.0, 1.0, 8.0) -EnterPlayMode `
  -SoakDurationSeconds 0 -ProfilerCaptureDurationSeconds 300 `
  -ExpectAnimatedChange -OutputDirectory out/evidence/profiler-rework/live-visual

# Run the 60-second-per-leg ABBA overhead smoke (disabled/recording/recording/disabled).
# The balanced order cancels monotonic thermal drift while retaining the unchanged
# three-percent median/P95 gate; this smoke is not the canonical stability run.
& .\Tests\Tools\Profiling\CaptureProfilerReworkBenchmark.ps1 `
  -Project <project> -Scene <project>/Assets/Scenes/Scene.tescene `
  -InstallRoot out/install/RelWithDebInfo -EditorExecutable <installed-editor> `
  -CameraHash <read-only-camera-hash> -Mode MatchedRecordingComparison -Runs 1 `
  -DurationSeconds 60 -OutputDirectory out/evidence/profiler-rework/overhead-smoke
```

# Run the single canonical five-minute detailed recording. Fixed 60-second health
# checkpoints measure protected collection cost per produced frame, backlog/loss,
# memory, and Finish evidence; raw Engine CPU first/final windows are diagnostic.
& .\Tests\Tools\Profiling\CaptureProfilerReworkBenchmark.ps1 `
  -Project <project> -Scene <project>/Assets/Scenes/Scene.tescene `
  -InstallRoot out/install/RelWithDebInfo -EditorExecutable <installed-editor> `
  -CameraHash <read-only-camera-hash> -Mode DetailedRecording -Runs 1 `
  -DurationSeconds 300 -OutputDirectory out/evidence/profiler-rework/final

Installed identity hashes cover both `TerraEngine-Editor.exe` and the matching
`TerraEngine.dll`; a missing engine DLL or a changed sibling package
fails closed. Resume and evidence comparison reject package-hash mismatches.

Failure-matrix evidence is produced by the focused profiler recovery,
queue, truncation, and stale-completion tests; retain their CTest transcript
under `out/evidence/profiler-rework/failure-matrix/` and reject any run that
does not publish a complete JournalV1 health/finalization record.

## animated virtual geometry validation animated virtual geometry

Animated virtual geometry has three validation layers. Run the focused CPU,
format, shader-contract, scheduler, streaming, publication, and recovery checks
before the complete suites:

```powershell
$env:TERRAENGINE_TEST_FILTER="AnimatedVirtualGeometry;VirtualGeometry.ShaderLayout;Renderer.AnimationView;ShaderGeometryContract"
.\out\build\RelWithDebInfo\TerraEngine-EngineTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
$env:TERRAENGINE_TEST_FILTER="SkinnedMeshCook;DeformableDemandedPageFormat;VirtualGeometry"
.\out\build\RelWithDebInfo\TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo --output-on-failure
```

Then build and inspect the installed tree. The three compute binaries and public
geometry includes must be loaded from `Build/Editor/Shaders`, not the source
tree:

```powershell
cmake --build --preset RelWithDebInfo --target install
Get-Item out/build/RelWithDebInfo/Build/Editor/Shaders/VirtualGeometry/AnimatedPageDeform.comp.spv
Get-Item out/build/RelWithDebInfo/Build/Editor/Shaders/VirtualGeometry/AnimatedClusterBounds.comp.spv
Get-Item out/build/RelWithDebInfo/Build/Editor/Shaders/VirtualGeometry/AnimatedRefinement.comp.spv
```

For GPU acceptance, launch the installed editor and use the deterministic
fixture in `Tests/Fixtures/AnimatedVirtualGeometry`. Capture adapter, driver,
resolution, quality/budgets, build revision, view count, working-set high-water
marks, reuse, fallback, and failure counters. Compare a rejection-disabled
million-decision visibility run with the current-bounds route and require zero
false rejections. The soak profile is
`Tests/Fixtures/AnimatedVirtualGeometry/stress-profile.json`; its release gate is
1,000 instances, four views, continuous churn, at least 99% exact-key reuse,
bounded memory/retirement, and five minutes without unrecovered failure.

To reproduce the animated LOD import regression specifically, run:

```powershell
$env:TERRAENGINE_TEST_FILTER="SkinnedMeshCook.PreservesAdaptiveLodsAndPerMeshPercentageThresholds;PackageImport.AppliesPercentageOverridesPerGeneratedMesh"
.\out\build\RelWithDebInfo\TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

The skinned cook must load a final deformable TEMF with more than one detail
level and an animated physical page outside LOD0. The package test proves that
two generated child meshes retain independent percentage thresholds.

### Current-format compatibility gate

This check reads the external validation project without importing, recooking,
or writing authored assets:

```powershell
$env:TERRAENGINE_CURRENT_PROJECT_ROOT = 'C:/Users/benti/Desktop/TerraEngine-Projects/New Animated Project'
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R 'Editor\.CurrentProjectCompatibility' --output-on-failure
Remove-Item Env:TERRAENGINE_CURRENT_PROJECT_ROOT
```

`Assets/Scenes/Scene.tescene` must retain SHA-256
`C15F9DCC784BA4ACF027D7C01D22DE8D65A20C3B4044931E7C9488FDF3AFD673`; the dense
animated validation scene is authored separately.

### Frame-publication profiler and tooling

The dependency-free tooling under `Tests/Tools/FramePublication` is installed with
the build. Validate a clean checkout without creating evidence:

Record `$framePubCameraHash` from the installed editor's read-only camera/diagnostic
response (for example the `camera.capture_custom` live-visual summary
`cameraHash` field). This is distinct from the scene file hash and must not be
inferred or substituted.

```powershell
.\Tests\Tools\FramePublication\CaptureFramePublicationPerformance.ps1 `
	-Project 'C:/Users/benti/Desktop/TerraEngine-Projects/New Animated Project' `
	-Scene 'C:/Users/benti/Desktop/TerraEngine-Projects/New Animated Project/Assets/Scenes/Scene.tescene' `
	-CameraHash '<recorded-camera-sha256>' `
	-Manifest '.\Tests\Tools\FramePublication\workloads.json' `
	-OutputDirectory '.\out\evidence\frame-publication' -DryRun
```

The command requires all six canonical scenarios, complete timing sources, the
fixed scene/camera identity, and a 30-second/120-frame warmup. Capture mode
requires five 60-second runs and preserves raw artifacts; unavailable timing is
reported as missing. `CompareFramePublicationPerformance.ps1` refuses any
identity mismatch rather than backfilling from the current machine. Contract
and soak scripts similarly fail when no configured executable can produce real
samples, so no fabricated evidence is accepted.

If the preserved baseline has `status: incomparable` or no identity, comparison
writes `comparable: false` with an explicit reason and exits nonzero; it never
backfills identity from the current machine.

After installation, run the native contract executable through the wrapper:

```powershell
& '.\Tests\Tools\FramePublication\RunFramePublicationContracts.ps1' `
	-InstallRoot '.\out\install\RelWithDebInfo' `
	-Manifest '.\Tests\Tools\FramePublication\workloads.json' `
	-OutputDirectory '.\out\evidence\frame-publication\contracts'
```

Start exactly one installed editor for live capture. Provide the recorded camera
hash; the tool refuses to infer it or use multiple editor processes:

```powershell
& '.\Tests\Tools\FramePublication\CaptureFramePublicationPerformance.ps1' `
	-Project $framePubProject -Scene $framePubScene -CameraHash $framePubCameraHash `
	-Manifest '.\Tests\Tools\FramePublication\workloads.json' `
	-InstallRoot '.\out\install\RelWithDebInfo' `
	-OutputDirectory '.\out\evidence\frame-publication\candidate' -Runs 5 -WarmupSeconds 30 -DurationSeconds 60
```

Compare only complete five-run bundles with matching identity. Finish with the
installed five-minute soak using `-DurationSeconds 300 -SampleIntervalSeconds 1`;
the soak retains one-second resource, generation, reuse, rejection, and present
samples and fails closed on missing adapters or incomplete evidence.
# Audio runtime foundation

Audio acceptance is intentionally split into deterministic and audible lanes.
The authoritative CI lane does not require an output device:

```powershell
cmake --preset RelWithDebInfo
cmake --build out/build/RelWithDebInfo --target TerraEngine-EngineTests TerraEngine-EditorTests TerraEngineAudioClipCompiler
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R "^(Engine|Editor)\\.Audio" --output-on-failure
./Tests/Tools/Audio/AcquireAudioFixtures.ps1 -VerifyCommitted
./Tests/Tools/Audio/GenerateAudioFixtures.ps1 -VerifyCommitted
cmake --build out/build/RelWithDebInfo --target install
```

To validate real decoding and mixing on a muted workstation or headless runner,
run the dedicated offline test:

```powershell
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R "^Engine.AudioOfflineRender$" --output-on-failure
```

This test configures miniaudio with `noDevice=true`, renders a committed WAV
fixture into an in-memory float PCM block, and checks its samples and deterministic
repeatability. It does not open a playback device and therefore is strong
non-audible evidence, but it does not replace the deferred human audible lane.

When a maintainer has an audible device, also run the installed editor audio
scenarios documented in `Docs/Audio.md`. A muted/headless run must record that
manual lane as deferred; it must never be reported as an audible pass.

## Effectively unbounded scenes

Configure and compile the strict canonical runtime from a clean checkout, then run the feature filter:

```powershell
cmake --fresh --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine TerraEngine-Editor TerraEngine-EngineTests TerraEngine-EditorTests TerraEngine-UnboundedSceneValidation
$env:TERRAENGINE_TEST_FILTER = 'WorldCoordinate;WorldSpatial;WorldStreaming;SpatialIndex;TransactionalMutation;SystemScheduler;RenderReferenceFrame;Physics.Frame;Unbounded'
& .\out\build\RelWithDebInfo\TerraEngine-EngineTests.exe
& .\out\build\RelWithDebInfo\TerraEngine-EditorTests.exe
Remove-Item Env:TERRAENGINE_TEST_FILTER
```

Run the complete suite and mandatory install gate after the focused tests:

```powershell
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -j 1 --output-on-failure
cmake --build --preset RelWithDebInfo --target install
```

The installed fixture manifests, workloads, tools, translated-world shaders, and runtime binaries must be validated from the runnable bundle at `out/build/RelWithDebInfo/Build`; source-tree fallbacks are failures. Qualification requires the installed scenario matrix, five-run 5% timing gate, fixed-residency 2% memory gate, five-minute smoke, and six-hour release soak. Missing hardware or evidence is incomplete, never a zero-cost pass.

## Terrain surface layers

Run the installed terrain surface matrix after the focused foundation and
sculpt lanes:

```powershell
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
    -R "^(Engine|Editor)\\.Terrain\\.(Surface|Layers)$" --output-on-failure
cmake --build --preset RelWithDebInfo --target install
```

The installed audit must discover `Tests/Tools/Terrain/workloads.json`, the
`terrain-surface-layers` record, runtime terrain shaders under
`Editor/Shaders/Terrain`, editor preview shaders in the same installed terrain
shader bundle, and the terrain fixture manifests. The layer workflow must
exercise 16 stable named layers, four local contributors, fifth-contributor
preview/displacement, missing-source fallback, boundary seams, and save/reload
canonical root/area hashes. Missing shaders, fallback sources, captures, or
hash evidence are unavailable failures; do not synthesize passing metrics.

## Terrain heightfield collision

Run the installed Jolt heightfield workload after the terrain collision
provider-neutral recipe and publication tests:

```powershell
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
    -R "^(Engine|Editor)\\.Terrain\\.(Collision|CollisionAlignment)$" --output-on-failure
cmake --build --preset RelWithDebInfo --target TerraEngineTerrainBenchmark
$installRoot = (Resolve-Path 'out/build/RelWithDebInfo/Build').Path
$tool = Join-Path $installRoot 'Tests/Tools/Terrain/TerraEngineTerrainBenchmark.exe'
$manifest = Join-Path $installRoot 'Tests/Fixtures/Terrain/manifest.json'
& $tool --fixture $manifest --route terrain-1000km-edit-route --collision true `
    --output (Join-Path $installRoot 'Tests/Tools/Terrain/terrain-collision-result.json')
cmake --build --preset RelWithDebInfo --target install
```

The collision workload records Jolt sample counts 257/513/1025 and requires
build/update latency, allocation bytes, sample count, and query-alignment
centimetres. Provider-neutral terrain collision headers must be present in the
installed `include/Physics/Terrain` tree. A missing backend, shader, query
capture, or timing counter is unavailable evidence, not a passing zero.

## Terrain procedural and spline operations

Run deterministic operation and cancellation coverage after the selection query,
procedural job, and spline grader sources are available:

```powershell
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
    -R "^(Engine|Editor)\\.Terrain\\.(Procedural|ProceduralJob|SplineGrading)$" --output-on-failure
cmake --build --preset RelWithDebInfo --target TerraEngine-EngineTests TerraEngine-EditorTests TerraEngineTerrainBenchmark
cmake --build --preset RelWithDebInfo --target install
```

The installed audit must discover the procedural workload, selection and
operation sources, cancellation/job receipts, and spline route evidence from
the installed terrain bundle. Verify strict affected bounds, deterministic
remap/terrace/noise/stamp/erosion hashes, cancellation with zero authored
changes, stale completion rejection, bounded memory/work estimates, and one
history group for road/river grading. Missing operation hashes or cancellation
receipts are unavailable evidence and must not be reported as passes.

## Terrain installed route and soak

Use the installed terrain bundle only. The soak script generates the sparse
256 km fixture, repeatedly executes the deterministic 1,000 km route, and
measures wall-clock elapsed route work with a stopwatch. `-DurationSecondsOverride`
is a bounded smoke run; it is useful for wiring checks but cannot satisfy the
30-minute acceptance gate.

```powershell
$installRoot = (Resolve-Path 'out/build/RelWithDebInfo/Build').Path
$tools = Join-Path $installRoot 'Tests/Tools/Terrain'
& (Join-Path $tools 'RunTerrainSoak.ps1') -InstallRoot $installRoot `
    -OutputDirectory (Join-Path $tools 'terrain-soak-smoke') `
    -DurationSecondsOverride 10 -Fixture terrain-256km `
    -Route terrain-1000km-edit-route
& (Join-Path $tools 'RunTerrainSoak.ps1') -InstallRoot $installRoot `
    -OutputDirectory (Join-Path $tools 'terrain-soak-30m') `
    -DurationMinutes 30 -Fixture terrain-256km `
    -Route terrain-1000km-edit-route `
    -TepakArchive (Join-Path $installRoot 'Content/Terrain.tpak') `
    -ContentMountRoot (Join-Path $installRoot 'Content')
```

When an archive or mount root is supplied, the script fails closed if it is
missing and records its SHA-256 route artifacts. Results must contain measured
elapsed seconds, iteration count, route identity, fixture/benchmark hashes,
and raw benchmark output. A duration field alone is not evidence. Missing
executables, fixtures, shaders, ContentMount/TEPAK inputs, backend counters, or
hardware capability are unavailable failures, never passing zeros.

## Terrain size expansion and crop gate

Terrain size expansion and crop adds the following focused registrations. Run them from a clean
RelWithDebInfo configure before the complete serial suite:

```powershell
cmake --fresh --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine TerraEngine-Editor TerraEngine-EngineTests TerraEngine-EditorTests TerraEngineTerrainBenchmark
$terrain055Regex = '^(Engine\.Terrain\.(Foundation|Resize|AtomicEdit|Surface|Lod|Residency|Collision|Procedural|MultithreadedStress|ExportRuntime)|Editor\.Terrain\.(CreateSculpt|CollisionAlignment|Procedural|Automation|History|Resize|SaveRecovery|LargeWorld))$'
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R $terrain055Regex --output-on-failure
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -j 1 --output-on-failure
cmake --build --preset RelWithDebInfo --target install
$installRoot = (Resolve-Path 'out/build/RelWithDebInfo/Build').Path
& (Join-Path $installRoot 'Tests/Tools/Terrain/VerifyInstalledTerrainContract.ps1') -InstallRoot $installRoot
```

The expected resize coverage is the signed/partial runtime fixture in
`Engine.Terrain.ExportRuntime`, the durable predecessor fixture in
`Editor.Terrain.SaveRecovery`, the 1x1-through-8x8 and long-narrow matrix in
`Engine.Terrain.MultithreadedStress`, and visible/automation fingerprint parity
in `Editor.Terrain.Automation`. The audit must find editor/runtime binaries,
terrain shaders, save-recovery fixtures, workload
records, benchmark/stress executables, and architecture verification scripts in
the installed tree. Source-tree artifacts do not satisfy this gate.

## Terrain rendering overhaul completion route

The terrain rendering overhaul uses durable CTest names rather than source-file names. Run the
matrix below three consecutive times before the full suite. Each invocation
must select every named lane and return zero; an empty selection is a failure.

| Contract | Registered CTest lanes |
|---|---|
| `TERRAROOT1` format and compatibility rejection | `Engine.Terrain.Format`, `Editor.Terrain.Compatibility` |
| Physical mapping and editor preview/apply/cancel | `Engine.Terrain.Mapping`, `Editor.Terrain.MappingPreview` |
| Layer limits, complete PBR, triplanar signs, and fallbacks | `Engine.Terrain.Surface`, `Editor.Terrain.Layers` |
| Effective filters, immutable sampler aliases, and residency touches | `Engine.Terrain.Sampler` |
| 258-sample halo, page cook, LOD, morph, stitch, and seam contracts | `Engine.Terrain.Lod` |
| Own-ancestor fallback, upload publication, eviction, and recovery | `Engine.Terrain.Residency` |
| Scheduler boundaries and concurrent rollback/visibility | `Engine.Terrain.MultithreadedStress` |
| Installed ContentMount/TEPAK runtime behavior | `Engine.Terrain.ExportRuntime` |

```powershell
$buildRoot = 'out/build/RelWithDebInfo'
$terrain054Regex = '^(Engine\.Terrain\.(Format|Mapping|Surface|Sampler|Lod|Residency|MultithreadedStress|ExportRuntime)|Editor\.Terrain\.(Compatibility|Layers|MappingPreview))$'
1..3 | ForEach-Object {
    Write-Host "Terrain rendering overhaul focused pass $_ of 3"
    ctest --test-dir $buildRoot -C RelWithDebInfo -R $terrain054Regex `
        --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Terrain rendering overhaul focused pass $_ failed." }
}
```

The broad `Engine.Terrain.Foundation` and `Editor.Terrain.Foundation` lanes are
useful extra coverage, but do not replace the named matrix: their umbrella
filters make a missing or misregistered focused lane harder to diagnose.

### Installed audit

Build `install` before auditing, then invoke the audit script from the installed
tree. The audit rejects missing or empty shaders, the retired vertex-only
terrain route, stale page ABI headers, reparse-point/source fallbacks, and
absolute checkout paths. ContentMount and TEPAK are checked separately because
the artifact audit does not claim live runtime capability.

```powershell
cmake --build --preset RelWithDebInfo --target install
$sourceRoot = (Resolve-Path '.').Path
$installRoot = (Resolve-Path "$buildRoot/Build").Path
$terrainEvidence = Join-Path $sourceRoot 'out/evidence/terrain-rendering-overhaul'
$auditOutput = Join-Path $terrainEvidence 'installed-audit'
$auditScript = Join-Path $installRoot 'Tests/Tools/Terrain/VerifyInstalledTerrainContract.ps1'
& $auditScript -InstallRoot $installRoot -OutputDirectory $auditOutput `
    -SourceRoot $sourceRoot
if ($LASTEXITCODE -ne 0) { throw 'Installed terrain contract audit failed.' }
$audit = Get-Content -Raw (Join-Path $auditOutput 'terrain-installed-contract.json') `
    | ConvertFrom-Json
if ($audit.status -ne 'available' -or $audit.shaderAudit.status -ne 'available' `
    -or $audit.pageFormatAudit.status -ne 'available' `
    -or $audit.sourceFallbackAudit.status -ne 'clear' `
    -or $audit.contentMountAudit.status -ne 'available' `
    -or $audit.tepakAudit.status -ne 'available') {
    throw 'Installed terrain audit is incomplete; unavailable ContentMount or TEPAK evidence is not a pass.'
}
```

Retain `terrain-installed-contract.json` with the completion evidence. Also
search the editor/runtime log captured by the live route for the absolute
`$sourceRoot`, `CMAKE_SOURCE_DIR`, and `PROJECT_SOURCE_DIR`; any match fails the
installed audit even if the static artifact scan passed.

### Exact 30-minute soak and live traversal

First run the installed CPU preparation/publication soak. Do not pass
`-DurationSecondsOverride` for acceptance: that switch creates smoke evidence
only. The script must report `status = measured`, at least 1,800 elapsed seconds,
a non-zero iteration count, the exact route identity, artifact hashes, and raw
metrics.

```powershell
$terrainTools = Join-Path $installRoot 'Tests/Tools/Terrain'
$soakOutput = Join-Path $terrainEvidence 'soak-30m'
& (Join-Path $terrainTools 'RunTerrainSoak.ps1') `
    -InstallRoot $installRoot `
    -OutputDirectory $soakOutput `
    -DurationMinutes 30 `
    -Fixture terrain-256km `
    -Route terrain-1000km-edit-route `
    -TepakArchive (Join-Path $installRoot 'Content/Terrain.tpak') `
    -ContentMountRoot (Join-Path $installRoot 'Content')
if ($LASTEXITCODE -ne 0) { throw 'Required 30-minute terrain soak failed.' }
$soak = Get-Content -Raw (Join-Path $soakOutput 'terrain-soak-result.json') `
    | ConvertFrom-Json
if ($soak.status -ne 'measured' -or $soak.elapsedSeconds -lt 1800 `
    -or $soak.iterations -lt 1 -or $soak.route -ne 'terrain-1000km-edit-route') {
    throw 'Terrain soak did not produce complete measured 30-minute evidence.'
}
```

The benchmark soak does not render and explicitly records GPU and live
ContentMount metrics as unavailable. It therefore never substitutes for the
live Vulkan traversal. Run the following route with the installed editor on a
real supported adapter, using an isolated terrain acceptance project:

```powershell
$terrainAcceptanceProject = (Resolve-Path $env:TERRAENGINE_TERRAIN_ACCEPTANCE_PROJECT).Path
$editor = Join-Path $installRoot 'Editor/TerraEngine-Editor.exe'
$liveOutput = Join-Path $terrainEvidence 'live-vulkan-30m'
New-Item -ItemType Directory -Force -Path $liveOutput | Out-Null
$editorStdout = Join-Path $liveOutput 'installed-editor.stdout.log'
$editorStderr = Join-Path $liveOutput 'installed-editor.stderr.log'
$editorProcess = Start-Process -FilePath $editor `
    -ArgumentList @('--project', $terrainAcceptanceProject) `
    -RedirectStandardOutput $editorStdout -RedirectStandardError $editorStderr `
    -PassThru
```

Start the 30-minute clock only after the project is loaded and Scene View,
Game View, Terrain, Resource Residency, and Scheduler Diagnostics are visible.
Execute this timed route without restarting the editor:

1. Minutes 0-5: traverse `terrain-multi-area-lod` in perspective and
   orthographic views across positive, negative, and render-origin-shifted area
   boundaries; exercise Scene, Game, shadow, capture, and picking views.
2. Minutes 5-10: edit tile metres, metre offsets, rotation, and mapping mode;
   inspect Base Color, signed-axis Normal, ORM, Emissive, and neutral fallback
   fixtures; apply one edit and cancel one edit.
3. Minutes 10-15: request nearest, bilinear, trilinear, and anisotropic policies;
   record requested and effective policy, sampler aliases, descriptor count,
   visible streaming touches, and any capability fallback reason.
4. Minutes 15-20: sweep around the 1.5-pixel threshold in both projections,
   cross area edges, exercise all 16 stitch masks, height/normal morph, and hole
   pinning; then hold the camera stationary for at least 60 seconds.
5. Minutes 20-25: withhold fine textures and pages through the installed
   residency controls, verify own-ancestor coverage with zero missing areas,
   publish the fine upload, and observe recovery only at the protected boundary.
6. Minutes 25-30: repeat the 1,000 km route, return to the initial working set,
   and capture the final residency/high-water values, diagnostics, profiler
   counters, screenshots, and log before closing the editor.

The retained live record must identify OS, GPU, driver, Vulkan capabilities,
filter/anisotropy limits, input/root/page hashes, visible areas and nodes,
demanded/resident/fallback pages, sampler/descriptor/touch counts, CPU
preparation/publication time, GPU frame time, allocations, seam/crack results,
and raw output. A capture is incomplete if any required metric is absent or
zero without a workload-specific proof that zero work was expected.

Unavailable hardware, adapter initialization failure, missing anisotropy, a
headless session, interrupted duration, missing capture tooling, or an absent
automation adapter must be recorded as `unavailable` or `incomplete`. None is a
pass, and CPU tests, dry-runs, smoke overrides, screenshots, or scheduled-but-
unexecuted routes may not be used to turn unavailable evidence green.

### Full terrain rendering overhaul completion commands

After the focused matrix, installed audit, measured soak, and live route are
complete, finish with the authoritative single-job suite and mandatory install
gate. Any command failure stops completion.

```powershell
cmake --fresh --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target `
    TerraEngine `
    TerraEngine-Editor `
    TerraEngine-EngineTests `
    TerraEngine-EditorTests `
    TerraEngineTerrainBenchmark
if ($LASTEXITCODE -ne 0) { throw 'Terrain rendering overhaul compile gate failed.' }

1..3 | ForEach-Object {
    ctest --test-dir $buildRoot -C RelWithDebInfo -R $terrain054Regex `
        --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "Terrain rendering overhaul focused pass $_ failed." }
}

ctest --test-dir $buildRoot -C RelWithDebInfo -j 1 --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Complete single-job CTest gate failed.' }
cmake --build --preset RelWithDebInfo --target install
if ($LASTEXITCODE -ne 0) { throw 'Mandatory terrain rendering overhaul install gate failed.' }
```

Re-run the installed audit after this final install. Completion requires the
same installed bytes that were audited, soaked, and traversed; evidence from a
different build, project identity, workload hash, or hardware identity is not
comparable.
# Lifecycle recovery validation

Configure with `BUILD_TESTING=ON` in an x64 Visual Studio developer environment,
then run `cmake --build --preset RelWithDebInfo --target install --parallel 2`.
The maintained installed tree is `out/build/RelWithDebInfo/Build`. Tests and
installed acceptance must use matching binaries and `correctness-artifacts.json`.

The six recovery CTest slices enforce these minimum selected case counts:

| Slice | Cases |
| --- | ---: |
| Engine.SceneLoadRecovery | 14 |
| Engine.NestedTaskSettlement | 11 |
| Editor.AssetDiscoveryRecovery | 8 |
| Editor.AssetImportAdmission | 13 |
| Editor.ExportArtifactValidation | 12 |
| Editor.ExportRetention | 10 |

Support slices `Engine.ArtifactValidation` and `Engine.OwnedImportExecutor`
require six and five cases. Nested settlement includes actual animation and
Vulkan rendering work; a helper-only run cannot satisfy that slice. Run the
full single-job CTest suite after these focused gates and compare exact failures
with the recorded pre-change baseline in
the retained lifecycle recovery validation record.

Imports default to four active physical jobs. `SetMaxActiveImports` accepts a
positive limit only while the owner is quiescent; failed configuration preserves
the current limit. Queued work and canceled-but-unsettled jobs remain owned.
`GetImportAdmissionDiagnostics` exposes capacity, queued/active/peak work,
cancellation and failure counts. Watcher recovery diagnostics expose pending
reconciliation and the accepted project generation. Catalog index saves coalesce
within one protected update; a rejected disk save retries on a later update.

Exports retain 64 terminal observations by default. The protected owner may
change that positive limit; lowering it expires the oldest results immediately.
Active work remains independently owned. `RetentionDiagnostics` reports the
limit, retained results, active/adoption work, expiry count and polling visits.
Consumers use explicit Active/Terminal/Expired/Unknown dispositions.

Use PowerShell 7 in the developer environment for installed acceptance. Give it
a fresh work directory outside the installed tree and retain an outer process
watchdog for the complete run, including NativeAOT compilation:

```powershell
$install = (Resolve-Path out/build/RelWithDebInfo/Build).Path
$pwsh = (Get-Command pwsh.exe).Source
& "$install/Tests/Tools/Correctness/RunWithWatchdog.ps1" `
	-Executable $pwsh -TimeoutSeconds 1800 `
	-LogDirectory out/validation/lifecycle-recovery-watchdog-fresh `
	-ArgumentList @('-NoProfile', '-File',
		"$install/Tests/Tools/LifecycleRecovery/RunInstalledLifecycleRecoveryAcceptance.ps1",
		'-InstallRoot', $install, '-WorkRoot',
		"$PWD/out/validation/lifecycle-recovery-fresh", '-AllowLiveAcceptance')
if ($LASTEXITCODE -ne 0) { throw 'Installed lifecycle acceptance failed.' }
```

The live editor uses the installed default layout and normal MCP approval UI.
Keep the Windows desktop unlocked for the complete interactive run. The editor
watchdog requests a visible window; native regression jobs stay hidden. A locked
desktop or unanswered dirty-scene exit prompt is a failed/incomplete live run,
not evidence of normal shutdown.
Acceptance requires fresh dirty reload/Save As/repaired reload, valid-word real
NativeAOT managed readiness, all native floors and normal process settlement.
Forced cleanup, stale artifacts, absent device checkpoints or missing approval
cannot produce a passing summary. See the feature quickstart and retained
validation record for the complete evidence map.

Installed acceptance on 2026-10-04 passed all eight lifecycle/support case floors
(14/11/8/13/12/10/6/5) with two actual frame workers. The live editor preserved
unsaved edits across failed reload, Save As and repaired-source reload; retained
managed callback continuity across export cancellation/retry; and closed normally
while export descendants were held. Normal exit reported zero surviving
processes and no watchdog timeout. The same fresh run validated a real NativeAOT
shared library, its bootstrap/hash/byte receipt, managed readiness, package
contents and orderly physics shutdown. Retained evidence is under
`out/validation/lifecycle-recovery-live-unlocked`, including both summaries and
the outer `lifecycle-recovery-live-watchdog-unlocked/watchdog.json`.

The final complete CTest run passed all 143 registrations in 377.31 seconds, including the repository naming audit with zero violations.

## Executable-adjacent Linux exports

The Linux launcher resolves `/proc/self/exe` before deriving its adjacent archive
and `Managed/` runtime paths. Deleted, unreadable, non-executable, relative or
missing discovery results fail with a diagnostic before mounting content. Windows
continues to use `GetModuleFileNameW`.

Linux compilation/runtime has not been executed on the Windows development host.
A genuine Linux build and complete export with bundled dependencies are required
for the following acceptance checks. Set `export_bin` to the real executable,
including its spaces if present, and run from a disposable directory containing
unrelated decoy archive/runtime files:

```bash
set -euo pipefail
export_bin=$(realpath '/absolute/path/to/My Game/MyGame')
export_dir=$(dirname "$export_bin")
exe_name=$(basename "$export_bin")
accept_dir=$(mktemp -d)
cd "$accept_dir"
timeout 60 "$export_bin" --validate-export-bootstrap > absolute.log 2>&1
relative_bin=$(realpath --relative-to="$PWD" "$export_bin")
timeout 60 "$relative_bin" --validate-export-bootstrap > relative.log 2>&1
PATH="$export_dir:$PATH" timeout 60 "$exe_name" --validate-export-bootstrap > path.log 2>&1
ln -s "$export_bin" game-alias
timeout 60 ./game-alias --validate-export-bootstrap > symlink.log 2>&1
# This diagnostic-only mode exits before mounting content or loading gameplay.
# A missing kernel link must fail; normal startup accepts no discovery override.
if timeout 60 "$export_bin" --validate-executable-discovery /proc/self/terraengine-missing-executable > discovery-failure.log 2>&1; then
    echo 'Missing executable discovery unexpectedly succeeded' >&2
    exit 1
fi
```

All four bootstrap invocations must use the real executable's adjacent archive
and runtime, with the same content fingerprints. Inspect the failure log for an
executable discovery diagnostic; a timeout alone is a failure of the check. Also
exercise the diagnostic seam with relative-target, deleted and non-executable
symlinks and require rejection. Preserve logs and actual Linux build results.

## Archive reads, independent terrain copies and HTTP authority

Configure with `cmake --preset RelWithDebInfo -DBUILD_TESTING=ON`, then run
`cmake --build --preset RelWithDebInfo --target install --parallel 2`. The installed
native harnesses and validation utilities live under
`out/build/RelWithDebInfo/Build/Tests`. Use the bundled compiler/dependency setup
and the developer environment described above.

Run the five independent native slices after installation:

```powershell
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R '^(Engine\.ContentArchiveSafety|Editor\.ContentArchiveSafety|Editor\.Terrain\.IndependentCopy|Editor\.McpAuthority|Engine\.AnimationCompileFailure)$' --no-tests=error --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Safety regressions failed.' }
# Check that all five entries were discovered and executed. Each harness slice
# enforces a nonzero contract case floor in its independent process watchdog.
& out/build/RelWithDebInfo/Build/Tests/Tools/ContentArchive/RunArchiveSafetyWatchdog.ps1 -InstallRoot out/build/RelWithDebInfo/Build -LogDirectory out/validation/archive-trees-fresh
& out/build/RelWithDebInfo/Build/Tests/Tools/ContentArchive/RunArchiveSafetyStress.ps1 -InstallRoot out/build/RelWithDebInfo/Build -LogDirectory out/validation/archive-stress-fresh
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo --output-on-failure
```

Use fresh evidence directories. Tree cases run in separate bounded children;
stress runs isolated reader processes, with unique fixture trees, alongside
animation worker exception/recovery cases. This does not assert that shared
reader objects permit concurrent mutation. Archive fixtures use the actual
writer for raw, compressed, dictionary, paged and empty output. Structural
corruptions retain valid metadata hashes so structural rejection is tested
independently. Range tests poison untouched payloads to verify lazy reads.
Extraction tests preserve external sentinels and exercise destination links;
record any privilege-dependent link coverage that could not run.

The Inspector's **Make Independent** action copies the captured saved terrain
root, including heights, holes, weights, layers and mapping metadata. Save edits
before requesting a copy. It prepares a private bundle outside the watched Assets
tree and reserves a fresh durable UUID. Pending status means preparation or a
queued rebind; receipt-confirmed status means the existing protected scene flush
committed the binding. Completed copy assets remain after undo so redo can
restore the binding. Failed preparation, import, admission or conflicting
receipts retain the original binding and conditionally clean up only owned copy
outputs. Cleanup failures remain visible and retryable; lifecycle transitions
settle receipts and owned work before releasing project resources.

For installed editor acceptance, create an isolated project with the normal
default editor layout. Author all terrain channels and at least three instances;
make two independent copies and compare three durable UUIDs, private stores and
resource paths. Edit one copy, save, reimport, close/reopen and compare original
and other-copy fingerprints. Observe pending/committed status and exercise
undo/redo. Native injected failure evidence must be distinguished from live
editor observations. Close normally and confirm owned workers/processes settle.

The HTTP listener requires exactly one `Host: 127.0.0.1:<actual-listener-port>`.
Native clients may omit Origin. Browser clients must use exactly one
`Origin: http://127.0.0.1:<actual-listener-port>`. Header names are case-insensitive;
outer whitespace is allowed, while duplicate/malformed authority fields, aliases,
omitted/wrong ports and alternate origins are rejected before session attachment
or JSON dispatch. Use the actual endpoint port after each restart. Interleave
rejected requests with valid native/browser traffic and compare dispatch counts,
selection, objects and session fingerprints. The installed bridge has no cumulative dispatch counter; its last diagnostic request and bounded activity log provide live fingerprints, while native callbacks prove exact rejected dispatch counts. Valid authority retains existing
read-only, write-mode, approval and cancellation controls.

## Texture ranges, RGBA rows, fatal task settlement and UI targeting

From the bundled developer environment, configure, compile and install:

```powershell
cmake --preset RelWithDebInfo -DBUILD_TESTING=ON
cmake --build --preset RelWithDebInfo --target install --parallel 2
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R '^(Engine\.TextureRangeValidation|Engine\.TextureRgbaLayout|Engine\.JobGraphFailureSettlement|Engine\.UIHitTesting)$' --no-tests=error --output-on-failure
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo --output-on-failure
```

The four owned-watchdog lanes require 8 texture bounds, 5 RGBA layout, 6 fatal
settlement and 6 UI hit-testing cases. Fatal settlement includes 100 held-worker
iterations at one worker and 100 at two workers, preserving the primary failure,
settled terminal states, no later admission and healthy graph reuse. The native
corpus generates legal mode-8 UASTC blocks with independently specified pixels;
structural admission, decoder success and representation are separate assertions.
Malformed reloads must preserve resident identity, pixels and byte accounting.

Installed GPU and physical-pointer acceptance requires an unlocked Windows
desktop and a fresh owned directory:

```powershell
& out/build/RelWithDebInfo/Build/Tests/Tools/Correctness/RunInstalledTextureUiAcceptance.ps1 -InstallRoot out/build/RelWithDebInfo/Build -WorkRoot out/validation/texture-ui-live-fresh -AllowLiveAcceptance
```

The installed runner verifies its artifact seal, matching native DLL closure,
bundled SDK, fixture generator and authored project. It seeds both default
layout templates, isolates preferences and recents, generates eight fresh
textures, checks loopback endpoint/process identity, compiles normal managed
scripts and enters Play. Approve any ordinary displayed request in the editor's
normal approval UI. Captures require `backendReadback=true` and
`deterministicFallback=false`; a fallback image never proves rendered acceptance.

After `pointer-ready.json`, use the actual Game View image bounds and the fixture
README to map logical button coordinates to desktop pixels. Observe real hover,
press/hold/release, second press, front overlap, hidden target and outside-image
input. The managed observer atomically writes real counters in
`observations/texture-ui.json`; it does not inject input. Save screenshots with
visible labels and retain coordinate/actions/count observations. Compare rendered
witness halves, quadrants, partial edges, alpha, sRGB midtone and compressed
control with the independently authored texture README; account for scaling and
display conversion. CPU byte tests remain the exact mip/color oracle.

The reviewing parent writes `live-review.json` only after these observations:
boolean fields `physicalPointer`, `hoverObserved`, `holdDidNotRepeat`,
`secondPressObserved`, `frontOnly`, `hiddenExcluded`, `outsideExcluded`,
`rgbaRegionsMatch`, `edgeRegionsMatch`, `mipRegionsMatch`, `alphaAndSrgbMatch`,
`compressedControlMatch`; at least six `observations` records and three
`screenshots` paths relative to WorkRoot. The runner validates these records,
two `kind: held-sample` records with distinct `state.updates`, `state.pointerDown=true` and translated count 1, then a `kind: release` record with `state.pointerDown=false` and count 1. It checks actual final counters (translated 2, front 1, back/hidden 0), captures a final GPU
image, exits Play and closes normally. A missing review remains pending; watchdog
termination, a locked desktop or synthetic native pointer tests cannot count as
physical acceptance. Preserve summary.json, MCP logs, PNGs and process logs.

## Capturing displayed runtime UI

Activate the Game View before reviewing the installed texture/UI fixture.
`game_view.capture` with `freshness: latestCompleted` reads its displayed target.
The `nextFrame` route creates a separate camera render with no UI; likewise,
custom-camera output does not prove these UI swatches. Keep backend readback
flags and frame IDs, review interior pixels with the documented color/blend
conversion, and use a native target-size capture for one-pixel mip controls.
Exercise a Game View resize as part of physical hover/click acceptance.
When the runner writes `pointer-ready.json`, activate and observe that owned
editor's Game View, then write `game-view-ready.json` with `observed: true` and
its `editorPid`. This prevents a fresh Scene-tab layout from being captured as
UI evidence. Proceed with pixel and physical review only after this actual view
is visible; write `live-review.json` after those checks pass.


## Visual Studio cannot find Windows SDK tools

If CMake fails its compiler check with `rc` unavailable or
`CMAKE_MT-NOTFOUND`, check the inherited `PATH` before reinstalling the SDK.
Windows `cmd.exe` ignores inherited environment variables longer than 8,191
characters. Temporary `.dotnet/tools` entries from isolated CLI homes can push
PATH past that limit and prevent Visual Studio's developer setup from finding
`reg.exe`, the SDK headers/libraries, `rc.exe`, and `mt.exe`.

TerraEngine's managed build and NativeAOT export commands set
`DOTNET_ADD_GLOBAL_TOOLS_TO_PATH=0`. This keeps SDK first-run setup from adding
each disposable `DOTNET_CLI_HOME` to the user's persistent PATH. Also set it
when manually running the SDK with a temporary CLI home; the older
`DOTNET_SKIP_FIRST_TIME_EXPERIENCE` setting does not provide this protection.

For an already affected machine, back up the user PATH and remove only the
recognized temporary build/export `.dotnet/tools` entries. Preserve normal
tool installations and the machine PATH. Close and reopen Visual Studio so it
inherits the repaired environment, then regenerate the selected CMake preset
and build/install using the commands above. Do not delete project sources or
install replacement system dependencies to work around a missing PATH.

References: [Windows command prompt variable limit](https://learn.microsoft.com/en-us/troubleshoot/windows-client/shell-experience/command-line-string-limitation)
and [.NET environment variables](https://learn.microsoft.com/en-us/dotnet/core/tools/dotnet-environment-variables).

### Native GLFW recovery regression

On a machine with an available native desktop, enable the isolated renderer
recovery test from an x64 Visual Studio developer terminal:

```powershell
cmake --preset RelWithDebInfo -DBUILD_TESTING=ON -DTERRAENGINE_BUILD_NATIVE_GLFW_RECOVERY_TEST=ON
cmake --build --preset RelWithDebInfo --target install --parallel 2
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R '^Editor\.RendererEfficiency\.NativeGlfwRecovery$' --no-tests=error --output-on-failure
& ./out/build/RelWithDebInfo/Build/Tests/Editor/NativeGlfwRecovery/TerraEngine-RendererEfficiencyGlfwRecoveryTests.exe
```

The test creates one hidden GLFW window and uses the bundled GLFW/ImGui
backends. It checks actual main-viewport destruction, the production platform
rebind, preserved editor callbacks, the first recovered GLFW frame, and cleanup.
Its installed directory includes its runtime libraries. The option defaults to
off because a native desktop is required; the headless editor tests remain
available independently. No Vulkan device is created, so these results support
the repair but do not replace the installed renderer-efficiency GPU matrix,
device-recovery receipts, or normal editor shutdown checks.

The ordinary `Engine.RendererEfficiency` suite also checks fallback texture
descriptor admission before renderer readiness. A scoped initialization grant
permits writes to the fresh descriptor sets without adding an idle wait; runtime
writes retain their existing synchronization. The CPU regression checks admission
and revocation. The installed recovery fixture must separately prove actual GPU
descriptor validity on both the initial and recreated device.

The frozen delete-then-transform rollback fixture produces the scene queue's
actual `conflicted` decision. Its host and evidence verifier require zero applied
writes, both operations rejected, and identical protected target state before and
after the existing flush. A fabricated `Rejected` label or incomplete rejected
write count cannot satisfy that fixture.

Cold HiZ descriptor slots use the device-owned white 1x1 texture until that
view has a completed, readable pyramid. The fallback supplies valid image
queries without making history ready or changing the occlusion gates. CPU
descriptor-plan tests cover cold and mixed-view selection; the installed
GPU-assisted recovery diagnostic must check both native device epochs before
the ordinary comparison matrix can establish acceptance.

The rollback fixture retains a validated prior scheduler packet and the actual
renderer spatial revision at its protected preparation boundary. A conflicted
transaction publishes no new revision, so its raw receipt may correctly contain
zero. The fixture proves unchanged target state and compares rendered frames
with the observed prior revision; it does not create a publication or flush the
scene journal to manufacture that token.

Shadow and LTC resource rebuilds retain their descriptor indices and rewrite
retiring descriptors to the uploaded white fallback at the existing protected
idle boundary before destroying image views or samplers. Active replacements
then bind their real images to those same slots. Bulk resource clearing also
removes texture aliases before reusing the descriptor heap. The
`Renderer.Efficiency.TextureRetirement` cases exercise retirement ordering,
stable assignments, and rejection before destruction; native validation and
rendered comparisons remain necessary to establish GPU correctness.

The `Renderer.Efficiency.Editor.AssetImportTemporary` cases exercise the same
Project Browser admission helper used by both directory listings. Exact private
model-package staging paths (`.ta-` followed by 32 or 16 lowercase hexadecimal
characters) and their descendants are rejected before filesystem status probes.
Authored near-matches remain visible, and their filesystem errors still propagate.
The regression injects a denied status probe without depending on a transient
Windows file lock. Run it from the freshly built and installed test tree:

```powershell
$priorAssetTemporaryTestFilter = [Environment]::GetEnvironmentVariable('TERRAENGINE_TEST_FILTER', 'Process')
try {
	$env:TERRAENGINE_TEST_FILTER = 'Renderer.Efficiency.Editor.AssetImportTemporary'
	& ./out/build/RelWithDebInfo/Build/Tests/Editor/TerraEngine-EditorTests.exe
	if($LASTEXITCODE -ne 0) { throw 'Private import-path regressions failed.' }
} finally {
	[Environment]::SetEnvironmentVariable('TERRAENGINE_TEST_FILTER', $priorAssetTemporaryTestFilter, 'Process')
}
```

These policy checks support the installed fixture workflow; they do not replace
normal editor lifecycle checks or the full renderer comparison matrix.

## Renderer efficiency P1–P5 workflow

The renderer efficiency implementation has passed the P1–P5 integrated code
gates. Consult `out/evidence/renderer-efficiency/final/build-and-tests.json` for
the final integrated compile/install and CPU verification outcome. Native
Serial/Concurrent collection and GPU/performance acceptance remain paused by
user direction. Supporting diagnostics and a preserved first-stage installation
do not establish an accepted comparison or total-frame regression baseline.

From an x64 Visual Studio developer PowerShell at the repository root:

```powershell
git submodule update --init --recursive
cmake --preset RelWithDebInfo -DBUILD_TESTING=ON
cmake --build --preset RelWithDebInfo --target install --parallel 2
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R '^(Engine|Editor)\.RendererEfficiency$|^Renderer\.Efficiency\..*Contracts$' --no-tests=error --output-on-failure
```

Use the maintained installation at `out/build/RelWithDebInfo/Build`. Verify the
files declared by its `renderer-efficiency-artifacts.json` and
`correctness-artifacts.json`, including their actual hashes, before using
installed tools. A checkout runner or mixed installation is not supported.

The current full registration contains 168 CTest groups. This CPU command selects
166, excluding the mixed nested-settlement group and hidden-window GLFW test:

```powershell
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -E '^(Engine\.NestedTaskSettlement|Editor\.RendererEfficiency\.NativeGlfwRecovery)$' --no-tests=error --output-on-failure
```

`Engine.SpatialUnitTests` already excludes the nested-settlement prefix. Run its
ten CPU-only cases separately through the installed watchdog wrapper:

```powershell
$installedTests = './out/build/RelWithDebInfo/Build/Tests'
$nestedCpuFilter = 'Engine.NestedTaskSettlement.Animation,Engine.NestedTaskSettlement.UnqueuedPooledVoidRemoved,Engine.NestedTaskSettlement.NoThrowUnwind,Engine.NestedTaskSettlement.ConsumedFutureSkipped,Engine.NestedTaskSettlement.HeldSiblingSettled,Engine.NestedTaskSettlement.PrimaryExceptionPreserved,Engine.NestedTaskSettlement.SuccessfulCollection'
& "$installedTests/Tools/Correctness/RunNativeCorrectnessSlice.ps1" -Executable "$installedTests/Engine/TerraEngine-EngineTests.exe" -Filter $nestedCpuFilter -MinimumCases 10 -TimeoutSeconds 120 -LogDirectory './out/evidence/renderer-efficiency-final-nested-cpu'
if ($LASTEXITCODE -ne 0) { throw 'Nested CPU settlement slice failed.' }
```

This compound filter does not change the normal mixed group's minimum of 11.
The separately registered `Editor.RendererEfficiency.NativeGlfwRecovery` uses a
real hidden GLFW window and needs a desktop; it is not part of the 166-group CPU
command. Neither command proves rendered pixels or GPU completion.

See the [renderer efficiency tools README](../Tests/Tools/RendererEfficiency/README.md)
for exact installed runner parameters, exit codes, fresh-root requirements,
transient options, fixtures and evidence paths. The installed manifest and schema
retain the frozen comparison counts and tolerances. Native commands are documented for a later explicitly
resumed run, not as a continuation of the CPU checks.

P5 reserves queried image requirements and reconciles actual allocation bytes
against its additional 64 MiB default budget. Zero disables admission; the
too-small fixture uses one byte. Reserved, live and retiring owners remain
charged, including pending recorded/submitted uses. Lowering a budget drains
without new admission; unsupported admission retains the complete final-shadow
fallback. Native receipts preserve partition rows, source copies, physical
inventory and actual command/submit/completion mappings.

Collection requires normal owned cleanup and complete content-addressed artifact
closure. First-control, residency and lighting-baseline receipts do not replace
the 60 warmup/300 measured windows. A failed collector or incomplete session
cannot be manually promoted into accepted evidence. The current independent
baseline compatibility collector deliberately reports Unverified until actual
matched total-frame baseline proof is supplied.
