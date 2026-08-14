# Animation performance and soak validation

This document defines how TerraEngine animation performance evidence is prepared,
captured, interpreted, and compared. It does not report a performance pass.
Candidate wall-clock timing, throughput, latency, diagnostics overhead, and
whole-engine regression acceptance require an explicit measured release run.
The continuous 24-hour soak remains a separate long-running release check.

Intermediate work may compile the harnesses, verify hashes and schemas, generate
deterministic project descriptors, and exercise non-timed correctness or
soak-safety paths. Synthetic test values are schema tests, never measurements.

## Immutable authorities

The following identities are fail-closed inputs. A changed byte requires review;
it must not be accepted by weakening a nearby constant.

| Authority | Identity |
|---|---|
| Pre-animation source commit | `6d12e3a99d7c0d41899fc96fa18b02b61e5a0aa9` |
| Locked reference baseline | `out/validation/animation/baseline/locked-baseline.json` — SHA-256 `cf4375934729e1afa745d9446faacfc1a7ba53a6d7db79b3d1e0e8164a5ced8f` |
| Reference-baseline evidence selection | 178 files, 371,567,204 bytes — aggregate SHA-256 `4020949bd881f94f0395853e947e4512fb587c7bdd1ce5592317e75c01b5fcb5` |
| Runtime limits contract | `Docs/Animation/README.md` — SHA-256 `774c6c7a40da4ebb8fa67feb3649e956235669428d4c50ad43cdb2dceca1a801` |
| Animation performance workloads | `Tests/Tools/Animation/PerformanceWorkloads.json` — SHA-256 `add34eb7debacbaddb03c07de115cf00f7935a3e62e849cf4b370f2cbf1bd707` |
| Animation stress definitions | `Tests/Fixtures/Animation/stress/workloads.json` — SHA-256 `b5c78e9f01dc0036db4fb4674f632ebf06a736e4a7c010ec56e1f3804b92131d` |
| Whole-engine coverage matrix | `Tests/Tools/Animation/PerformanceCoverageMatrix.json` — SHA-256 `e9a861037edf63207368d9e31b3a85a32bdf3723cdfb28eeaa7b5223c5a64670` |
| Soak iteration schema | `Tests/Fixtures/Animation/stress/soak-result.schema.json` — SHA-256 `fa1b6548872e9ca70a8e1d5aeef9e810d82cc8bcd45e386c2a29744df8991659` |

Candidate tools may write only below `out/` and may never overwrite the reference
baseline tree. A candidate build is not allowed to become its own reference.

## Frozen reference host and platform

Final animation measurements use exactly this profile:

- ASUS ROG Strix G18 G834JZR/G814JZR notebook on AC power.
- Intel Core i9-14900HX, 24 physical cores and 32 logical processors.
- NVIDIA GeForce RTX 4080 Laptop GPU with 12 GB, selected explicitly.
- Windows driver `32.0.15.9597` / NVIDIA `595.97`.
- 32 GB system memory.
- Windows 11 x64 `10.0.26200`, build `26200`.
- C++ `RelWithDebInfo` installed tree only; source-tree or transferred build
  artifacts are incomparable.
- Vulkan 1.4 loader, 1920 x 1080 animation reference window, VSync disabled,
  and the same quality settings and editor layout for both sides of a pair.
- Windows Performance power plan, stable performance/fan profile, completed
  warm-up, and a recorded materially comparable ambient-process set.
- 31 TerraEngine workers unless a pre-existing reference control has its own locked
  worker count.

The exact firmware-visible device, Vulkan API/driver, toolchain, executable,
project, fixture, display, hybrid-routing, power, thermal, and ambient-process
facts recorded by the reference baseline must match. Missing or unknown identity is not a warning;
it rejects the comparison.

## Frozen content profiles

| Profile | Exact content |
|---|---|
| Crowd | 1,000 active characters; exactly 100 joints each; two blended clips; four morph targets; one shadow-casting view; 30-second fixed warm-up; 60 measured seconds; five paired runs. |
| Complex hero | One 256-joint source rig partitioned at the 256-joint palette limit; layered controller; two active morphs; two IK chains; motion matching; deformable virtual geometry; partial ragdoll; two game/capture views; shadows; 30-second warm-up; 60 measured seconds; five paired runs. |
| Motion database | 250,000 searchable samples; 48 quantized features; trajectory/contact/tag exclusions; top-K 32; 100,000 fixed queries. |
| Mode churn | The same recorded timeline of 10,000 requested-policy, capability, budget, and visibility changes across CPU Deterministic/Fallback, GPU Visual, and Hybrid Authoritative. |
| Randomized scheduler | PCG32 v1 with the five committed seeds, 100,000 ticks per seed, worker counts 1/4/16/31, and spawn/despawn/parameter/command/visibility/reload/cancel operations. |
| Device loss | Loss injected at ticks 120/480/960/1920 in GPU planning, GPU evaluation, deformation, and presentation; safe CPU fallback and generation recovery are mandatory. |
| Memory pressure | Reviewed frame-arena, pose-cache, GPU-residency, and detailed-diagnostics budgets exercised at 100/75/50/25/10 percent. |
| Import workflow | Generated glTF and FBX packages with two skins, four clips, step/linear/cubic tracks, eight morphs, four materials, sockets, nested transforms, and one prefab. |
| Authoring workflow | Prepared imported assets plus an empty controller in the clean installed-editor layout. |

