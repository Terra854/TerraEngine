# RunAudioSoak.ps1 - repeatedly execute the deterministic workload for a real duration.
[CmdletBinding()]
param(
	[int]$DurationMinutes = 30,
	[int]$RequestedSources = 128,
	[int]$RepeatCount = 100,
	[string]$RunnerPath = '',
	[string]$Output = (Join-Path $PSScriptRoot 'Evidence\audio-soak-evidence.json')
)
$ErrorActionPreference = 'Stop'
if($DurationMinutes -lt 1) { throw 'Duration must be positive.' }
if(!$RunnerPath) { $RunnerPath = Join-Path $PSScriptRoot 'TerraEngineAudioStressRunner.exe' }
if(!(Test-Path $RunnerPath)) { throw "Runner not found: $RunnerPath" }

# A clean install has no Evidence directory until its first run. Create only the
# requested output parent so publishing remains predictable for custom paths.
$outputParent = Split-Path $Output -Parent
if($outputParent -and !(Test-Path $outputParent)) {
	New-Item -ItemType Directory -Path $outputParent -Force | Out-Null
}

$originalPath = $env:PATH
$installRoot = Split-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) -Parent
$engineDirectory = Join-Path $installRoot 'Engine'
$stop = (Get-Date).AddMinutes($DurationMinutes)
$iterations = 0
$maximumMedianDeltaMs = 0.0
$maximumP95DeltaMs = 0.0
$maximumRetainedResources = 0
$voiceLimit = 0
$maximumVoicesObserved = 0
$lastHash = ''
$clock = [Diagnostics.Stopwatch]::StartNew()
try {
	if(Test-Path $engineDirectory) { $env:PATH = "$engineDirectory;$env:PATH" }
	while((Get-Date) -lt $stop) {
		$raw = & $RunnerPath $RequestedSources $RepeatCount
		if($LASTEXITCODE -ne 0) { throw "Soak child failed with exit code $LASTEXITCODE" }
		$result = $raw | ConvertFrom-Json
		if(!$result.deterministic) { throw 'Soak child reported a determinism failure.' }
		if($lastHash -and [string]$result.hash -ne $lastHash) { throw 'Soak child hash changed between iterations.' }
		if($result.maximumVoicesObserved -gt $result.voiceLimit) { throw 'Soak child exceeded the voice limit.' }
		if($result.resourceGrowthPercent -gt 5.0) { throw 'Soak child retained-resource growth exceeded 5%.' }
		if($result.medianFrameDeltaMs -gt 1.0 -or $result.p95FrameDeltaMs -gt 2.0) { throw 'Soak child exceeded the frame-time acceptance limits.' }
		$lastHash = [string]$result.hash
		$voiceLimit = $result.voiceLimit
		$maximumVoicesObserved = [math]::Max($maximumVoicesObserved, $result.maximumVoicesObserved)
		$maximumMedianDeltaMs = [math]::Max($maximumMedianDeltaMs, $result.medianFrameDeltaMs)
		$maximumP95DeltaMs = [math]::Max($maximumP95DeltaMs, $result.p95FrameDeltaMs)
		$maximumRetainedResources = [math]::Max($maximumRetainedResources, $result.finalRetainedResources)
		$iterations++
	}
} finally {
	$clock.Stop()
	$env:PATH = $originalPath
}
[ordered]@{
	schemaVersion = 1
	kind = 'audio-soak'
	durationMinutes = $DurationMinutes
	actualElapsedSeconds = [math]::Round($clock.Elapsed.TotalSeconds, 3)
	iterations = $iterations
	requestedSources = $RequestedSources
	repeatCount = $RepeatCount
	hash = $lastHash
	deterministic = $true
	voiceLimit = $voiceLimit
	maximumVoicesObserved = $maximumVoicesObserved
	maximumMedianFrameDeltaMs = $maximumMedianDeltaMs
	maximumP95FrameDeltaMs = $maximumP95DeltaMs
	baselineRetainedResources = 0
	finalRetainedResources = $maximumRetainedResources
	resourceGrowthPercent = $(if($maximumRetainedResources -eq 0) { 0.0 } else { 100.0 })
	provider = 'provider-neutral-preparation'
	providerRevision = 'terraengine-audio-contract-v1'
	deviceConfiguration = 'device-independent'
	machine = [Environment]::MachineName
	operatingSystem = [Environment]::OSVersion.VersionString
	referenceScene = 'synthetic-128-source-churn-reference'
	clipSet = 'synthetic-provider-neutral-metadata'
	baseline = 'zero-retained-packet-resources'
	schedulerDeclarations = @('AudioSnapshot', 'AudioPrepare', 'AudioCommit')
	generatedUtc = (Get-Date).ToUniversalTime().ToString('o')
} | ConvertTo-Json | Set-Content $Output -Encoding utf8
