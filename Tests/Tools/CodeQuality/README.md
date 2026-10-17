# Source review validation

This native C++20 utility inventories production comments, compares comment-only
patches, and measures exact lexical brace spans. It uses the standard library;
it does not require a renderer, graphify, an installed parser package, or files
from the ignored local `specs/` directory. The committed `Baseline/` manifests
preserve the reviewed source inventory, stage boundaries and cleanup ownership.

Use the repository's bundled dependencies and an x64 Visual Studio developer
shell on Windows. From the repository root, configure, compile and install:

```powershell
cmake --preset RelWithDebInfo -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw 'Configure failed.' }
cmake --build --preset RelWithDebInfo --target install --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Compile/install failed.' }
ctest --test-dir out/build/RelWithDebInfo -C RelWithDebInfo `
	-R '^Repository\.(SourceReviewAuditParser|StageOwnershipContracts)$' --no-tests=error --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Source lexer contracts failed.' }
```

The maintained install is `out/build/RelWithDebInfo/Build`. Commands below use
the installed utility and an explicit checkout path:

```powershell
$audit = './out/build/RelWithDebInfo/Build/Tests/Tools/CodeQuality/TerraEngine-SourceReviewAudit.exe'
& $audit --self-test
& $audit --inventory (Get-Location).Path
# Compare immutable pre-comment and candidate copies of one file:
& $audit --compare './out/evidence/before/File.cpp' './Engine/Module/File.cpp'
# Measure an exact opening-brace byte offset from a matching source manifest:
& $audit --span './Engine/Module/File.cpp' 1234 400
& $audit --file-budget './Engine/Renderer/RenderSystem.cpp' 2000
```

After a complete install, changes confined to this standalone utility can be
compiled and installed without replacing unrelated runtime artifacts:

```powershell
cmake --build --preset RelWithDebInfo --target TerraEngine-SourceReviewAudit --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Audit compile failed.' }
cmake --install out/build/RelWithDebInfo --config RelWithDebInfo --component SourceReview
if ($LASTEXITCODE -ne 0) { throw 'Audit install failed.' }
```

Comparison and span examples require real file paths and matching current-byte
offsets. Baseline offsets cannot be applied to edited files. All balanced brace
spans include nested bodies; a reviewed symbol/ownership manifest identifies
which spans are functions. A lexical span does not establish semantic ownership
or runtime safety. Invalid or unsupported syntax must fail visibly.

`VerifyStageOwnership.ps1` checks the manifest's candidate source/header closure,
source hashes, exact native spans, callable/file budgets and ownership references.
Every other brace needs an explicit classification. It rejects included
implementation fragments and creates its output exclusively. Pass absolute
`-RepoRoot`, `-StageManifest`, `-AuditExecutable` and a fresh `-OutputPath` whose
parent directory exists. The manifest must contain a complete candidate section;
the frozen baseline alone is insufficient. Its report explicitly leaves semantic
classification, borrower lifetime and runtime acceptance to separate review and
tests. `Repository.StageOwnershipContracts` runs 26 synthetic rejection and
output-preservation checks using the installed scripts and native lexer, without
loading the engine.

Inventory mode may succeed while reporting baseline boilerplate; that is useful
before cleanup. The detector covers the three original filler patterns and the
repeated generic "Guard this branch" sentence. It scans comment tokens, so a
literal containing the same words is not classified as a comment. The original
2,345-occurrence baseline remains frozen; follow-up cleanups refresh only current
source hashes and stage byte offsets, preserving physical token lines and columns.
Strict-final mode is reserved for final-source checks and cannot
replace occurrence dispositions, token comparisons, behavioral regressions,
installed GPU/editor observations, or sealed timing and parity evidence. Missing
required evidence leaves the corresponding finding incomplete.

Keep immutable pre-comment copies and a separate disposition receipt per batch.
Preserve useful explanations and exact executable/literal spelling; unrelated
formatting is outside comment cleanup. In every cleanup file, preserve physical
newlines, executable token lines and original byte columns, including tab and
non-ASCII prefix spelling: included macros can capture source locations even
when the calling file does not name that facility. Pad removed inline comments
without shifting executable bytes; leave removed comment lines blank. The lexer
supports LF and CRLF, rejects bare CR in both languages, and rejects physical
NEL/LS/PS line separators in C#. Unsupported C# raw interpolations fail closed.
These checks have explicit lexical limits; they do not prove compiler-dialect
coverage or semantic equivalence. `acceptance-contract.json` lists planned cases,
not executed results.

The Windows frame replay collector calls normal `Engine.Initialize`, which
creates the GLFW window and Vulkan renderer, then runs actual engine stages.
Run native replay processes serially; each case uses a separate fresh project
and working directory with a pinned runtime closure. Its separate `FullEngine` and
`ProducerBoundaries` lanes preserve the maintained full-engine flush callback
and expose producer lifetime boundaries respectively. Synthetic protocol
fixtures exercise the comparer only; they do not qualify either runtime lane.

The producer lane executes the initialized Scripting, Animation, Physics,
AnimationPresentation, UI, virtual-geometry streaming and renderer dependency
closure with unchanged native declarations. It records system-schedule validity
and diagnostics before ingress, rejects empty frame plans, and observes actual
scheduler compatibility `Update` calls alongside the declared producer callbacks.
Its protected callback applies the scene queue and acknowledges the included
physics/scripting providers at the maintained publication boundary. The sealed
profiles contain only transforms, animators, meshes and cameras with ignored
root motion and suppressed animation events. This reduced lane does not qualify
spatial, navigation, region streaming, managed resource or authored UI behavior;
the separate `FullEngine` lane retains the complete maintained engine callback.

Build the reference host from the same collector source against the complete
headers of the preserved runtime revision. Extract that exact revision into a
fresh directory with `git archive`, then configure its absolute source path:

```powershell
cmake --preset RelWithDebInfo -DBUILD_TESTING=ON `
	-DTERRAENGINE_FRAME_REPLAY_REFERENCE_SOURCE_ROOT=<absolute-reference-source>
cmake --build --preset RelWithDebInfo --target install --parallel 2
```

