# Renderer efficiency validation tools

These tools generate disposable fixtures, collect installed-editor evidence, and compare completion-qualified native results. The P1–P5 implementation has passed its integrated compile/install and CPU code gates. Native acceptance was subsequently stopped at the user's request; GPU/performance gates remain open until complete evidence passes.

Reusable C++ validation code lives in [Tests/Support/RendererEfficiency](../../Support/RendererEfficiency/README.md), including the native fixture helpers and editor session/evidence harness. Engine and Editor keep their rendering, lifecycle, scheduler, and automation integration hooks. Building and checking this support does not restart native acceptance.

## Build, install, and CPU checks

From the repository root in an x64 Visual Studio developer PowerShell:

```powershell
git submodule update --init --recursive
cmake --preset RelWithDebInfo -DBUILD_TESTING=ON
cmake --build --preset RelWithDebInfo --target install --parallel 2
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo -R '^(Engine|Editor)\.RendererEfficiency$|^Renderer\.Efficiency\..*Contracts$' --no-tests=error --output-on-failure
```

The maintained installation is `out/build/RelWithDebInfo/Build`. Its `renderer-efficiency-artifacts.json` seals the installed tools, editor, dependencies, shader and asset closures. Invoke the runner from that installation, with the same directory as `-InstallRoot`. A checkout script or a mixture of two installations is rejected. Reinstall after changing code, shaders, fixtures, schemas, or tools.

The focused CTest expression includes the engine/editor policy slices and the installed fixture, collection, recovery, native-evidence, lighting, depth, batching, and static-shadow contract tests. These CPU tests open no validation editor and cannot establish completed GPU work, pixels, timestamps, or a performance threshold. The optional hidden-window GLFW recovery test has its own desktop prerequisite and is documented in `Docs/Testing.md` in the source tree.

The installed profiler journal inspector validates a complete terminal, no recovered tail, and every selected frame's consecutive index, completeness, positive duration and unique `engine_cpu`/`ProfilerClock` lane. A stop request may leave a truthful partial final frame outside the selected 360-frame window; selecting that frame still fails. The live timing collector reads a sealed journal with shared read/write access because the profiler retains its flushed output handle after finalization. It verifies source and copied lengths/hashes before and after native inspection. The 21 decoder cases and 16 capture contract cases cover these boundaries. They support the separate matched SC-008 timing comparison and do not replace native stage acceptance.

For the final broad CPU pass, the current 168 registered groups select 166 with `-E '^(Engine\.NestedTaskSettlement|Editor\.RendererEfficiency\.NativeGlfwRecovery)$'`. Run the ten CPU-only nested settlement cases separately using the installed watchdog wrapper and compound filter documented in `Docs/Testing.md`; the normal mixed-group floor remains 11. Verify both the correctness and renderer installed manifests. The original final code gate passed before acceptance repairs; rerun affected checks and integrated verification after those repairs. This registration count is not a claim that native acceptance passed.

## Runner contract and preflight

```powershell
$install = (Resolve-Path './out/build/RelWithDebInfo/Build').Path
$runner = Join-Path $install 'Tests/Tools/RendererEfficiency/Invoke-RendererEfficiencyValidation.ps1'
& $runner -InstallRoot $install -WorkRoot './out/validation/renderer-efficiency-preflight' -OutputDirectory './out/evidence/renderer-efficiency-preflight' -Stage All -Mode Compare -DryRun
if ($LASTEXITCODE -ne 0) { throw 'Installed preflight failed.' }
```

`-DryRun` verifies the installed closure and planned selections without launching a process or creating a project/output directory. Even its paths must pass the fresh-path checks. `WorkRoot` and `OutputDirectory` must be absent, disjoint, outside the installed bundle, and free of redirected/reparse ancestors. Choose new paths for each actual invocation. Do not point either path at an authored project.

