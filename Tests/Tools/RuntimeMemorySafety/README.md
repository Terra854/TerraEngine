# Runtime memory safety validation

Use these installed validation tools to collect failure, lifetime, presentation,
renderer ownership, editor diagnostics, and performance evidence. Commands and
contracts here describe the required checks; results belong in a separate
acceptance ledger with links to immutable raw receipts. This README is not an
acceptance result. See [TerraEngine Testing](../../../Docs/Testing.md) in the
source checkout for the maintained CTest workflow.

## Clean configure, compile, install, and CTest

Requires Windows x64, PowerShell 7, the maintained Visual Studio developer
environment and Windows SDK, CMake/Ninja, and repository-pinned dependencies.
Runtime dependencies must come from the installed closure, not a developer PATH
or system package manager. GPU, Renderer, editor, and timing checks additionally
require a usable desktop/Vulkan adapter and an actually enabled Khronos
validation layer. Missing capability is nonpassing evidence.

Run from the repository root in the x64 developer PowerShell environment. Close
an installed editor before replacing its loaded binaries.

```powershell
git submodule update --init --recursive
if ($LASTEXITCODE -ne 0) { throw 'Pinned dependency initialization failed.' }
cmake --preset RelWithDebInfo -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw 'Configure failed.' }
cmake --build --preset RelWithDebInfo --target install --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Compile/install failed.' }
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
	-R '^(Engine|Editor)\.RuntimeMemorySafety\.|^Runtime\.MemorySafety\.' `
	--no-tests=error --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Focused runtime memory safety checks failed.' }
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
	--no-tests=error --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Complete CTest failed; inspect retained evidence.' }
```

The maintained install root is `out/build/RelWithDebInfo/Build`, beneath
`ENGINE_INSTALL_DESTINATION`. Invoke installed tools beneath
`Tests/Tools/RuntimeMemorySafety`. Required entries include the runner and editor
helper scripts, `RuntimeMemorySafetyFixture.exe`, timing tools, `ValidationCases.json`,
`Fixtures/manifest.json`, engine/editor test executables, and installed correctness
watchdogs. `runtime-memory-safety-artifacts.json` pins actual executable, script,
DLL, shader, managed, and fixture inputs by SHA-256 and length. Missing or changed
closure files, source-tree runner invocation, redirected paths, and reused or
overlapping evidence roots are rejected. A successful build or protocol test does
not establish installed runtime acceptance.

## Installed runner

Each invocation needs a new `WorkRoot` outside candidate, reference, and tool
installs. Do not reuse one when changing mode. The examples create fresh roots
and assume the complete installed closure above is available.

```powershell
$installRoot = (Resolve-Path './out/build/RelWithDebInfo/Build').Path
$runner = Join-Path $installRoot 'Tests/Tools/RuntimeMemorySafety/RunInstalledRuntimeMemorySafety.ps1'
$evidenceRoot = Join-Path (Get-Location).Path 'out/validation/runtime-memory-safety'
function New-RmsEvidenceRoot([string]$Lane) {
	Join-Path $evidenceRoot ($Lane + '-' + [guid]::NewGuid().ToString('N'))
}
& $runner -InstallRoot $installRoot -WorkRoot (New-RmsEvidenceRoot 'cpu') `
	-Mode Cpu -Iterations 100 -WorkerCounts @(1,2,8)
if ($LASTEXITCODE -ne 0) { throw 'CPU acceptance was nonpassing.' }
& $runner -InstallRoot $installRoot -WorkRoot (New-RmsEvidenceRoot 'vulkan') `
	-Mode Vulkan -Iterations 100
if ($LASTEXITCODE -ne 0) { throw 'Vulkan acceptance was nonpassing.' }
& $runner -InstallRoot $installRoot -WorkRoot (New-RmsEvidenceRoot 'renderer') `
	-Mode Renderer -Iterations 100 -McpPort 43186
if ($LASTEXITCODE -ne 0) { throw 'Renderer/editor acceptance was nonpassing.' }
```

`Cpu` first executes the exact frozen registry. Minimum unique coverage is 54
cases: 50 engine and four editor cases. Group floors are independent:

