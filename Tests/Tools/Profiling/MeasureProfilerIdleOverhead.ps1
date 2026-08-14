# Measures installed-editor frame timing with the Profiler window closed and
# open-idle in alternating order. Each pair shares one clean editor process so
# startup, asset residency, and shader warmup cannot be mistaken for profiler
# overhead. The authored crowd scene is opened without saving it.
[CmdletBinding()]
param(
	[string]$InstalledEditor = 'out/build/RelWithDebInfo/Build/Editor/TerraEngine-Editor.exe',
	[string]$Project = 'C:\Users\benti\Desktop\TerraEngine-Projects\New Animated Project',
	[string]$OutputDirectory = 'Tests/Fixtures/RendererPerformanceBaseline/results/profiler',
	[ValidateRange(1, 20)]
	[int]$PairCount = 5,
	[ValidateRange(1, 300)]
	[int]$CaptureDurationSeconds = 30,
	[int]$McpPort = 43155,
	[switch]$ReuseExistingRuns
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if($PairCount -eq 5 -and $CaptureDurationSeconds -gt 30)
{
	throw 'The canonical five-pair open-idle run uses 30 seconds per condition (300 seconds total).'
}

function Get-Median([double[]]$Values)
{
	if($null -eq $Values -or $Values.Count -eq 0)
	{
		throw 'A median requires at least one measured value.'
	}
	$ordered = @($Values | Sort-Object)
	$middle = [int][Math]::Floor($ordered.Count / 2)
	if(($ordered.Count % 2) -eq 1)
	{
		return [double]$ordered[$middle]
	}
	return ([double]$ordered[$middle - 1] + [double]$ordered[$middle]) / 2.0
}

$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
$captureScript = Join-Path $repo 'Tests/Tools/VirtualGeometry/CaptureVirtualGeometryLiveVisual.ps1'
$outputRoot = if([IO.Path]::IsPathRooted($OutputDirectory)) {
	[IO.Path]::GetFullPath($OutputDirectory)
} else {
	[IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
}
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$projectRoot = [IO.Path]::GetFullPath($Project)
$crowdScenePath = Join-Path $projectRoot 'Assets/Scenes/Scene.tescene'
if(-not (Test-Path -LiteralPath $crowdScenePath -PathType Leaf)) {
	throw "The profiler crowd scene does not exist: $crowdScenePath"
}
$crowdSceneHashBefore = (Get-FileHash -LiteralPath $crowdScenePath -Algorithm SHA256).Hash.ToLowerInvariant()
$projectIdentityPath = Join-Path $projectRoot 'game.json'
if(-not (Test-Path -LiteralPath $projectIdentityPath -PathType Leaf)) {
	throw "Profiler overhead identity requires project game.json: $projectIdentityPath"
}
$editorHash = if(Test-Path -LiteralPath $InstalledEditor -PathType Leaf) {
	(Get-FileHash -LiteralPath $InstalledEditor -Algorithm SHA256).Hash.ToLowerInvariant()
} else { 'unavailable' }
$sourceCommitValue = (& git -C $repo rev-parse HEAD 2>$null | Select-Object -First 1)
$sourceCommit = if($null -eq $sourceCommitValue) { '' } else { ([string]$sourceCommitValue).Trim() }
$initialEditors = @(Get-Process -Name 'TerraEngine-Editor' -ErrorAction SilentlyContinue)
if($initialEditors.Count -ne 0) {
	throw 'Profiler idle-overhead measurement starts and owns exactly one editor per pair.'
}

$measurements = [Collections.Generic.List[object]]::new()
$runDirectory = Join-Path $outputRoot 'paired-session'
$summaryPath = Join-Path $runDirectory 'live-visual-summary.json'
$identityManifestPath = Join-Path $outputRoot 'paired-idle-overhead.json'
if($ReuseExistingRuns -and (Test-Path -LiteralPath $identityManifestPath -PathType Leaf)) {
	$existingIdentity = Get-Content -LiteralPath $identityManifestPath -Raw | ConvertFrom-Json
	$identityMismatch = $null -eq $existingIdentity.identity -or
		[string]$existingIdentity.captureDurationSeconds -ne [string]$CaptureDurationSeconds -or
		[string]$existingIdentity.pairCount -ne [string]$PairCount -or
		[string]$existingIdentity.identity.projectHash -ne [string](Get-FileHash -LiteralPath $projectIdentityPath -Algorithm SHA256).Hash.ToLowerInvariant() -or
		[string]$existingIdentity.identity.sceneHash -ne [string]$crowdSceneHashBefore -or
		[string]$existingIdentity.identity.installedEditorHash -ne [string]$editorHash
	if($identityMismatch) { throw 'ReuseExistingRuns refused: paired profiler identity does not match the requested project, scene, build, pair count, or duration.' }
}
if(-not $ReuseExistingRuns -or -not (Test-Path -LiteralPath $summaryPath -PathType Leaf))
{
	$arguments = @{
		InstalledEditor = $InstalledEditor
		Project = $Project
		# Measure the same immutable authored crowd used by every profiler-rework
		# benchmark. No validation prefab is added, so object placement remains an
		# exact comparison input instead of a hidden per-tool workload variation.
		ScenePath = 'Assets/Scenes/Scene.tescene'
		PrefabInstanceCount = 0
		OutputDirectory = $runDirectory
		ValidationCameraPosition = @(0.0, 1.0, 9.0)
		ValidationCameraTarget = @(0.0, 1.0, 8.0)
		SoakDurationSeconds = 1
		SoakSampleIntervalSeconds = 1
		McpPort = $McpPort
		ValidationRoute = 'hardware'
		RepresentationRoute = 'virtual-hardware'
		DiagnosticsViewType = 'Game'
		ProfilerMaxFrames = 0
		ProfilerCaptureDurationSeconds = 1
		SkipProfilerCapture = $true
		# All five AB/BA pairs run in one Play Mode session. This isolates window
		# visibility from project reload, asset residency, and listener restarts.
		ProfilerIdlePairedMeasurement = $true
		ProfilerIdlePairCount = $PairCount
		ProfilerIdleMeasurementDurationSeconds = $CaptureDurationSeconds
		ProfilerIdleMeasurementIntervalMilliseconds = 100
		EnterPlayMode = $true
		ExpectAnimatedChange = $true
	}
	& $captureScript @arguments
	if(-not $?) { throw 'Profiler paired session failed.' }
}
$summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
$pairedTiming = $summary.profilerIdleEvidence
if($pairedTiming.status -ne 'measured' -or [int]$pairedTiming.pairCount -ne $PairCount) {
	throw 'Profiler paired session did not publish every requested condition pair.'
}
foreach($pairEvidence in @($pairedTiming.pairs))
{
	$pair = [int]$pairEvidence.pair
	$order = @($pairEvidence.order)
	foreach($state in $order)
	{
		$timing = if($state -eq 'open-idle') {
			$pairEvidence.conditions.openIdle
		} else { $pairEvidence.conditions.closed }
		if($timing.status -ne 'measured' -or [uint64]$timing.sampleCount -eq 0) {
			throw "Profiler pair $pair/$state did not publish a measured idle sample set."
		}
		$measurements.Add([ordered]@{
			pair = $pair
			state = $state
			frameCount = [uint64]$timing.sampleCount
			medianMs = [double]$timing.medianMs
			p95Ms = [double]$timing.p95Ms
			p99Ms = [double]$timing.p99Ms
			worstMs = [double]$timing.maximumMs
			summary = $summaryPath
			sha256 = (Get-FileHash -LiteralPath $summaryPath -Algorithm SHA256).Hash.ToLowerInvariant()
		})
	}
}

$crowdSceneHashAfter = (Get-FileHash -LiteralPath $crowdScenePath -Algorithm SHA256).Hash.ToLowerInvariant()
if($crowdSceneHashAfter -ne $crowdSceneHashBefore) {
	throw 'Profiler idle-overhead capture changed the authored crowd scene.'
}

$pairs = [Collections.Generic.List[object]]::new()
for($pair = 1; $pair -le $PairCount; ++$pair)
{
	$closed = @($measurements | Where-Object { $_.pair -eq $pair -and $_.state -eq 'closed' })[0]
	$open = @($measurements | Where-Object { $_.pair -eq $pair -and $_.state -eq 'open-idle' })[0]
	$pairEvidenceForPair = @($pairedTiming.pairs | Where-Object { [int]$_.pair -eq $pair })[0]
	if($null -eq $pairEvidenceForPair) {
		throw "Profiler paired session is missing block evidence for pair $pair."
	}
	$deltaPercent = if($closed.medianMs -gt 0.0) {
		(($open.medianMs - $closed.medianMs) / $closed.medianMs) * 100.0
	} else { 0.0 }
	$p95DeltaPercent = if($closed.p95Ms -gt 0.0) {
		(($open.p95Ms - $closed.p95Ms) / $closed.p95Ms) * 100.0
	} else { 0.0 }
	# Prefer the live harness's paired-block deltas. These compare adjacent
	# closed/open blocks and cancel workload drift before the pair aggregate.
	if($null -ne $pairEvidenceForPair.medianDeltaPercent) { $deltaPercent = [double]$pairEvidenceForPair.medianDeltaPercent }
	if($null -ne $pairEvidenceForPair.p95DeltaPercent) { $p95DeltaPercent = [double]$pairEvidenceForPair.p95DeltaPercent }
	$pairs.Add([ordered]@{
		pair = $pair
		pairedBlockCount = @($pairEvidenceForPair.blockDeltas).Count
		closedMedianMs = $closed.medianMs
		openIdleMedianMs = $open.medianMs
		medianDeltaPercent = $deltaPercent
		closedP95Ms = $closed.p95Ms
		openIdleP95Ms = $open.p95Ms
		p95DeltaPercent = $p95DeltaPercent
		# Opening faster is an improvement, so only positive overhead is gated.
		passed = $deltaPercent -le 2.0 -and
			$p95DeltaPercent -le 2.0
	})
}

# Retain the raw condition medians and p95s for diagnosis, but evaluate the
# matched AB/BA experiment from the median of its paired percentage deltas. A
# ratio of independently aggregated condition values can pair samples from
# different workload phases and report a direction that no median pair saw.
$aggregateClosedMedianMs = Get-Median @($pairs | ForEach-Object { [double]$_.closedMedianMs })
$aggregateOpenIdleMedianMs = Get-Median @($pairs | ForEach-Object { [double]$_.openIdleMedianMs })
$aggregateDeltaPercent = Get-Median @($pairs | ForEach-Object {
	[double]$_.medianDeltaPercent
})
$aggregateClosedP95Ms = Get-Median @($pairs | ForEach-Object { [double]$_.closedP95Ms })
$aggregateOpenIdleP95Ms = Get-Median @($pairs | ForEach-Object { [double]$_.openIdleP95Ms })
$aggregateP95DeltaPercent = Get-Median @($pairs | ForEach-Object {
	[double]$_.p95DeltaPercent
})
# Both central tendency and the high-percentile responsiveness gate must pass.
# This prevents a low median from hiding regular profiler-window stalls.
# Keep signed aggregate deltas: negative values are faster-open improvements.
$aggregatePassed = $aggregateDeltaPercent -le 2.0 -and
	$aggregateP95DeltaPercent -le 2.0

$manifest = [ordered]@{
	schemaVersion = 2
	capturedAt = [DateTime]::UtcNow.ToString('o')
	method = 'installed-editor-same-process-paired-ab-ba-unlimited-history'
	identity = [ordered]@{
		projectHash = (Get-FileHash -LiteralPath $projectIdentityPath -Algorithm SHA256).Hash.ToLowerInvariant()
		sceneHash = $crowdSceneHashBefore
		installedEditorHash = $editorHash
		buildHash = if([string]::IsNullOrWhiteSpace($sourceCommit)) { 'unavailable' } else { $sourceCommit }
		camera = [ordered]@{ position = @(0.0, 1.0, 9.0); target = @(0.0, 1.0, 8.0); verticalFovDegrees = 60 }
		viewport = [ordered]@{ diagnosticsView = 'Game'; captureWidth = 480; captureHeight = 270 }
		workload = 'authored-scene-exact-placement'
		captureSettings = [ordered]@{ profilerState = 'idle'; sampleIntervalMilliseconds = 100 }
		durationSeconds = $CaptureDurationSeconds
	}
	pairCount = $PairCount
	captureDurationSeconds = $CaptureDurationSeconds
	thresholdPercent = 2.0
	authoredScene = [ordered]@{
		path = $crowdScenePath
		sha256Before = $crowdSceneHashBefore
		sha256After = $crowdSceneHashAfter
		unchanged = $true
	}
	measurements = $measurements
	pairs = $pairs
	aggregate = [ordered]@{
		criterion = 'median-of-paired-median-and-p95-deltas'
		closedMedianMs = $aggregateClosedMedianMs
		openIdleMedianMs = $aggregateOpenIdleMedianMs
		medianDeltaPercent = $aggregateDeltaPercent
		closedP95Ms = $aggregateClosedP95Ms
		openIdleP95Ms = $aggregateOpenIdleP95Ms
		p95DeltaPercent = $aggregateP95DeltaPercent
		passed = $aggregatePassed
	}
	passed = $aggregatePassed
}
$manifestPath = Join-Path $outputRoot 'paired-idle-overhead.json'
[IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
Write-Output "Profiler idle-overhead evidence: $manifestPath"
