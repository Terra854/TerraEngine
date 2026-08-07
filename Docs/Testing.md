# TerraEngine Testing

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
$vgTools = Join-Path $installRoot 'Tools/VirtualGeometry'
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

The current closure is pinned to .NET SDK `10.0.302`, runtime pack `10.0.10`,
and RID `win-x64`. Confirm the exact SDK and verify every archive, license, and
manifest entry before configuring:

```powershell
dotnet --version
$archives = @(
	Get-ChildItem .nuget/feed/*.nupkg |
	Select-Object -ExpandProperty FullName
)
$env:NUGET_CERT_REVOCATION_MODE = "offline"
dotnet nuget verify @archives --all --verbosity minimal
Remove-Item Env:NUGET_CERT_REVOCATION_MODE
cmake -DTERRAENGINE_RUNTIME_ID=win-x64 `
	-P cmake/TerraEngineOfflineNuGet.cmake
```

The first command must print `10.0.302`. NuGet verifies each archive's
available author and repository signatures without an online revocation
lookup. The CMake verifier rejects changed or unmanifested packages, mismatched
SDK metadata, and changed legal files. Set
`-DTERRAENGINE_NUGET_CACHE_MODE=OFF` when network access is prohibited; this
keeps verification fail-closed instead of regenerating a missing archive.
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
$vgTools = Join-Path $installRoot 'Tools/VirtualGeometry'
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
`Tools/Animation/` for fixture generation, installed-model checks, performance
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
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/ExportRuntimeBaseline/RunBaseline.ps1 -InstallRoot out/build/RelWithDebInfo/Build -WorkRoot out/ci/export-runtime-baseline
```

Run the same installed gate beyond 360 characters whenever editor scripting,
NativeAOT, package staging, or output publication changes:

```powershell
$deepWorkRoot = "out/ci/export-runtime-baseline/" + (
  (1..8 | ForEach-Object { "segment-abcdefghijklmnopqrstuvwxyz-{0:d2}" -f $_ }) -join "/"
)
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File Tools/ExportRuntimeBaseline/RunBaseline.ps1 `
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
out/build/RelWithDebInfo/Build/Editor/Managed/TerraEngine.CSharp.dll
out/build/RelWithDebInfo/Build/Editor/Managed/Analyzers/TerraEngine.Export.Generator.dll
out/build/RelWithDebInfo/Build/Launcher/TerraEngine-Launcher.exe
```

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
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Physics/RunInstalledEditorPhysicsStress.ps1 -InstallRoot out/build/RelWithDebInfo/Build -WorkRoot out/validation/physics-live -ObjectCount 10000 -ActiveDynamicCount 1000 -ExplicitStepCount 100 -AllowLiveStress
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/ExportRuntimeBaseline/RunBaseline.ps1 -InstallRoot out/build/RelWithDebInfo/Build -WorkRoot out/ci/physics-export-baseline
```

The live harness accepts `-RunManagedReloadAutomationLoopback` for managed/automation checks and
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
& .\Tools\VirtualGeometry\CaptureVirtualGeometryLiveVisual.ps1 `
	-InstalledEditor 'out/build/RelWithDebInfo/Build/Editor/TerraEngine-Editor.exe' `
	-Project 'C:\Users\benti\Desktop\TerraEngine-Projects\New Animated Project' `
	-Asset 'Assets/ValidationModels/DenseAnimatedVG/CesiumMan.glb' `
	-Prefab 'Assets/ValidationModels/DenseAnimatedVG/CesiumMan_Generated/CesiumMan.teprefab' `
	-CreateValidationScene -CleanupValidationScene `
	-ValidationScenePath 'Assets/Scenes/.terra-validation/DenseAnimatedVGSoak5m.tescene' `
	-OutputDirectory 'Tests/Fixtures/RendererPerformanceBaseline/results/soak/dense-animated-vg-5m' `
	-ValidationCameraPosition 0.0,0.5,1.0 -ValidationCameraTarget 0.0,0.5,0.0 `
	-SoakDurationSeconds 300 -SoakSampleIntervalSeconds 10 -McpPort 43129 `
	-ValidationRoute hardware -RepresentationRoute virtual-hardware `
	-PrefabRequestedAnimationMode 1 -EnterPlayMode -ExpectAnimatedChange
```

The Profiler idle-overhead gate uses the same installed editor, project,
disposable dense-animation scene, and fixed port for both sides of every pair.
It alternates closed/open-idle order, records unlimited frame history, and
requires each pair's median frame-time delta to remain within two percent. Run
five 60-second pairs and retain the generated summaries and manifest:

```powershell
& .\Tools\Profiling\MeasureProfilerIdleOverhead.ps1 `
	-InstalledEditor 'out/build/RelWithDebInfo/Build/Editor/TerraEngine-Editor.exe' `
	-Project 'C:\Users\benti\Desktop\TerraEngine-Projects\New Animated Project' `
	-OutputDirectory 'Tests/Fixtures/RendererPerformanceBaseline/results/profiler' `
	-PairCount 5 -CaptureDurationSeconds 60 -McpPort 43129
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

The install target also runs managed `dotnet` publish/build targets. If
`dotnet` is not available on PATH, native test targets can still validate but
install will fail at managed publish.

### Renderer Stability Hardening

scene runtime validation adds deterministic filters for `VulkanPresentationLifecycle`,
`VulkanUiSubmissionFailure`, `EditorFrameCompletionTracker`,
`Editor.StartupRenderer`, `Editor.PostFrameBoundary`,
`Editor.CheckedImGuiVulkanBridge`, and `Editor.ViewportTextureRetirement`.
Run both test executables with those filters before live GPU work.

Live resize/minimize validation is deliberately opt-in and paced. Use
`Tools/RendererStability/RunLiveSmoke.ps1` with `-AllowLiveGpuStress`, at most
50 operations per batch, and inspect its process/driver evidence after each
batch. The harness snapshots the latest Windows System-log RecordId/time before
launch and polls after every resize and throughout minimize/restore waits. A
qualifying newer display-driver event records `driver-event-failure.json`, stops
the launched editor process tree, and never advances to the next operation; an
older event cannot invalidate the run. Run the CPU-only seam regression before
live work with `powershell.exe -NoProfile -ExecutionPolicy Bypass -File
Tools/RendererStability/RunLiveSmoke.Tests.ps1`. The harness also waits for the
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
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Physics/RunInstalledEditorPhysicsStress.ps1 `
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

## animation production validation profiler v2 evidence

The installed harness keeps profiler capture selection additive. New recordings
use payload v2; v1 compatibility is verified by immutable fixture load tests and
never by rewriting a v1 artifact. Use `-RunProfilerCaptureProfilerChecks` with
`-CaptureLevel detailed|sampled|aggregate` and `-CaptureVersion v2`:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Physics/RunInstalledEditorPhysicsStress.ps1 `
    -InstallRoot out/build/RelWithDebInfo/Build `
    -WorkRoot out/validation/profiler-capture/detailed-v2 `
    -CaptureLevel detailed -CaptureVersion v2 -RunProfilerCaptureChecks -AllowLiveStress
```

Evidence records the selected level/version, frame counts, comparable baseline,
metric availability, artifact bytes, and a durable committed-artifact load probe.
The harness holds the recording open for at least five wall-clock seconds and
applies a 10 MiB size gate. Missing measurements remain explicit unavailable
values rather than being invented as zero.
Compare like-for-like level, version, workload, workers, and duration.

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
bounded memory/retirement, and 30 minutes without unrecovered failure.

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
