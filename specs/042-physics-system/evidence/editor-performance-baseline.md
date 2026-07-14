# RelWithDebInfo Editor Performance Baseline

## Scope and configuration

This document records T105 and T106 before production optimization. All native
and managed binaries came from the repository's `RelWithDebInfo` preset and the
custom staged install tree at `out/build/RelWithDebInfo/Build`. The disposable
fixture contained 10,000 box-collider objects: 7,000 dynamic, 1,000 kinematic,
and 2,000 static. The editor was controlled only through its loopback MCP
surface.

- Date: 2026-07-14 (Asia/Singapore)
- OS: Microsoft Windows 11 Home 10.0.26200
- CPU: Intel Core i9-14900HX, 32 logical processors
- Memory: 33,959,215,104 bytes
- GPU observed by the installed editor: NVIDIA GeForce RTX 4080 Laptop GPU
- Build: CMake/Ninja `RelWithDebInfo`
- Install tree: `out/build/RelWithDebInfo/Build`
- Raw run 1: `out/validation/t105-editor-performance-baseline-1/evidence/summary.json`
- Raw run 2: `out/validation/t105-editor-performance-baseline-2/evidence/summary.json`
- Raw run 3: `out/validation/t105-editor-performance-baseline-3/evidence/summary.json`
- Authoritative profiler capture: `out/validation/t105-editor-performance-baseline-3/project/Profiler/project-20260714-062722.teprof`

The baseline command was:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
	-File Tools/Physics/RunInstalledEditorPhysicsStress.ps1 `
	-InstallRoot out/build/RelWithDebInfo/Build `
	-WorkRoot out/validation/t105-editor-performance-baseline-1 `
	-ObjectCount 10000 -McpPort 43127 -ExplicitStepCount 100 `
	-StartupTimeoutSeconds 600 -AllowLiveStress
