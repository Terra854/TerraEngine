# Export Runtime Baseline

This document tracks the manual and CI workflow for the minimal exported
runtime baseline. The baseline is intentionally small: it should prove that one
project can export and launch with render settings, a startup scene, a mesh, a
material, a texture, UI content, and script behavior.

## Expected Workflow

```powershell
cmake --preset Debug
cmake --build out/build/Debug --target TerraEngine-EngineTests
cmake --build out/build/Debug --target TerraEngine-EditorTests
ctest --test-dir out/build/Debug --output-on-failure
cmake --build out/build/Debug --target install
```

## Manual Baseline

1. Use `Tests/Fixtures/ExportRuntimeBaseline` as the sample project.
2. Export the project with its baseline build profile.
3. Launch the exported runtime from the installed tree or run launcher
   validation with `--validate-export-bootstrap`.
4. Confirm the boot summary reports the startup scene, resolved project render
   settings, required systems, material fallback warnings, and runtime entry
   result.
5. Confirm the scene displays the sample mesh, material, texture, UI, and script
   signal.

Current executable helper:

```powershell
powershell -ExecutionPolicy Bypass -File Tests/Tools/ExportRuntimeBaseline/ExportSample.ps1 -BuildDir out/build/Debug -Profile baseline
```

The helper reports the sample project, build tree, and profile. The actual
export remains editor-driven through the Build Profiles window until a headless
CI export wrapper is added.

## Sample Paths

- Project manifest: `Tests/Fixtures/ExportRuntimeBaseline/project.json`
- Startup scene: `Tests/Fixtures/ExportRuntimeBaseline/Scenes/Startup.tescene`
- Mesh fixture: `Tests/Fixtures/ExportRuntimeBaseline/Assets/Meshes/Baseline.temf`
- Material fixture: `Tests/Fixtures/ExportRuntimeBaseline/Assets/Materials/Baseline.tmat`
- Texture fixture: `Tests/Fixtures/ExportRuntimeBaseline/Assets/Textures/Baseline.tetf`
- UI fixture: `Tests/Fixtures/ExportRuntimeBaseline/UI/Baseline.ui`
- Script fixture: `Tests/Fixtures/ExportRuntimeBaseline/Scripts/BaselineScript.cs`
- Helper script: `Tests/Tools/ExportRuntimeBaseline/ExportSample.ps1`

The checked-in `*.temf`, `*.tetf`, and `*.tmat` files are placeholders that
identify the required compiled assets. Before a runnable manual export, rebuild
those fixtures through the project asset compilers so the export archive
contains binary TEMF/TETF/TMAT payloads.

## Expected Signals

- Boot summary includes `startupScene=Scenes/Startup.tescene`
- Boot summary includes `renderProfile={profile=Project Render Settings`
- UI text includes `Export Runtime Baseline Ready`
- Script output includes `ExportRuntimeBaseline.ScriptStarted`
