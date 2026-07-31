# Animation Validation Evidence

Runtime and authoring documentation is split by workflow. In addition to the
asset, playback, controller, gameplay, procedural, retargeting, motion-matching,
ragdoll, editor, streaming/export, automation, determinism, and performance
guides in this directory, use [Troubleshooting](Troubleshooting.md) for failure
diagnosis and [Migration](Migration.md) when converting older projects or APIs.

animation production validation is accepted phase by phase. Every phase stores its evidence below `out/validation/animation/` and updates `specs/complete-animation-system/acceptance-matrix.md` only for requirements proven by both automated and installed-tree live validation.

`Tools/Animation/AnimationEvidenceTemplate.json` is the canonical evidence shape. Copy it into the phase evidence directory before a run, fill identity and command fields before execution, and update results as each bounded step finishes. The phase-gate script must leave a durable failed result when a required command, artifact, metric, click, MCP response, camera capture, or shutdown signal is missing.

## Required phase sequence

1. Verify the locked animation-validation-is-completed-immutable-pre-feature-reference-capture-remains-sole-pre baseline identity and digest without modifying it.
2. Record the source revision, build configuration, host, power state, graphics route, toolchain, workload hashes, editor settings, and ambient-process checklist.
3. Run focused correctness, corruption, rollback, scheduler, mutation, stress, and repair work for the phase. Intermediate phases do not run a measured performance case.
4. Build the integrated phase and run the CMake install target.
5. Launch only the installed editor and installed runtime artifacts against an isolated project copy below `out/`.
6. Preserve real-click actions, MCP JSONL traffic, Play Mode transitions, authored-state restoration, Scene View, Game View, custom in-game camera evidence, profiler captures, export/runtime results, and orderly shutdown.
7. For an intermediate phase, write `performance/deferral.json` with explicit authority and leave performance pass/comparability/repeatability as `null` (deliberately not measured, not failed). Only final implementation stage runs the comparable warm-up, candidate capture, feature benchmarks, and paired measurements.
8. Record defects and fixes, then rerun every affected focused and integrated gate cleanly.
9. Update only acceptance rows supported by the retained evidence and write the final pass/fail result.

## Evidence directory convention

```text
out/validation/animation/<phase>/
|-- phase-evidence.json
|-- commands/
|-- tests/
|-- build/
|-- install/
|-- editor/
|   |-- clicks.jsonl
|   |-- manual-checklist.json
|   |-- manual-complete.json
|   |-- manual-evidence.json
|   |-- native-window.json
|   |-- mcp.jsonl
|   |-- mcp-operations.json
|   |-- operations/
|   |-- play-mode.json
|   `-- cameras/
|-- performance/
|-- export-runtime/
|-- defects/
|-- result.json
`-- ../<run-id>-acceptance-commit.json
```

Generated projects, editor preferences, and runtime output must remain inside the phase directory. A script may clean only a path it has first resolved and proven to be below the repository `out/` root.

Each `clicks.jsonl` line is one object with the exact checklist `id` and
`input`, `inputDelivery` equal to `human` or `Win32SendInput`, the actual JSON
Boolean `passed: true`, a non-empty `observed` result, and a non-empty
`evidence` array. Every checklist item requires a
`native-window-screenshot` evidence object containing a run-root-relative PNG
path, `actionId` equal to the checklist record, the next unique integer
`actionSequence`, and `nativeWindow` metadata: the launched editor process ID,
hexadecimal window handle, current window title, `captureMethod: "PrintWindow"`,
a round-trip UTC capture timestamp, `dpiAware: true`, and exact pixel
dimensions. Capture timestamps must increase in action order, and every action
must use a newly captured path and image digest. The gate decodes the PNG and
rejects black, uniform, transparent-only, reused, wrong-process, wrong-window,
stale, missing, or path-traversing proof.
`manual-complete.json` must contain the actual JSON Boolean
`completed: true` and a JSON `defects` array; any listed defect must have
non-empty `id`/`observed` fields and exact status `fixed`.

