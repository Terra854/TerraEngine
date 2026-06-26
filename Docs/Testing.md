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

The editor MCP bridge checks live under `Tests/Editor/Automation`. They cover
automation envelopes, capability metadata, local-only transport checks, bridge
state transitions, bounded activity history, approval prompts, and resource/tool
routing. The glTF package import checks live under both
`Tests/Editor/AssetPipeline` and `Tests/Editor/Automation`; they validate
fastgltf package parsing, generated material/texture/assignment metadata,
diagnostics, stable reimport source hashes, MCP import responses, and MCP path
boundary rejection. For a focused Windows run, build through the Visual Studio
developer environment and run the editor harness with the MCP/Automation or glTF
filters:

```powershell
cmd /c "C:\PROGRA~1\MIB055~1\18\COMMUN~1\Common7\Tools\VsDevCmd.bat -arch=x64 -host_arch=x64 && cmake --build --preset Debug --target TerraEngine-EditorTests TerraEngine-Editor"
$env:TERRAENGINE_TEST_FILTER="Editor.MCP;Editor.Automation;Editor.AssetPipeline.Gltf"
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
