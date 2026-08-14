[CmdletBinding()]
param(
	[string]$Runner = 'out/build/RelWithDebInfo/Build/Tests/Tools/VirtualGeometry/TerraEngineVirtualGeometryBenchmark.exe',
	[string]$RuntimeAdapter = '',
	[string]$Output = 'out/evidence/virtual-geometry-production/production-soak.json',
	[string]$Workloads = 'Tests/Tools/VirtualGeometry/workloads.json',
	[string]$Matrix = 'Tests/Tools/VirtualGeometry/hardware-matrix.json',
	[string]$Quality = 'Tests/Tools/VirtualGeometry/quality-profiles.json',
	[int]$DurationMinutes = 30,
	[int]$DurationSecondsOverride = 0,
	[int]$SampleIntervalMilliseconds = 1000,
	[switch]$Overnight,
	[switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if($DurationMinutes -lt 30)
{
	throw 'Production soak requires at least 30 minutes.'
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

# This override is for local adapter smoke tests only. The result records it
# explicitly and remains incomplete because a short test is not production
# bounded-growth evidence.
$requiredDurationSeconds = $DurationMinutes * 60
$runtimeDurationSeconds = if($DurationSecondsOverride -gt 0)
{
	$DurationSecondsOverride
}
else
{
	$requiredDurationSeconds
}
$isShortTestRun = $DurationSecondsOverride -gt 0

if($DryRun)
{
	Write-Output "validated runtime-backed production soak duration=$runtimeDurationSeconds seconds overnight=$Overnight testOverride=$isShortTestRun (dry run; no measurement)"
	exit 0
}
if(-not (Test-Path -LiteralPath $Runner -PathType Leaf))
{
	throw "Production soak runner unavailable: $Runner"
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

# Keep the duration loop inside the native benchmark so every sample invokes
# the runtime adapter and preserves its own raw capture artifact.
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
	throw 'Production soak runner did not publish raw evidence.'
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
	overnight = [bool]$Overnight
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
	note = 'Only repeated measured Vulkan adapter captures with bounded counters and the full requested wall-clock duration satisfy this production gate.'
}
$result | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $Output -Encoding utf8
if(-not $complete)
{
	exit 2
}
