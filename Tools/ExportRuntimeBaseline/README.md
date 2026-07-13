# Headless Export Runtime Baseline

`RunBaseline.ps1` prepares and exports the smallest source-controlled project
that covers a mesh, material, embedded texture, scene, UI document, managed
script, render settings, runtime staging, and TEPAK validation. It uses only
installed TerraEngine binaries and writes every generated file below `out/`.

## Prerequisites

From a Visual Studio x64 developer shell, configure, build, and install the
Debug tree before running the baseline:

```powershell
cmake --preset Debug
cmake --build out/build/Debug --target TerraEngine-Export TerraEngineModelPackageCompiler TerraEnginePhysicsColliderCompiler TerraEngine-EditorTests
cmake --build out/build/Debug --target install
```

The install must contain:

- `out/build/Debug/Build/Editor/TerraEngine-Export.exe`
- `out/build/Debug/Build/Editor/TerraEngineModelPackageCompiler.exe`
- `out/build/Debug/Build/Editor/TerraEnginePhysicsColliderCompiler.exe`
- `out/build/Debug/Build/Editor/Managed/TerraEngine.CSharp.dll`
- `out/build/Debug/Build/Editor/Managed/Analyzers/TerraEngine.Export.Generator.dll`

## One-command gate

```powershell
pwsh -NoProfile -File Tools/ExportRuntimeBaseline/RunBaseline.ps1 `
	-InstallRoot out/build/Debug/Build `
	-WorkRoot out/ci/export-runtime-baseline
```

`Invoke-ExportRuntimeBaseline.ps1` is a descriptive forwarding alias. The
historical `Tests/Tools/ExportRuntimeBaseline/ExportSample.ps1` also forwards
to the same command.

## Inputs and generated output

The source of truth is
`Tests/Fixtures/ExportRuntimeBaseline/Assets/Models/Baseline.gltf`. Its embedded
buffer and PNG make the fixture reviewable and self-contained. The adjacent
metadata fixes the package GUID, while `BuildProfiles/baseline.json` is the
single editor/headless build profile.

The embedded 17-by-17-cell grid contains 578 source triangles. That is small
enough for a fast baseline but deliberately above the production cooker's
512-triangle threshold, so the generated TEMF exercises current virtual-
geometry metadata instead of taking the small-mesh skip path.

The helper copies authored directories into
`<WorkRoot>/project`, stages installed managed references under
`project/Build/Assembly-CSharp/References`, and runs the installed model package
compiler with these stable inputs:

- source relative path: `Models/Baseline.gltf`
- package GUID: `11111111222222223333333344444444`
- default shader GUID: `00000000000000000000000000000001`
- compiled root: `<WorkRoot>/project/Build/Assets`

The compiler must generate nonempty TEMF, TMAT, and TETF children below
`Build/Assets/Models/Baseline_Generated`. Those generated binaries, the model
import sidecar, NativeAOT output, runtime export, and evidence are disposable;
none belongs in source control.

The helper also copies the source-only physics recipe from
`Tests/Fixtures/PhysicsExportRuntime` into its disposable project and invokes
the installed `TerraEnginePhysicsColliderCompiler`. It requires a published
`Build/Assets/Colliders/Startup.tepcoll` envelope with the repository-pinned
Jolt 5.6.0 revision before export. The archive must contain that cooked
envelope and Physics Settings, while excluding the `.tecollider` source recipe.

## Expected signals and diagnosis

A passing run reports `Export runtime baseline passed`, publishes the runtime
under `<WorkRoot>/project/Builds/Baseline`, and writes all six evidence files
under `<WorkRoot>/evidence`. It also records:

- `baseline-prepare.log`
- `logs/model-package-compiler.log`
- `logs/headless-export.log`
- `logs/headless-export.stdout.jsonl`
- `logs/headless-export.stderr.log`
- `logs/archive-inspection.log`

The helper fails immediately when an installed input is missing, a compiler
returns nonzero, a child format header is invalid, a payload contains the word
`placeholder`, evidence is incomplete, the inventory marks a placeholder, or
the resolved startup scene differs from the standalone profile.
