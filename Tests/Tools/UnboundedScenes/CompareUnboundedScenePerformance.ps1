[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string]$Baseline,
	[Parameter(Mandatory=$true)][string]$Candidate,
	[Parameter(Mandatory=$true)][string]$Output,
	[int]$MinimumRuns = 5
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
function Read-Evidence([string]$Path){ if(-not (Test-Path -LiteralPath $Path -PathType Leaf)){ throw "Evidence missing: $Path" }; return Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json }
function Median([double[]]$Values){ if($Values.Count -lt 1){ throw 'Measured values are required.' }; $sorted=@($Values|Sort-Object); return [double]$sorted[[Math]::Floor(($sorted.Count-1)/2)] }
function Has-Property($Value,[string]$Name){ return $null -ne $Value -and $null -ne $Value.PSObject.Properties[$Name] }
function Samples($Evidence){ if(Has-Property $Evidence 'rawSamples'){ return @($Evidence.rawSamples) }; $all=@(); foreach($scenario in $Evidence.scenarios.PSObject.Properties){ $all += @($scenario.Value.rawSamples) }; return $all }
$baselineEvidence=Read-Evidence $Baseline
$candidateEvidence=Read-Evidence $Candidate
if(-not (Has-Property $baselineEvidence 'status') -or -not (Has-Property $candidateEvidence 'status') -or $baselineEvidence.status -notin @('measured','comparable') -or $candidateEvidence.status -notin @('measured','comparable')){ throw 'Only finalized measured evidence can be compared.' }
$baselineIdentity=if(Has-Property $baselineEvidence 'identity'){$baselineEvidence.identity}else{@($baselineEvidence.scenarios.PSObject.Properties)[0].Value.identity}
$candidateIdentity=if(Has-Property $candidateEvidence 'identity'){$candidateEvidence.identity}else{@($candidateEvidence.scenarios.PSObject.Properties)[0].Value.identity}
foreach($field in @('workloadId','contentHash','cameraPathHash','hardwareIdentity','runtimeRevision')){ if([string]$baselineIdentity.$field -ne [string]$candidateIdentity.$field){ throw "Evidence identity mismatch: $field" } }
$baseValues=@(Samples $baselineEvidence); $candidateValues=@(Samples $candidateEvidence)
if($baseValues.Count -lt $MinimumRuns -or $candidateValues.Count -lt $MinimumRuns){ throw "At least $MinimumRuns measured runs per side are required." }
$baseTime=Median @($baseValues|ForEach-Object {[double]$_.frameTimeNanoseconds}); $candidateTime=Median @($candidateValues|ForEach-Object {[double]$_.frameTimeNanoseconds})
$baseMemory=Median @($baseValues|ForEach-Object {[double]$_.memoryBytes}); $candidateMemory=Median @($candidateValues|ForEach-Object {[double]$_.memoryBytes})
$timeRatio=$candidateTime/$baseTime; $memoryRatio=$candidateMemory/$baseMemory
$accepted=($timeRatio -le 1.05 -and $memoryRatio -le 1.02)
$result=[ordered]@{schemaVersion=2;status=if($accepted){'accepted'}else{'regression'};baselineRuns=$baseValues.Count;candidateRuns=$candidateValues.Count;medianTimingRatio=$timeRatio;medianResidentMemoryRatio=$memoryRatio;maximumMedianTimingRatio=1.05;maximumFixedResidentMemoryGrowthRatio=1.02;unavailableIsNotZero=$true}
$result|ConvertTo-Json -Depth 8|Set-Content -LiteralPath $Output -Encoding utf8
if(-not $accepted){exit 2}
