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

## Configure

```powershell
cmake --preset Debug
```

## Build Unit Tests

```powershell
cmake --build out/build/Debug --target TerraEngine-EngineTests
cmake --build out/build/Debug --target TerraEngine-EditorTests
```

## Run CTest

Vendored dependency tests are disabled in the TerraEngine CMake configuration,
so this command runs only the project-owned CTest entries.

```powershell
ctest --test-dir out/build/Debug --output-on-failure
```

## Export Runtime Baseline

The baseline sample lives at `Tests/Fixtures/ExportRuntimeBaseline`. Its checked-in
TEMF/TETF/TMAT files are placeholders for compiled binary assets; regenerate
them through the asset compilers before using the project as a runnable manual
export. The automated tests still guard the manifest, scene references, UI
signal, script signal, and startup coordinator decision.

## Install After Code Changes

Run the install target before finishing any task that changed code so the
install tree stays verified with the compiled outputs.

```powershell
cmake --build out/build/Debug --target install
```
