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

Scene View overlay coverage for `specs/037-scene-view-overlays` lives in the
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

This pass currently proves fail-closed containment and diagnostics. Full
loss-safe Vulkan device teardown/recreation is intentionally tracked separately
because a real `VK_ERROR_DEVICE_LOST` cannot safely reuse the ordinary
device-idle cleanup path.

Visible-command reuse coverage validates static-view cache-hit telemetry,
deduplicated streaming touch publication, scheduler resource declarations,
automation stats, profiler rows, and repeated-resource performance gates. Use
this focused pass while iterating on `specs/032-visible-command-reuse`:

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
`specs/035-gpu-profiler-stats` lives in the existing engine/editor harnesses.
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

Feature `specs/029-virtual-geometry-hardening` adds the staged hardening pass
for Hi-Z correctness, explainable Hi-Z diagnostics, real page streaming,
counter source truth, transition stability, multi-view independence, visualizer
cleanup, and broad asset coverage:

```powershell
ctest --test-dir out/build/Debug -R "Engine.VirtualGeometryHardening|Editor.VirtualGeometryHardening" --output-on-failure -C Debug
```

Feature `specs/030-vg-gpu-driven-batching` adds the staged virtual geometry
scaling pass for cluster packet batching, runtime compaction, real page
residency, performance gates, shadow-view selection, cone-safety metadata, and
Hi-Z baseline preservation:

```powershell
ctest --test-dir out/build/Debug -R "Engine.VirtualGeometryBatching|Engine.VirtualGeometryCompaction|Engine.VirtualGeometryPerformanceGate|Engine.VirtualGeometryShadow|Engine.VirtualGeometryCulling|Engine.VirtualGeometryRenderer|Engine.VirtualGeometryResource|Editor.VirtualGeometryAssetPipeline|Editor.VirtualGeometryAutomation|Editor.VirtualGeometryExport|Editor.VirtualGeometryDiagnostics" --output-on-failure -C Debug
```

When implementing `030`, run each phase or user-story slice before advancing to
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

Feature `specs/031-vg-gpu-compaction` advances the already-selected virtual
geometry cluster cut into GPU-visible packet buffers, GPU packet compaction,
generated submission records, and source-labeled per-view counter readbacks.
The current CPU-selected cluster cut remains the correctness oracle, and the
current Hi-Z behavior remains locked as the accepted baseline:

```powershell
ctest --test-dir out/build/Debug -R "Engine.VirtualGeometryBatching|Engine.VirtualGeometryCompaction|Engine.VirtualGeometryPerformanceGate|Engine.VirtualGeometryHiZ|Engine.VirtualGeometryMultiView|Engine.VirtualGeometryRenderer|Engine.Renderer|Engine.SystemScheduler" --output-on-failure -C Debug
```

When implementing `031`, run each phase or user-story slice before advancing to
the next one. The GPU packet buffer slice must validate packet and batch layout
before shader consumption is enabled. The compaction slice must validate
candidate, compacted, rejected, fail-open, generated submission, and overflow
counters. The Hi-Z slice is preservation-only: it must prove the existing
moving-wall, camera-cut, resize, thin-geometry, and multi-view behavior still
matches the accepted baseline without adding quality tiers or retuning
occlusion.

The feature `031` performance gates fail when dense selected packet work does
not collapse to submitted batches at or below `10%`, when CPU visible-command
preparation regresses beyond the configured budget, when compaction counters
miss fixture truth by more than `1%` or one packet, when generated submissions
do not match frame/view ownership, when Scene View and Game View share state
across 300 frames, or when the Hi-Z false-occlusion count is non-zero.
Unsupported prerequisites are reported as `Unsupported`, not as passing
zero-valued metrics.

Feature `specs/033-vg-page-streaming` turns virtual geometry residency into a
true page-sourced render contract. Use the page-streaming slices while
implementing that feature:

```powershell
ctest --test-dir out/build/Debug -R "VirtualGeometryPageStreaming|VirtualGeometryPageResidency|Engine.VirtualGeometryResource|Engine.VirtualGeometryHiZ|Engine.VirtualGeometryMultiView|Engine.VirtualGeometryPerformanceGate" --output-on-failure -C Debug
```

For the page request, budget pressure, eviction, and re-request lifecycle, run
the resource-labeled slice after the engine test target builds:

```powershell
ctest --test-dir out/build/Debug -C Debug -L Resource --output-on-failure
```

