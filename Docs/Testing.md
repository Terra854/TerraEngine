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

The scheduler checks cover the phase scheduler, general job graph, and ECS query
task graph layers. The engine suite validates component read/write declarations,
deterministic chunking, deferred scene mutation flushes, transform dependency
levels, and exported runtime scheduler smoke coverage. The editor suite validates
that Play Mode can consume the scheduler summaries while C# authors keep normal
script workflows without adding job or query annotations.

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
requirement. Run these commands from the repository root:

```powershell
cmake --preset Debug
cmake --build out/build/Debug --target TerraEngine-EngineTests
cmake --build out/build/Debug --target TerraEngine-EditorTests
ctest --test-dir out/build/Debug --output-on-failure
cmake --build out/build/Debug --target install
```