`editor/mcp-operations.json` is parsed rather than merely checked for
existence. It must identify the scenario phase, report reconnect success as the
actual JSON Boolean `true`, and carry exactly one unique, expectation-bound
record for every declared operation and camera capture. Each record preserves
the reviewed stage/order/tool/expectation, a typed passing result, and a digest
of the returned MCP envelope. Every non-capture operation also declares a
unique run-root-relative `evidencePath` that must appear in the scenario's
`requiredEvidence`; the gate writes the full arguments, reviewed expectation,
typed envelope, and envelope digest there, then re-parses and hash-checks it
before locking the run. Duplicate JSON object keys are rejected in every strict
evidence document.

## Acceptance rules

- The baseline at `out/validation/animation/baseline/locked-baseline.json` is read-only. A digest mismatch blocks the phase.
- Installed-editor proof must identify the exact executable hash and may not use a build-tree-only executable or source asset.
- Performance evidence preserves every run, p50/median, p95, p99, worst value, repeatability envelope, required threshold, and comparison decision.
- Renderer, physics, resource, virtual-geometry, scheduler, profiler, MCP, startup, scripting, memory, and export controls remain cumulative.
- A threshold, workload size, resolution, worker count, warm-up, sample count, fixture, or missing-metric rule cannot be weakened to obtain a pass.
- Rejected or conflicting mutations must show atomic rollback and the declared visibility boundary.
- Authored state before and after preview or Play Mode must match unless the evidence identifies an explicit successful authoring transaction.
- A row remains `Evidence incomplete` or `Blocked` when its live proof is unavailable, even if a unit test passes.
- An acceptance update must contain at least one unique row and at least one row whose exact status is `Accepted`; a batch that only records incomplete or blocked rows cannot unblock the next phase. Accepted rows require one `acceptedEvidence` binding whose requirement IDs exactly match the accepted rows and whose operation, capture, and required manual IDs exactly match their reviewed passing scenario evidence. The binding also names at least four retained proof artifacts and includes a specific explanatory statement.

The final animation-validation-reconciliation-animation-semantic-timing-correctness-remains-intermediate-work audit reconciles this accumulated evidence; it does not create replacement proof for an earlier phase.

For a successful gate with acceptance updates, the immutable run-root
`result.json` deliberately records the pre-commit validation state and keeps
`nextPhaseUnblocked: false`. Only after that file and the rest of the run are
evidence-locked does the gate compare-and-swap the matrix. The sibling
`<run-id>-acceptance-commit.json` is the terminal transaction receipt: it
hash-binds the immutable pre-commit result and evidence lock, records the exact
matrix before/after hashes and rows, and alone sets
`nextPhaseUnblocked: true`. A missing receipt means acceptance did not commit.

## Intermediate gate invocation

Use the explicit deferral switch for animation-validation-foundational-focused-non-performance-tests-integrated-build-install-installed-editor and every later non-final phase gate. The authority must name the reviewed decision that permits execution to wait for final integration; a generic empty approval is rejected. Do not pass either performance manifest in this mode.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
	-File Tools/Animation/RunAnimationValidationGate.ps1 `
	-Phase foundation `
	-GateTask foundation-validation `
	-FixtureRoot <fixture-project> `
	-ScenarioManifest <foundation-scenario.json> `
	-AcceptanceUpdateManifest <foundation-acceptance-update.json> `
	-DeferPerformanceExecutionToFinalIntegration `
	-PerformanceDeferralAuthority "animation production validation final-only performance schedule amendment (2026-07-17)" `
	-Execute
