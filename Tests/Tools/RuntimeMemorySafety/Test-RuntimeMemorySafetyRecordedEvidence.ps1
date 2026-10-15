<#
Read-only replay of parent-owned historical receipts and tampered copies.
No process launch, build, GPU call, or new runtime acceptance occurs here.
Historical smoke lacks native image/frame counts and observed context teardown: replay can establish parser
compatibility only; the normal runner remains nonpassing until counts exist.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$WorkRoot,
	[Parameter(Mandatory)][string]$VulkanSmokeRoot,
	[Parameter(Mandatory)][string]$CacheReceiptPath,
	[Parameter(Mandatory)][string]$RedEvidenceRoot
)
Set-StrictMode -Version Latest;$ErrorActionPreference='Stop'
$replayRoot=[IO.Path]::GetFullPath($WorkRoot);if(Test-Path $replayRoot){throw 'Replay WorkRoot must be fresh'};$null=[IO.Directory]::CreateDirectory($replayRoot)
. (Join-Path $PSScriptRoot 'RunInstalledRuntimeMemorySafety.ps1')
$script:replayChecks=0;$script:sourcePins=New-Object 'Collections.Generic.List[object]'
function Check([string]$Name,[scriptblock]$Body){& $Body;$script:replayChecks++;Write-Output "[PASS] $Name"}
function Reject([string]$Name,[scriptblock]$Body,[string]$Pattern='*'){$message=$null;try{& $Body|Out-Null}catch{$message=$_.Exception.Message};if($null -eq $message -or $message -notlike $Pattern){throw "Expected rejection $Name ($Pattern), actual: $message"};$script:replayChecks++;Write-Output "[PASS] $Name rejected"}
function LoadPinned([string]$Path){$script:sourcePins.Add((RmsPin ([IO.Path]::GetFullPath($Path))));return Get-Content -LiteralPath $Path -Raw|ConvertFrom-Json}
$scenarios=@('fresh-acquisition','recreated-acquisition','previously-presented','acquire-wait-consumed','record-failure','fence-reset-failure','submit-failure','candidate-rollback','maintenance-enabled','maintenance-disabled')
$recorded=@{}
foreach($scenario in $scenarios){
	$r=LoadPinned (Join-Path $VulkanSmokeRoot "$scenario.json");$recorded[$scenario]=$r
	Check "recorded-vulkan-$scenario-compatible-bounds-unverified" {$null=RmsCheckFixture $r vulkan $scenario 1 -ProtocolReplayHistoricalReceipt}
}
Reject 'historical-smoke-cannot-pass-strict-acceptance' {RmsCheckFixture $recorded['fresh-acquisition'] vulkan fresh-acquisition 1} '*contextShutdownAttempted*'
# These explicitly synthetic additions exercise the final schema using a copy;
# they are never written into retained native receipts or counted as acceptance.
$strictCopy=$recorded['fresh-acquisition']|ConvertTo-Json -Depth 100|ConvertFrom-Json
$strictRow=$strictCopy.observations[0]
$strictRow|Add-Member contextShutdownAttempted $true
$strictRow|Add-Member contextInitializedAfterShutdown $false
foreach($native in @($strictRow.operation)+@($strictRow.healthyFrames|ForEach-Object {$_.native})){$native|Add-Member swapchainImageCount 1024;$native|Add-Member frameSlotCount 1024}
Check 'synthetic-final-teardown-observation-schema' {$null=RmsCheckFixture $strictCopy vulkan fresh-acquisition 1}
foreach($fault in @('attempt-missing','attempt-false','attempt-null','attempt-string','initialized-missing','initialized-null','initialized-true','initialized-string')){
	$copy=$strictCopy|ConvertTo-Json -Depth 100|ConvertFrom-Json;$row=$copy.observations[0]
	switch($fault){
		'attempt-missing'{$row.PSObject.Properties.Remove('contextShutdownAttempted')}
		'attempt-false'{$row.contextShutdownAttempted=$false}
		'attempt-null'{$row.contextShutdownAttempted=$null}
		'attempt-string'{$row.contextShutdownAttempted='true'}
		'initialized-missing'{$row.PSObject.Properties.Remove('contextInitializedAfterShutdown')}
		'initialized-null'{$row.contextInitializedAfterShutdown=$null}
		'initialized-true'{$row.contextInitializedAfterShutdown=$true}
		'initialized-string'{$row.contextInitializedAfterShutdown='false'}
	}
	Reject "strict-teardown-$fault" {RmsCheckFixture $copy vulkan fresh-acquisition 1} '*context*'
	if($fault -notin @('attempt-missing','initialized-missing')){Reject "historical-replay-rejects-supplied-teardown-$fault" {RmsCheckFixture $copy vulkan fresh-acquisition 1 -ProtocolReplayHistoricalReceipt} '*context*'}
}
$countlessCopy=$strictCopy|ConvertTo-Json -Depth 100|ConvertFrom-Json;$countlessCopy.observations[0].operation.PSObject.Properties.Remove('swapchainImageCount')
Reject 'final-teardown-still-requires-native-counts' {RmsCheckFixture $countlessCopy vulkan fresh-acquisition 1} '*UNVERIFIED: Actual acquired image/frame counts are missing*'
$faults=@('missing-validation','validation-error','missing-cleanup','missing-worker-drain','healthy-count','healthy-serial','healthy-ordinary-submit','healthy-completion','healthy-session','healthy-generation','image-negative','image-sentinel','slot-negative','operation-generation','uncalled-reset-result','uncalled-submit-result','uncalled-present-result','called-result-null','consumed-wait-missing','recreated-proof-missing','previous-image-unproven','maintenance-mismatch','candidate-cleanup-missing','candidate-checkpoint-missing','actual-image-count-range','actual-slot-count-range')
foreach($fault in $faults){
	$scenario=switch($fault){'uncalled-reset-result'{'record-failure'}'uncalled-submit-result'{'submit-failure'}'uncalled-present-result'{'record-failure'}'consumed-wait-missing'{'acquire-wait-consumed'}'recreated-proof-missing'{'recreated-acquisition'}'previous-image-unproven'{'previously-presented'}'maintenance-mismatch'{'maintenance-disabled'}'candidate-cleanup-missing'{'candidate-rollback'}'candidate-checkpoint-missing'{'candidate-rollback'}default{'fresh-acquisition'}}
	$copy=$recorded[$scenario]|ConvertTo-Json -Depth 100|ConvertFrom-Json;$row=$copy.observations[0]
	switch($fault){
		'missing-validation'{$row.PSObject.Properties.Remove('validationActuallyEnabled')}
		'validation-error'{$row.allSessionValidationErrors=1}
		'missing-cleanup'{$row.contextCleanupCompleted=$false}
		'missing-worker-drain'{$row.threadPoolCleanupCompleted=$false}
		'healthy-count'{$row.healthyFrames=@($row.healthyFrames|Select-Object -First 1)}
		'healthy-serial'{$row.healthyFrames[1].native.ordinarySubmissionSerial=$row.healthyFrames[0].native.ordinarySubmissionSerial}
		'healthy-ordinary-submit'{$row.healthyFrames[0].native.ordinarySubmitAccepted=$false}
		'healthy-completion'{$row.healthyFrames[0].native.ordinaryCompletionObserved=$false}
		'healthy-session'{$row.healthyFrames[0].rendererSessionGeneration++}
		'healthy-generation'{$row.healthyFrames[1].native.generation++}
		'image-negative'{$row.operation.imageIndex=-1}
		'image-sentinel'{$row.operation.imageIndex=4294967295}
		'slot-negative'{$row.operation.frameSlot=-1}
		'operation-generation'{$row.operation.generation=0}
		'uncalled-reset-result'{$row.operation.resetResult=0}
		'uncalled-submit-result'{$row.operation.submitResult=0}
		'uncalled-present-result'{$row.operation.presentResult=0}
		'called-result-null'{$row.operation.submitResult=$null}
		'consumed-wait-missing'{$row.operation.acquireWaitConsumedBefore=$false}
		'recreated-proof-missing'{$row.recreatedBeforeFirstAcquisition=$false}
		'previous-image-unproven'{$row.preludeFrames=@()}
		'maintenance-mismatch'{$row.maintenanceEnabled=$true;$row.operation.maintenanceEnabled=$true}
		'candidate-cleanup-missing'{$row.candidateSubcases[1].native.candidatePartialCleanupCompleted=$false}
		'candidate-checkpoint-missing'{$row.candidateSubcases=@($row.candidateSubcases|Select-Object -First 1)}
		'actual-image-count-range'{$row.operation|Add-Member swapchainImageCount 1;$row.operation.imageIndex=1}
		'actual-slot-count-range'{$row.operation|Add-Member frameSlotCount 1;$row.operation.frameSlot=1}
	}
	$path=Join-Path $replayRoot "tampered-$fault.json";RmsWrite $path $copy
	Reject "tampered-vulkan-$fault" {RmsCheckFixture (Get-Content $path -Raw|ConvertFrom-Json) vulkan $scenario 1 -ProtocolReplayHistoricalReceipt}
}

