<#
.SYNOPSIS
Tests fail-closed installed audio evidence validation without launching an editor.
.DESCRIPTION
Prerequisites: PowerShell 7; no graphics/audio device required. Run:
pwsh -NoProfile -File Tests/Tools/AudioRendererSafety/TestAudioControlAcceptanceContracts.ps1
These deliberately malformed protocol records supplement actual native/managed
acceptance and cannot satisfy its callback, provider or allocation proof.
#>
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../Audio/AudioControlAcceptanceContract.ps1')
$script:checks=0
function Reject([string]$Name,[scriptblock]$Action) {
	$rejected=$false; try { & $Action } catch { $rejected=$true }
	if (!$rejected) { throw "Malformed protocol passed: $Name" }
	++$script:checks; Write-Output "[PASS] $Name"
}
function Clone($Value) { return ($Value | ConvertTo-Json -Depth 80 | ConvertFrom-Json) }
$expectedTimestamp='2026-10-15T17:05:25.0000000+00:00'
$decodedTimestamp=('{"builtAtUtc":"2026-10-15T17:05:25Z"}' | ConvertFrom-Json).builtAtUtc
foreach ($timestamp in @($decodedTimestamp,'2026-10-15T17:05:25Z',[DateTimeOffset]::Parse('2026-10-15T19:05:25+02:00'),[DateTime]::new(2026,10,15,17,5,25,[DateTimeKind]::Utc))) {
	if ((ConvertTo-AudioManifestTimestamp $timestamp) -ne $expectedTimestamp) { throw 'Typed/ISO manifest timestamp lost its instant.' }
}
Reject 'invalid manifest timestamp' { ConvertTo-AudioManifestTimestamp '2026-02-30T17:05:25Z' }
Reject 'missing manifest timestamp' { ConvertTo-AudioManifestTimestamp $null }
Reject 'culture-dependent manifest timestamp' { ConvertTo-AudioManifestTimestamp '10/15/2026 17:05:25' }
Reject 'offsetless manifest timestamp' { ConvertTo-AudioManifestTimestamp '2026-10-15T17:05:25' }
$root=Join-Path ([IO.Path]::GetTempPath()) ('TerraEngine-audio-protocol-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $root | Out-Null
# Preserve these small owned inputs on failure as ordinary diagnostic evidence.
$artifact=Join-Path $root 'artifact.bin'; [IO.File]::WriteAllBytes($artifact,[byte[]](1,2,3))
$entry=@{length=3;sha256=(Get-FileHash $artifact).Hash}; Assert-AudioArtifact $artifact $entry
Reject 'hash mismatch' { Assert-AudioArtifact $artifact @{length=3;sha256=('0'*64)} }
Reject 'missing artifact' { Assert-AudioArtifact (Join-Path $root 'missing.bin') $entry }
Reject 'reused WorkRoot' { Assert-AudioFreshWorkRoot $root }
Reject 'wrong endpoint' { Assert-AudioEndpoint @(@{OwningProcess=123}) 124 $root $root }
Reject 'missing endpoint' { Assert-AudioEndpoint @() 124 $root $root }
Reject 'wrong project' { Assert-AudioEndpoint @(@{OwningProcess=123}) 123 $root ($root+'-other') }
Reject 'absent callback' { Assert-AudioManagedEvidence $null }
$stageNames=@('autoplay-observed','autoplay-advanced','fresh-facade-pause','pause-observed','fresh-facade-resume','resume-advanced','fresh-facade-stop','stop-observed','pending-play','pending-pause','pending-play-pause-observed','other-producer-pause','other-producer-controlled','stale-facade-rejected','replacement-pause','replacement-controlled')
$managed=@{schemaVersion=1;passed=$true;failure='';instanceId='actual-instance';updates=40;positiveDeltaFrames=20;stages=@(foreach ($name in $stageNames) { @{name=$name;state=@{Audible=$true;PlaybackGeneration=7;LogicalFrame=0;State=1};command=@{ResolvedPlaybackGeneration=7;Status=1}} })}
($managed.stages | Where-Object name -eq 'autoplay-observed').state.LogicalFrame=1
($managed.stages | Where-Object name -eq 'autoplay-advanced').state.LogicalFrame=2
($managed.stages | Where-Object name -eq 'pause-observed').state.State=2
($managed.stages | Where-Object name -eq 'pause-observed').state.LogicalFrame=2
($managed.stages | Where-Object name -eq 'resume-advanced').state.LogicalFrame=3
($managed.stages | Where-Object name -eq 'stop-observed').state.State=0
($managed.stages | Where-Object name -eq 'stale-facade-rejected').command.Status=4
Assert-AudioManagedEvidence $managed
$bad=Clone $managed; $bad.positiveDeltaFrames=0
Reject 'zero-delta managed timeline' { Assert-AudioManagedEvidence $bad }
$bad=Clone $managed; ($bad.stages | Where-Object name -eq 'resume-advanced').state.LogicalFrame=2
Reject 'resume cursor did not advance' { Assert-AudioManagedEvidence $bad }
$bad=Clone $managed; ($bad.stages | Where-Object name -eq 'fresh-facade-pause').command.ResolvedPlaybackGeneration=8
Reject 'pause receipt bound another instance' { Assert-AudioManagedEvidence $bad }
$bad=Clone $managed; ($bad.stages | Where-Object name -eq 'stale-facade-rejected').command.Status=1
Reject 'old facade accepted replacement' { Assert-AudioManagedEvidence $bad }
Reject 'zero selected tests' { Assert-AudioSelection 0 12 }
Reject 'under-floor tests' { Assert-AudioSelection 11 12 }
Reject 'abnormal child exit' { Assert-AudioOrderlyExit @{timedOut=$false;exitCode=86;activeProcessesAfterRootExit=0} }
Reject 'watchdog cleanup' { Assert-AudioOrderlyExit @{timedOut=$true;exitCode=124;activeProcessesAfterRootExit=0} }
Reject 'leaked descendant' { Assert-AudioOrderlyExit @{timedOut=$false;exitCode=0;activeProcessesAfterRootExit=1} }
$continuity=@{passed=$true;runs=@(1,2,8 | ForEach-Object { @{producers=$_;requests=20000;applied=20000;pendingPeak=8;completedPeak=4096;evictions=15904;duplicateApplications=0;terminalOutcomes=20000} })}
Assert-AudioContinuityEvidence $continuity
$bad=Clone $continuity; $bad.runs[2].producers=1
Reject 'missing producer lane' { Assert-AudioContinuityEvidence $bad }
$bad=Clone $continuity; $bad.runs[0].duplicateApplications=1
Reject 'duplicate application' { Assert-AudioContinuityEvidence $bad }
$bad=Clone $continuity; $bad.runs[0].completedPeak=4097
Reject 'unbounded retention' { Assert-AudioContinuityEvidence $bad }
$bad=Clone $continuity; $bad.runs[0].pendingPeak=4097
Reject 'unbounded pending ownership' { Assert-AudioContinuityEvidence $bad }
$bad=Clone $continuity; $bad.runs[0].completedPeak=4095
Reject 'incomplete completed retention window' { Assert-AudioContinuityEvidence $bad }
$bad=Clone $continuity; $bad.runs[0].evictions=15903
Reject 'inconsistent eviction accounting' { Assert-AudioContinuityEvidence $bad }
$settings=@{passed=$true;frameCount=120;frames=@(1..120 | ForEach-Object { @{frame=$_;expectedVolume=0.25;actualVolume=0.25;expectedPitch=1.5;actualPitch=1.5} });rollbackVerified=$true;authoredOverrideVerified=$true;retirementVerified=$true;independentFieldsVerified=$true}
Assert-AudioSettingsEvidence $settings
$bad=Clone $settings; $bad.frameCount=119
Reject 'incomplete frame evidence' { Assert-AudioSettingsEvidence $bad }
$bad=Clone $settings; $bad.frameCount=121
Reject 'declared frame count differs from rows' { Assert-AudioSettingsEvidence $bad }
$bad=Clone $settings; $bad.frames[119].frame=119
Reject 'duplicate observed frame' { Assert-AudioSettingsEvidence $bad }
$bad=Clone $settings; $bad.frames[119].frame=121
Reject 'skipped observed frame' { Assert-AudioSettingsEvidence $bad }
$bad=Clone $settings; $bad.frames[0].frame=0
Reject 'nonpositive observed frame' { Assert-AudioSettingsEvidence $bad }
$bad=Clone $settings; $bad.frames[0].actualPitch=[double]::NaN
Reject 'nonfinite numeric value' { Assert-AudioSettingsEvidence $bad }
$bad=Clone $settings; $bad.frames[0].actualPitch='NaN'
Reject 'JSON string masquerading as numeric value' { Assert-AudioSettingsEvidence $bad }
$bad=Clone $settings; $bad.frames[0].expectedVolume=1.0; $bad.frames[0].actualVolume=1.0
Reject 'default target values' { Assert-AudioSettingsEvidence $bad }
$bad=Clone $settings; $bad.frames[119].actualVolume=1.0
Reject 'reverted source value' { Assert-AudioSettingsEvidence $bad }
$bad=Clone $settings; $bad.rollbackVerified=$false
Reject 'missing rollback' { Assert-AudioSettingsEvidence $bad }
$lanes=@('rejection','scratch','receipt-success','receipt-rejection','seal-null','seal-rejected','seal-valid','publish-success','publish-session','publish-stale')
$records=@(foreach ($name in $lanes) { foreach ($iteration in 0..99) { @{lane=$name;iteration=$iteration;attempts=0;failedAllocations=0;rootBefore=1;rootAfter=1;outcome=9;healthyFollowup=$true} } })
$outcomes=@{'rejection'=9;'scratch'=2;'receipt-success'=0;'receipt-rejection'=9;'seal-null'=4;'seal-rejected'=9;'seal-valid'=0;'publish-success'=0;'publish-session'=9;'publish-stale'=4}
foreach ($record in $records) { $record.outcome=$outcomes[$record.lane] }
foreach ($record in $records | Where-Object lane -eq 'scratch') { $record.attempts=1; $record.failedAllocations=1; $record.outcome=2 }
$allocation=@{normalExit=$true;repetitions=100;checkpoints=1000;lanes=$lanes;records=$records}
Assert-AudioAllocationEvidence $allocation 100
Reject 'under-floor allocation repetitions' { Assert-AudioAllocationEvidence $allocation 99 }
$bad=Clone $allocation; $bad.records[0].rootAfter=2
Reject 'rejected publication changed root' { Assert-AudioAllocationEvidence $bad 100 }
$bad=Clone $allocation; $bad.normalExit=$false
Reject 'terminated allocation child' { Assert-AudioAllocationEvidence $bad 100 }
$bad=Clone $allocation; foreach ($record in $bad.records) { $record.failedAllocations=0 }
Reject 'synthetic allocation failure' { Assert-AudioAllocationEvidence $bad 100 }
Write-Output "$script:checks audio acceptance protocol checks passed. Inputs: $root"
exit 0
