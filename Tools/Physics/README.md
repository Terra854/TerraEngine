# Installed Editor Physics Stress Harness

`RunInstalledEditorPhysicsStress.ps1` supports the deferred T048 MVP demonstration, T105 baseline, T111 performance gate, T113 US5 demonstration, T129 managed/automation loopback, T150 integrated validation, and T153 final acceptance.
It creates a disposable project below `out/`, writes 10,000 physics objects
(1,000 active dynamic, 1,000 kinematic, and 8,000 static by default), starts
the installed editor with isolated MCP preferences, and drives Play Mode
through the loopback JSON-RPC endpoint.

Run it only after the spec 042 RelWithDebInfo build, CTest, and install gates succeed. Other configurations are not accepted for this feature:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Physics/RunInstalledEditorPhysicsStress.ps1 `
	-InstallRoot out/build/RelWithDebInfo/Build -ActiveDynamicCount 1000 -AllowLiveStress
```

The repository's custom install target stages the runnable tree under
`out/build/RelWithDebInfo/Build`; use that staged tree as `-InstallRoot`.
Use `-WorkerCount 1`, `2`, `4`, or `max` (default) to select the shared native
ThreadPool size for a run. `max` preserves hardware-concurrency behavior; the
selected request and resolved worker count are recorded in `summary.json`.
`-ActiveDynamicCount` may raise the dynamic-body count for an explicitly named
overload run; the acceptance default matches `stress-fixtures.json` exactly.
For the T113 installed-editor acceptance, add `-RunAuthoringWalkthrough`. That
mode uses the advertised public MCP contract to create and edit an object,
verify exact undo/redo, create and instantiate a prefab, apply and revert real
overrides, then unwind and save the temporary edits so normal editor shutdown
remains part of the evidence. It is optional because those authoring calls must
not contaminate the T105/T111 performance baseline.

For the T129 installed managed/automation proof, add `-RunUs6Loopback`. The
harness performs a second durable RelWithDebInfo managed reload, discovers a
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
