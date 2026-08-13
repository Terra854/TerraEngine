# RunAudioStress.ps1 - execute the installed native runner before publishing evidence.
[CmdletBinding()]
param(
	[int]$RequestedSources = 128,
	[int]$RepeatCount = 100,
	[string]$RunnerPath = '',
	[string]$Output = (Join-Path $PSScriptRoot 'Evidence\audio-stress-evidence.json')
)
$ErrorActionPreference = 'Stop'
if($RequestedSources -lt 1 -or $RepeatCount -lt 1) { throw 'Counts must be positive.' }
if(!$RunnerPath) { $RunnerPath = Join-Path $PSScriptRoot 'TerraEngineAudioStressRunner.exe' }
if(!(Test-Path $RunnerPath)) { throw "Runner not found: $RunnerPath" }

# Evidence publication is allowed to target a new nested directory. Resolve the
# parent explicitly and create only that directory before the atomic final write.
$outputParent = Split-Path $Output -Parent
if($outputParent -and !(Test-Path $outputParent)) {
	New-Item -ItemType Directory -Path $outputParent -Force | Out-Null
}

# An installed runner lives under Build/Tools/Audio while TerraEngine.dll lives
# under Build/Engine. Add that sibling only for this child invocation.
$originalPath = $env:PATH
$installRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$engineDirectory = Join-Path $installRoot 'Engine'
try {
	if(Test-Path $engineDirectory) { $env:PATH = "$engineDirectory;$env:PATH" }
	$raw = & $RunnerPath $RequestedSources $RepeatCount
	if($LASTEXITCODE -ne 0) { throw "Stress runner failed with exit code $LASTEXITCODE" }
} finally {
	$env:PATH = $originalPath
}
$result = $raw | ConvertFrom-Json
if(!$result.deterministic) { throw 'Runner did not report deterministic success.' }
if($result.maximumVoicesObserved -gt $result.voiceLimit) { throw 'Runner exceeded the configured voice limit.' }
if($result.medianFrameDeltaMs -gt 1.0) { throw 'Median audio preparation delta exceeded 1 ms.' }
if($result.p95FrameDeltaMs -gt 2.0) { throw 'P95 audio preparation delta exceeded 2 ms.' }
if($result.resourceGrowthPercent -gt 5.0) { throw 'Retained audio resources grew by more than 5%.' }
[ordered]@{
	schemaVersion = 1
	kind = 'audio-stress'
	requestedSources = $RequestedSources
	repeatCount = $RepeatCount
	hash = $result.hash
	voiceLimit = $result.voiceLimit
	maximumVoicesObserved = $result.maximumVoicesObserved
	medianFrameDeltaMs = $result.medianFrameDeltaMs
	p95FrameDeltaMs = $result.p95FrameDeltaMs
	baselineRetainedResources = $result.baselineRetainedResources
	finalRetainedResources = $result.finalRetainedResources
	resourceGrowthPercent = $result.resourceGrowthPercent
	deterministic = $true
	provider = 'provider-neutral-preparation'
	providerRevision = 'terraengine-audio-contract-v1'
	deviceConfiguration = 'device-independent'
	machine = [Environment]::MachineName
	operatingSystem = [Environment]::OSVersion.VersionString
	referenceScene = 'synthetic-128-source-reference'
	clipSet = 'synthetic-provider-neutral-metadata'
	baseline = 'audio-disabled-authored-identity-walk'
	schedulerDeclarations = @('AudioSnapshot', 'AudioPrepare', 'AudioCommit')
	generatedUtc = (Get-Date).ToUniversalTime().ToString('o')
} | ConvertTo-Json | Set-Content $Output -Encoding utf8
