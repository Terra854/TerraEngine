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

## Runtime limits (version 1)

This is the complete human-readable runtime-limit authority. The typed values
are [`kAnimationRuntimeLimits`](../../Engine/Animation/AnimationTypes.h); fixture
materialization mirrors them in `contracts/runtime-limits-v1.json`. Performance
and stress tools pin this document's exact bytes, so a reviewed edit must update
both tool pins and the [performance authority table](Performance.md) together.

Operational defaults are policy budgets; hard maxima are content/ABI safety
bounds. A hard maximum is not a throughput or memory guarantee. Import, compile,
allocation, decompression, GPU upload and publication must reject unsafe bounds
before side effects; supported deterministic reductions must remain explicit.
Empty/optional data still follows its feature contract rather than being filled
to these maxima. Times below are seconds unless the row names another unit.

| Typed limit | Operational default | Hard maximum |
|---|---:|---:|
| `SkeletonJoints` (skeleton joints) | 256 | 4,096 |
| `PaletteJoints` (palette joints) | 256 | 256 |
| `RetainedInfluencesPerVertex` (retained influences per vertex) | 4 | 4 |
| `PackageSkinsOrSkeletons` (package skins or skeletons) | 64 | 1,024 |
| `PackageClips` (package clips) | 256 | 4,096 |
| `ClipDurationSeconds` (clip duration seconds) | 1,800 | 86,400 |
| `ClipChannels` (clip channels) | 16,384 | 65,536 |
| `ClipKeys` (clip keys) | 4,194,304 | 16,777,216 |
| `MeshMorphTargets` (mesh morph targets) | 256 | 4,096 |
| `ControllerNodes` (controller nodes) | 8,192 | 65,536 |
| `StateMachineDepth` (state machine depth) | 16 | 64 |
| `SubgraphCallDepth` (subgraph call depth) | 8 | 32 |
| `ControllerLayers` (controller layers) | 16 | 32 |
| `ControllerParameters` (controller parameters) | 1,024 | 4,096 |
| `TransitionsPerState` (transitions per state) | 256 | 4,096 |
| `ClipAuthoredEvents` (clip authored events) | 65,536 | 1,048,576 |
| `AnimatorEventsPerUpdate` (animator events per update) | 1,024 | 4,096 |
| `AnimatorRetainedEvents` (animator retained events) | 2,048 | 8,192 |
| `AnimatorProceduralGoals` (animator procedural goals) | 64 | 256 |
| `ChainIkIterations` (chain IK iterations) | 32 | 128 |
| `MotionDatabaseSamples` (motion database samples) | 1,000,000 | 16,777,216 |
| `MotionQueryTopK` (motion query top-K) | 32 | 256 |
| `RecordingDurationSeconds` (recording duration seconds) | 1,800 | 14,400 |
| `RecordingSampleRateHz` (recording sample rate Hz) | 60 | 240 |
| `RollbackHistoryTicks` (rollback history ticks) | 240 | 4,096 |
| `DetailedDiagnosticHistoryFrames` (detailed diagnostic history frames) | 600 | 36,000 |
| `ConcurrentAutomationJobs` (concurrent durable automation jobs) | 16 | 64 |

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