The generated Khronos and Quaternius validation downloads added later do not
replace these repository-owned, deterministic workload definitions.

## Exact acceptance thresholds

All thresholds come from the runtime contract or immutable reference-baseline configuration:

- animation-outcome-creator-import-representative-animated-gltf-fbx-package-obtain-correctly-referenced-playable-prefab: each of three clean import-workflow trials finishes in under 300
  seconds, measured from the installed-editor import command until the prefab is
  visible and selectable in Scene and Game views.
- animation-outcome-creator-build-preview-save-reload-run-locomotion-plus-action-controller-nested-states: each of three clean authoring trials finishes in under 900 seconds.
- animation-outcome-motion-matching-queries-over-reference-production-size-database-return-eligible-result-within: at least 99.9% of 100,000 motion queries publish an eligible result in
  the next 16.667 ms update; every remainder uses the bounded fallback.
- animation-outcome-reference-scene-containing-concurrently-active-joint-characters-two-blended-clips-sustains-least: the crowd sustains at least 60 updates per second and has no
  animation-attributed frame above 50 ms.
- animation-outcome-reference-hero-scene-combining-layered-graphs-morphs-ik-motion-matching-deformable-virtual: the complex hero sustains at least 60 updates per second and memory is
  bounded after warm-up.
- animation-outcome-optional-animation-profiling-detailed-diagnostics-disabled-versus-enabled-but-collapsed-differ-no: enabled-but-collapsed diagnostics add no more than 2% to disabled
  median frame time. The comparison uses paired medians, not an average.
- animation-outcome-character-reference-scene-gpu-visual-mode-reduces-animation-related-cpu-evaluation-time: crowd GPU Visual reduces animation-related CPU evaluation time by at
  least 75% versus CPU Deterministic/Fallback while still satisfying animation-outcome-reference-scene-containing-concurrently-active-joint-characters-two-blended-clips-sustains-least.
- animation-outcome-instrumented-ordinary-gpu-visual-hybrid-authoritative-frames-perform-zero-complete-pose-gpu: complete-pose GPU-to-CPU transfers are exactly zero and
  animation-caused CPU waits for ordinary GPU pose completion are exactly zero.
- animation-outcome-gpu-visual-hybrid-authoritative-poses-match-cpu-reference-poses-within-declared-visual: discrete state, event order, root-motion interval, motion selection,
  generation, and deterministic hashes are exact. GPU visual tolerances are
  0.5 mm joint translation, 0.1 degrees rotation, 0.05% scale, and 0.0005
  absolute morph weight. Root motion uses 0.5 mm / 0.1 degrees unless physics
  reports the exact adjusted result.
- animation-outcome-ten-thousand-forced-transitions-among-gpu-visual-hybrid-authoritative-cpu-deterministic-fallback and animation-outcome-ten-thousand-rapid-visibility-short-lived-budget-changes-produce-zero-effective-mode: all 10,000 mode changes have zero bind flashes, state loss,
  duplicate/lost events, root discontinuities, stale generations, mixed
  generations, and hysteresis-violating frame oscillation.
- animation-outcome-gpu-motion-matching-scoring-reduces-animation-related-cpu-search-time-least-reference: GPU motion search reduces animation-related CPU search time by at
  least 80% and returns the same authoritative selection for every deterministic
  fixture.
- animation-outcome-continuous-hour-mixed-animation-soak-completes-without-deadlock-crash-stale-publication-unbounded: the continuous 24-hour mixed soak has zero deadlock, crash, stale or
  partial publication, unrecoverable renderer/physics failure, and unbounded
  retained history or resource growth.

Every timed workload has one fixed warm-up and at least five paired measured
runs. Evidence retains median, p95, p99, and worst values. A missing/non-finite
metric, missing tail sample, altered fixture, unknown host fact, incomplete run,
or incomparable setting rejects the result.

## Reading animation profiler evidence

Active captured frames expose fixed typed aggregate evidence. Named stages are:

1. `Animation.SchedulerMembership`
2. `Animation.WorkPlanning`
3. `Animation.CpuGraphEvaluation`
4. `Animation.MotionMatching`
5. `Animation.IkSolve`
6. `Animation.RagdollHandoff`
7. `Animation.CpuAuthority`
8. `Animation.GpuPlanning`
9. `Animation.GpuEvaluation`
10. `Animation.Deformation`
11. `Animation.ProtectedPublication`
12. `Animation.Presentation`

