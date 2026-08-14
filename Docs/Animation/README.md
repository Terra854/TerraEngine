# Animation Validation Tools

Animation validation is split into reusable workflows instead of feature-phase
or planning-task gates. Runtime and authoring behavior is covered by the native
CTest suites, while the scripts under `Tests/Tools/Animation/` provide fixture,
installed-project, performance, and soak validation.

Use [Troubleshooting](Troubleshooting.md) for failure diagnosis and
[Migration](Migration.md) when converting older projects or APIs. The asset,
playback, controller, gameplay, procedural, retargeting, motion-matching,
ragdoll, editor, streaming/export, automation, determinism, and performance
documents describe their focused contracts.

## Reproducible native validation

From a Visual Studio x64 developer shell:

```powershell
cmake --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target TerraEngine-EngineTests TerraEngine-EditorTests TerraEngine-Editor
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R "Animation|AssetPipeline|SystemScheduler|TransactionalMutation" --output-on-failure
cmake --build --preset RelWithDebInfo --target install
```

The install step must complete before any installed-editor or soak workflow.
Generated projects, logs, captures, and reports belong below `out/`; none should
be committed.

## Tool inventory

- `GenerateAnimationFixtures.ps1` materializes deterministic source fixtures
  below a caller-owned output directory.
- `GenerateAnimationStressProject.ps1` creates the disposable stress project
  consumed by performance and soak runs.
- `CaptureAnimationPerformance.ps1` records a candidate run and
  `CompareAnimationPerformance.ps1` compares compatible evidence.
- `RunAnimationSoak.ps1` drives the installed
  `TerraEngineAnimationSoakRunner` in bounded slices and retains its evidence.
- `AdvancedAnimationFixtureMaterializer.cpp` and the controller fixture producer
  under `Tests/Fixtures/Animation/ControllerValidation/` reproduce checked
  binary fixtures through production serializers.

The scripts fail closed when required inputs, metrics, artifacts, or installed
binaries are unavailable. A missing measurement must remain unavailable; it may
not be converted into a passing result.

## Performance and soak validation

The canonical workloads live in `PerformanceWorkloads.json`; cross-subsystem
coverage is declared by `PerformanceCoverageMatrix.json`. Capture and comparison
inputs must use the same configuration, worker policy, graphics route, workload,
and measurement availability rules.

For a short validation slice:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
	-File Tests/Tools/Animation/RunAnimationSoak.ps1 `
	-RunnerPath out/build/RelWithDebInfo/Build/Tests/Tools/Animation/TerraEngineAnimationSoakRunner.exe `
	-DurationHours 0.01
```

Long-running acceptance requires the duration and workload appropriate to the
release being validated. Preserve every failed slice and its diagnostics before
rerunning.
