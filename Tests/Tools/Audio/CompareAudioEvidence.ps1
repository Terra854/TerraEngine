# CompareAudioEvidence.ps1 - validate stress evidence against a committed floor.
[CmdletBinding()]
param(
	[string]$Evidence,
	[string]$Baseline = (Join-Path $PSScriptRoot '..\..\..\Tests\Fixtures\Audio\Performance\baseline.json')
)
$ErrorActionPreference = 'Stop'
if(!(Test-Path $Evidence)) { throw "Audio evidence was not found: $Evidence" }
if(!(Test-Path $Baseline)) { throw "Audio baseline was not found: $Baseline" }
$actual = Get-Content $Evidence -Raw | ConvertFrom-Json
$expected = Get-Content $Baseline -Raw | ConvertFrom-Json
if($actual.kind -ne 'audio-stress') { throw 'Only deterministic audio-stress evidence can be compared to this baseline.' }
if($actual.requestedSources -lt $expected.minimumSources) { throw 'Stress evidence did not exercise the required source count.' }
if($actual.repeatCount -lt $expected.minimumRepeatCount) { throw 'Stress evidence did not exercise the required replay count.' }
if([string]$actual.hash -ne [string]$expected.expectedHash) { throw 'Stress evidence hash differs from the committed deterministic baseline.' }
if($actual.maximumVoicesObserved -gt $actual.voiceLimit) { throw 'Stress evidence exceeded its configured voice limit.' }
if($actual.medianFrameDeltaMs -gt $expected.maximumMedianFrameDeltaMs) { throw 'Stress evidence exceeded the median frame-time budget.' }
if($actual.p95FrameDeltaMs -gt $expected.maximumP95FrameDeltaMs) { throw 'Stress evidence exceeded the p95 frame-time budget.' }
if($actual.resourceGrowthPercent -gt $expected.maximumResourceGrowthPercent) { throw 'Stress evidence exceeded the retained-resource growth budget.' }
Write-Output 'Audio stress evidence matches the committed deterministic baseline.'