| Native prefix | Minimum cases |
| --- | ---: |
| `Engine.RuntimeMemorySafety.TaskCache` | 8 |
| `Engine.RuntimeMemorySafety.DependencyAdmission` | 10 |
| `Engine.RuntimeMemorySafety.Allocators.FixedPool` | 4 |
| `Engine.RuntimeMemorySafety.Allocators.Alignment` | 4 |
| `Engine.RuntimeMemorySafety.Allocators.Overflow` | 6 |
| `Engine.RuntimeMemorySafety.RendererScratch` | 10 |
| `Engine.RuntimeMemorySafety.EmptyFramePolicy` | 8 |
| `Editor.RuntimeMemorySafety.Diagnostics` | 4 |

The aggregate Allocators CTest slice contains the same 14 allocator cases; it
does not add unique coverage. The runner then executes 100
`TaskCache.CompletedNestedCapture` and 100 `TaskCache.NeverDispatchedNestedCapture`
repetitions, plus 100 `DependencyAdmission.MultipleAncestors` faults at each actual
worker count 1, 2, and 8. These are 500 repeated checks at the minimum, in addition
to the initial registry selection. `Iterations` must be at least 100; the worker
matrix must contain exactly 1, 2, and 8. Each native child uses an independent
10-second process-tree watchdog. Require exact selected/PASS names, registered
floors, actual ownership/settlement observations, and normal child completion;
requested worker settings or a forced kill cannot establish those facts.

`Vulkan` executes each of the ten frozen lanes at least 100 times:
`fresh-acquisition`, `recreated-acquisition`, `previously-presented`,
`acquire-wait-consumed`, `record-failure`, `fence-reset-failure`, `submit-failure`,
`candidate-rollback`, `maintenance-enabled`, and `maintenance-disabled`. Receipts
must prove actual acquired indices within observed image/frame counts, native
generation, command recording/reset/submission order, acquire-wait consumption,
retirement, enabled validation without errors, and two healthy ordinary
follow-up frames. Fault checkpoints occur before the targeted native call;
uncalled operations retain null VkResults. Candidate rollback and disabled
maintenance also require both pre-handoff and partial-pass cleanup subcases,
with observed recovery or the specified fail-closed terminal shutdown. Cleanup
requires each iteration's `contextShutdownAttempted=true`, observed
`contextInitializedAfterShutdown=false`, `contextCleanupCompleted=true`, and
`threadPoolCleanupCompleted=true`. Returning from Shutdown alone is insufficient.
Historical parser compatibility replay is never final Vulkan acceptance.

Each Vulkan lane retains one hidden native window while its 100 iterations
construct, initialize, shut down, and destroy fresh Vulkan contexts, devices,
surfaces, and swapchains. The window remains live across iterations; context
and thread-pool cleanup is still observed per iteration. Qualification preserves
enabled overlays and existing global graphics settings. Reusing the window does
not reduce the ten 100-iteration lanes, required failure checkpoints, two healthy
follow-up frames, or actual native retirement proof.

The native receipt uses `vulkanEvidenceVersion=2`. Its root `windowSession`
requires `windowInitializeCount=1`, `windowShutdownCount=1`,
`windowCleanupCompleted=true`, the actual `windowIdentity`, `windowHidden=true`,
and `handleCleared=true`. Each row's `sharedWindow.windowIdentity` must equal that
root identity, with `windowRetainedForSession=true`,
`liveBeforeContextInitialization=true`, `liveAfterContextCleanup=true`,
`hiddenBeforeContextInitialization=true`, and `hiddenAfterContextCleanup=true`.
Each row's `contextLifetime.fixtureLifetimeId` matches its iteration ordinal,
with `constructed=true`, `destructionCompleted=true`, and actual nonnull
`instanceHandle`/`deviceHandle` strings. Native handle values may be reused by
later lifetimes; their numerical uniqueness is not a lifetime requirement.
Per-row window shutdown is not claimed. Root `allSessionLog`,
`validationCallbackLog`, and `allSessionValidationErrors=0` cover window
initialization through final window shutdown. The same receipt must join every
iteration's context/thread-pool cleanup with that final window closure. This
fixture lifecycle establishes no acceptance result without the actual completed
installed matrix and its retained receipts.

An optional, explicit compatibility setup retains one **separate fixture-owned
Vulkan instance** across the same ten100-context lanes:

```powershell
./RunInstalledRuntimeMemorySafety.ps1 -InstallRoot <installed-Build> `
  -WorkRoot <fresh-absolute-directory> -Mode Vulkan -Iterations 100 -VulkanHostInstance
