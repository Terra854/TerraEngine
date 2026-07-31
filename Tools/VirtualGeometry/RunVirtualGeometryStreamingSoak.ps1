[CmdletBinding()]
param(
	[string]$Runner = 'out/build/RelWithDebInfo/Build/Tools/TerraEngineVirtualGeometryBenchmark.exe',
	[string]$RuntimeAdapter = '',
	[string]$Output = 'out/evidence/045/streaming-soak.json',
	[string]$Workloads = 'Tools/VirtualGeometry/workloads.json',
	[string]$Matrix = 'Tools/VirtualGeometry/hardware-matrix.json',
	[string]$Quality = 'Tools/VirtualGeometry/quality-profiles.json',
	[int]$DurationMinutes = 30,
	[int]$DurationSecondsOverride = 0,
	[int]$SampleIntervalMilliseconds = 1000,
	[switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if($DurationMinutes -lt 1)
{
	throw 'DurationMinutes must be positive.'
}
if($DurationSecondsOverride -lt 0)
{
	throw 'DurationSecondsOverride cannot be negative.'
}
if($SampleIntervalMilliseconds -lt 1)
{
	throw 'SampleIntervalMilliseconds must be positive.'
}
foreach($manifest in @($Workloads, $Matrix, $Quality))
{
	if(-not (Test-Path -LiteralPath $manifest -PathType Leaf))
	{
		throw "Manifest missing: $manifest"
	}
}

# A seconds override is intentionally marked as a local test run below. It
# exercises adapter and evidence wiring quickly, but cannot satisfy the
# required 30-minute streaming gate.
$requiredDurationSeconds = $DurationMinutes * 60
$runtimeDurationSeconds = if($DurationSecondsOverride -gt 0)
{
	$DurationSecondsOverride
}
else
{
	$requiredDurationSeconds
}
$isShortTestRun = $DurationSecondsOverride -gt 0 -or $DurationMinutes -lt 30

if($DryRun)
{
	Write-Output "validated runtime-backed streaming soak duration=$runtimeDurationSeconds seconds testOverride=$isShortTestRun (dry run; no residency claim)"
	exit 0
}
if(-not (Test-Path -LiteralPath $Runner -PathType Leaf))
{
	throw "Soak runner unavailable: $Runner"
}
if([string]::IsNullOrWhiteSpace($RuntimeAdapter) -or -not (Test-Path -LiteralPath $RuntimeAdapter -PathType Leaf))
{
	throw 'A runnable -RuntimeAdapter is required. It must publish measured bounded Vulkan capture JSON for every requested sample.'
}

$parent = Split-Path -Parent $Output
if(-not [string]::IsNullOrWhiteSpace($parent))
{
	New-Item -ItemType Directory -Force -Path $parent | Out-Null
}
$raw = "$Output.raw.json"
$started = [DateTime]::UtcNow

# The benchmark owns the timing loop and invokes the installed adapter for each
# sample. This wrapper measures its wall time independently before deciding
# whether the requested soak duration was actually met.
& $Runner `
	'--workloads' $Workloads `
	'--matrix' $Matrix `
	'--quality' $Quality `
	'--workload' 'page-residency-pressure' `
	'--runtime-adapter' $RuntimeAdapter `
	'--duration-seconds' $runtimeDurationSeconds `
	'--sample-interval-milliseconds' $SampleIntervalMilliseconds `
	'--output' $raw
$runnerExitCode = $LASTEXITCODE
$elapsedSeconds = ([DateTime]::UtcNow - $started).TotalSeconds
if(-not (Test-Path -LiteralPath $raw -PathType Leaf))
{
	throw 'Streaming soak runner did not publish raw evidence.'
}

$rawJson = Get-Content -Raw -LiteralPath $raw | ConvertFrom-Json
$samples = @($rawJson.rawSamples | Where-Object {
	$_.availability -eq 'measured' -and
	-not [string]::IsNullOrWhiteSpace([string]$_.executionId) -and
	$_.durationNanoseconds -gt 0
})
$boundedCounters = $false
if($null -ne $rawJson.statistics -and $null -ne $rawJson.statistics.PSObject.Properties['boundedCounters'])
{
	$boundedCounters = [bool]$rawJson.statistics.boundedCounters
}
$runtimeSampling = $rawJson.PSObject.Properties['runtimeSampling']
$runtimeDurationSatisfied = $null -ne $runtimeSampling -and
	[double]$runtimeSampling.Value.actualDurationSeconds -ge $runtimeDurationSeconds
$gateDurationSatisfied = -not $isShortTestRun -and
	$elapsedSeconds -ge $requiredDurationSeconds -and $runtimeDurationSatisfied
$complete = $runnerExitCode -eq 0 -and $gateDurationSatisfied -and
	$samples.Count -gt 0 -and $boundedCounters

$result = [ordered]@{
	schemaVersion = 1
	status = if($complete){'measured-result-published'}else{'incomplete'}
	startedUtc = $started.ToString('o')
	elapsedSeconds = $elapsedSeconds
	requiredDurationMinutes = $DurationMinutes
	runtimeDurationSeconds = $runtimeDurationSeconds
	testDurationOverrideSeconds = $DurationSecondsOverride
	testRun = $isShortTestRun
	runner = $Runner
	runtimeAdapter = $RuntimeAdapter
	runnerExitCode = $runnerExitCode
	measuredSampleCount = $samples.Count
	boundedCounters = $boundedCounters
	runtimeDurationSatisfied = $runtimeDurationSatisfied
	gateDurationSatisfied = $gateDurationSatisfied
	rawResult = $raw
	rawSamples = @($rawJson.rawSamples)
	runnerHash = (Get-FileHash -LiteralPath $Runner -Algorithm SHA256).Hash.ToLowerInvariant()
	hardwareAvailability = 'runtime-adapter-reported'
	note = 'Only repeated measured Vulkan adapter captures with bounded counters and the full requested wall-clock duration satisfy this soak gate.'
}
$result | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $Output -Encoding utf8
if(-not $complete)
{
	exit 2
}