The optional `TerraEngine-FrameStageReplayReference` target uses those Engine
headers and the repository's unchanged bundled External headers. It links the
current import-library file without inheriting current Engine include paths.
Before running it, audit its actual PE imports (`dumpbin /imports`) against the
original DLL's exports (`dumpbin /exports`) and check compiler include tracing
for current Engine header leakage. Matching symbol names alone do not prove
matching object layouts. Keep the original source archive, compiler flags,
included-header hashes, import audit and runtime closure with the evidence.

The installed reference host lives under `Reference/` without adjacent runtime
DLLs. `RunFrameStageReplay.ps1` prepends the explicitly selected original DLL
directory to PATH and uses a fresh working directory for logs and caches. Every
collector verifies the expected DLL's physical path and SHA-256 before calling
any engine API. Preserve and verify the original install before and after runs.
The current host is installed with its own runtime DLLs beside it.

Prepare genuine cooked inputs through the installed editor, then run a pinned
host against those closed inputs. Each work directory must be fresh:

```powershell
$tools = Join-Path <absolute-testing-install> 'Tests/Tools/CodeQuality'
& (Join-Path $tools 'PrepareFrameReplayProjects.ps1') -InstallRoot <absolute-testing-install> `
	-WorkRoot <absolute-fresh-project-preparation>
& (Join-Path $tools 'RunFrameStageReplay.ps1') -InstallRoot <absolute-selected-runtime-install> `
	-CollectorExecutable <absolute-matching-header-host> `
	-ExpectedCollectorSha256 <reviewed-host-sha256> `
	-ExpectedEnginePath <absolute-selected-TerraEngine.dll> `
	-ExpectedEngineSha256 <reviewed-runtime-sha256> `
	-WorkloadManifest <absolute-Fixtures/frame-replay-workloads.json> `
	-ProfileManifest <absolute-project-preparation/preparation.json> `
	-WorkRoot <absolute-fresh-campaign>
```

