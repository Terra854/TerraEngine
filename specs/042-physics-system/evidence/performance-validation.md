# Fully Integrated Performance Validation (T147-T148)

This is the post-US7 `RelWithDebInfo` performance gate from 2026-07-14. The
reference machine was Windows 11 Home `10.0.26200`, Intel Core i9-14900HX, 32
logical processors, 33,959,215,104 bytes of RAM, and an RTX 4080 Laptop GPU.
The installed tree was `out/build/RelWithDebInfo/Build`.

## Corrected fixture and hot-path bound

The first T147 matrix revealed that the older live harness created 7,000
dynamic bodies while the committed acceptance manifest specified 1,000 active
dynamics. That overload matrix remained useful evidence: all four runs finished
and restored exact authored bytes, but publication p95 was 158.23/155.85/
161.52/149.02 ms at 1/2/4/max workers, so the first three exceeded the unchanged
150 ms budget. A quiet four-worker repeat passed at 137.18 ms, demonstrating
material tail variance rather than a deterministic provider regression.

The harness now exposes `-ActiveDynamicCount` and defaults to the committed
10,000-object fixture: 1,000 dynamic, 1,000 kinematic, and 8,000 static bodies.
It records all counts in `summary.json`. Transaction diagnostics also stopped
copying inverse transform vectors into its 64-entry receipt history. Full
caller-owned inverse data remains available for rollback/undo, while the
diagnostic history is again bounded metadata instead of a recurring large
allocation sink.

## Reproduction

The following command was repeated with worker counts `1`, `2`, `4`, and
`max`, unique MCP ports, and work roots `out/validation/t147-canonical-{count}`:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
  -File Tools/Physics/RunInstalledEditorPhysicsStress.ps1 `
  -InstallRoot out/build/RelWithDebInfo/Build `
  -WorkRoot out/validation/t147-canonical-1 `
  -ObjectCount 10000 -ActiveDynamicCount 1000 `
  -ExplicitStepCount 100 -WorkerCount 1 -McpPort 43311 `
  -AllowLiveStress
```

All four lanes passed, published exactly 100 Next ticks, preserved identical
scene SHA-256 values before/after Play Mode, shut down normally, and recorded
`forcedTermination=false`.

| Workers | 100-step stage ms | fixed tick p50/p95/p99 ms | play.step p50/p95/p99 ms | publication p50/p95/p99 ms | frame p95/max ms |
|---:|---:|---:|---:|---:|---:|
| 1 | 6,095.77 | 2.393/4.035/5.633 | 16.55/27.60/31.85 | 56.49/92.86/108.09 | 28.16/49.53 |
| 2 | 5,005.43 | 1.730/2.523/2.827 | 11.66/21.41/22.46 | 47.34/66.47/77.84 | 24.16/36.16 |
| 4 | 5,177.81 | 1.220/1.528/2.202 | 12.64/23.03/26.73 | 49.48/65.92/74.87 | 23.16/42.42 |
| max (32) | 5,303.05 | 1.777/2.280/2.390 | 13.25/20.19/21.82 | 50.73/68.02/71.89 | 24.01/33.80 |

Fixed-tick percentiles select the 100 non-empty `PhysicsSimulate` samples from
each profiler capture. They pass the 16.67/20 ms p95/p99 provider budgets at
every supported worker count. Play entry/exit stayed below 2,000 ms, complete
frame p95 stayed below 100 ms, MCP step p95 stayed below 100 ms, publication
p95 stayed below 150 ms, and every 100-step stage stayed below 15,000 ms.

## Stage, boundary, allocation, and cache evidence

| Workers | Prepare p95 | Simulate p95 | Extract p95 | Publish p95 | publication barrier p95 | After Physics flush p95 | EditorSystem p95 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 3.992 | 4.035 | 2.395 | 2.365 | 0.022 | 0.540 | 16.537 |
| 2 | 1.857 | 2.523 | 1.760 | 2.093 | 0.017 | 0.519 | 13.392 |
| 4 | 2.349 | 1.528 | 1.717 | 2.131 | 0.019 | 0.515 | 13.412 |
| max (32) | 2.909 | 2.280 | 1.750 | 2.069 | 0.017 | 0.507 | 13.359 |

Each lane reported 1,040,000 snapshot bytes and 10,060 live provider
allocations. Provider allocation high-water counts were 10,077, 10,087,
10,097, and 10,232. The primitive-only fixture correctly recorded zero cooked
cache assets, bytes, hits, and misses; cooked-cache correctness and reuse are
covered separately by the cooked-collider and export suites.

The installed inventory contained 494 files totaling 392,904,149 bytes.
`TerraEngine.dll` was 19,518,464 bytes, `TerraEngine-Editor.exe` 20,083,712
bytes, and `TerraEngine-Launcher.exe` 464,896 bytes.

The current MCP diagnostics contract explicitly reports native worker-
utilization history, protected-stall history, native stage history, and full
frame-responsiveness history as unavailable. This record does not invent those
values: the `.teprof` task samples establish worker execution and the measured
stage/barrier percentiles above, while requested/actual worker counts establish
the 1/2/4/32 matrix. Adding time-series utilization percentages remains an
instrumentation improvement, not a reason to weaken the recorded budgets.