$cache=LoadPinned $CacheReceiptPath
Check 'recorded-cache-200-actual-logs-and-watchdogs' {
	RmsRequire ($cache.status -ceq 'Passed' -and $cache.completedCount -eq 100 -and $cache.neverDispatchedCount -eq 100 -and @($cache.runs).Count -eq 200) 'Recorded cache matrix incomplete'
	$counts=@{};$directories=@{}
	foreach($run in $cache.runs){
		RmsRequire (!$directories.ContainsKey($run.directory)) 'Repeated cache evidence directory';$directories[$run.directory]=$true
		$stdoutPath=Join-Path $run.directory 'stdout.log';$watchPath=Join-Path $run.directory 'watchdog.json';$pin=RmsPin $stdoutPath;$script:sourcePins.Add($pin)
		RmsRequire ($pin.sha256 -ceq $run.stdoutSha256.ToLowerInvariant()) 'Recorded cache stdout hash changed'
		$watch=LoadPinned $watchPath;$name=$run.facts.caseId
		RmsRequire ($name -cin @('Engine.RuntimeMemorySafety.TaskCache.CompletedNestedCapture','Engine.RuntimeMemorySafety.TaskCache.NeverDispatchedNestedCapture')) 'Unexpected cache repetition case'
		$null=RmsParseNative ([IO.File]::ReadAllText($stdoutPath)) $watch $watch.exitCode @($name) 1
		if(!$counts.ContainsKey($name)){$counts[$name]=0};$counts[$name]++
	}
	RmsRequire ($counts.Count -eq 2 -and @($counts.Values|Where-Object {$_ -ne 100}).Count -eq 0) 'Cache repetitions pooled across scenarios'
}
$red=LoadPinned (Join-Path $RedEvidenceRoot 'red-results.json')
foreach($lane in @('admission','overflow')){
	$logs=Join-Path $RedEvidenceRoot $lane;$watch=LoadPinned (Join-Path $logs 'watchdog.json');$stdoutPath=Join-Path $logs 'stdout.log';$script:sourcePins.Add((RmsPin $stdoutPath));$text=[IO.File]::ReadAllText($stdoutPath)
	Reject "actual-red-$lane-remains-nonpassing" {RmsParseNative $text $watch $watch.exitCode @('Engine.RuntimeMemorySafety.UnverifiedPlaceholder') 1}
}
foreach($pin in $script:sourcePins){RmsCheckPin $pin}
$report=@{schemaVersion=1;status='RecordedEvidenceParserPassed';caseCount=$script:replayChecks;sourceEvidence=$script:sourcePins.ToArray();vulkanBoundsEvidenceAvailable=$false;vulkanObservedContextTeardownEvidenceAvailable=$false;maintenanceDisabledCandidateRollbackEvidenceAvailable=$false;historicalSmokeOnly=$true;currentInstalledRuntimeVerified=$false;nativeAcceptance=$false;performanceAcceptance=$false;sourceIntegrationReleased=$false}
RmsWrite (Join-Path $replayRoot 'result.json') $report
Write-Output "$($script:replayChecks) recorded evidence parser cases passed; no new runtime acceptance"
exit 0