Before bootstrap, startup readiness admits each unique authored native
dependency through its existing asynchronous API and checks its retained future
before admitting the next dependency. All admissions share the same global
60-second deadline. This orders setup publications that share a resource
generation counter; waiting only after concurrent admissions cannot order those
publications. Exact publication generations and immutable native owners remain
verified. Measured callbacks retain the requested 1/2/8-worker concurrency.

The default campaign requires all 15 scenarios at actual 1/2/8 workers. Explicit
`-Scenario cold-direct-complete -Workers 1` selects a partial development probe.
The runner retains failures and does not retry them. A prepared/imported project
does not establish its requested runtime route: the collector must observe it.
Missing observations remain incomplete. Freeze any readiness-control revision
with the same collector source for old/current header-matched hosts, preserve
prior failures and original-only diagnostic receipts, and qualify fresh original
A/A runs before collecting a replacement original/candidate matrix. A/A success
establishes control repeatability for its exercised cases, not final parity.
These correctness controls do not replace or change the independently sealed
60/300-frame timing campaign.

Use `CompareFrameStageReplay.ps1` with a trusted launch-provenance join and an
optional reviewed bijective session map, as documented in
[`Fixtures/frame-replay-protocol/README.md`](Fixtures/frame-replay-protocol/README.md).
The comparer preserves authoritative values and array order. Run its installed
protocol contracts with `Repository.FrameStageReplayComparison`; those synthetic
checks launch no renderer and confer no GPU or performance acceptance.

## Exact replay comparison

Run the complete default 15-scenario matrix at actual workers 1/2/8 for each
runtime/header-matched host. Preserve the original install and complete matching
headers. Candidate hosts normally load `Tests/Tools/CodeQuality/TerraEngine.dll`
from beside their executable; the reference host under `Reference/` has no
adjacent DLL and selects the preserved runtime's `Editor/TerraEngine.dll` through
the runner's dependency directory. `-ExpectedEnginePath` must name the module
actually loaded, even when another installed copy has identical bytes.

For each matched scenario/worker receipt pair, use the installed comparer:

```powershell
& (Join-Path $tools 'CompareFrameStageReplay.ps1') `
	-ReferencePath <absolute-reference-receipt.json> `
	-CandidatePath <absolute-candidate-receipt.json> `
	-ManifestPath <absolute-frozen-frame-replay-workloads.json> `
	-ProvenanceJoinPath <absolute-trusted-launch-join.json> `
	-OutputPath <absolute-fresh-comparison.json>