```

The default is false and keeps the version2 receipt/behavior above. The switch is
accepted only with `-Mode Vulkan` or `-Mode Renderer`; it cannot affect CPU, editor or timing
campaigns, including through `-Mode All`. The runner passes the explicit native
option `--vulkan-host-instance true`. The native parser accepts only `true` or
`false`, rejects duplicates/other modes, and requires at least100 iterations
when enabled. No Engine, device, surface, swapchain or vendor DLL is pinned or
shared through this owner; overlays and graphics settings remain enabled/unchanged.

This configuration is transparent lifetime compatibility, **not a NVIDIA crash
fix or proof that the failed unhosted campaigns passed**. It still constructs,
initializes, validates, shuts down and destroys all100 real Engine contexts and
their own instances/devices/surfaces/swapchains per lane. It deliberately no longer
exercises the fixture returning to its last owned instance after each iteration;
that final host-instance release occurs once before final window/GLFW shutdown.
It makes no claim about the number of other instances in the process or driver
dependency residency. Sequential Engine handles may repeat; each concurrently
live Engine instance must differ from the live fixture host instance.

Enabled receipts use `vulkanEvidenceVersion=3`, with root `hostInstance` recording
`requested=true`, actual `createdCount=enumeratedCount=destroyedCount=1`, actual
`createResult=enumerateCountResult=enumerateResult=0`, positive
`physicalDeviceCount`, the actual nonzero `instanceHandle`,
`cleanupResult=ReturnedNormally`, and `handleCleared=true`. Its
`coverageModel=RetainedFixtureHostInstance` is explicit. Both extension/layer counts
are zero, so `hostValidationCallbacks=false`: the bootstrap instance does not claim
debug-messenger coverage. `engineOwnersShared`, `driverModulePinned`,
`globalProcessInstanceCountClaim`, and
`perIterationLastOwnedInstanceTeardownExercised` are all false. Each Engine context
still requires actually enabled validation, every existing native operation and
retirement assertion, and zero session validation errors.

Each row's `hostInstance.instanceHandle` joins that root identity, with actual
`liveBeforeContextInitialization`, `liveAfterContextCleanup`, and
`distinctFromContextInstance` all true; the runner independently compares the
observed Engine instance handle too. The source owns private create/GIPA and
instance-specific destroy/enumeration function pointers from GLFW's loader. It
never installs the bootstrap into Volk's mutable Engine dispatch globals. Its
normal/exception cleanup runs after all context lifetimes and before GLFW can
release its loader.

The existing ordered session log additionally includes
`HostInstanceInitializeBegin/End` after `WindowInitializeEnd`, then all iterations,
then `HostInstanceShutdownBegin/End` before `WindowShutdownBegin`. These four
successful actual boundaries, the matching row slices, final window closure and
normal watchdog-observed process exit are mandatory. Failure during final host
destruction cannot produce an accepted completed case. Version3 cannot be silently
accepted through a default/version2 or historical-protocol request; malformed,
missing or unrequested host facts fail closed. Its protocol regressions exercise
synthetic schema evidence only; parent-installed execution supplies native proof.

With `-Mode Renderer -VulkanHostInstance`, the single normal Engine host remains
unchanged and `rendererEvidenceVersion=3` adds the same host-instance facts. The
bootstrap starts after `HostInitializeEnd`, before retiring Engine's startup
renderer. Thus startup context observation is explicitly
`ExistingStartupContextAfterHostCreation` with `hostLiveBefore=false`; it does not
pretend that startup initialization occurred under an instance created later.
All later Primary/Restart contexts use `ContextInitializedUnderHost`, with actual
host liveness before and after their initialization/retirement.

Root `rendererContexts` contains ordered actual `contexts` observations with
iteration/role, native instance/device handles, initialized/validation facts,
`hostLiveBefore/After`, `distinctFromHost`, `shutdownAttempted`, `contextReleased`
and the checkpoint above. `observedInitializationCount` and
`observedRetirementCount` count these observed context lifetimes, not instrumented
low-level Vulkan API calls. Startup occurs once; each iteration requires Primary,
and the restart lane additionally requires Restart. Every row's
`hostInstance.contextLifecycles` must exactly join its actual root subset.
Sequential native handles may repeat, but none may equal the concurrently live
host handle.

Successful ordering is `HostInitializeEnd`, bootstrap InitializeBegin/End,
`StartupRendererShutdownBegin/End`, all iteration boundaries, bootstrap
ShutdownBegin/End, then `HostShutdownBegin/End`. Final explicit renderer Shutdown
must observe a null context while the window remains live; root
`finalRendererShutdownAttempted`, `finalContextReleased`, and
`windowLiveBeforeHostClose` are mandatory. Only then close the independent host,
then Engine (which owns GLFW/pool teardown). Exception cleanup follows the same
order without throwing from destructors. A retained Engine RHI still owns its own
instance; closing the independent bootstrap on failure does not borrow or release
that Engine instance, and cannot turn incomplete retirement into a passing row.
Engine must still finish with zero workers, null renderer context and no frame
failure. Existing scratch pressure, borrower, cancellation and rollback checks
remain unchanged. The default false Renderer version2 path is preserved.

`Renderer` executes `completed`, `failed`, `cancelled`, `scene-clear`, `shutdown`,
and `restart` at least 100 times each. It verifies real capture/terminal identities,
held-worker read ranges and digests, original failure propagation, owner lifetime,
fallback allocation/deallocation balance, cancellation replacement, new lifetime
after restart, and fresh healthy ordinary rendering where required. Authored
camera/light parity and allocator pressure must be observed, not inferred from
successful startup or a synthetic episode.

Each Renderer lane owns one Engine session and bundled managed host for its
entire process. Its 100 iterations initialize and retire distinct renderer
lifetimes; within each lane, `capturedLifetimeIds` must be distinct and exactly
match observation order. Six lanes therefore qualify at least 600 renderer
lifetimes, with identities qualified by their lane/process. Renderer
initialization/shutdown is repeated; Engine/managed-host
initialization is performed once per lane. The installed process watchdog covers
the whole lane, including final Engine shutdown.

The native renderer receipt uses `rendererEvidenceVersion=2`. Each observation's
`rendererStartup` proves initialization, actually enabled validation, and a
positive renderer session generation. Its `rendererCleanup` must report
`attempted=true`, `completed=true`, `contextReleased=true`, and
`activeAfterRetirement=false`; `retiredState` must be inactive with balanced
fallback reclamation for the final healthy lifetime, or the original lifetime in
the shutdown lane. Every admitted borrower must settle before that retirement.
`workerCountAfterRetirement` remains positive while the shared Engine session is
alive. The root `hostSession` separately requires `engineInitializeCount=1`,
`engineShutdownCount=1`, `engineCleanupCompleted=true`, a positive
`threadPoolWorkerCount`, `threadPoolWorkersAfterCleanup=0`, and `frameFailure=null`.
Per-iteration renderer cleanup cannot substitute for final Engine worker drain.

Root `allSessionLog`, `validationCallbackLog`, and
`allSessionValidationErrors=0` cover Engine initialization through final
shutdown. Ordered records retain `iteration`, `phase`, `eventKind` (`logger` or
`fixtureBoundary`), actual logger level or null, and message. Explicit host initialize/shutdown and
iteration begin/end boundaries record their actual success. Keep these logs,
each renderer's cleanup/borrower/reclamation proof, and final host cleanup in
the same receipt. This session structure does not reduce the six 100-iteration
lanes or establish acceptance without passing their actual installed runs.

Renderer mode also joins installed editor loopback evidence. The helper owns a
fresh generated project, isolated editor preferences, loopback listener/process,
and normal shutdown. It uses the existing `diagnostics.get_summary` endpoint to
read copied `data.rendererScratch` facts during paused Play with no explicit
simulation step. It checks project/scene generations, authoring object identity
and structure, unchanged authored scene bytes, read-only diagnostics, and the
isolated Play/Stop restoration. It does not expose allocator controls. Raw RPC,
authoring observations, and lifecycle receipts remain available for audit.

To collect that helper evidence independently, use a fresh output and work root:

```powershell
$editorWork = New-RmsEvidenceRoot 'editor'
$editorReceipt = $editorWork + '.json'
$editorHelper = Join-Path $installRoot 'Tests/Tools/RuntimeMemorySafety/InvokeRuntimeMemorySafetyEditorDiagnostics.ps1'
& $editorHelper -InstallRoot $installRoot -ToolInstallRoot $installRoot `
	-WorkRoot $editorWork -OutputPath $editorReceipt -McpPort 43187
if ($LASTEXITCODE -ne 0) { throw 'Editor diagnostics loopback was nonpassing.' }
```