| Parameter | Meaning and default |
| --- | --- |
| `Stage` | `P1` through `P5`, or `All` (default). Shared fixture steps are selected by the installed `RendererEfficiencyStageSteps.ps1`. |
| `Mode` | `Reference`, `Candidate`, or `Compare` (default). A single side collects evidence but cannot pass a matched comparison. |
| `ValidationPreparation` | `Serial` or `Concurrent` (default). This selects preparation inside one editor, not overlapping editor processes. |
| `Runs`, `WarmupFrames`, `MeasuredFrames` | Defaults 3, 60, 300. Use these frozen acceptance windows; no step-skipping or reduced-window option exists. |
| `McpPort` | Loopback endpoint, default 43161. Use distinct ports for separate preparation runs. |
| `StartupTimeoutSeconds` | Default 180; separate from operation and cleanup deadlines. |
| `OperationTimeoutSeconds` | Default 21600 (six hours) for each complete native operation. A timeout is not acceptance or permission to truncate samples. |
| `FoundationSmoke` | Ownership/options/lifecycle smoke only. Missing GPU evidence remains Unverified. |
| `RecoveredSessions` | Explicit independently verified closed recovery receipts; the current contract supports only completed P1 `shadow-invalidation` runs 0–2. |
| `BaselineInstallRoot` | Independent post-P1 installation for compatibility capture. Its current collector does not provide final total-frame regression acceptance. |

Runner and comparer exit codes are: **0** preflight success or a passing requested comparison, **1** failed work/checks or failed owned cleanup, **2** Unverified (missing/unavailable evidence, single-side collection, smoke, or incomplete baseline proof), and **3** invalid inputs/identity/schema. Read the summary and individual sessions: a dry-run exit 0 is not a GPU result, and `Collected` is not a success-criterion verdict.

## Native collection

When the user resumes native validation, use fresh Serial and Concurrent matrices, one editor process at a time. A P5 example is:

```powershell
& $runner -InstallRoot $install -WorkRoot './out/validation/renderer-efficiency-p5-serial' -OutputDirectory './out/evidence/renderer-efficiency-p5-serial' -Stage P5 -Mode Compare -ValidationPreparation Serial -Runs 3 -WarmupFrames 60 -MeasuredFrames 300 -McpPort 43161
# Inspect/preserve the actual exit and summary before the next invocation.
& $runner -InstallRoot $install -WorkRoot './out/validation/renderer-efficiency-p5-concurrent' -OutputDirectory './out/evidence/renderer-efficiency-p5-concurrent' -Stage P5 -Mode Compare -ValidationPreparation Concurrent -Runs 3 -WarmupFrames 60 -MeasuredFrames 300 -McpPort 43162
```

P1 Compare collects Candidate before Reference; later stages collect Reference before Candidate. Every selected fixture/step still needs its complete window. Each P1/P4/P5 matrix has three fixtures × three runs × two modes: 18 sessions per preparation, 36 across both. P2/P3 include their additional stage-specific fixtures. Preserve both preparation sets for the final combined comparison; one preparation's summary cannot prove the complete matrix.

The runner materializes owned projects, queues transient options, waits for their actual generations, drives typed transactional fixture changes, and waits for native scene/resource publications. It restores the prior options and normally closes only its owned editor. A forced close, missing restoration, or missing session/lifecycle record remains failure. These settings are validation controls, not saved project preferences.

| Stage | Candidate work compared with its matched Reference |
| --- | --- |
| P1 | Exact whole-shadow reuse versus `AlwaysRefresh`. |
| P2 | Dirty lighting writes versus Full writes, separately for FinalLights and Analysis. |
| P3 | Cooperative depth reduction versus CompleteScan, with isolated completion-qualified query pairs and actual bounds/oracle payloads. |
| P4 | Grouped shadow submission versus Singleton, with equivalent `AlwaysRefresh` work. |
| P5 | Static depth copy plus dynamic overlay with a 64 MiB budget versus budget 0; both retain Grouped/`AlwaysRefresh` controls. |

Stage-owned fixtures run alongside `multiview-lights` and the applicable `recovery-capacity` fault steps. The materializer accepts `--stage P1` through `P5`; the installed runner supplies it. Keep the sealed manifest's IDs, seeds, geometry, ordering, tolerances, and counts unchanged.

## Static classification, budget, and fallback

A static caster needs authored static state, a proven default shadow ABI, complete current owner publications, supported material/geometry, actual deformation absence, and full reusable semantic evidence. Custom, deformable, or unknown casters remain dynamic. Their missing reuse proof does not disable independently proven static rows. Category changes replace the complete partition atomically.

Static admission reserves queried native image requirements before allocation and reconciles actual VMA allocation bytes. The additional GPU budget defaults to **64 MiB**; **0 disables admission**. Charge reservations, live allocations, and retiring owners until actual safe release. Point-light admission reserves all six faces together. Lowering below charged bytes keeps the old effective bound during draining and permits no new admission; requested and effective budgets are separate observations. The too-small fixture requests **1 byte**, not 1 MiB.

