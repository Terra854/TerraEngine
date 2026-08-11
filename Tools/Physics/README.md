# Installed Editor Physics Stress Harness

`RunInstalledEditorPhysicsStress.ps1` is the shared installed-editor performance,
correctness, project-readiness, and managed/automation validation harness. Its
evidence uses stable workflow names so reports remain meaningful across engine
revisions.
It creates a disposable project below `out/`, writes 10,000 physics objects
(1,000 active dynamic, 1,000 kinematic, and 8,000 static by default), starts
the installed editor with isolated MCP preferences, and drives Play Mode
through the loopback JSON-RPC endpoint.

Run it only after the physics foundation RelWithDebInfo build, CTest, and install gates succeed. Other configurations are not accepted for this feature:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Physics/RunInstalledEditorPhysicsStress.ps1 `
	-InstallRoot out/build/RelWithDebInfo/Build -ActiveDynamicCount 1000 -AllowLiveStress
```

The repository's custom install target stages the runnable tree under
`out/build/RelWithDebInfo/Build`; use that staged tree as `-InstallRoot`.
Use `-WorkerCount 1`, `2`, `4`, or `max` (default) to select the shared native
ThreadPool size for a run. `max` preserves hardware-concurrency behavior and
removes `TERRAENGINE_THREADPOOL_WORKERS` from the child environment; the
selected request, resolved worker identity, and threshold settings are recorded
in `summary.json`. The default preparation and extraction thresholds are 512
items each. Explicit 1/2/4 values are matrix overrides only; the native pool
does not cap explicit environment/API requests, and the harness/build scripts
do not pass a `--parallel` argument.

For the worker-scaling acceptance matrix, run one isolated command:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Physics/RunInstalledEditorPhysicsStress.ps1 `
	-InstallRoot out/build/RelWithDebInfo/Build `
	-WorkRoot out/validation/worker-scaling `
	-RunWorkerScalingMatrix -AllowLiveStress
```

This executes paused, unchanged, small, and large scenarios at 1/2/4/max
workers, compares deterministic observation hashes and scaling coverage, and
reports the fastest successful correctness-gated configuration in
`worker-scaling-matrix-summary.json`. Override `-ParallelPreparationThreshold` or
`-ParallelExtractionThreshold` to capture a reproducible threshold comparison.
`-ActiveDynamicCount` may raise the dynamic-body count for an explicitly named
overload run; the acceptance default matches `stress-fixtures.json` exactly.
For the installed authoring acceptance, add `-RunAuthoringWalkthrough`. That
mode uses the advertised public MCP contract to create and edit an object,
verify exact undo/redo, create and instantiate a prefab, apply and revert real
overrides, then unwind and save the temporary edits so normal editor shutdown
remains part of the evidence. It is optional because those authoring calls must
not contaminate the performance baseline.

For the managed-reload-automation installed managed/automation proof, add both `-RunManagedReloadAutomationLoopback` and
`-RunColliderCookIncrementalMatrix`. managed-reload-automation evidence completeness requires the collider-cook matrix in
the same installed-editor run so rejected-batch rollback and protected-boundary
visibility cannot be inferred from a separate artifact. The final gate also
requires exact one-tick measured step deltas, a deterministic state-evidence
hash, normal non-forced shutdown, explicit unavailable-metric records, and a
nonempty project artifact inventory; a missing item changes `summary.json` to
`success: false` and is detailed under `managed-reload-automationEvidenceCompleteness`. The harness
performs a second durable RelWithDebInfo managed reload, discovers a
generation-safe body through a bounded raycast job, inspects scheduler/body
snapshots, replays one idempotent mass-independent acceleration receipt, and
verifies exactly one custom-gravity velocity change on a separate paused step.
It then runs the unchanged 100-step stress interval and reads retained contact
evidence without altering managed gameplay subscriptions. These semantic calls
remain outside the timed performance interval.

For an editor-memory regression run, add both
`-EnablePhysicsDebugVisualization`, `-CaptureProcessMemory`, and
`-SkipProfilerCapture`. The first uses
the maximum 100,000-primitive debug budget with the normal two-tick retention;
the second records private-byte and working-set samples at MCP readiness,
physics readiness, every paused Next publication, and Play Mode exit; the last
excludes the intentionally growing live capture document so the counters isolate
physics/editor ownership. These switches are opt-in so Windows process queries
and debug rendering do not alter the canonical performance baseline.

For the readiness acceptance, run the isolated matrix after the
install target succeeds:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Physics/RunInstalledEditorPhysicsStress.ps1 `
	-InstallRoot out/build/RelWithDebInfo/Build `
	-WorkRoot out/validation/readiness `
	-RunReadinessMatrix -AllowLiveStress
```

The matrix executes cold, warm, stale, corrupt, and forced-build scenarios in
fresh editor processes while reusing only the matrix's disposable project and
`Library/` artifacts where a warm comparison requires it. Cold removes derived
scene and managed-readiness cache data; warm reopens unchanged output; stale
changes the disposable authored source identity; corrupt damages only the
disposable derived envelope; forced-build invokes public `script.compile` even
after a valid warm state. Authored `.tescene` bytes remain the authority and are
never replaced with derived output.

Each scenario retains `project.state` derived-scene/readiness evidence,
`script.status` managed readiness, any durable forced-compile job, monotonic
stage timings, authored hashes, artifact inventory, MCP JSONL, and normal versus
forced shutdown. Accept the matrix only when warm managed readiness is at most
2.5 seconds, stale/corrupt runs fall back without partial scene visibility,
regeneration is reported, forced compile reports a real forced build, the
authored hash changes only in the intentional stale disposable case, and every
editor exits normally.

The script requires a free explicit MCP port (default `43127`) and writes its
disposable project plus JSON evidence below
`out/validation/installed-editor-physics-stress`. It verifies live tick/body
diagnostics, 100 exact paused Next ticks, Play Mode restoration, unchanged
authored scene bytes, and editor process health. It attempts a normal editor
shutdown first and records any last-resort forced termination in `summary.json`.
The summary also contains monotonic stage durations, per-MCP operation p50/p95/p99
latencies, explicit-step publication samples, bounded diagnostics-summary
snapshots, hardware/build metadata, profiler capture job/artifact/summary
evidence, and an artifact inventory. Metrics that the installed MCP contract
cannot expose (for example native worker utilization, protected-boundary stall
histories, or frame responsiveness) are listed explicitly under
`unavailableMetrics`; the run fails if the required profiler capture cannot be
persisted and summarized.

## Profiler capture matrix

Run the opt-in profiler evidence mode for each detail level:

```powershell
foreach ($level in @("detailed", "sampled", "aggregate")) {
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Physics/RunInstalledEditorPhysicsStress.ps1 `
        -InstallRoot out/build/RelWithDebInfo/Build `
        -WorkRoot ("out/validation/profiler-capture/{0}-journal-v1" -f $level) `
        -CaptureLevel $level -RunProfilerCaptureChecks -AllowLiveStress
}
```

New recordings always use the sole `TerraEngineProfilerJournalV1` format;
historical profiler artifacts are unsupported and are never rewritten or
partially interpreted. The harness passes the V1 format identity to
`profiler.capture.start`, holds the recording open for five
wall-clock seconds, loads the committed artifact as a recovery probe, and retains
`summary.json` plus MCP JSONL. Each summary reports
physics, Scene View, scheduler, protected-boundary, responsiveness, presentation,
and work-volume metric availability and applies the 10 MiB five-second size
gate. Keep the installed tree, workload, worker request, and duration constant
when comparing detailed, sampled, and aggregate captures.