Renderer/All may consume this receipt with
`-EditorDiagnosticsEvidencePath $editorReceipt` instead of launching the helper
again. Its installed runtime identity, raw response-to-copy joins, authoring
invariants, and normal closure are still verified. It must refer to the same
candidate installation; an older successful run against different bytes is
insufficient.

## Frozen reference, timing, and independent parity

Foundation order is part of the measurement contract: archive unchanged runtime,
freeze `ValidationCases.json` and `Fixtures/manifest.json`, install/self-test the
compatible timing tools, then capture and seal reference A/A calibration before
production fixes. Archived reference runtime bytes must remain unchanged. New
journal tools decode archived files as data and never load new scratch APIs into
old runtimes. The deterministic materializer preserves its bootstrap separately
and owns only its derivative project; it never launches an editor. Authored inputs
and successful preflight do not establish rendering acceptance.

Before calibration, preserve a verified copy of the entire installed timing tool
tree and use that same immutable `ToolInstallRoot` for reference and candidate.
The feature's frozen v2 tree is
`out/reference/runtime-memory-safety-validation-tools-v2/Build`; the pre-change
runtime is `out/reference/runtime-memory-safety-prechange/Build`. These are
separate from the current candidate install. Shared codec/header changes can
relink a decoder; do not substitute newly built tools for the frozen closure.
The CPU/GPU/Renderer fixture uses the current install because it needs new APIs.