Unsupported transfer/format, failed allocation, insufficient budget, incomplete ownership, or unsupported grouping uses the complete existing final-shadow path. The fallback must retain every required caster and face. It cannot claim a static copy, a reusable unknown source, or a successful failed upload. Static copy rows retain their source allocation/region, full face masks, content token, and actual source command/submit receipts. Per-face draws count fresh static and dynamic singleton calls; reused static content contributes no new static draws.

## Evidence, persistence, and comparison

Each output session has `session.json`, owned editor logs, lifecycle evidence, and `artifacts/evidence.json` beside copied project-relative artifacts. `summary.json` describes the invocation. Native artifacts are content-addressed and hash-checked; resolve them relative to their evidence document, not the checkout. Retain the complete directory tree.

Root frames contain only Warmup/Measured samples. Additional hashed receipts retain first native observations without consuming those windows: `ControlTransition`, the two `ResidencyPublication` observations, `LightingBaseline`, and sealed depth recipes. Preserve their nested frame/artifact closure. Scene/Game may use distinct command buffers; each qualified view's rows must agree. Color, depth, lighting and query owners must join through actual frame, command, submit, completion, session, control, options and fixture correlation.

Static raw counters expose complete static/dynamic semantic rows, physical allocation inventory, recorded/submitted uses, accepted command-to-submit mappings, and actual completion/discard histories. Command and submission serials are independent clocks. A recorded use may still have submit 0 in its earlier snapshot while its later accepted mapping and completion receipt are present. Logical tokens, planned groups, or CPU assertions do not prove a fence or physical release.

Evidence publication serializes large arrays one item at a time into an exclusively owned temporary, then atomically replaces the archive. Sharing/lock failures 32/33/1175 receive bounded publication retries. A completed temporary and prior destination are preserved after permanent failure; other errors remain immediate. Preserve those forensic bytes. Do not manufacture a missing session, relabel a failure, or manually promote a temporary into accepted evidence.

`-RecoveredSessions` requires the original failed records, a fully completed native operation, normal/restored exit 0, exact unchanged native identities, raw-byte transformation proof, and every artifact. It copies verified evidence into a fresh suite as `RecoveredCollected`, leaving the original failure untouched. Abruptly interrupted or incomplete sessions cannot use this path.

After complete collection, supply all matched evidence paths to the installed comparer:

```powershell
$comparer = Join-Path $install 'Tests/Tools/RendererEfficiency/Compare-RendererEfficiencyEvidence.ps1'
# Populate these arrays from verified, normally closed session records in both preparations.
& $comparer -Reference $referenceEvidencePaths -Candidate $candidateEvidencePaths -Stage P5 -Manifest (Join-Path $install 'Tests/Fixtures/RendererEfficiency/workloads.json') -Schema (Join-Path $install 'Tests/Tools/RendererEfficiency/Schemas/evidence.schema.json') -Output './out/evidence/renderer-efficiency-p5-comparison.json'
```

Passing CPU contracts or producing counters alone cannot waive paired pixels/depth, coverage, ownership, lifecycle, or performance gates. Missing GPU observations remain Unverified; malformed or mismatched proof is invalid or failed.

## Independent post-P1 baseline

An accepted baseline needs complete fresh P1 Serial/Concurrent shadow invalidation and stable-reuse comparisons, cross-mode/failure correctness and reproducible diagnostics, normally closed sessions, actual adapter/driver identity, and a separately copied/hash-verified full installation. Copy all dependencies, templates, tools, shaders and assets, not only the executable; retain its original seal and approval/evidence hashes. The preserved first-stage installation is provisional until its exact native bytes receive that acceptance.

The maintained runner's `BaselineInstallRoot` branch captures compatibility camera/profiler/transaction artifacts and deliberately reports Unverified pending deterministic total-frame baseline proof. `Compare-RendererEfficiencyEvidence.ps1 -PostP1Baseline` grades supplied matched timing evidence; it does not create that evidence. No accepted archive or total-frame regression result should be inferred from compatibility capture or the provisional preservation receipt. Parent-only OUT archive wrappers have run-specific pins and are not a maintained general-purpose archive CLI.