Each stage records an integer nanosecond value, availability, and whether the
sample came from a reduced cadence. An unavailable stage is not a zero-time
stage. Final evidence must declare every stage required by its workload.

The aggregate reports raw worker capacity/busy time, useful items, animation
volumes, memory/current high-water bytes, transfers/readbacks, waits/stalls, and
pose/clip/residency cache hits and misses. Compute worker utilization as
`workerBusyNanoseconds / workerCapacityNanoseconds` only when worker evidence is
available and capacity is non-zero. Derive cache reuse from the matching hit and
miss pair; never combine unrelated cache families.

Availability bits distinguish missing counter families from measured zeros.
Final evidence must contain the families required by the workload. Current bytes
may not exceed their high-water values. Full-pose readback bytes and ordinary GPU
pose waits must remain zero even when bounded asynchronous metadata readback is
present.

Per-instance rows are optional diagnostic detail, capped at 256 records per
frame. Detailed mode retains them every frame; Sampled mode uses the configured
cadence; Aggregate mode drops them. Dormant frames allocate no active aggregate
and retain only scheduler-membership count, dormancy, and baseline-identity
facts. Opening no profiler capture takes the existing atomic fast exit before a
mutex or detail allocation.

## Whole-engine regression matrix

`CompareAnimationPerformance.ps1 -FinalPhaseMatrix` opts into whole-engine
coverage. Without that
switch, the tool verifies matrix readiness and explicitly reports performance
execution as deferred. The final matrix covers renderer, camera, UI, physics,
scripting, resource streaming/residency, virtual geometry, scheduler, profiler,
MCP/editor automation, startup, and export.

Each domain maps to one immutable reference-baseline control and lists the metrics/facts that
must survive the mapping. All mapped candidate workloads still require the same
warm-up, five measured results, raw result paths, installed artifact identity,
and source-controlled settings hash used by the original comparator. The matrix
does not define replacement thresholds; it inherits only the reviewed gates in
`PerformanceWorkloads.json`. Missing domains, metrics, facts, or source contracts
fail closed.

Threshold or policy properties in candidate evidence are forbidden. A final
candidate cannot weaken a threshold, change a direction, replace a baseline
sample, omit a bad run, or overwrite a normalized/raw result path. Regression
and invalid-evidence exits remain distinct.

## Reproducible readiness and final commands

From a clean checkout, configure, compile, and install before any final run:

```powershell
cmake --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo
cmake --build --preset RelWithDebInfo --target install
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo --output-on-failure
```

During intermediate phases, verify only the definitions and protocols:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tests/Tools/Animation/GenerateAnimationStressProject.ps1 -ValidateOnly
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tests/Tools/Animation/RunAnimationSoak.ps1 -ValidateOnly -DurationHours 0.01
```

To materialize deterministic descriptors without running a benchmark:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tests/Tools/Animation/GenerateAnimationStressProject.ps1 `
  -OutputRoot out/generated/animation-stress
```

Supply fresh installed-run candidate evidence through the capture pipeline, then
perform the whole-engine comparison explicitly:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tests/Tools/Animation/CompareAnimationPerformance.ps1 `
  -CandidateManifest out/validation/animation/final/candidate-manifest.json `
  -OutputPath out/validation/animation/final/whole-engine-comparison.json `
  -FinalPhaseMatrix
```

The installed `TerraEngineAnimationSoakRunner` writes one
`soak-result.schema.json`-conforming result per slice to the path in
`TERRAENGINE_ANIMATION_SOAK_RESULT`. A slice defaults to one second and four
workers; `-RunnerArguments @('--slice-ms', '5000', '--workers', '8')` may select
100-600,000 milliseconds and 1-32 workers without changing an acceptance
threshold. The runner uses the production animator-instance, bounded rollback
history, streaming cancellation, and atomic hot-reload paths. It is headless,
so `gpuResidentBytes` is honestly zero; renderer and physics recovery stay in
their dedicated correctness/live gates. The driver writes `state.json`
through a replace boundary and appends `progress.jsonl` with write-through flushes.
Create the configured cancellation sentinel for a controlled stop, remove it,
then pass `-Resume` with the exact same manifest, schema, runner hash, arguments,
duration, timeout, and final-phase setting. A changed configuration or stale
iteration directory is rejected.

The full final invocation is intentionally not run by intermediate tasks:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tests/Tools/Animation/RunAnimationSoak.ps1 `
  -RunnerPath out/build/RelWithDebInfo/Build/Tests/Tools/Animation/TerraEngineAnimationSoakRunner.exe `
  -OutputRoot out/validation/animation/final/soak `
  -DurationHours 24 -FinalPhase
```

The runner is installed at the path shown above by the normal CMake install.
Completion is accepted only when durable state reaches the full target, every heartbeat advances,
every failure counter stays zero, history remains at or below 36,000 records,
and private bytes, GPU residency, handles, and threads remain inside the reviewed
growth bounds. A controlled cancellation is resumable evidence, not a pass.
