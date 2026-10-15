# Matching-ABI renderer parity validation

These Windows validation tools require a completed candidate build/install, the immutable reference runtime with its matching `Compatibility/Engine` headers and `Compatibility/Libraries/TerraEngine.lib`, a sealed reference capture closure receipt, the unchanged frozen validation-tool install and an existing bundled Python runtime. Reference receipts, cooked assets and calibration results are run inputs; they are not distributed with these utilities.

The standalone CMake project builds two hosts from the same source. Baseline uses only archived engine declarations/import library. Candidate uses current declarations/import library and the candidate-only typed scratch observation API. The wrapper stages each complete matching native/Managed closure separately and runs graphics processes serially under the existing frozen watchdog. Do not run concurrently with other graphics validation.

Generate renderer source provenance before running parity. Supply the existing Git executable explicitly; the generator performs read-only Git queries and hashes actual checkout/archive files, and writes only into a fresh directory beneath the checkout's `out` directory. It extracts the six archived blobs from the exact revision in the reference receipt, checks their Git identities, pins current sources and the actual archived compatibility header, and verifies the normal camera loop and mode-specific eight-slice overflow policy. It never starts an engine or changes a reference artifact.

```powershell
$sourceProofRoot = Join-Path $repositoryRoot ('out/validation/renderer-source-proof-' + [Guid]::NewGuid().ToString('N'))
& (Join-Path $installedParityDirectory 'New-RuntimeMemorySafetyRendererSourceProof.ps1') `
	-RepositoryRoot $repositoryRoot -ReferenceReceipt $referenceReceipt `
	-ReferenceCompatibilityRoot (Join-Path $referenceArchiveRoot 'Compatibility') `
	-GitPath $existingGitExecutable -OutputRoot $sourceProofRoot
if ($LASTEXITCODE -ne 0) { throw 'Renderer source proof generation failed' }
$sealedMatchingRendererSourceProof = Join-Path $sourceProofRoot 'source-proof.json'
```

The named paths above must be absolute; `$existingGitExecutable` names the already available Git executable used for the checkout. The original source receipt schema is retained with additional generator/Git/reference-input provenance pins. Failed partial output directories remain available for diagnosis; select a fresh output directory for every attempt.

From the same PowerShell 7 session, define absolute paths for the remaining inputs, then run the installed wrapper:

```powershell
& (Join-Path $candidateInstall 'Tests/Tools/RuntimeMemorySafety/Parity/Run-RuntimeMemorySafetyParity.ps1') `
	-RepositoryRoot $repositoryRoot -CandidateInstallRoot $candidateInstall `
	-ReferenceInstallRoot $referenceInstall -ReferenceReceipt $referenceReceipt `
	-ReferenceClosureReceipt $sealedPostImportClosureReceipt -FrozenToolRoot $frozenValidationInstall `
	-RendererSourceProofPath $sealedMatchingRendererSourceProof `
	-PythonPath $bundledPython -WorkRoot $freshNonexistentWorkRoot
```

The candidate import library defaults to `TerraEngine.lib` beside the candidate Build directory; use `-CandidateImportLibrary` for a relocated layout. The wrapper performs the narrow configure/build of both hosts, then creates fresh small/normal/stress materializations. It checks the original materialization, qualifies only the exact complete observed post-import runtime closure in fresh owned derivatives, and preserves the original receipt and authored scene. Matching runtime CPU validation must accept the complete canonical geometry before native startup. Both original and reimported skeleton/clip files remain present; scene component UUIDs are unchanged.

For a separate explicit narrow build, use the repository's existing Windows CMake environment helper:

```powershell
& (Join-Path $repositoryRoot 'Tests/Tools/Locomotion/InvokeLocomotionBuild.ps1') -CMakeArguments @(
	'-S',$installedParityDirectory,'-B',$freshBuildDirectory,'-G','Ninja','-DCMAKE_BUILD_TYPE=RelWithDebInfo',
	('-DTERRA_REPOSITORY='+$repositoryRoot),('-DTERRA_REFERENCE='+$referenceArchiveRoot),
	('-DTERRA_CANDIDATE_LIBRARY='+$candidateImportLibrary))
& (Join-Path $repositoryRoot 'Tests/Tools/Locomotion/InvokeLocomotionBuild.ps1') -CMakeArguments @(
	'--build',$freshBuildDirectory,'--target','RuntimeMemorySafetyParityBaseline','RuntimeMemorySafetyParityCandidate','--parallel','2')
```

Parent CMake installs this complete source/script/support package; a complete main install remains required before validation. The narrow hosts are built per run against the selected reference and candidate ABI and staged by the wrapper, rather than distributing prebuilt hosts with an unrelated runtime.

`Collect-RuntimeMemorySafetyPostImportClosure.ps1 -ReferenceWorkRoot <absolute sealed calibration root> -ReferenceOnly -OutputPath <fresh absolute receipt>` creates the required supplemental observation after validating all 18 actual reference closures. To compare nine final candidate timing lanes, also supply `-CandidateWorkRoot` and omit `-ReferenceOnly`. This reads files only and never changes calibration. Post-close file equality does not establish in-memory identity during the earlier timing window.

The host requires nonzero native draw/triangle counters, complete same-frame native metadata for every authored camera command and the completed final shared Game View image. The comparator requires exact authored identity/order/component projections, complete ordered native view/command metadata, exact GPU lighting bytes/IDs, native completion receipts and every normalized RGBA channel within 2/255. Candidate additionally requires actual captured typed arrays equal to an independent protected query and actual terminal/reclamation facts for the selected frame. The old common API does not expose private reference camera arrays or per-command camera UUIDs; the receipt states those limits.

Run the CPU-only protocol check with `& $bundledPython -B (Join-Path $installedParityDirectory 'Test-RuntimeMemorySafetyParity.py')`. It checks strict JSON/integer admission, pixel tolerance including alpha and PNG exact-byte roundtrip; it supplies no native acceptance evidence. Wrapper exit 0 means all requested scale comparisons completed equal; differences return 1 and unverified/missing evidence returns 2. `parity-run.json` pins actual commands, host receipts, qualifications and comparator outputs. Feature acceptance remains the integrated runner's responsibility.

The current normal-output protocol uses editor mode without a camera/view override. It retains two native metadata rows per authored camera command in serial order, compares every actually available complete lighting artifact pair, and reads the final completed shared Game View through backend-only `latestCompleted` non-dedicated capture. This is distinct from individual per-camera image coverage, which is not claimed. Eight is concurrent physical snapshot capacity; the unchanged editor backend serializes/completes a prefix and reuses its slices for the sixteen-camera workload. Actual fallback events/counters are retained with the same-frame final image, and baseline/candidate facts must match. Earlier inference that only one command is rendered from `cameras.front()` was superseded by the full per-camera submission loop; runtime-mode eight-command skipping also does not describe this editor path.
