# TerraEngine Testing

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

Active object handle coverage for `specs/022-active-object-handles` extends the
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

The current Vulkan backend advertises native image readback as unavailable, so
MCP capture tools return a deterministic PNG fallback plus metadata and
diagnostics. This proves the MCP image transport, disabled-camera resolution,
custom-camera validation, and protected-boundary diagnostics without claiming
live GPU pixels until a backend readback implementation lands.

After code changes, rebuild the Visual Studio-compatible targets and run the
install target so native, managed, editor, launcher, and shader-tool outputs are
verified together:

```powershell
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"" >nul && cmake --build out\build\CodexDebug --target TerraEngine-EngineTests TerraEngine-EditorTests --config Debug"
ctest --test-dir out\build\CodexDebug --output-on-failure -C Debug
cmd /c "call ""C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"" >nul && cmake --build out\build\CodexDebug --target install --config Debug"
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
while implementing `specs/026-virtual-geometry` tasks:

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

Feature `specs/027-adaptive-virtual-geometry` adds focused adaptive slices for
configurable cook ratios, Inspector-only per-mesh overrides, real page
streaming, stronger Hi-Z validation, mixed-detail cluster selection, and runtime
visualization. Use this pass while implementing or reviewing that feature:

```powershell
ctest --test-dir out/build/Debug -R "Engine.AdaptiveVirtualGeometry|Editor.AdaptiveVirtualGeometry" --output-on-failure -C Debug
```

The broader virtual-geometry regression set covers renderer, resource,
editor asset pipeline, automation, export, and debug visualizer behavior:

```powershell
ctest --test-dir out/build/Debug -R "Engine.VirtualGeometryRenderer|Engine.VirtualGeometryResource|Editor.VirtualGeometryAssetPipeline|Editor.VirtualGeometryAutomation|Editor.VirtualGeometryExport" --output-on-failure -C Debug
```

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

After virtual geometry code changes, always finish with the normal configure,
compile, CTest, and install verification flow so the installed editor/runtime
tree has the matching TEMF loader, shaders, diagnostics, and asset pipeline
behavior:

```powershell
cmake --preset Debug
cmd /c "C:\PROGRA~1\MIB055~1\18\COMMUN~1\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake --build --preset Debug --target TerraEngine TerraEngine-Editor TerraEngine-EngineTests TerraEngine-EditorTests"
ctest --test-dir out/build/Debug -R "Engine.AdaptiveVirtualGeometry|Editor.AdaptiveVirtualGeometry|Engine.VirtualGeometryRenderer|Engine.VirtualGeometryResource|Editor.VirtualGeometryAssetPipeline|Editor.VirtualGeometryAutomation|Editor.VirtualGeometryExport" --output-on-failure -C Debug
cmd /c "C:\PROGRA~1\MIB055~1\18\COMMUN~1\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake --build --preset Debug --target install"
```

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
`Windows > MCP Bridge` to confirm disabled/read-only startup, local-only
connection behavior, capability/resource listing, write-mode blocking, approval
prompts, and activity history.

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

The baseline sample lives at `Tests/Fixtures/ExportRuntimeBaseline`. Its checked-in
TEMF/TETF/TMAT files are placeholders for compiled binary assets; regenerate
them through the asset compilers before using the project as a runnable manual
export. The automated tests still guard the manifest, scene references, UI
signal, script signal, and startup coordinator decision.

## Renderer Performance Baseline

Renderer threading fixtures live at `Tests/Fixtures/RendererPerformanceBaseline`.
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

### Install After Code Changes

Run the install target before finishing any task that changed code so the
install tree stays verified with the compiled outputs.

```bash
cmake --build out/build/Debug --target install
```

The install target also runs managed `dotnet` publish/build targets. If
`dotnet` is not available on PATH, native test targets can still validate but
install will fail at managed publish.

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