For US6 page-source diagnostics, build both native test targets and run the
registry filters that match the editor-facing contract names:

```powershell
cmd /c "set TERRAENGINE_TEST_FILTER=VirtualGeometry.Diagnostics.US6&& out\build\Debug\TerraEngine-EngineTests.exe"
cmd /c "set TERRAENGINE_TEST_FILTER=VirtualGeometry.Editor.Diagnostics.US6&& out\build\Debug\TerraEngine-EditorTests.exe"
cmd /c "set TERRAENGINE_TEST_FILTER=VirtualGeometry.Editor.Automation.US6&& out\build\Debug\TerraEngine-EditorTests.exe"
```

The literal task checkpoint
`ctest --test-dir out/build/Debug -C Debug -R VirtualGeometry.Editor --output-on-failure`
currently matches no top-level CTest target. Treat the registry filters above
as the authoritative US6 validation until CTest target names are expanded.

For US7 asset cook/runtime integrity, run the inspector, runtime loader, and
automation slices before the asset-pipeline CTest checkpoint:

```powershell
cmd /c "set TERRAENGINE_TEST_FILTER=VirtualGeometry.Editor.Inspector.US7&& out\build\Debug\TerraEngine-EditorTests.exe"
cmd /c "set TERRAENGINE_TEST_FILTER=Resource.Manager.Mesh.US7&& out\build\Debug\TerraEngine-EngineTests.exe"
cmd /c "set TERRAENGINE_TEST_FILTER=VirtualGeometry.Editor.Automation.US7&& out\build\Debug\TerraEngine-EditorTests.exe"
ctest --test-dir out/build/Debug -C Debug -R AssetPipeline --output-on-failure
```

These checks protect the TEMF v1 page-integrity contract, tiny/simple mesh skip
reasons, mesh-only Inspector controls, and automation fields for page readiness
or integrity failure diagnostics.

For US8 page-streaming performance and memory gates, run the page-gate filters
before the broad VirtualGeometry label checkpoint:

```powershell
cmd /c "set TERRAENGINE_TEST_FILTER=VirtualGeometry.PerformanceGate.US8&& out\build\Debug\TerraEngine-EngineTests.exe"
cmd /c "set TERRAENGINE_TEST_FILTER=VirtualGeometry.Render.MultiView.US8&& out\build\Debug\TerraEngine-EngineTests.exe"
ctest --test-dir out/build/Debug -C Debug -L VirtualGeometry --output-on-failure
```

The US8 gates fail when resident page payload bytes exceed the configured page
budget, resident page payloads approach full owning-mesh residency, page-source
coverage is missing, embedded full-mesh fallback is used as the normal path, or
Scene View/Game View page-source counters lose their view/counter-source
identity.

The clean-checkout fixture names are `page-sourced-resident-payload`,
`page-fallback-only-coverage`, `page-budget-churn`, and
`page-integrity-malformed`; `dense-page-budget-streaming-gate` adds the US8
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

US9 broad asset coverage is represented by
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

For feature `034` true page residency, run exact Resource Residency checks
before renderer payload checks:

```powershell
ctest --test-dir out/build/Debug -C Debug -R "ResourceResidency|ResourceStreaming|ResourceManagerMesh|ResourceResidencyWindow" --output-on-failure
ctest --test-dir out/build/Debug -C Debug -R "VirtualGeometry\\.(Render|PageStreaming|ShaderLayout|Batching|HiZ|Hardening|MultiView|Diagnostics|PageResidency)|VirtualGeometry.*DebugView" --output-on-failure
```

These tests must prove that `Stream CPU Bytes` and `Render GPU Bytes` are exact
current live bytes for the row's asset, package parent rows do not borrow child
bytes, unavailable accounting stays non-numeric, and page-source diagnostics do
not silently treat owning mesh buffers as true page-owned rendering. Current
feature `034` builds still decode shader geometry through owner mesh streams;
until a page-local shader-decodable payload layout lands, the no-full-owner
performance gate should expose owning-mesh fallback bytes with an explicit
reason instead of passing as a page-streamed normal path.

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

During feature `029`, run the hardening slice after each phase or user story,
then run the broader virtual-geometry regression set before the install target.
The hardening fixture manifest lives at
`Tests/Fixtures/VirtualGeometry/hardening-fixtures.json`.

During feature `030`, rebuild the compiler tools before CTest whenever
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
