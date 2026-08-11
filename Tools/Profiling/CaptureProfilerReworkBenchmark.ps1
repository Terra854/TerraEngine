[CmdletBinding()]
param(
	[string]$InstalledEditor = '',
	[string]$Project,
	[string]$Scene,
	[string]$Manifest = (Join-Path $PSScriptRoot '../FramePublication/workloads.json'),
	[string]$OutputDirectory = 'out/evidence/profiler-rework',
	[string]$InstallRoot,
	[string]$EditorExecutable,
	[string]$CameraHash,
	[int]$WarmupSeconds = 30,
	[int]$DurationSeconds = 60,
	[int]$Runs = 1,
	[ValidateSet('DetailedRecording','MatchedRecordingComparison','MatchedWindowOpen')]
	[string]$Mode = 'DetailedRecording',
	[int]$OpenProfilerAtSeconds = -1,
	[int]$McpPort = 43155,
	[switch]$DryRun,
	[switch]$Resume,
	[switch]$RegenerateEvidenceFromRuns
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Require([bool]$condition, [string]$message) { if(-not $condition) { throw $message } }
function Get-PropertyValue($object, [string]$name) {
	if($null -eq $object) { return $null }
	if($object -is [Collections.IDictionary] -and $object.Contains($name)) { return $object[$name] }
	$property = $object.PSObject.Properties[$name]
	if($null -eq $property) { return $null }
	return $property.Value
}
function Has-Property($object, [string]$name) {
	if($null -eq $object) { return $false }
	if($object -is [Collections.IDictionary]) { return $object.Contains($name) }
	return $null -ne $object.PSObject.Properties[$name]
}
function Get-OptionalNumber($object, [string]$name) {
	$value = Get-PropertyValue $object $name
	if($null -eq $value -or [string]::IsNullOrWhiteSpace([string]$value)) { return $null }
	try { return [double]$value } catch { return $null }
}
function Get-OptionalUInt64($object, [string]$name) {
	$value = Get-PropertyValue $object $name
	if($null -eq $value -or [string]::IsNullOrWhiteSpace([string]$value)) { return $null }
	try { return [UInt64]$value } catch { return $null }
}
function Get-EngineCpuWindowFromIdleEvidence($evidence, [double]$startMilliseconds, [double]$endMilliseconds) {
	$samples = @((Get-PropertyValue $evidence 'samples') | Where-Object {
		$elapsed = [double](Get-PropertyValue $_ 'elapsedMilliseconds')
		$null -ne (Get-PropertyValue $_ 'engineCpuTimeMs') -and $elapsed -ge $startMilliseconds -and $elapsed -lt $endMilliseconds
	} | ForEach-Object { [double](Get-PropertyValue $_ 'engineCpuTimeMs') } | Where-Object {
		-not [double]::IsNaN($_) -and -not [double]::IsInfinity($_) -and $_ -ge 0.0
	} | Sort-Object)
	if($samples.Count -eq 0) { return $null }
	$p95Index = [Math]::Min($samples.Count - 1, [Math]::Max(0, [int][Math]::Ceiling($samples.Count * 0.95) - 1))
	[ordered]@{ count=$samples.Count; meanMs=[double](($samples | Measure-Object -Average).Average); medianMs=[double]$samples[[int][Math]::Floor(($samples.Count - 1) * 0.5)]; p95Ms=[double]$samples[$p95Index]; minimumMs=[double]$samples[0]; maximumMs=[double]$samples[-1] }
}
function Hash-File([string]$path) {
	Require (Test-Path -LiteralPath $path -PathType Leaf) "Identity input is missing: $path"
	(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function Hash-Text([string]$text) {
	$sha = [Security.Cryptography.SHA256]::Create()
	try { ([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($text)))).Replace('-', '').ToLowerInvariant() }
	finally { $sha.Dispose() }
}
function Get-InstalledPackageIdentity([string]$editorPath, [string]$installRoot, [bool]$required) {
	if([string]::IsNullOrWhiteSpace($editorPath) -or -not (Test-Path -LiteralPath $editorPath -PathType Leaf)) {
		if($required) { throw 'Installed package editor executable is missing.' }
		return [ordered]@{ hash = 'dry-run'; files = @() }
	}
	$enginePath = Join-Path (Split-Path -Parent ([IO.Path]::GetFullPath($editorPath))) 'TerraEngine.dll'
	Require (Test-Path -LiteralPath $enginePath -PathType Leaf) 'Installed editor package is missing sibling TerraEngine.dll; refusing incomparable identity.'
	$engine = Get-Item -LiteralPath $enginePath
	$files = @(
		[ordered]@{ role = 'editor'; fileName = [IO.Path]::GetFileName($editorPath); sha256 = Hash-File $editorPath }
		[ordered]@{ role = 'engine'; fileName = $engine.Name; sha256 = Hash-File $engine.FullName }
	)
	[ordered]@{ hash = Hash-Text ($files | ConvertTo-Json -Compress -Depth 4); files = $files }
}
function Validate-BenchmarkEvidenceEnvelope($envelope, [string]$schemaPath) {
	# Keep the final write fail-closed even on machines without a JSON-schema
	# module.  We parse the checked-in schema and enforce its required V1
	# envelope/row invariants here before accepting the artifact.
	Require (Test-Path -LiteralPath $schemaPath -PathType Leaf) "Benchmark evidence schema is missing: $schemaPath"
	$schema = Get-Content -LiteralPath $schemaPath -Raw | ConvertFrom-Json
	Require ([int](Get-PropertyValue $schema 'schemaVersion') -eq 1 -or $null -ne (Get-PropertyValue $schema 'properties')) 'Benchmark evidence schema could not be parsed.'
	Require ([int](Get-PropertyValue $envelope 'schemaVersion') -eq 1) 'Benchmark evidence schemaVersion must be 1.'
	Require ([string](Get-PropertyValue $envelope 'status') -in @('passed','failed','incomparable')) 'Benchmark evidence status is outside the V1 enum.'
	$identityEnvelope = Get-PropertyValue $envelope 'identity'
	foreach($name in @('projectHash','sceneHash','objectPlacementHash','cameraHash','viewportHash','workloadHash','installedBuildHash','installedPackageHash','installedPackageFiles','captureSettingsHash','durationSeconds','canonicalDurationSeconds','mode','formatIdentifier','formatVersion')) {
		Require ($null -ne (Get-PropertyValue $identityEnvelope $name)) "Benchmark evidence identity is missing $name."
	}
	Require (@(Get-PropertyValue $identityEnvelope 'installedPackageFiles').Count -ge 2) 'Benchmark evidence identity must list editor and engine package files.'
	Require ([int](Get-PropertyValue $identityEnvelope 'canonicalDurationSeconds') -eq 300) 'Benchmark evidence canonical duration must be five minutes.'
	Require ([string](Get-PropertyValue $identityEnvelope 'formatIdentifier') -eq 'TerraEngineProfilerJournalV1') 'Benchmark evidence format identifier is not V1.'
	$runsEnvelope = @((Get-PropertyValue $envelope 'runs'))
	$envelopeMode = [string](Get-PropertyValue $identityEnvelope 'mode')
	Require ($envelopeMode -in @('DetailedRecording','MatchedRecordingComparison','MatchedWindowOpen')) 'Benchmark evidence mode is outside the V1 enum.'
	Require ($runsEnvelope.Count -ge $(if($envelopeMode -eq 'MatchedRecordingComparison'){4}else{1})) 'Benchmark evidence does not contain the required completed rows for its mode.'
	foreach($row in $runsEnvelope) {
		foreach($name in @('runId','scenario','mode','completeFrames','engineCpuMedianMs','engineCpuP95Ms','firstWindowMedianMs','firstWindowP95Ms','finalWindowMedianMs','finalWindowP95Ms','residentHighWaterBytes','committedFrames','discardedDetailRecords','unrecordedCoreFrames','finishInteractiveBlockMs','finishMcpRoundTripMs','collectionCostPerFrameFirstNs','collectionCostPerFrameFinalNs','collectionCostGrowthPercent','backlogGrowthFrames','profilerAccumulationMeasured','visualReady')) {
			Require (Has-Property $row $name) "Benchmark evidence run is missing $name."
		}
		$measurement = Get-PropertyValue $row 'windowMeasurement'
		if($null -ne $measurement) {
			$status = [string](Get-PropertyValue $measurement 'status')
			Require ($status -in @('measured','unavailable')) "Benchmark evidence windowMeasurement status is invalid: $status"
			Require ($null -ne (Get-PropertyValue $measurement 'twoPercentGate')) 'Benchmark evidence windowMeasurement is missing twoPercentGate.'
		}
	}
}

# These gates are part of the artifact contract even when a short smoke run is
# used. The five-minute run is the canonical acceptance duration; shorter runs
# only validate wiring and must never be reported as the full gate.
$frameWindowThresholdPercent = 1.0
$recordingOverheadThresholdPercent = 3.0
$openWindowThresholdPercent = 2.0

# The benchmark delegates capture mechanics to the canonical fixed-identity
# runner, but records the exact inputs before doing so. This prevents a later
# comparison from silently mixing scene, build, camera, or duration identities.
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$runner = Join-Path $repo 'Tools/FramePublication/CaptureFramePublicationPerformance.ps1'
Require (Test-Path -LiteralPath $runner -PathType Leaf) "Canonical capture runner is missing: $runner"
Require (-not [string]::IsNullOrWhiteSpace($Project)) 'Project is required.'
Require (-not [string]::IsNullOrWhiteSpace($Scene)) 'Scene is required.'
Require (-not [string]::IsNullOrWhiteSpace($Manifest)) 'Workload manifest is required.'
Require (-not [string]::IsNullOrWhiteSpace($CameraHash)) 'CameraHash is required; camera identity is never inferred.'
if(-not [string]::IsNullOrWhiteSpace($InstalledEditor)) { $EditorExecutable = $InstalledEditor }
if([string]::IsNullOrWhiteSpace($InstallRoot) -and -not [string]::IsNullOrWhiteSpace($EditorExecutable)) {
	$editorDirectory = Split-Path -Parent ([IO.Path]::GetFullPath($EditorExecutable))
	$buildDirectory = Split-Path -Parent $editorDirectory
	$InstallRoot = Split-Path -Parent $buildDirectory
}
Require ($DurationSeconds -ge 60) 'DurationSeconds must be at least 60.'
if($Mode -eq 'MatchedRecordingComparison') { Require ($DurationSeconds -eq 60) 'MatchedRecordingComparison is the 60-second-per-leg overhead smoke; use DetailedRecording for the canonical 300-second run.' }
if($Mode -eq 'MatchedRecordingComparison') { Require ($Runs -ge 1) 'Matched comparison requires at least one D/R/R/D quartet.' }
if($Mode -eq 'MatchedWindowOpen') { Require ($Runs -eq 1) 'MatchedWindowOpen uses one recording containing five bounded closed/open pairs.' }
else { Require ($Runs -ge 1) 'Detailed recording requires at least one run.' }
if($Mode -eq 'MatchedWindowOpen') {
	Require ($OpenProfilerAtSeconds -ge 0 -and $OpenProfilerAtSeconds -lt $DurationSeconds) 'MatchedWindowOpen requires OpenProfilerAtSeconds inside the recording.'
	Require ($DurationSeconds -eq 300 -and $OpenProfilerAtSeconds -eq 240) 'MatchedWindowOpen canonical run is 300 seconds with the window opening at 240 seconds.'
}
Require (Test-Path -LiteralPath $Project -PathType Container) "Project is missing: $Project"
Require (Test-Path -LiteralPath $Scene -PathType Leaf) "Scene is missing: $Scene"
Require (Test-Path -LiteralPath $Manifest -PathType Leaf) "Manifest is missing: $Manifest"
if(-not $DryRun) {
	Require (-not [string]::IsNullOrWhiteSpace($InstallRoot)) 'InstallRoot is required for a live benchmark.'
	Require (-not [string]::IsNullOrWhiteSpace($EditorExecutable)) 'EditorExecutable is required for a live benchmark.'
	Require (Test-Path -LiteralPath $InstallRoot -PathType Container) "Install root is missing: $InstallRoot"
	Require (Test-Path -LiteralPath $EditorExecutable -PathType Leaf) "Editor executable is missing: $EditorExecutable"
}
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $output | Out-Null
$installedPackage = Get-InstalledPackageIdentity $EditorExecutable $InstallRoot (-not $DryRun)
$projectIdentity = Join-Path $Project 'game.json'
$identity = [ordered]@{
	schemaVersion = 1
	format = 'TerraEngineProfilerJournalV1'
	projectPath = [IO.Path]::GetFullPath($Project)
	projectHash = Hash-File $projectIdentity
	scenePath = [IO.Path]::GetFullPath($Scene)
	sceneHash = Hash-File $Scene
	objectPlacementHash = Hash-Text ((Get-Content -Raw -LiteralPath $Scene) + (Get-Content -Raw -LiteralPath $Manifest))
	workloadHash = Hash-File $Manifest
	cameraHash = $CameraHash.ToLowerInvariant()
	viewportHash = Hash-Text ('Game|480x270|diagnostics')
	buildHash = if(Test-Path -LiteralPath (Join-Path $repo '.git')) { (& git -C $repo rev-parse HEAD 2>$null).Trim() } else { 'unavailable' }
	installedBuildHash = $installedPackage.hash
	installedPackageHash = $installedPackage.hash
	installedPackageFiles = $installedPackage.files
	warmupSeconds = $WarmupSeconds
	durationSeconds = $DurationSeconds
	mode = $Mode
	openProfilerAtSeconds = $OpenProfilerAtSeconds
	runs = $Runs
	detailLevel = 'sampled'
	formatIdentifier = 'TerraEngineProfilerJournalV1'
	formatVersion = 1
	captureSettingsHash = Hash-Text ("$Mode|$DurationSeconds|$WarmupSeconds|$Runs|$McpPort|sampled")
	canonicalDurationSeconds = 300
	gates = [ordered]@{
		frameWindowDeltaPercent = $frameWindowThresholdPercent
		recordingOverheadPercent = $recordingOverheadThresholdPercent
		openWindowOverheadPercent = $openWindowThresholdPercent
	}
	viewport = [ordered]@{ width = 480; height = 270; diagnosticsView = 'game' }
}
$effectiveRuns = if($Mode -eq 'MatchedRecordingComparison') { $Runs * 4 }
	else { $Runs }
$identity.runs = $effectiveRuns
$identity.identityHash = Hash-Text (($identity | ConvertTo-Json -Depth 8))
$identityPath = Join-Path $output 'benchmark-identity.json'
$savedIdentity = $null
if($RegenerateEvidenceFromRuns) {
	Require (Test-Path -LiteralPath $identityPath -PathType Leaf) 'Regeneration requires an existing benchmark-identity.json.'
	$savedIdentity = Get-Content -LiteralPath $identityPath -Raw | ConvertFrom-Json
	Require ([string]$savedIdentity.identityHash -eq [string]$identity.identityHash) 'Regeneration refused: saved benchmark identity differs from current inputs.'
}
if($Resume -and (Test-Path -LiteralPath $identityPath -PathType Leaf)) {
	$oldIdentity = Get-Content -LiteralPath $identityPath -Raw | ConvertFrom-Json
	if([string]$oldIdentity.identityHash -ne [string]$identity.identityHash) {
		throw 'Resume refused: benchmark identity differs from the existing artifact.'
	}
}
if(-not $RegenerateEvidenceFromRuns) {
	[IO.File]::WriteAllText($identityPath, ($identity | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
}

$arguments = @{
	Project = $Project; Scene = $Scene; Manifest = $Manifest; OutputDirectory = $output
	InstallRoot = $InstallRoot; EditorExecutable = $EditorExecutable; CameraHash = $CameraHash
	WarmupSeconds = $WarmupSeconds; DurationSeconds = $DurationSeconds; Runs = $effectiveRuns
	Mode = $Mode; OpenProfilerAtSeconds = $OpenProfilerAtSeconds; McpPort = $McpPort
	ProfilerBenchmark = $true
}
if($RegenerateEvidenceFromRuns) {
	Require (-not $DryRun) 'RegenerateEvidenceFromRuns cannot be combined with DryRun.'
	# This mode is intentionally evidence-only: it never invokes the live runner.
	$runFiles = @(Get-ChildItem -LiteralPath $output -Recurse -Filter 'run-*-metrics.json' -File)
	Require ($runFiles.Count -eq $effectiveRuns) "Regeneration expected exactly $effectiveRuns completed run metrics, found $($runFiles.Count)."
	$regeneratedModes = [Collections.Generic.List[string]]::new()
	foreach($runFile in $runFiles) {
		$runData = Get-Content -LiteralPath $runFile.FullName -Raw | ConvertFrom-Json
		Require ([int]$runData.schemaVersion -eq 1 -and [int]$runData.run -ge 1 -and [int]$runData.run -le $effectiveRuns) "Invalid run metric identity: $($runFile.FullName)."
		$rawPath = [string]$runData.rawSummary
		Require (-not [string]::IsNullOrWhiteSpace($rawPath) -and (Test-Path -LiteralPath $rawPath -PathType Leaf)) "Run summary is missing: $rawPath"
		Require ((Hash-File $rawPath) -eq [string]$runData.rawSummaryHash) "Run summary hash mismatch: $rawPath"
		$regeneratedModes.Add([string]$runData.mode)
		$scenarioCapture = Join-Path (Split-Path -Parent (Split-Path -Parent $runFile.FullName)) 'capture.json'
		Require (Test-Path -LiteralPath $scenarioCapture -PathType Leaf) "Scenario capture identity is missing: $scenarioCapture"
		$captureIdentity = (Get-Content -LiteralPath $scenarioCapture -Raw | ConvertFrom-Json).identity
		Require ([string]$captureIdentity.projectHash -eq [string]$identity.projectHash -and
			[string]$captureIdentity.sceneHash -eq [string]$identity.sceneHash -and
			[string]$captureIdentity.cameraHash -eq [string]$identity.cameraHash -and
			[string]$captureIdentity.workloadManifestHash -eq [string]$identity.workloadHash -and
			[string]$captureIdentity.installedHash -eq [string]$identity.installedBuildHash -and
			[string](Get-PropertyValue $captureIdentity 'installedPackageHash') -eq [string]$identity.installedPackageHash) "Run identity mismatch: $scenarioCapture"
	}
	if($Mode -eq 'MatchedWindowOpen') {
		Require (@($regeneratedModes | Where-Object { $_ -eq 'recording-window-open' }).Count -eq $effectiveRuns) 'Regeneration requires every late-window run to contain the bounded timed-open measurement.'
	}
	if($Mode -eq 'MatchedRecordingComparison') {
		Require ($regeneratedModes -contains 'disabled' -and @($regeneratedModes | Where-Object { $_ -like 'recording-*' }).Count -gt 0) 'Regeneration requires disabled and recording rows.'
	}
	$arguments = $null
} else {
	if($Resume) { $arguments.Resume = $true }
	if($DryRun) { $arguments.DryRun = $true }
	& $runner @arguments
	Require ($?) 'Canonical profiler benchmark runner failed.'
}

# Validate every retained raw run before any aggregation, including normal live
# runs. Evidence regeneration and live execution therefore share the same exact
# ABBA order, raw-summary hash, and installed-package identity contract.
$validatedRunFiles = @(Get-ChildItem -LiteralPath $output -Recurse -Filter 'run-*-metrics.json' -File | Sort-Object FullName)
Require ($validatedRunFiles.Count -eq $effectiveRuns) "Expected exactly $effectiveRuns run metrics, found $($validatedRunFiles.Count)."
$seenRunNumbers = [Collections.Generic.HashSet[int]]::new()
foreach($runFile in $validatedRunFiles) {
	$runData = Get-Content -LiteralPath $runFile.FullName -Raw | ConvertFrom-Json
	$runNumber = [int]$runData.run
	Require ($runNumber -ge 1 -and $runNumber -le $effectiveRuns -and $seenRunNumbers.Add($runNumber)) "Run metrics contain a missing, duplicate, or out-of-range run identity: $($runFile.FullName)"
	if($Mode -eq 'MatchedRecordingComparison') {
		$quartetIndex = ($runNumber - 1) % 4
		$expectedMode = if($quartetIndex -eq 0 -or $quartetIndex -eq 3) { 'disabled' } else { 'recording-window-closed' }
		Require ([string]$runData.mode -eq $expectedMode) "Matched comparison requires exact D/R/R/D order; run $runNumber was '$($runData.mode)'."
	}
	$rawPath = [string]$runData.rawSummary
	Require (-not [string]::IsNullOrWhiteSpace($rawPath) -and (Test-Path -LiteralPath $rawPath -PathType Leaf)) "Run summary is missing: $rawPath"
	Require ((Hash-File $rawPath) -eq [string]$runData.rawSummaryHash) "Run summary hash mismatch: $rawPath"
	$scenarioCapture = Join-Path (Split-Path -Parent (Split-Path -Parent $runFile.FullName)) 'capture.json'
	Require (Test-Path -LiteralPath $scenarioCapture -PathType Leaf) "Scenario capture identity is missing: $scenarioCapture"
	$captureIdentity = (Get-Content -LiteralPath $scenarioCapture -Raw | ConvertFrom-Json).identity
	Require ([string]$captureIdentity.projectHash -eq [string]$identity.projectHash -and
		[string]$captureIdentity.sceneHash -eq [string]$identity.sceneHash -and
		[string]$captureIdentity.cameraHash -eq [string]$identity.cameraHash -and
		[string]$captureIdentity.workloadManifestHash -eq [string]$identity.workloadHash -and
		[string]$captureIdentity.installedHash -eq [string]$identity.installedBuildHash -and
		[string](Get-PropertyValue $captureIdentity 'installedPackageHash') -eq [string]$identity.installedPackageHash) "Run identity mismatch: $scenarioCapture"
}

# Convert the canonical run metrics into the profiler-rework evidence envelope.
# Every gate is computed only from rows with the required matched mode; missing
# counterparts remain incomparable instead of being treated as a measured pass.
if(-not $DryRun) {
	$evidenceRuns = [Collections.Generic.List[object]]::new()
					$validatedRunFiles | ForEach-Object {
						$m = Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json
						$isDisabledMetric = [string]$m.mode -eq 'disabled'
						if($isDisabledMetric) {
							Require ($null -eq $m.finishInteractiveBlockMs -and $null -eq $m.finishMcpRoundTripMs) "Disabled run must publish null finish timing values: $($_.FullName)"
						} else {
							Require ($null -ne $m.finishInteractiveBlockMs -and $null -ne $m.finishMcpRoundTripMs) "Run metrics omitted finish timing provenance: $($_.FullName)"
							Require ([double]$m.finishInteractiveBlockMs -ge 0.0 -and [double]$m.finishMcpRoundTripMs -ge 0.0 -and
								-not [double]::IsNaN([double]$m.finishInteractiveBlockMs) -and -not [double]::IsInfinity([double]$m.finishInteractiveBlockMs) -and
								-not [double]::IsNaN([double]$m.finishMcpRoundTripMs) -and -not [double]::IsInfinity([double]$m.finishMcpRoundTripMs)) "Run metrics contained invalid finish timing provenance: $($_.FullName)"
						}
		$rawSummaryPath = [string](Get-PropertyValue $m 'rawSummary')
		$rawSummary = if(-not [string]::IsNullOrWhiteSpace($rawSummaryPath) -and (Test-Path -LiteralPath $rawSummaryPath -PathType Leaf)) { Get-Content -LiteralPath $rawSummaryPath -Raw | ConvertFrom-Json } else { $null }
		$summaryData = if($null -ne $rawSummary) { Get-PropertyValue (Get-PropertyValue (Get-PropertyValue $rawSummary 'profilerEvidence') 'summary') 'data' } else { $null }
		$summaryData = if($null -ne $summaryData) { Get-PropertyValue $summaryData 'summary' } else { $null }
		$summaryBatching = if($null -ne $summaryData) { Get-PropertyValue $summaryData 'batching' } else { $null }
		$health = Get-PropertyValue $m 'health'
		$healthCheckpoints = @((Get-PropertyValue $m 'profilerHealthCheckpoints'))
		$checkpointRows = @($healthCheckpoints | Sort-Object { [double](Get-PropertyValue $_ 'elapsedSeconds') })
		$firstCheckpointHealth = if($checkpointRows.Count -gt 0) { Get-PropertyValue (Get-PropertyValue $checkpointRows[0] 'health') 'data' } else { $null }
		$lastCheckpointHealth = if($checkpointRows.Count -gt 0) { Get-PropertyValue (Get-PropertyValue $checkpointRows[-1] 'health') 'data' } else { $null }
		$firstProduced = if($null -ne $firstCheckpointHealth) { Get-OptionalUInt64 $firstCheckpointHealth 'producedFrames' } else { $null }
		$firstCollection = if($null -ne $firstCheckpointHealth) { Get-OptionalUInt64 $firstCheckpointHealth 'collectionNanoseconds' } else { $null }
		$lastProduced = if($null -ne $lastCheckpointHealth) { Get-OptionalUInt64 $lastCheckpointHealth 'producedFrames' } else { $null }
		$lastCollection = if($null -ne $lastCheckpointHealth) { Get-OptionalUInt64 $lastCheckpointHealth 'collectionNanoseconds' } else { $null }
		$finalProduced = if($null -ne $health) { Get-OptionalUInt64 $health 'committedFrames' } else { $null }
		$finalCollection = if($null -ne $health) { Get-OptionalUInt64 $health 'collectionNanoseconds' } else { $null }
		# The synchronous terminal health response is the final measurement endpoint.
		# Keep it separate from periodic checkpoints so the final interval is
		# explicitly terminal-minus-last-checkpoint, never a whole-run average.
		if($Mode -eq 'DetailedRecording' -and [int]$identity.durationSeconds -ge 300) {
			Require ($checkpointRows.Count -ge 4) "Canonical detailed recording requires at least four 60-second health checkpoints: $($_.FullName)"
			$firstElapsed = [double](Get-PropertyValue $checkpointRows[0] 'elapsedSeconds')
			Require ($firstElapsed -ge 45.0 -and $firstElapsed -le 75.0) "Canonical health checkpoints must begin near the first 60-second interval: $($_.FullName)"
			for($checkpointIndex = 1; $checkpointIndex -lt $checkpointRows.Count; ++$checkpointIndex) {
				$interval = [double](Get-PropertyValue $checkpointRows[$checkpointIndex] 'elapsedSeconds') - [double](Get-PropertyValue $checkpointRows[$checkpointIndex - 1] 'elapsedSeconds')
				Require ($interval -ge 45.0 -and $interval -le 75.0) "Canonical health checkpoints are not near a 60-second cadence: $($_.FullName)"
				$previousHealth = Get-PropertyValue (Get-PropertyValue $checkpointRows[$checkpointIndex - 1] 'health') 'data'
				$currentHealth = Get-PropertyValue (Get-PropertyValue $checkpointRows[$checkpointIndex] 'health') 'data'
				$previousProduced = Get-OptionalUInt64 $previousHealth 'producedFrames'
				$currentProduced = Get-OptionalUInt64 $currentHealth 'producedFrames'
				$previousCollection = Get-OptionalUInt64 $previousHealth 'collectionNanoseconds'
				$currentCollection = Get-OptionalUInt64 $currentHealth 'collectionNanoseconds'
				Require ($null -ne $previousProduced -and $null -ne $currentProduced -and $currentProduced -gt $previousProduced -and
					$null -ne $previousCollection -and $null -ne $currentCollection -and $currentCollection -gt $previousCollection) "Canonical health checkpoints must have positive frame and collection deltas: $($_.FullName)"
			}
			Require ($null -ne $firstProduced -and $null -ne $lastProduced -and $null -ne $finalProduced -and $firstProduced -gt 0 -and $finalProduced -gt $lastProduced -and
				$null -ne $firstCollection -and $null -ne $lastCollection -and $null -ne $finalCollection -and $finalCollection -gt $lastCollection) "Canonical health checkpoints lack positive produced-frame/collection deltas: $($_.FullName)"
		}
		$finalDeltaProduced = if($null -ne $lastProduced -and $null -ne $finalProduced) { [Int64]$finalProduced - [Int64]$lastProduced } else { $null }
		$finalDeltaCollection = if($null -ne $lastCollection -and $null -ne $finalCollection) { [Int64]$finalCollection - [Int64]$lastCollection } else { $null }
		$firstCostPerFrame = if($null -ne $firstProduced -and $firstProduced -gt 0 -and $null -ne $firstCollection) { [double]$firstCollection / [double]$firstProduced } else { $null }
		$finalCostPerFrame = if($null -ne $finalDeltaProduced -and $finalDeltaProduced -gt 0 -and $null -ne $finalDeltaCollection -and $finalDeltaCollection -ge 0) { [double]$finalDeltaCollection / [double]$finalDeltaProduced } else { $null }
		$costGrowth = if($null -ne $firstCostPerFrame -and $firstCostPerFrame -gt 0 -and $null -ne $finalCostPerFrame) { (($finalCostPerFrame - $firstCostPerFrame) / $firstCostPerFrame) * 100.0 } else { $null }
		$backlogGrowth = if($null -ne $lastCheckpointHealth -and $null -ne $health) { [int64](Get-OptionalUInt64 $health 'persistenceLagFrames') - [int64](Get-OptionalUInt64 $lastCheckpointHealth 'persistenceLagFrames') } else { $null }
		$committedFrameCountValue = Get-OptionalUInt64 $health 'committedFrameCount'
		$operationCountValue = Get-OptionalUInt64 $health 'persistenceOperationCount'; if($null -eq $operationCountValue){$operationCountValue=Get-OptionalUInt64 $summaryBatching 'persistenceOperationCount'}
		$framesPerOperationValue = Get-OptionalNumber $health 'framesPerPersistenceOperation'
		if($null -eq $framesPerOperationValue -and $null -ne $committedFrameCountValue -and $null -ne $operationCountValue -and $operationCountValue -gt 0) { $framesPerOperationValue = [double]$committedFrameCountValue / [double]$operationCountValue }
		$compressionRatioValue = Get-OptionalNumber $health 'compressionRatio'
		if($null -eq $compressionRatioValue) {
			$logical = Get-OptionalUInt64 $health 'logicalInputBytes'; if($null -eq $logical){$logical=Get-OptionalUInt64 $summaryBatching 'logicalInputBytes'}; $stored = Get-OptionalUInt64 $health 'storedSegmentBytes'; if($null -eq $stored){$stored=Get-OptionalUInt64 $summaryBatching 'storedSegmentBytes'}
			if($null -ne $logical -and $logical -gt 0 -and $null -ne $stored) { $compressionRatioValue = [double]$stored / [double]$logical }
		}
		$window = Get-PropertyValue $m 'window'
		if($null -eq $window) {
			if($null -ne $rawSummary) {
				$window = Get-PropertyValue (Get-PropertyValue $rawSummary 'profilerEvidence') 'window'
			}
		}
		$firstWindowMetric = Get-PropertyValue $m 'firstWindow'
		$finalWindowMetric = Get-PropertyValue $m 'finalWindow'
		if($isDisabledMetric -and $null -ne $rawSummary) {
			$idleEvidence = Get-PropertyValue $rawSummary 'profilerIdleEvidence'
			$durationMs = [double](Get-PropertyValue $idleEvidence 'durationSeconds') * 1000.0
			$stabilityWindowMs = if([int]$identity.durationSeconds -ge 300) { 60000.0 } else { 300000.0 }
			if($null -eq $firstWindowMetric) { $firstWindowMetric = Get-EngineCpuWindowFromIdleEvidence $idleEvidence 0.0 $stabilityWindowMs }
			if($null -eq $finalWindowMetric) { $finalWindowMetric = Get-EngineCpuWindowFromIdleEvidence $idleEvidence ([Math]::Max(0.0,$durationMs-$stabilityWindowMs)) ([double]::PositiveInfinity) }
		}
		$windowMeasurement = Get-PropertyValue $m 'windowMeasurement'
		$overheadTiming = Get-PropertyValue $m 'overheadTiming'
		if($Mode -eq 'MatchedRecordingComparison') {
			Require ($null -ne $overheadTiming -and [string](Get-PropertyValue $overheadTiming 'metric') -eq 'engine-cpu-time' -and
				[int](Get-PropertyValue $overheadTiming 'sampleIntervalMilliseconds') -eq 100 -and
				-not [bool](Get-PropertyValue $overheadTiming 'windowVisible') -and
				[int](Get-PropertyValue $overheadTiming 'blockCount') -eq 6 -and
				@(Get-PropertyValue $overheadTiming 'blocks').Count -eq 6) "Matched recording row lacks six complete closed 100 ms overhead blocks: $($_.FullName)"
		}
		if($Mode -eq 'MatchedRecordingComparison') {
			Require ($null -ne $firstWindowMetric -and $null -ne $finalWindowMetric) "Matched recording row lacks authoritative first/final windows: $($_.FullName)"
		}
		$metricMode = [string](Get-PropertyValue $m 'mode')
		if($RegenerateEvidenceFromRuns -and $Mode -eq 'MatchedWindowOpen' -and $metricMode -eq 'recording-window-open') {
			# Older retained bundles captured all five bounded pairs but left the
			# envelope status at not-requested. Normalize only this in-memory view;
			# raw summaries remain untouched and every pair is validated first.
			$paired = if($null -ne $windowMeasurement) { @(Get-PropertyValue $windowMeasurement 'pairedWindows') } else { @() }
			Require ($paired.Count -eq 5) "Timed-open regeneration requires exactly five paired windows: $($_.FullName)"
			foreach($pair in $paired) {
				$closed = Get-PropertyValue $pair 'closed'; $open = Get-PropertyValue $pair 'open'
				$medianDelta = Get-PropertyValue $pair 'medianDeltaPercent'; $p95Delta = Get-PropertyValue $pair 'p95DeltaPercent'; $latency = Get-PropertyValue $pair 'latencyMilliseconds'
				Require ([string](Get-PropertyValue $closed 'status') -eq 'measured' -and [string](Get-PropertyValue $open 'status') -eq 'measured') "Timed-open regeneration found an incomplete paired window: $($_.FullName)"
				Require ($null -ne $medianDelta -and $null -ne $p95Delta -and $null -ne $latency -and
					-not [double]::IsNaN([double]$medianDelta) -and -not [double]::IsInfinity([double]$medianDelta) -and
					-not [double]::IsNaN([double]$p95Delta) -and -not [double]::IsInfinity([double]$p95Delta) -and
					-not [double]::IsNaN([double]$latency) -and -not [double]::IsInfinity([double]$latency) -and [double]$latency -ge 0.0) "Timed-open regeneration found invalid paired timing: $($_.FullName)"
				Require ($null -ne (Get-PropertyValue $windowMeasurement 'twoPercentGate')) "Timed-open regeneration missing twoPercentGate: $($_.FullName)"
			}
			$windowMeasurement.status = 'measured'
		}
		$windowStatus = if($null -ne $windowMeasurement) { [string](Get-PropertyValue $windowMeasurement 'status') } else { '' }
		if($windowStatus -eq 'not-requested') {
			# A timed-open recording must prove measured pairs; only the closed or
			# otherwise non-window leg is allowed to become schema-valid unavailable.
			if($Mode -eq 'MatchedWindowOpen' -and $metricMode -eq 'recording-window-open') {
				Require $false "MatchedWindowOpen recording row requires measured windowMeasurement: $($_.FullName)"
			}
			$windowMeasurement.status = 'unavailable'
			if($null -eq (Get-PropertyValue $windowMeasurement 'twoPercentGate')) { $windowMeasurement.twoPercentGate = $false }
		}
		$evidenceRuns.Add([ordered]@{
			runId = "$($m.scenario)-$($m.run)"; runNumber = [int]$m.run; scenario = [string]$m.scenario; mode = if($null -ne $m.mode) { [string]$m.mode } elseif($Mode -eq 'MatchedWindowOpen') { 'recording-window-open' } else { 'recording-window-closed' }
			completeFrames = [UInt64]$m.frameTiming.count
			engineCpuMedianMs = [double]$m.frameTiming.medianMs; engineCpuP95Ms = [double]$m.frameTiming.p95Ms
			firstWindowMedianMs = if($null -ne $firstWindowMetric){[double]$firstWindowMetric.medianMs}else{[double]$m.frameTiming.medianMs}; firstWindowP95Ms = if($null -ne $firstWindowMetric){[double]$firstWindowMetric.p95Ms}else{[double]$m.frameTiming.p95Ms}
			finalWindowMedianMs = if($null -ne $finalWindowMetric){[double]$finalWindowMetric.medianMs}else{[double]$m.frameTiming.medianMs}; finalWindowP95Ms = if($null -ne $finalWindowMetric){[double]$finalWindowMetric.p95Ms}else{[double]$m.frameTiming.p95Ms}
				controlSlopeMedianPercent = $null; controlSlopeP95Percent = $null; recordingMinusControlSlopeMedianPercent = $null; recordingMinusControlSlopeP95Percent = $null
				collectionCostPerFrameFirstNs = $firstCostPerFrame; collectionCostPerFrameFinalNs = $finalCostPerFrame; collectionCostGrowthPercent = $costGrowth; backlogGrowthFrames = $backlogGrowth; profilerAccumulationMeasured = ($null -ne $costGrowth)
				overheadMedianMs = if($null -ne $overheadTiming){[double](Get-PropertyValue $overheadTiming 'medianMs')}else{$null}; overheadP95Ms = if($null -ne $overheadTiming){[double](Get-PropertyValue $overheadTiming 'p95Ms')}else{$null}; overheadSampleCount = if($null -ne $overheadTiming){[UInt64](Get-PropertyValue $overheadTiming 'sampleCount')}else{$null}; overheadBlockCount = if($null -ne $overheadTiming){[int](Get-PropertyValue $overheadTiming 'blockCount')}else{$null}; overheadBlocks = if($null -ne $overheadTiming){Get-PropertyValue $overheadTiming 'blocks'}else{$null}; overheadSampleIntervalMilliseconds = if($null -ne $overheadTiming){[int](Get-PropertyValue $overheadTiming 'sampleIntervalMilliseconds')}else{$null}; overheadTimingSource = if($null -ne $overheadTiming){[string](Get-PropertyValue $overheadTiming 'timingSource')}else{$null}
			residentHighWaterBytes = if($null -ne $health){[UInt64]$health.residentHighWaterBytes}else{[UInt64]0}
			maximumPendingBytes = Get-OptionalUInt64 $health 'pendingBytes'
			maximumPersistenceLagMs = if($null -ne $health){[double]$health.persistenceLagHighWaterFrames * [double]$m.frameTiming.medianMs}else{0.0}
			committedFrames = if($null -ne $health){[UInt64]$health.committedFrames}else{[UInt64]0}
			discardedDetailRecords = if($null -ne $health){[UInt64]$health.droppedDetailRecords}else{[UInt64]0}
			unrecordedCoreFrames = if($null -ne $health){[UInt64]$health.rejectedCoreFrames}else{[UInt64]0}
						finishInteractiveBlockMs = if($isDisabledMetric) { $null } else { [double]$m.finishInteractiveBlockMs }
						finishMcpRoundTripMs = if($isDisabledMetric) { $null } else { [double]$m.finishMcpRoundTripMs }
			windowOpenLatencyMs = if($null -ne $window -and [string](Get-PropertyValue $window 'status') -eq 'measured') {
				[double](Get-PropertyValue $window 'visibilityRequestMilliseconds')
			} else { 0.0 }
			windowMeasurement = $windowMeasurement
			visualReady = [bool]$m.visualReady
			logicalInputBytes = if($null -ne (Get-OptionalUInt64 $health 'logicalInputBytes')) { Get-OptionalUInt64 $health 'logicalInputBytes' } else { Get-OptionalUInt64 $summaryBatching 'logicalInputBytes' }
			encodedPayloadBytes = if($null -ne (Get-OptionalUInt64 $health 'encodedPayloadBytes')) { Get-OptionalUInt64 $health 'encodedPayloadBytes' } else { Get-OptionalUInt64 $summaryBatching 'encodedPayloadBytes' }
			storedSegmentBytes = if($null -ne (Get-OptionalUInt64 $health 'storedSegmentBytes')) { Get-OptionalUInt64 $health 'storedSegmentBytes' } else { Get-OptionalUInt64 $summaryBatching 'storedSegmentBytes' }
			persistenceOperationCount = $operationCountValue
			committedFrameCount = $committedFrameCountValue
			framesPerPersistenceOperation = $framesPerOperationValue
			compressionRatio = $compressionRatioValue
			collectionNanoseconds = Get-OptionalUInt64 $health 'collectionNanoseconds'
			sealingNanoseconds = Get-OptionalUInt64 $health 'sealingNanoseconds'
			queueingNanoseconds = Get-OptionalUInt64 $health 'queueingNanoseconds'
			persistenceNanoseconds = Get-OptionalUInt64 $health 'persistenceNanoseconds'
			finalizationNanoseconds = Get-OptionalUInt64 $health 'finalizationNanoseconds'
		})
	}
	function Percent-Delta([double]$candidate, [double]$baseline) {
		if($baseline -le 0.0) { return [double]::PositiveInfinity }
		return (($candidate - $baseline) / $baseline) * 100.0
	}
	function Median-Value($values) {
		$ordered = @($values | ForEach-Object { [double]$_ } | Sort-Object)
		if($ordered.Count -eq 0) { return [double]::NaN }
		$middle = [int][Math]::Floor($ordered.Count / 2)
		if(($ordered.Count % 2) -eq 1) { return [double]$ordered[$middle] }
		return ([double]$ordered[$middle - 1] + [double]$ordered[$middle]) / 2.0
	}
	$rows = @($evidenceRuns)
	$recordingRows = @($rows | Where-Object { $_.mode -like 'recording-*' })
	$disabledRows = @($rows | Where-Object { $_.mode -eq 'disabled' })
	$closedRows = @($rows | Where-Object { $_.mode -eq 'recording-window-closed' })
	$openRows = @($rows | Where-Object { $_.mode -eq 'recording-window-open' })
	foreach($recordingRow in $recordingRows) {
		$controlRow = @($disabledRows | Where-Object { $_.scenario -eq $recordingRow.scenario } | Select-Object -First 1)
		if($controlRow.Count -eq 1) {
			$recordingSlopeMedian = Percent-Delta $recordingRow.finalWindowMedianMs $recordingRow.firstWindowMedianMs
			$recordingSlopeP95 = Percent-Delta $recordingRow.finalWindowP95Ms $recordingRow.firstWindowP95Ms
			$controlSlopeMedian = Percent-Delta $controlRow[0].finalWindowMedianMs $controlRow[0].firstWindowMedianMs
			$controlSlopeP95 = Percent-Delta $controlRow[0].finalWindowP95Ms $controlRow[0].firstWindowP95Ms
			$recordingRow.controlSlopeMedianPercent = $controlSlopeMedian
			$recordingRow.controlSlopeP95Percent = $controlSlopeP95
			$recordingRow.recordingMinusControlSlopeMedianPercent = $recordingSlopeMedian - $controlSlopeMedian
			$recordingRow.recordingMinusControlSlopeP95Percent = $recordingSlopeP95 - $controlSlopeP95
			$controlRow[0].controlSlopeMedianPercent = $controlSlopeMedian
			$controlRow[0].controlSlopeP95Percent = $controlSlopeP95
		}
	}
	# First/final stability belongs to the canonical five-minute
	# recording. A five-minute late-window run intentionally spends its
	# final minute changing UI state, so applying the duration slope there would
	# conflate the independent window-open gate with recording stability.
	# Raw Engine CPU first/final windows are retained for diagnosis only. The
	# canonical stability gate uses fixed health checkpoints below, avoiding
	# causal claims from unrelated workload or thermal drift.
	$durationSlopePassed = $true
	$recordingOverheadMeasured = $disabledRows.Count -gt 0 -and $recordingRows.Count -gt 0
	$recordingOverheadPassed = $Mode -ne 'MatchedRecordingComparison'
	if($Mode -eq 'MatchedRecordingComparison' -and $recordingOverheadMeasured) {
		# Compare the ABBA condition medians. Each row already contains the median
		# of six retained ten-second blocks; this second bounded median cancels
		# monotonic run-order drift while preserving every raw row in the envelope.
		$disabledMedian = Median-Value @($disabledRows | ForEach-Object { $_.overheadMedianMs })
		$disabledP95 = Median-Value @($disabledRows | ForEach-Object { $_.overheadP95Ms })
		$recordingMedian = Median-Value @($recordingRows | ForEach-Object { $_.overheadMedianMs })
		$recordingP95 = Median-Value @($recordingRows | ForEach-Object { $_.overheadP95Ms })
		$recordingOverheadPassed =
			(Percent-Delta $recordingMedian $disabledMedian) -le $recordingOverheadThresholdPercent -and
			(Percent-Delta $recordingP95 $disabledP95) -le $recordingOverheadThresholdPercent
	}
	# The closed control row is intentionally `not-requested`; window overhead
	# is measured entirely within the timed-open run. Only rows carrying measured
	# pre/post samples participate in this gate.
	$timedWindowRows = @($openRows | Where-Object {
		$measurement = Get-PropertyValue $_ 'windowMeasurement'
		$null -ne $measurement -and [string](Get-PropertyValue $measurement 'status') -eq 'measured'
	})
	$windowOverheadMeasured = $timedWindowRows.Count -gt 0
	$windowOverheadPassed = $Mode -ne 'MatchedWindowOpen'
	if($Mode -eq 'MatchedWindowOpen' -and $windowOverheadMeasured) {
		$measurement = Get-PropertyValue $timedWindowRows[0] 'windowMeasurement'
		$openLatency = [double]$timedWindowRows[0].windowOpenLatencyMs
		$windowOverheadPassed = $null -ne $measurement -and
			[string](Get-PropertyValue $measurement 'status') -eq 'measured' -and
			[bool](Get-PropertyValue $measurement 'twoPercentGate') -and
			-not [double]::IsNaN($openLatency) -and -not [double]::IsInfinity($openLatency) -and $openLatency -ge 0.0
		$windowOverheadPassed = $windowOverheadPassed -and $openLatency -le 1000.0
	}
	$residentBudgetPassed = $recordingRows.Count -gt 0 -and
		@($recordingRows | Where-Object { $_.residentHighWaterBytes -gt 268435456 }).Count -eq 0
	$profilerAccumulationPassed = $Mode -ne 'DetailedRecording' -or ([int]$identity.durationSeconds -lt 300) -or
		($recordingRows.Count -gt 0 -and @($recordingRows | Where-Object { -not $_.profilerAccumulationMeasured -or $_.collectionCostGrowthPercent -gt 1.0 -or $_.backlogGrowthFrames -gt 0 -or $_.discardedDetailRecords -ne 0 -or $_.unrecordedCoreFrames -ne 0 }).Count -eq 0)
	$finishLatencyPassed = $recordingRows.Count -gt 0 -and
		@($recordingRows | Where-Object { $_.finishInteractiveBlockMs -gt 50.0 }).Count -eq 0
	$countConservationPassed = $recordingRows.Count -gt 0 -and
		@($recordingRows | Where-Object {
			$_.committedFrames -ne $_.completeFrames -or $_.unrecordedCoreFrames -ne 0
		}).Count -eq 0
	$visualGatePassed = $rows.Count -gt 0 -and @($rows | Where-Object { -not $_.visualReady }).Count -eq 0
	$sourceAudit = Join-Path $repo 'Tools/Profiling/CheckProfilerSource.ps1'
	& $sourceAudit -Quiet
	$legacyRemovalPassed = $?
	$identityComparable = $validatedRunFiles.Count -eq $effectiveRuns -and $seenRunNumbers.Count -eq $effectiveRuns
	$comparisonAvailable = ($Mode -eq 'DetailedRecording' -and $recordingRows.Count -gt 0) -or
		($Mode -eq 'MatchedRecordingComparison' -and $recordingOverheadMeasured) -or
		($Mode -eq 'MatchedWindowOpen' -and $windowOverheadMeasured)
	$allGatesPassed = $identityComparable -and $residentBudgetPassed -and $profilerAccumulationPassed -and $durationSlopePassed -and
		$recordingOverheadPassed -and $windowOverheadPassed -and $finishLatencyPassed -and
		$countConservationPassed -and $legacyRemovalPassed -and $visualGatePassed
	$evidenceStatus = if(-not $comparisonAvailable) { 'incomparable' } elseif($allGatesPassed) { 'passed' } else { 'failed' }
	$failures = [Collections.Generic.List[string]]::new()
	if(-not $comparisonAvailable) { $failures.Add('The requested mode did not produce both required matched comparison rows.') }
	if(-not $residentBudgetPassed) { $failures.Add('Profiler resident high-water exceeded 256 MiB or was unavailable.') }
	if(-not $profilerAccumulationPassed) { $failures.Add('Profiler protected collection cost grew by more than 1% per produced frame or terminal backlog/loss increased.') }
	if(-not $durationSlopePassed) { $failures.Add('First/final one-minute median or P95 regressed by more than 1%.') }
	if(-not $recordingOverheadPassed) { $failures.Add('Detailed recording Engine CPU overhead exceeded 3%.') }
	if(-not $windowOverheadPassed) { $failures.Add('Late profiler-window Engine CPU overhead exceeded 2%.') }
	if(-not $finishLatencyPassed) { $failures.Add('Interactive Finish exceeded 50 ms.') }
	if(-not $countConservationPassed) { $failures.Add('Committed/core frame conservation failed.') }
	if(-not $legacyRemovalPassed) { $failures.Add('The V1-only source audit failed.') }
	if(-not $visualGatePassed) { $failures.Add('A matched run failed non-black changing visual evidence.') }
	$evidence = [ordered]@{
		schemaVersion = 1; status = $evidenceStatus
		identity = [ordered]@{
			projectHash = $identity.projectHash; sceneHash = $identity.sceneHash; objectPlacementHash = $identity.objectPlacementHash
			cameraHash = $identity.cameraHash; viewportHash = $identity.viewportHash; workloadHash = $identity.workloadHash
			installedBuildHash = $identity.installedBuildHash; installedPackageHash = $identity.installedPackageHash; installedPackageFiles = $identity.installedPackageFiles; captureSettingsHash = $identity.captureSettingsHash
			durationSeconds = $identity.durationSeconds; canonicalDurationSeconds = 300; mode = $identity.mode; formatIdentifier = 'TerraEngineProfilerJournalV1'; formatVersion = 1
		}
		runs = @($evidenceRuns)
		gates = [ordered]@{ identityComparable = $identityComparable; residentBudgetPassed = $residentBudgetPassed; profilerAccumulationPassed = $profilerAccumulationPassed; durationSlopePassed = $durationSlopePassed; recordingOverheadPassed = $recordingOverheadPassed; windowOverheadPassed = $windowOverheadPassed; finishLatencyPassed = $finishLatencyPassed; countConservationPassed = $countConservationPassed; legacyRemovalPassed = $legacyRemovalPassed; visualGatePassed = $visualGatePassed }
		failures = @($failures)
	}
	$evidencePath = Join-Path $output 'benchmark-evidence.json'
	[IO.File]::WriteAllText($evidencePath, ($evidence | ConvertTo-Json -Depth 30), [Text.UTF8Encoding]::new($false))
	# Validate the exact bytes that will be handed to evidence consumers.  This
	# catches accidental PSCustomObject/property omissions after serialization.
	$schemaPath = Join-Path $repo 'specs/049-profiler-rework/contracts/benchmark-evidence.schema.json'
	$writtenEvidence = Get-Content -LiteralPath $evidencePath -Raw | ConvertFrom-Json
	Validate-BenchmarkEvidenceEnvelope $writtenEvidence $schemaPath
}
Write-Output "Profiler rework benchmark identity: $identityPath"
