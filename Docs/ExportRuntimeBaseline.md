# Export Runtime Baseline

The export runtime baseline is the smallest project that exercises TerraEngine's
installed asset compiler, shared export pipeline, transactional publication,
TEPAK validation, and machine-readable evidence. The gate is intentionally
headless: it must not create a window, initialize a graphics device, or depend
on editor preferences.

## Source of truth

The reviewable project source lives in
`Tests/Fixtures/ExportRuntimeBaseline`. Its textual glTF model contains the
baseline mesh, material, and texture inputs. Compiled TEMF, TMAT, and TETF files
are generated in a disposable project below `out/`; they are not checked-in
source artifacts and must never be replaced by marker or placeholder text.

The baseline helper performs the complete preparation flow:

1. Copy the fixture into a clean disposable project.
2. Stage managed references from the installed editor tree.
3. Compile the glTF package with the installed model package compiler.
4. Reject missing, empty, stale, or placeholder-generated child assets.
5. Run the installed `TerraEngine-Export` executable with explicit paths.
6. Inspect the TEPAK archive and compare the archived manifest with evidence.

## Local reproduction

Run these commands from the repository root in a Windows x64 Visual Studio
developer environment. They are the authoritative local validation gate.

```powershell
cmake --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine-EngineTests TerraEngine-EditorTests TerraEngineModelPackageCompiler TerraEngine-Export TerraEngine-Editor
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R "Editor.ExportUnitTests|Editor.HeadlessExport" --output-on-failure
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo --output-on-failure
cmake --build --preset RelWithDebInfo --target install
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tests/Tools/ExportRuntimeBaseline/RunBaseline.ps1 -InstallRoot out/build/RelWithDebInfo/Build -WorkRoot out/ci/export-runtime-baseline
```

The long-path lane uses the same installed binaries and fixture, but places the
project beyond 360 characters. It proves that both the editor's ordinary
Assembly-CSharp build and the NativeAOT export toolchain use scoped short
workspaces without leaking transient restore, object, cache, or publish state
into the project:

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

The compatibility script at
`Tests/Tools/ExportRuntimeBaseline/ExportSample.ps1` forwards to the same
authoritative helper. It does not contain a second export implementation.

After installation, the gate requires these files:

```text
out/build/RelWithDebInfo/Build/Editor/TerraEngine-Export.exe
out/build/RelWithDebInfo/Build/Editor/TerraEngineModelPackageCompiler.exe
out/build/RelWithDebInfo/Build/Editor/Managed/TerraEngine.CSharp.dll
out/build/RelWithDebInfo/Build/Editor/Managed/Analyzers/TerraEngine.Export.Generator.dll
out/build/RelWithDebInfo/Build/Launcher/TerraEngine-Launcher.exe
```

To invoke the installed CLI directly after the disposable project has been
prepared, use explicit project, profile, output, and evidence paths:

```powershell
& out/build/RelWithDebInfo/Build/Editor/TerraEngine-Export.exe `
  --project out/ci/export-runtime-baseline/project `
  --profile out/ci/export-runtime-baseline/project/BuildProfiles/baseline.json `
  --output out/ci/export-runtime-baseline/project/Builds/Baseline `
  --evidence out/ci/export-runtime-baseline/evidence
```

The executable resolves every installed runtime dependency relative to its own
canonical path. Changing the current working directory must not change export
behavior.

## Evidence contract

A successful prepared baseline writes the runtime to
`out/ci/export-runtime-baseline/project/Builds/Baseline` and writes these six
files to `out/ci/export-runtime-baseline/evidence`:

```text
export-result.json
resolved-manifest.json
content-inventory.json
validation-report.json
progress.jsonl
package-validation.txt
```

`export-result.json` is the primary automation result. A successful run has
schema version 1, `success=true`, `failureCategory=none`, and exit code 0.
`resolved-manifest.json` must match the `game.json` bytes stored in the archive.
`content-inventory.json` is sorted by normalized path and no required entry may
set `placeholderDetected=true`. `progress.jsonl` contains independently
parseable records in emission order, including exactly one final result record.
The helper also requires the installed editor's script asset to reach
`compileStatus=Ready`, verifies its non-empty managed assembly, and rejects a
project-local `Library/NativeAot` directory after export.

## Local validation gate

The supported gate is the ordered Windows x64 command sequence above. It uses
the RelWithDebInfo preset, builds the focused targets, runs focused and full
CTest passes, installs the product, verifies installed paths, and exercises a
clean prepared export. Commands retain their native exit codes so a failed
stage cannot be mistaken for a successful result.

Keep `out/ci/export-runtime-baseline` until diagnosis is complete. The work
root contains the disposable project and the `headless-export` evidence files
produced before a failure. There is no hosted workflow for this gate; run it
locally whenever export, packaging, installation, or runtime staging changes.

## Failure diagnosis

Start with `export-result.json`, using `stage`, `failureCategory`, `message`,
and the ordered diagnostics to choose the corresponding log. The CLI exit code
is stable and can be used without parsing human text:

| Exit code | Failure category | First evidence to inspect |
|---:|---|---|
| 2 | `invalid_arguments` or `invalid_runtime_layout` | CLI result and installed-path checks |
| 3 | `output_conflict` | Result path and competing export process |
| 4 | `validation_failed` | `validation-report.json` |
| 5 | `managed_compilation_failed` | `baseline-prepare.log` and validation report |
| 6 | `asset_staging_failed` | Compiler output and `content-inventory.json` |
| 7 | `packaging_failed` or `package_validation_failed` | `package-validation.txt` |
| 8 | `runtime_staging_failed` or `evidence_write_failed` | Result diagnostics and evidence directory |
| 9 | `publication_failed` | Final, staging, and backup paths in the result |
| 10 | `unexpected_failure` | Final result and the complete baseline log |

If the gate fails before `export-result.json` exists, inspect the earliest
failing configure, build, test, install, or baseline command. Preserve partial
evidence in the work root before cleaning it. Re-run the local command sequence
above from a clean `out/ci/export-runtime-baseline` work root; opening the
editor is not required.

## Expected runtime signals

The archive manifest names `Scenes/Startup.tescene` as the startup scene. The
fixture UI includes `Export Runtime Baseline Ready`, and the managed script
contains the `ExportRuntimeBaseline.ScriptStarted` signal. These small signals
make source, package, and runtime-bootstrap regressions distinguishable without
requiring a rendered screenshot.