```

Deferred focused testing excludes standalone CTests matching `Performance` or exact `Editor.ExportBaseline`. Broad registries also receive exact `TERRAENGINE_TEST_EXCLUDE_FILTER` tokens for the animation benchmark namespaces and currently audited measured profiler, virtual-geometry, physics, and editor timing/load cases. The gate deliberately avoids generic harness tokens such as `Performance`, `PerformanceGate`, `Timing`, or `Overhead`, which would also hide structural result-schema, aggregation, and diagnostics checks. This policy retains the non-timed `Animation.Dormant.*` structural invariants and untimed stress/safety coverage. The gate verifies the locked pre-change baseline, runtime contract, renderer workload manifest, and performance workload manifest without weakening any workload or threshold. A passing intermediate result may unblock later correctness work, but final integration remains blocked until all deferred performance comparisons pass.

## Final integration invocation

Final integration rejects performance deferral. It requires both the animation-performance manifest and the engine-wide candidate, and validates them through the same locked comparator.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass `
	-File Tools/Animation/RunAnimationValidationGate.ps1 `
	-Phase final-live `
	-GateTask animation-validation-repair-capable-final-checks-t-t-are-clean-final-blocking `
	-FixtureRoot <final-fixture-project> `
	-ScenarioManifest <final-scenario.json> `
	-AcceptanceUpdateManifest <final-acceptance-update.json> `
	-FinalAnimationPerformanceEvidenceManifest out/validation/animation/final/animation-performance/animation-validation-runs-every-accumulated-feature-workflow-benchmark-t-runs-every-unrelated-manifest.json `
	-PerformanceCandidateManifest out/validation/animation/final/engine-performance/candidate-manifest.json `
	-Execute
```

There is not yet a separate canonical animation-validation-runs-every-accumulated-feature-workflow-benchmark-t-runs-every-unrelated JSON schema. Until animation-validation-runs-every-accumulated-feature-workflow-benchmark-t-runs-every-unrelated publishes one, the gate fails closed unless its manifest explicitly provides feature `complete-animation-system`, task `animation-validation-runs-every-accumulated-feature-workflow-benchmark-t-runs-every-unrelated`, `passed: true`, a pass status/outcome, `deferred: false`, final-animation-performance scope, and exactly one object for each of animation-outcome-creator-import-representative-animated-gltf-fbx-package-obtain-correctly-referenced-playable-prefab, animation-outcome-creator-build-preview-save-reload-run-locomotion-plus-action-controller-nested-states, animation-outcome-motion-matching-queries-over-reference-production-size-database-return-eligible-result-within, animation-outcome-reference-scene-containing-concurrently-active-joint-characters-two-blended-clips-sustains-least through animation-outcome-optional-animation-profiling-detailed-diagnostics-disabled-versus-enabled-but-collapsed-differ-no, and animation-outcome-character-reference-scene-gpu-visual-mode-reduces-animation-related-cpu-evaluation-time through animation-outcome-gpu-motion-matching-scoring-reduces-animation-related-cpu-search-time-least-reference. Every criterion object must carry the actual JSON Boolean `passed: true`, a passing status, and non-empty metrics, measurements, evidence, or result data; a bare criterion ID is never proof. The validated file is hash-bound and copied into the live-gate evidence. This proves the supplied animation-validation-runs-every-accumulated-feature-workflow-benchmark-t-runs-every-unrelated result is non-deferred, but the current schema cannot independently prove capture time or bind every measurement to the live editor's executable; animation-validation-runs-every-accumulated-feature-workflow-benchmark-t-runs-every-unrelated should add those fields rather than having animation-validation-repair-capable-final-checks-t-t-are-clean-final-blocking infer them.

animation-validation-continuous-hour-mixed-animation-soak-renderer-physics-reload-cancellation-streaming's 24-hour soak remains an independent mandatory final task. No canonical animation-validation-us-cancellable-long-soak-execution-leak-deadlock-stale-publication-checks/animation-validation-continuous-hour-mixed-animation-soak-renderer-physics-reload-cancellation-streaming soak manifest exists yet, so this phase-gate change does not fabricate a soak pass; the future soak contract must add explicit final evidence before animation-validation-repair-capable-final-checks-t-t-are-clean-final-blocking/animation-validation-reconciliation-animation-semantic-timing-correctness-remains-intermediate-work can claim complete feature acceptance.