if ($LASTEXITCODE -ne 0) { throw 'Replay comparison failed.' }
```

The launching owner must construct the trusted join from retained launch/module,
watchdog and owned-observation pins; an untrusted child receipt cannot certify
itself. Add `-SessionMapPath <absolute-reviewed-map.json>` only for an explicit
reviewed bijection permitted by the protocol. Do not use `-ProtocolOnly` for
production acceptance. See the protocol README above for exact fields and
normalization limits. Replay equality does not establish performance acceptance.

## Final source quality and independent token proof

`Repository.SourceReviewQuality` runs the installed wrapper in `Portable` mode
against current checkout bytes and committed final manifests. It requires the
native strict-final inventory, complete stage closure/budgets and all frozen
2,345 occurrence dispositions. It needs no ignored before-copy archive and
reports `tokenProofExecuted=false`; Portable success is not independent
before/after executable-token proof.

Run the separate `PrivateProof` mode with the parent-sealed post-extraction,
pre-comment archive for that proof. Both modes require absolute paths and fresh
output/derived output files beneath an existing directory:

```powershell
$repo = (Get-Location).Path
$qualityTools = Join-Path <absolute-testing-install> 'Tests/Tools/CodeQuality'
$qualityArgs = @{
	RepoRoot = $repo
	AuditExecutable = Join-Path $qualityTools 'TerraEngine-SourceReviewAudit.exe'
	StageValidator = Join-Path $qualityTools 'VerifyStageOwnership.ps1'
	StageManifest = Join-Path $repo 'Tests/Tools/CodeQuality/Baseline/stage-ownership.json'
	Dispositions = Join-Path $repo 'Tests/Tools/CodeQuality/Baseline/comment-dispositions.json'
	ProductionComments = Join-Path $qualityTools 'Baseline/production-comments.json'
	CommentBatches = Join-Path $qualityTools 'Baseline/comment-batches.json'
}
& (Join-Path $qualityTools 'VerifySourceReviewQuality.ps1') @qualityArgs `
	-Mode Portable -OutputPath <absolute-fresh-portable.json>
if ($LASTEXITCODE -ne 0) { throw 'Portable source-quality checks failed.' }
& (Join-Path $qualityTools 'VerifySourceReviewQuality.ps1') @qualityArgs `
	-Mode PrivateProof -EvidenceRoot <absolute-sealed-pre-comment-root> `
	-OutputPath <absolute-fresh-private-proof.json>
if ($LASTEXITCODE -ne 0) { throw 'Independent comment-only token proof failed.' }
```

PrivateProof performs Portable checks plus native comparisons against hashed
before copies and comparator controls. Keep its independent receipt and input
pins. Neither mode proves semantic ownership, runtime lifetime safety, replay
parity or timing; missing required evidence stays incomplete.

## Requested-size synthetic capture cost

After full compile/install, `TerraEngine-SyntheticCaptureCost.exe` calls the
installed production `PrepareSyntheticCapture` directly. There are exactly six
cases: 320x240, 1280x720 and 1920x1080, each opaque and transparent, overlays off.
Each case has three warmups and 30 measured calls. The JSON retains raw
nanoseconds, median and nearest-rank p95, production byte counts and consumption
digests for every validated result.

```powershell
$install = (Resolve-Path './out/build/RelWithDebInfo/Build').Path
$costTools = Join-Path $install 'Tests/Tools/CodeQuality'
$costExe = Join-Path $costTools 'TerraEngine-SyntheticCaptureCost.exe'
$costEngine = (Resolve-Path (Join-Path $costTools 'TerraEngine.dll')).Path
# Verify this complete installed closure against its trusted artifact receipt
# first. Pin the actual adjacent DLL, rather than the separate Editor copy.
$costEngineHash = (Get-FileHash -LiteralPath $costEngine -Algorithm SHA256).Hash.ToLowerInvariant()
$costOutput = Join-Path <absolute-existing-evidence-directory> ('synthetic-cost-' + [guid]::NewGuid().ToString('N') + '.json')
& $costExe --output $costOutput --expected-engine-path $costEngine --expected-engine-sha256 $costEngineHash
if ($LASTEXITCODE -ne 0) { throw 'Synthetic capture characterization failed.' }
```

The fixture checks actual loaded DLL path/hash before helper calls, records its
host executable and DLL pins, then rehashes them afterward. `Editor/TerraEngine.dll`
is a distinct physical path: equal hashes cannot satisfy the loaded-module path
guard. Retain the parent's complete installed-closure pins as well.

This is synchronous helper latency, including RGBA allocation/zero fill, alpha
fill where needed, PNG/Base64, metadata and internal temporary destruction. It
excludes queue/task-pool throughput, backend/GPU, editor/transport, decode, caller
validation/digest and returned-result destruction. Results must have successful
synthetic provenance, exact extents and preflight lengths, no backend target or
overlays, and complete correlation metadata. This cost characterization has no
historical 1x1 ratio or performance threshold and is separate from preparation
parity and sealed total-frame timing.
