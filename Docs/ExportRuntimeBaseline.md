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

Run these commands from the repository root in an x64 Visual Studio developer
environment. They are the authoritative local equivalent of
`.github/workflows/export-baseline.yml`.

```powershell
cmake --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine-EngineTests TerraEngine-EditorTests TerraEngineModelPackageCompiler TerraEngine-Export TerraEngine-Editor
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R "Editor.ExportUnitTests|Editor.HeadlessExport" --output-on-failure
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo --output-on-failure
cmake --build --preset RelWithDebInfo --target install
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/ExportRuntimeBaseline/RunBaseline.ps1 -InstallRoot out/build/RelWithDebInfo/Build -WorkRoot out/ci/export-runtime-baseline
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

## GitHub Actions gate

`.github/workflows/export-baseline.yml` runs for pull requests targeting
`main`, pushes to `main`, and manual dispatch. The job uses a clean
`windows-latest` runner, recursive submodules, x64 MSVC, the .NET 10 SDK,
read-only repository permission, no build cache, and a 60-minute timeout.

The ordered stages are checkout, toolchain initialization, RelWithDebInfo configure,
targeted build, focused export tests, full CTest, install, installed-path
verification, clean baseline preparation and export, evidence validation, and
artifact upload. Commands retain their native exit codes; no failed stage is
converted into a successful result.

The always-run artifact is named
`headless-export-evidence-windows-relwithdebinfo`. It contains:

```text
configure.log
build.log
focused-ctest.log
ctest.log
install.log
baseline-prepare.log
headless-export/
|-- export-result.json
|-- resolved-manifest.json
|-- content-inventory.json
|-- validation-report.json
|-- progress.jsonl
`-- package-validation.txt
```

The supported automated gate is Windows x64. The export implementation may
contain Linux behavior, but this workflow does not claim Linux hosted-runner
coverage or GPU/runtime launch coverage.

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

If the job fails before `export-result.json` exists, inspect the earliest
available configure, build, test, install, or baseline log in the artifact.
Because artifact staging always runs, partial evidence remains available after
early failures. Re-run the local command sequence above from a clean
`out/ci/export-runtime-baseline` work root; opening the editor is not required.

## Expected runtime signals

The archive manifest names `Scenes/Startup.tescene` as the startup scene. The
fixture UI includes `Export Runtime Baseline Ready`, and the managed script
contains the `ExportRuntimeBaseline.ScriptStarted` signal. These small signals
make source, package, and runtime-bootstrap regressions distinguishable without
requiring a rendered screenshot.