```

Run 1 completed successfully, including 10,000-body publication, 100 exact
paused Next requests, unchanged authored-scene SHA-256, clean Play Mode
restoration, and normal editor shutdown. Its end-to-end duration was
200,052.02 ms. The evidence collector sampled the large diagnostics-summary
surface too often during this run, so its 180,701.76 ms stepping duration is an
observer-inflated upper bound rather than the optimization comparison value.

Run 2 reduced diagnostics sampling to once per five seconds with a 16-snapshot
bound and recorded a profiler capture across all 100 Next requests. Its
explicit-stepping stage was 140,790.63 ms. A PowerShell deadline-expression bug
then rejected the already-persisted profiler stop job and forced shutdown, so
run 2 is retained as valid profiler/bottleneck evidence but not as final
end-to-end correctness evidence. The deadline expression was corrected before
the post-fix gate.

Run 3 used that corrected harness and is the authoritative complete baseline.
It passed in 169,100.34 ms with 10,000 published bodies, 16 bounded scheduler
snapshots, 100 exact Next publications (tick 24 through tick 124), byte-identical
authored scene before/after Play Mode, successful profiler persistence and
summary, no forced termination, and a normal installed-editor shutdown. The
100-step stage took 148,592.18 ms. Its MCP p50/p95 values were 400.38/456.05 ms
for `play.step` and 417.60/502.86 ms for `physics.state`. The persisted capture
contains 315 frames (314 complete); its engine/editor-wall stream has a
470.87 ms mean, 612.90 ms p95, and 1,218.61 ms maximum. The viewport itself is
not the bottleneck: viewport CPU p95 is 0.477 ms and viewport GPU p95 is
0.300 ms.

## Baseline distributions

Run 1 automation and scheduler observations:

| Metric | p50 | p95 | p99 |
|---|---:|---:|---:|
| Explicit Next publication, observer-inflated | 2,065.45 ms | 2,167.26 ms | 2,185.17 ms |
| `play.step` MCP capability | 376.87 ms | 407.94 ms | 414.19 ms |
| `physics.state` MCP capability | 386.76 ms | 451.35 ms | 490.05 ms |
| Protected commit region | 462.11 ms | 520.42 ms | 532.09 ms |
| Worker preparation | 5.16 ms | 13.85 ms | not recorded |
| Worker utilization | 1.00% | 2.65% | not recorded |

The run 1 stage durations were: scripting readiness 6,952.83 ms, live physics
readiness 3,016.55 ms, disposable project/scene generation 1,725.54 ms, pause
1,331.65 ms, Play exit and restore 1,170.68 ms, installed process/MCP readiness
1,033.41 ms, editor shutdown 867.05 ms, and Play entry 724.64 ms.

The run 2 and run 3 profiler captures both contain 315 frames and agree on the
native critical path. The detailed scope extraction below uses run 2 because
it was parsed directly before the run-3 summary confirmed the same frame-level
distribution:

| Scope | samples | p50 | p95 | p99 | maximum |
|---|---:|---:|---:|---:|---:|
| Complete frame | 315 | 400.61 ms | 569.91 ms | 596.11 ms | 1,130.37 ms |
| Scheduler protected/coordinator region | 314 | 366.99 ms | 537.47 ms | 564.17 ms | 1,101.14 ms |
| `EditorSystem` protected job | 314 | 160.68 ms | 174.37 ms | 187.19 ms | 202.83 ms |
| `RenderSystem` protected job | 315 | 140.51 ms | 152.53 ms | 165.25 ms | 888.72 ms |
| Render world-transform resolution | 315 | 89.81 ms | 99.28 ms | 106.17 ms | 116.59 ms |
| Render sky-state resolution | 315 | 48.58 ms | 54.84 ms | 71.11 ms | 796.83 ms |
| `PhysicsSnapshot` protected job | 315 | 55.52 ms | 63.32 ms | 68.92 ms | 72.59 ms |
| Physics prepare | 315 | 57.35 ms | 65.18 ms | 71.27 ms | 74.35 ms |
| Physics publish | 315 | 26.35 ms | 43.09 ms | 46.54 ms | 51.09 ms |
| `Flush:After Physics` protected job | 315 | 0.004 ms | 122.04 ms | 129.45 ms | 152.78 ms |
| Jolt fixed step | 100 | 7.86 ms | 9.94 ms | 10.51 ms | 10.53 ms |

The profiler confirms that provider simulation is not the dominant problem.
The engine spends most of each frame in protected editor/render work, followed
by full-world physics snapshot/prepare/publication and transform writeback.
Physics memory evidence was stable at 1,040,000 snapshot bytes, with 10,062
outstanding and 10,234 high-water provider allocations. The baseline published
10,000 bodies and 10,000 shapes with empty command/query/event queues and no
rejected or stale operations.

Native per-stage history was unavailable through the narrow
`physics.diagnostics.get` MCP payload in run 1; the profiler artifact supplies
those values for run 2. Per-thread CPU utilization and protected-stall history
beyond the scheduler's frame summaries remain explicitly unavailable rather
than inferred.

## T106 pass/fail budgets

These are hard gates for T111 and are not weaker than SC-001, SC-003, SC-004,
or SC-007. Correctness gates apply before timing gates.

| Area | Pass budget |
|---|---|
| SC-001 creator workflow | Clean installed-editor falling-body workflow completes in at most 600,000 ms; the automated 10,000-object fixture completes in at most 120,000 ms. |
| Scene/project preparation | Disposable 10,000-object scene generation at most 10,000 ms; process plus MCP readiness at most 15,000 ms. |
| Play lifecycle | Play entry p95 at most 2,000 ms and Play exit/restoration p95 at most 2,000 ms, with byte-identical authored scene and no partial restoration failures. |
| SC-003 body scale | At least 10,000 mixed bodies and at least 1,000 active dynamic bodies are published. Jolt fixed-step p95 at most 16.67 ms and p99 at most 20 ms in `RelWithDebInfo`. |
| Engine-side parallelism | Physics-frame scheduler worker utilization p50 at least 20%, protected/coordinator p95 at most 50 ms, and no undeclared resource or protected-boundary change. |
| Physics stages | Snapshot p95 at most 10 ms, prepare p95 at most 10 ms, publish p95 at most 10 ms, and After Physics flush p95 at most 20 ms. |
| Editor responsiveness | Complete captured frame p95 at most 100 ms; `physics.state` and `play.step` MCP p95 at most 100 ms. |
| SC-007 pause/Next | Paused state produces zero spontaneous ticks; all 100 Next requests produce exactly one tick/publication/frame and return to paused. Publication p95 at most 150 ms and the 100-step stage at most 15,000 ms. |
| SC-004 queries | Every 10,000-ray batch is bounded to its configured result capacity, 100 repeats have identical ordering/hash, no unbounded allocation growth occurs, and p95 batch time is at most 1,000 ms on the recorded hardware. |
| Memory/allocation | Snapshot high water at most 2 MiB for the reference fixture, provider allocation high water does not grow after warm-up, and no per-body temporary allocation is introduced in snapshot/prepare/publish. |

Worker-count comparisons use 1, 2, 4, and the supported maximum. The 60 Hz
provider-stage budget applies at every supported count; editor responsiveness
is evaluated at the normal supported maximum. A correctness failure cannot be
offset by faster timing.

## Ranked critical path and ownership

1. **Editor protected frame (`EditorSystem`, 174.37 ms p95)** — T107/T109 owner:
   editor hierarchy/window and Play lifecycle paths. Inspect and eliminate
   per-frame full-scene UI traversal while preserving editor selection,
   transactional mutation, and renderer-unavailable behavior.
2. **Renderer protected preparation (`RenderSystem`, 152.53 ms p95)** — T109
   owner: render world-transform and sky-state resolution. Reuse revisioned
   immutable snapshots and avoid repeated full-world scans without hiding
   transform changes.
3. **Physics authoring snapshot/prepare (`63.32/65.18 ms p95`)** — T108 owner:
   `PhysicsAuthoringSnapshot` and `PhysicsSystem`. Replace full idle recapture,
   linear dirty-object membership tests, and per-collider repeated work with
   typed revisioned/indexed data.
4. **Physics publication and After Physics transform flush (`43.09/122.04 ms
   p95`)** — T108 owner: changed/active-body extraction and typed batched
   publication. Preserve the declared protected flush and atomic visibility.
5. **Jolt fixed step (`9.94 ms p95`)** — no provider optimization selected for
   the first pass because it already satisfies the 16.67 ms budget. T110 job
   adapter changes require separate worker-count evidence and must not displace
   the higher measured costs.

The editor/render and physics assignments own disjoint files and may proceed in
parallel. Central integration, full builds, CTest, install, worker-count runs,
and installed-editor acceptance remain with the orchestrator.