The installed `InvokeRuntimeMemorySafetyTimingMatrix.ps1` owns fresh project and
editor processes through the installed process-job watchdog. `-Mode Calibrate`
uses `-RuntimeInstallRoot` for the immutable archive and `-ToolInstallRoot` for
the frozen tools; provide the original archive receipt with
`-RuntimeIdentityPath` and fresh `-WorkRoot`/`-OutputPath`. It preserves
normal-close evidence and rejects a failed lifecycle. `-ProbeOnly` captures one
reference run for compatibility diagnosis and returns nonpassing exit 2; it
never creates a calibration or replaces any member of the fixed A/A matrix.
Do not rerun calibration to alter an already sealed comparison baseline.

Timing measures the sum of the three existing renderer preparation callback
scopes: `RenderSystem: Protected Publication Capture`,
`RenderSystem: Geometry Route Preparation`, and
`RenderSystem: Virtual Geometry Preparation`. Use Detailed CPU capture every
frame, admitted `maxFrames=4096`, and paused Play without explicit stepping.
Select exactly journal ordinals 0 through 359 as consecutive complete frames,
discard 60, and compute median and nearest-rank p95 on 300 values. The median is
the mean of sorted values 150/151; p95 is sorted value 285 (one-based). Required
phase samples must be unique, complete, positive, finite, correctly ordered,
nonoverlapping, and in frame bounds. Do not double-count child/worker scopes.
Whole-engine duration cannot replace preparation timing.

Collect A1,A'1,A2,A'2,A3,A'3 at each small/normal/stress scale before fixes.
Freeze six separate `max(2%, maximum paired A/A fractional difference)` limits,
one per scale/metric. Capture candidate C1-C3 at each scale and compare Ci only
against its frozen Ai. All 18 run/metric comparisons must pass individually.
Evaluate the inclusive gate as `C <= A * (1 + limit)` with unrounded doubles,
without an epsilon, pooled results, selected outliers, or wider tolerances.
Preserve raw journal/scope records, timestamps, fresh process identities,
environment/settings/fixture/runtime/tool pins, health, normal closure,
thresholds, and failures. Original sealed calibration and archive receipts stay
immutable. Changed tools, fixtures, or environment invalidate affected comparison
sets; document invalidation and repeat the complete required matrix rather than
selecting favorable runs. The frozen measurement contract is identified by the
archived registry's `protocolSha256`; the installed timing rules are documented
in this section.

Performance also requires independent pinned parity evidence at all three scales.
The installed [matching-ABI parity package](Parity/README.md) builds the two
validation hosts against their respective runtime declarations and records the
actual capture, qualification, and pixel-comparison evidence for that join.
Timing and authored fixture hashes alone do not prove captured camera/light
identity, order, or equivalent published outputs. The parity JSON uses
`schemaVersion=runtime-memory-safety-parity-v1`, `status=Passed`, the candidate
`runtimeManifestSha256`, and frozen `fixtureSha256`. Its `scales` array has exactly
one small, normal, and stress row, each with `cameraLightIdentityExact`,
`cameraLightOrderExact`, and `publishedOutputsEquivalent` true, a nonnegative
`fallbackCount`, and nonempty `rawEvidence` pins `{path,sha256,bytes}` to actual
observations. Preserve exact captured-array and common reference output evidence;
use the fixture's frozen image tolerance without excluded image regions. Do not
construct a passing parity receipt from unobserved values or protocol mocks.

