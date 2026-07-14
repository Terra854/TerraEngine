# T111 Performance Stabilization Evidence

This record separates the T105 pre-optimization baseline, intermediate T111
diagnostics, and the final bounded-acquire worker-count gate. Results are from
the installed `RelWithDebInfo` tree on 2026-07-14 (Windows 11 Home,
Intel Core i9-14900HX, 32 logical processors, RTX 4080 Laptop GPU).

## Reproducible commands

All runs used the staged install tree produced by the repository build:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Physics/RunInstalledEditorPhysicsStress.ps1 `
	-InstallRoot out/build/RelWithDebInfo/Build `
	-WorkRoot out/validation/t111-bounded-acquire-1 `
	-ObjectCount 10000 -WorkerCount 1 -ExplicitStepCount 100 `
	-McpPort 43148 -AllowLiveStress
```

The same command was repeated with `-WorkerCount 2`, `4`, and `max`, changing
`-WorkRoot` and MCP port to 43149, 43150, and 43147 respectively. The
pre-optimization T105 authoritative command used the same install root,
`-WorkRoot out/validation/t105-editor-performance-baseline-3`, and the hardware
maximum worker count.

## Before, intermediate, and final results

T105 authoritative baseline run 3 passed correctness but was slow: 169,100.34
ms end-to-end, 148,592.18 ms for 100 steps, `play.step` p50/p95/p99
400.38/456.05/505.02 ms, and `physics.state` 417.60/502.86/832.74 ms.
Run 1 was observer-inflated (2,065.45 ms publication p50); run 2 was retained
for profiler evidence but failed only because its harness rejected a persisted
stop job. These are not final-gate results.

The first stabilized-max attempt was intermediate evidence (80,294.58 ms,
`play.step` p95 135.1 ms, publication p95 756.5 ms). A second stabilized-max
run passed cleanly in 36,573.10 ms but retained high publication-tail variance;
the bounded-acquire matrix below is the final reproducible gate.

| Shared workers | Result | 100-step stage (ms) | `play.step` p50/p95/p99 (ms) | `physics.state` p50/p95/p99 (ms) | publication p50/p95/p99 (ms) |
|---:|---|---:|---:|---:|---:|
| 1 | pass | 10,525.97 | 21.43/29.99/31.62 | 26.62/48.54/52.66 | 107.36/127.85/134.07 |
| 2 | pass | 9,942.26 | 21.17/26.69/30.20 | 24.46/43.51/58.00 | 98.36/125.82/134.16 |
| 4 | pass | 9,869.61 | 21.34/30.36/33.29 | 24.98/37.57/46.20 | 99.25/126.47/136.31 |
| max (32) | pass | 10,149.11 | 21.23/31.00/33.72 | 24.64/42.78/59.26 | 103.81/124.48/132.34 |

Every final run published 10,000 bodies, executed exactly 100 paused Next
steps, restored Play Mode, preserved authored-scene bytes, shut down normally,
and recorded `forcedTermination=false`. The final summaries also record the
resolved worker count and the `RelWithDebInfo` install root.

## Profiler task and boundary evidence

The pre-presentation-fix worker-budget captures are JSON `.teprof` artifacts:

- `out/validation/t111-worker-1-budget/project/Profiler/project-20260714-072515.teprof`
- `out/validation/t111-worker-2-budget/project/Profiler/project-20260714-072601.teprof`
- `out/validation/t111-worker-4-budget/project/Profiler/project-20260714-072632.teprof`
- `out/validation/t111-worker-max-budget/project/Profiler/project-20260714-072705.teprof`

The 400 ms frame spikes are protected `EditorSystem` work, not Jolt:

| Workers | EditorSystem p50/p95/p99/max (ms) | Frame p50/p95/p99/max (ms) | PhysicsSimulate p99/max (ms) | PhysicsPublish max (ms) |
|---:|---:|---:|---:|---:|
| 1 | 17.49/25.33/326.08/400.15 | 29.23/61.89/337.30/415.37 | 22.14/23.54 | 8.60 |
| 2 | 17.69/26.79/334.91/402.66 | 29.54/61.83/352.28/405.77 | 17.20/21.62 | 8.79 |
| 4 | 17.76/29.82/348.58/385.30 | 26.51/55.98/356.22/397.80 | 12.33/13.34 | 9.96 |
| max | 17.68/34.19/321.20/397.73 | 29.24/59.46/345.87/411.39 | 10.65/11.91 | 12.90 |

`PhysicsExtract` maxima are 4.46, 5.62, 3.01, and 3.72 ms; `PhysicsPrepare`
maxima are 3.82, 4.51, 5.05, and 4.13 ms. `PhysicsPublicationBarrier` stays
below 0.1 ms. Protected flush p95/p99 remain approximately 2.3/2.8 ms. The
periodic spikes align with `JobGraph::ProtectedJob` taskLabel `EditorSystem`
and diagnostics showing 10,000 runtime objects and 3,301 visible empty-object
markers. Publication latency is therefore a downstream wait on the editor
protected boundary, not a provider-step bottleneck.

The follow-up scopes isolated that protected cost to `vkAcquireNextImageKHR`:
acquire p95/p99/max was 249.94/338.24/398.59 ms while UI recording, submit,
and queue-present each remained below 10 ms. After the one-millisecond bounded
acquire/retry-later fix, final engine-frame p95/max was 59.75/68.41 ms at one
worker, 55.10/75.07 ms at two, 49.78/70.74 ms at four, and 62.57/73.68 ms at
32. Final captures are:

- `out/validation/t111-bounded-acquire-1/project/Profiler/project-20260714-073709.teprof`
- `out/validation/t111-bounded-acquire-2/project/Profiler/project-20260714-073739.teprof`
- `out/validation/t111-bounded-acquire-4/project/Profiler/project-20260714-073805.teprof`
- `out/validation/t111-bounded-acquire-max/project/Profiler/project-20260714-073626.teprof`

## One-worker deadlock and fallback

The failed one-worker installed-editor run is recorded at
`out/validation/t111-worker-1-final/evidence/summary.json`: requested and actual
worker count were 1, `play.enter` returned, then `physics.diagnostics.get`
timed out after 30 seconds and the process was force-terminated. The traced
cycle was `PhysicsSimulate` running as the sole JobGraph worker, Jolt queuing
`PhysicsJobAdapter` pump work onto that same pool, and `WaitForIdle` waiting for
the queued pump. In one-worker serial mode the adapter now executes each ready
pump synchronously and reports Jolt concurrency 1, so no queued pump reference
can wait behind its own caller. Multi-worker bounded pumping is unchanged. This
is distinct from the final bounded-acquire gate, which passed all worker counts.

## Presentation acquire scope

`out/validation/t111-present-scope-diagnostic/evidence/summary.json` records the
backend boundary explicitly: swapchain acquire, acquired-image validation,
queue submit/present, fence/semaphore lifecycle, UI present, and recovery remain
protected because of swapchain affinity. Editor acquire now waits for at most
one millisecond; `VK_TIMEOUT`/`VK_NOT_READY` returns a typed skipped result and
retries on a later frame. Existing protected BeginFrame cleanup drains any
stranded render semaphore before slot reuse. Presentation was not moved to an
unsafe worker and its fence/semaphore ownership was not weakened.

## Integrated RelWithDebInfo validation

```powershell
cmake --build --preset RelWithDebInfo --target TerraEngine-EngineTests TerraEngine-EditorTests --parallel 4
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
	-R "^(Engine\.PhysicsFoundation|Engine\.PhysicsUS5|Engine\.PhysicsPerformance|Editor\.PhysicsUS5|Editor\.PhysicsPerformance|Engine\.VirtualGeometryPerformanceGate)$" `
	--output-on-failure --no-tests=error
cmake --build --preset RelWithDebInfo --target install --parallel 4
```

Build and install exited 0. The focused CTest gate passed 6/6 with zero
failures in 0.76 seconds. Direct filtered regressions for the one-worker nested
Jolt pump and bounded editor-presentation contract passed 2/2.

## Correctness, installation, and remaining limits

Final summaries under `out/validation/t111-bounded-acquire-{1,2,4,max}/evidence/summary.json`
contain matching authored-scene SHA-256 values before/after Play Mode, exact
tick progression, 10,000-body publication, clean pause/exit, profiler artifact
inventory, and normal editor shutdown. The staged install root is
`out/build/RelWithDebInfo/Build`; the run artifacts include managed binaries,
scene files, MCP logs, summaries, and profiler captures. CTest/build/install
results are owned by the orchestrator and are referenced here as acceptance
inputs rather than rerun by this documentation task.

The installed MCP contract still marks native worker-utilization history,
protected-boundary stall history, and complete frame-responsiveness history as
unavailable. Profiler task samples cover the measured loop instead. The final
principal limit is ordinary 50–63 ms frame p95 under the artificial 10,000-body
editor load; the former periodic 300–400 ms protected EditorSystem spikes are
absent from the final matrix.
