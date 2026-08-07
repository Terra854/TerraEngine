# Measures installed-editor frame timing with the Profiler window closed and
# open-idle in alternating order. Each sample launches a clean editor process,
# opens the unchanged authored crowd scene without saving it, records unlimited
# profiler history, and retains the source summary used for the paired calculation.
[CmdletBinding()]
param(
	[string]$InstalledEditor = 'out/build/RelWithDebInfo/Build/Editor/TerraEngine-Editor.exe',
	[string]$Project = 'C:\Users\benti\Desktop\TerraEngine-Projects\New Animated Project',
	[string]$OutputDirectory = 'Tests/Fixtures/RendererPerformanceBaseline/results/profiler',
	[ValidateRange(1, 20)]
	[int]$PairCount = 5,
	[ValidateRange(1, 300)]
	[int]$CaptureDurationSeconds = 60,
	[int]$McpPort = 43129,
	[switch]$ReuseExistingRuns
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

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

$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$captureScript = Join-Path $repo 'Tools/VirtualGeometry/CaptureVirtualGeometryLiveVisual.ps1'
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

$measurements = [Collections.Generic.List[object]]::new()
for($pair = 1; $pair -le $PairCount; ++$pair)
{
	# AB/BA alternation reduces order, temperature, and residency bias.
	$order = if(($pair % 2) -eq 1) { @('closed', 'open-idle') } else { @('open-idle', 'closed') }
	foreach($state in $order)
	{
		$runDirectory = Join-Path $outputRoot ("pair-{0:D2}-{1}" -f $pair, $state)
		$summaryPath = Join-Path $runDirectory 'live-visual-summary.json'
		if(-not $ReuseExistingRuns -or -not (Test-Path -LiteralPath $summaryPath -PathType Leaf))
		{
			$arguments = @{
			InstalledEditor = $InstalledEditor
			Project = $Project
			Asset = 'Assets/ValidationModels/DenseAnimatedVG/CesiumMan.glb'
			Prefab = 'Assets/ValidationModels/DenseAnimatedVG/CesiumMan_Generated/CesiumMan.teprefab'
			# Scene.tescene is the project's recorded startup scene. Avoid a redundant
			# scene.open mutation; this runner verifies its bytes after every pair.
			OutputDirectory = $runDirectory
			ValidationCameraPosition = @(0.0, 0.5, 1.0)
			ValidationCameraTarget = @(0.0, 0.5, 0.0)
			SoakDurationSeconds = 1
			SoakSampleIntervalSeconds = 1
			McpPort = $McpPort
			ValidationRoute = 'hardware'
			RepresentationRoute = 'virtual-hardware'
			DiagnosticsViewType = 'Game'
			PrefabRequestedAnimationMode = 1
			ProfilerMaxFrames = 0
			# Capture coherence is verified separately from the idle-window phase.
			# Keep this per-run artifact short; the timing gate below samples frame
			# deltas only while the profiler state is idle.
			ProfilerCaptureDurationSeconds = 1
			ProfilerIdleMeasurementDurationSeconds = $CaptureDurationSeconds
			ProfilerIdleMeasurementIntervalMilliseconds = 100
			EnterPlayMode = $true
			ExpectAnimatedChange = $true
			}
			if($state -eq 'open-idle') { $arguments.ProfilerIdleMeasurementWindowVisible = $true }
			& $captureScript @arguments
			# Invoking another PowerShell script does not reliably populate
			# $LASTEXITCODE. The automatic success flag is the authoritative status
			# for this script-to-script boundary and remains compatible with strict
			# mode when the child succeeds without launching a native executable.
			if(-not $?) { throw "Profiler pair $pair/$state failed." }
		}
		$summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
		$timing = $summary.profilerIdleEvidence
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
	$deltaPercent = if($closed.medianMs -gt 0.0) {
		(($open.medianMs - $closed.medianMs) / $closed.medianMs) * 100.0
	} else { 0.0 }
	$pairs.Add([ordered]@{
		pair = $pair
		closedMedianMs = $closed.medianMs
		openIdleMedianMs = $open.medianMs
		deltaPercent = $deltaPercent
		passed = [Math]::Abs($deltaPercent) -le 2.0
	})
}

# Compare the median editor frame time for the two conditions. Retain each pair
# and its local delta for outlier diagnosis, but decide the gate from the
# medians across every AB/BA pair so one cold-launch sample cannot replace the
# aggregate with an unintended "every launch must pass" rule.
$aggregateClosedMedianMs = Get-Median @($pairs | ForEach-Object { [double]$_.closedMedianMs })
$aggregateOpenIdleMedianMs = Get-Median @($pairs | ForEach-Object { [double]$_.openIdleMedianMs })
$aggregateDeltaPercent = if($aggregateClosedMedianMs -gt 0.0) {
	(($aggregateOpenIdleMedianMs - $aggregateClosedMedianMs) / $aggregateClosedMedianMs) * 100.0
} else { 0.0 }
$aggregatePassed = [Math]::Abs($aggregateDeltaPercent) -le 2.0

$manifest = [ordered]@{
	schemaVersion = 2
	capturedAt = [DateTime]::UtcNow.ToString('o')
	method = 'installed-editor-ab-ba-unlimited-history'
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
		criterion = 'median-of-paired-condition-medians'
		closedMedianMs = $aggregateClosedMedianMs
		openIdleMedianMs = $aggregateOpenIdleMedianMs
		deltaPercent = $aggregateDeltaPercent
		passed = $aggregatePassed
	}
	passed = $aggregatePassed
}
$manifestPath = Join-Path $outputRoot 'paired-idle-overhead.json'
[IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
Write-Output "Profiler idle-overhead evidence: $manifestPath"