Use the original sealed calibration and an independently collected parity file:

```powershell
$referenceRoot = (Resolve-Path './out/reference/runtime-memory-safety-prechange/Build').Path
$timingTools = (Resolve-Path './out/reference/runtime-memory-safety-validation-tools-v2/Build').Path
$calibration = (Resolve-Path './out/evidence/runtime-memory-safety/reference-calibration.json').Path
# Set this to the actual independently collected, pinned parity receipt.
$parity = (Resolve-Path './out/evidence/runtime-memory-safety/parity.json').Path
& $runner -InstallRoot $installRoot -WorkRoot (New-RmsEvidenceRoot 'performance') `
	-Mode Performance -ReferenceInstallRoot $referenceRoot `
	-ReferenceCalibrationPath $calibration -ToolInstallRoot $timingTools `
	-ParityEvidencePath $parity -McpPort 43186
if ($LASTEXITCODE -ne 0) { throw 'Performance/parity acceptance was nonpassing.' }
& $runner -InstallRoot $installRoot -WorkRoot (New-RmsEvidenceRoot 'all') `
	-Mode All -Iterations 100 -WorkerCounts @(1,2,8) `
	-ReferenceInstallRoot $referenceRoot -ReferenceCalibrationPath $calibration `
	-ToolInstallRoot $timingTools -ParityEvidencePath $parity -McpPort 43186
if ($LASTEXITCODE -ne 0) { throw 'At least one required acceptance lane was nonpassing.' }
```

`All` requests Cpu, Vulkan, Renderer/editor, and Performance in order and retains
lane failures. It requires the same immutable reference/calibration/tool and
independent parity inputs as Performance. The example parity path is a caller
input, not a file generated or certified by this README.

## Outcomes and retained evidence

Runner exit 0 means every requested lane passed, exit 1 means Failed, and exit 2
means Unverified. Both nonzero outcomes are nonpassing. Inspect `summary.json`
and retained child receipts; setup errors may stop before a summary is created.
Missing/unsupported evidence, corruption, regression, validation errors, forced
termination, and partial matrices cannot become a pass. Individual timing tools
have their own documented exit codes; use the outer runner outcome for acceptance.

Retain raw stdout/stderr, watchdog process/exit/timeout data, native selected/PASS
names and `RMS_OBSERVATION` JSON, per-iteration native fixture JSON, editor RPC and
authoring/lifecycle evidence, profiler journals and decoded scope proof, parity
pins, all 18 comparison results, and installed/source identities. Keep failed
attempts and invalidation reasons. Unit, decoder/comparator, synthetic protocol,
historical replay, and foundation capability checks report their actual limited
scope and do not replace live graphics, editor, or performance acceptance. The
separate acceptance ledger records which gates passed, failed, or remain
unavailable and points to their receipts; no acceptance status is embedded here.

## Arena and renderer ownership policies

The standalone arena retains its public allocation interfaces. An empty request
through `FrameArena::Allocate(0, validAlignment)`, `NewArray<T>(0)`, or
`FrameAllocator<T>::allocate(0)` returns null without advancing storage or
retaining a fallback. Alignment must be a nonzero power of two; invalid alignment
throws `std::invalid_argument` before state changes, including for zero bytes.
Unrepresentable element counts throw `std::bad_array_new_length`; impossible
byte, padding, or container requests throw `std::bad_alloc`. Allocation and
tracking failures preserve live ranges, position, and existing fallback ownership.
Individual deallocation remains a no-op: destroy containers and settle borrowing
jobs before reset. The six overflow cases use bounded native fault checkpoints
and sentinels rather than huge allocations or wrapped-pointer writes.

Renderer camera/light capture uses explicitly bound PMR vectors backed by the
existing `ReusableArenaResource` in a stable shared scratch owner. One renderer
lifetime retains 16 MiB of backing; an exclusive episode lease precedes vector
construction and outlives all borrowing workers. Capture occurs at the declared
protected boundary, and later preparation reads the captured arrays. Vector and
worker owners retire before the next protected reset or clear; worker lease
destruction does not reset storage. Published output keeps its own ownership
rather than retaining an episode allocator. Diagnostics copy lifetime/episode
identities, terminal outcome, pressure, and reclamation counters. Shutdown and
scene clearing settle execution before scratch retirement, and restart creates a
new lifetime. The Renderer lanes test these boundaries with actual pressure and
held reads rather than treating a reusable allocator as synchronization.
