# Synthetic schema checks. No native process/graphics acceptance.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$WorkRoot)
Set-StrictMode -Version Latest;$ErrorActionPreference='Stop'
$rendererTestRoot=[IO.Path]::GetFullPath($WorkRoot);if(Test-Path $rendererTestRoot){throw 'Renderer protocol root must be fresh'};$null=[IO.Directory]::CreateDirectory($rendererTestRoot)
. (Join-Path $PSScriptRoot 'RunInstalledRuntimeMemorySafety.ps1')
$script:rendererChecks=0
function Check([string]$Name,[scriptblock]$Body){& $Body;$script:rendererChecks++;Write-Output "[PASS] $Name"}
function Reject([string]$Name,[scriptblock]$Body){$caught=$false;try{& $Body|Out-Null}catch{$caught=$true};if(!$caught){throw "Expected rejection $Name"};$script:rendererChecks++;Write-Output "[PASS] $Name rejected"}
function Episode([int]$Lifetime=10,[int]$Id=2,[int]$Outcome=1){return @{lifetimeId=$Lifetime;episodeId=$Id;frameId=$Id;capacityBytes=16777216;usedBytes=16777200;peakBytes=16777200;lifetimeHighWaterBytes=16777200;reuseAllocationCount=1;fallbackAllocationCount=1;resetCount=1;outcome=$Outcome}}
function State([int]$Lifetime=10,[int]$Id=2,[int]$Outcome=2,[bool]$Active=$false){return @{active=$Active;latest=(Episode $Lifetime $Id $Outcome);previous=(Episode 9 1 2);reclamation=@{lifetimeId=$Lifetime;totalFallbackAllocationCount=1;totalFallbackDeallocationCount=1;outstandingFallbackAllocationCount=0;outstandingFallbackBytes=0;resetCount=2}}}
function Boundary([int]$Sequence,[int]$Iteration,[string]$Phase){return @{sequence=$Sequence;iteration=$Iteration;phase=$Phase;eventKind='fixtureBoundary';level=$null;message=$Phase;succeeded=$true}}
function ShiftLifetimeIds($Value,[int]$Shift){
	if($null -eq $Value -or $Value -is [string] -or $Value -is [ValueType]){return}
	if($Value -is [Array]){foreach($item in $Value){ShiftLifetimeIds $item $Shift};return}
	foreach($property in $Value.PSObject.Properties){if($property.Name -ceq 'lifetimeId'){$property.Value=[long]$property.Value+$Shift}else{ShiftLifetimeIds $property.Value $Shift}}
}
function Fixture([string]$Scenario,[int]$Count=1){
	$row=@{iteration=1;scenarioId=$Scenario;status='Passed';graphicsInstantiated=$true;validationActuallyEnabled=$true;windowHidden=$true;authoredCapturedParity=$true;threadPoolWorkerCount=8;allSessionValidationErrors=0;validationCallbackLog=@();allSessionLog=@();authoredLightCount=131072;adapter=@{hardwareName='PROTOCOL-ONLY';driverVersion='MOCK';apiVersion='MOCK'};captured=(Episode);rendererStartup=@{initialized=$true;validationActuallyEnabled=$true;renderSessionGeneration=1}}
	if($Scenario -in @('completed','failed')){
		$row.heldRead=@{arrived=$true;completed=$true;begin=0;end=65536;digest=123;activeWhileHeld=$true;phaseUnsettledWhileHeld=$true;protectedClearRejectedWhileHeld=$true;originalFailure=$(if($Scenario -ceq 'failed'){'RMS renderer inline failure after held child arrived'}else{''})}
		$row.chunks=@(@{begin=0;end=65536;beforeDigest=123;afterDigest=123;computedSignatureDigest=456;ended=$true},@{begin=65536;end=131072;beforeDigest=789;afterDigest=$(if($Scenario -ceq 'failed'){0}else{789});computedSignatureDigest=456;ended=($Scenario -cne 'failed')})
		$row.terminal=State 10 2 $(if($Scenario -ceq 'failed'){3}else{2})
	}else{
		if($Scenario -ceq 'cancelled'){$state=State 10 2 4 $true;$row.nextCapture=$state;$row.retired=$state;$row.nextCaptured=Episode 10 3 1}
		else{$row.retired=State 10 2 4}
	}
	if($Scenario -cne 'shutdown'){
		$lifetime=$(if($Scenario -in @('restart','scene-clear')){11}else{10})
		$row.healthyFollowup=@{captured=(Episode $lifetime 4 1);ordinarySubmissionSerial=2;previousOrdinarySubmissionSerial=1;sequence=3;previousAcquisitionSequence=2;acquired=$true;generation=2;commandBufferCount=1;forcedEmpty=$false;ordinarySubmitAccepted=$true;ordinaryCompletionObserved=$true;waitIdleAttempted=$true;waitIdleResult=0;waitIdleCompletionObserved=$true;presentAttempted=$true;presentResult=0;recoverySuspended=$false}
		$row.balancedAfterClear=State $lifetime 4 2
	}
	$finalLifetime=$(if($Scenario -ceq 'shutdown'){10}else{$lifetime})
	$row.rendererCleanup=@{attempted=$true;completed=$true;contextReleased=$true;activeAfterRetirement=$false;retiredState=(State $finalLifetime 4 2);workerCountAfterRetirement=8}
	$logs=@((Boundary 1 0 HostInitializeBegin),(Boundary 2 0 HostInitializeEnd));$rows=@();$ids=@();$sequence=2
	for($i=1;$i -le $Count;$i++){$copy=$row|ConvertTo-Json -Depth 100|ConvertFrom-Json;ShiftLifetimeIds $copy (($i-1)*100);$copy.iteration=$i;$copy.rendererStartup.renderSessionGeneration=$i;$sequence++;$begin=Boundary $sequence $i IterationBegin;$sequence++;$end=Boundary $sequence $i IterationEnd;$copy.allSessionLog=@($begin,$end);$logs+=@($begin,$end);$rows+=,$copy;$ids+=,$copy.captured.lifetimeId}
	$sequence++;$logs+=,(Boundary $sequence 0 HostShutdownBegin);$sequence++;$logs+=,(Boundary $sequence 0 HostShutdownEnd)
	return @{schemaVersion=1;rendererEvidenceVersion=2;mode='renderer';scenario=$Scenario;requestedIterations=$Count;attemptedIterations=$Count;completedIterations=$Count;status='Passed';nativeAcceptance=$true;observations=$rows;capturedLifetimeIds=$ids;hostSession=@{engineInitializeCount=1;engineShutdownCount=1;engineCleanupCompleted=$true;threadPoolWorkerCount=8;threadPoolWorkersAfterCleanup=0;frameFailure=$null};allSessionLog=$logs;validationCallbackLog=@();allSessionValidationErrors=0}|ConvertTo-Json -Depth 100|ConvertFrom-Json
}
foreach($lane in @('completed','failed','cancelled','scene-clear','shutdown','restart')){Check "renderer-schema-$lane" {$null=RmsCheckFixture (Fixture $lane) renderer $lane 1}}
$faults=@('missing-validation','cleanup','workers-after-cleanup','capacity','fallback-pressure','terminal-identity','terminal-outcome','held-inactive','held-completion','held-range','held-digest','chunk-gap','chunk-overlap','chunk-missing-end','failure-message','fallback-leak','fallback-lifetime','restart-reused-lifetime','cancelled-reused-episode','cancelled-missing-next-capture','healthy-submit','healthy-completion','healthy-stale-sequence','healthy-stale-serial','healthy-present','healthy-wait-result','healthy-forced-empty','healthy-suspended','input-parity','duplicate-iteration','duplicate-lifetime')
foreach($fault in $faults){
	$lane=switch($fault){'failure-message'{'failed'}'restart-reused-lifetime'{'restart'}'cancelled-reused-episode'{'cancelled'}'cancelled-missing-next-capture'{'cancelled'}default{'completed'}}
	$r=Fixture $lane;$o=$r.observations[0]
	switch($fault){
		'missing-validation'{$o.PSObject.Properties.Remove('validationActuallyEnabled')}
		'cleanup'{$o.rendererCleanup.completed=$false}
		'workers-after-cleanup'{$r.hostSession.threadPoolWorkersAfterCleanup=1}
		'capacity'{$o.captured.capacityBytes=33554432}
		'fallback-pressure'{$o.captured.fallbackAllocationCount=0}
		'terminal-identity'{$o.terminal.latest.lifetimeId++}
		'terminal-outcome'{$o.terminal.latest.outcome=4}
		'held-inactive'{$o.heldRead.activeWhileHeld=$false}
		'held-completion'{$o.heldRead.completed=$false}
		'held-range'{$o.heldRead.end=131073}
		'held-digest'{$o.chunks[0].afterDigest=124}
		'chunk-gap'{$o.chunks[1].begin=65537}
		'chunk-overlap'{$o.chunks[1].begin=65535}
		'chunk-missing-end'{$o.chunks[1].ended=$false}
		'failure-message'{$o.heldRead.originalFailure='replacement error'}
		'fallback-leak'{$o.balancedAfterClear.reclamation.outstandingFallbackBytes=1}
		'fallback-lifetime'{$o.balancedAfterClear.reclamation.lifetimeId=99}
		'restart-reused-lifetime'{$o.healthyFollowup.captured.lifetimeId=$o.captured.lifetimeId}
		'cancelled-reused-episode'{$o.nextCaptured.episodeId=$o.captured.episodeId}
		'cancelled-missing-next-capture'{$o.PSObject.Properties.Remove('nextCaptured')}
		'healthy-submit'{$o.healthyFollowup.ordinarySubmitAccepted=$false}
		'healthy-completion'{$o.healthyFollowup.ordinaryCompletionObserved=$false}
		'healthy-stale-sequence'{$o.healthyFollowup.sequence=$o.healthyFollowup.previousAcquisitionSequence}
		'healthy-stale-serial'{$o.healthyFollowup.ordinarySubmissionSerial=$o.healthyFollowup.previousOrdinarySubmissionSerial}
		'healthy-present'{$o.healthyFollowup.presentResult=$null}
		'healthy-wait-result'{$o.healthyFollowup.waitIdleResult=$null}
		'healthy-forced-empty'{$o.healthyFollowup.forcedEmpty=$true}
		'healthy-suspended'{$o.healthyFollowup.recoverySuspended=$true}
		'input-parity'{$o.authoredCapturedParity=$false}
		'duplicate-iteration'{$r=Fixture $lane 2;$r.observations[1].iteration=1}
		'duplicate-lifetime'{$r=Fixture $lane 2;$r.capturedLifetimeIds[1]=$r.capturedLifetimeIds[0]}
	}
	$path=Join-Path $rendererTestRoot "$fault.json";RmsWrite $path $r
	Reject "renderer-$fault" {$null=RmsCheckFixture (Get-Content $path -Raw|ConvertFrom-Json) renderer $lane $r.requestedIterations}
}
Reject 'renderer-subset-cannot-claim-six-lane-acceptance' {RmsRunFixture $null renderer @('completed') 100}
Reject 'renderer-under100-repetitions-cannot-pass' {RmsRunFixture $null renderer @('completed','failed','cancelled','scene-clear','shutdown','restart') 99}
foreach($lane in @('completed','failed','cancelled','scene-clear','shutdown','restart')){Check "renderer-two-iterations-one-engine-session-$lane" {$null=RmsCheckFixture (Fixture $lane 2) renderer $lane 2}}
Check 'renderer100-distinct-lifetimes-one-engine-initialization-and-shutdown' {$r=Fixture completed 100;$null=RmsCheckFixture $r renderer completed 100;RmsRequire ($r.hostSession.engineInitializeCount -eq 1 -and $r.hostSession.engineShutdownCount -eq 1) 'Engine host restarted per iteration'}
foreach($fault in @('missing-version','wrong-version','missing-host-session','host-init-zero','host-init-repeated','host-shutdown-zero','host-shutdown-repeated','host-cleanup-incomplete','host-pool-never-started','host-frame-failure','host-late-validation-count','host-late-native-error-unreported','host-missing-initialize-span','host-missing-shutdown-span','host-initialize-end-failed','host-shutdown-end-failed','host-iteration-end-failed','host-missing-logs','host-log-sequence-repeated','host-native-callback-omitted','live-context-after-retirement','active-borrower-after-retirement','retired-state-active','retired-fallback-leak','pool-zero-before-host-shutdown','shared-worker-count-changed','renderer-startup-incomplete','renderer-startup-no-validation','renderer-startup-no-generation','obsolete-perrow-engine-cleanup','row-log-slice-omitted','row-log-record-forged','captured-root-id-mismatch','captured-ids-regressed','retired-lifetime-mismatch','renderer-retirement-not-attempted')){
	$r=Fixture completed;$o=$r.observations[0]
	switch($fault){
		'missing-version'{$r.PSObject.Properties.Remove('rendererEvidenceVersion')}
		'wrong-version'{$r.rendererEvidenceVersion=1}
		'missing-host-session'{$r.PSObject.Properties.Remove('hostSession')}
		'host-init-zero'{$r.hostSession.engineInitializeCount=0}
		'host-init-repeated'{$r.hostSession.engineInitializeCount=2}
		'host-shutdown-zero'{$r.hostSession.engineShutdownCount=0}
		'host-shutdown-repeated'{$r.hostSession.engineShutdownCount=2}
		'host-cleanup-incomplete'{$r.hostSession.engineCleanupCompleted=$false}
		'host-pool-never-started'{$r.hostSession.threadPoolWorkerCount=0}
		'host-frame-failure'{$r.hostSession.frameFailure='late actual failure'}
		'host-late-validation-count'{$r.allSessionValidationErrors=1}
		'host-late-native-error-unreported'{$entry=[pscustomobject]@{sequence=7;iteration=0;phase='HostShutdown';eventKind='logger';level=3;message='[Vulkan] actual late error'};$r.allSessionLog+=,$entry;$r.validationCallbackLog+=,$entry}
		'host-missing-initialize-span'{$r.allSessionLog=@($r.allSessionLog|Select-Object -Skip 1)}
		'host-missing-shutdown-span'{$r.allSessionLog=@($r.allSessionLog|Select-Object -First 5)}
		'host-initialize-end-failed'{$r.allSessionLog[1].succeeded=$false}
		'host-shutdown-end-failed'{$r.allSessionLog[5].succeeded=$false}
		'host-iteration-end-failed'{$r.allSessionLog[3].succeeded=$false}
		'host-missing-logs'{$r.allSessionLog=$null}
		'host-log-sequence-repeated'{$r.allSessionLog[1].sequence=$r.allSessionLog[0].sequence}
		'host-native-callback-omitted'{$r.allSessionLog+=,[pscustomobject]@{sequence=7;iteration=0;phase='HostShutdown';eventKind='logger';level=2;message='[Vulkan] actual native warning'}}
		'live-context-after-retirement'{$o.rendererCleanup.contextReleased=$false}
		'active-borrower-after-retirement'{$o.rendererCleanup.activeAfterRetirement=$true}
		'retired-state-active'{$o.rendererCleanup.retiredState.active=$true}
		'retired-fallback-leak'{$o.rendererCleanup.retiredState.reclamation.outstandingFallbackAllocationCount=1}
		'pool-zero-before-host-shutdown'{$o.rendererCleanup.workerCountAfterRetirement=0}
		'shared-worker-count-changed'{$o.rendererCleanup.workerCountAfterRetirement=7}
		'renderer-startup-incomplete'{$o.rendererStartup.initialized=$false}
		'renderer-startup-no-validation'{$o.rendererStartup.validationActuallyEnabled=$false}
		'renderer-startup-no-generation'{$o.rendererStartup.renderSessionGeneration=0}
		'obsolete-perrow-engine-cleanup'{$o|Add-Member engineCleanupCompleted $true}
		'row-log-slice-omitted'{$o.allSessionLog=@($o.allSessionLog|Select-Object -First 1)}
		'row-log-record-forged'{$o.allSessionLog[0].message='forged fixture record'}
		'captured-root-id-mismatch'{$r.capturedLifetimeIds[0]++}
		'captured-ids-regressed'{$r=Fixture completed 2;$r.capturedLifetimeIds=@(110,10)}
		'retired-lifetime-mismatch'{$o.rendererCleanup.retiredState.reclamation.lifetimeId=99}
		'renderer-retirement-not-attempted'{$o.rendererCleanup.attempted=$false}
	}
	Reject "renderer-session-$fault" {$null=RmsCheckFixture $r renderer completed $r.requestedIterations}
}
function WithBetweenIterationLog([string]$Phase='BetweenIterations',[int]$Iteration=0,[int]$Level=1,[string]$Message='Healthy background worker remains alive'){
	$r=Fixture completed;$entry=[pscustomobject]@{sequence=5;iteration=$Iteration;phase=$Phase;eventKind='logger';level=$Level;message=$Message};$tail=@($r.allSessionLog|Select-Object -Skip 4);foreach($record in $tail){$record.sequence++};$r.allSessionLog=@($r.allSessionLog|Select-Object -First 4)+@($entry)+$tail;if($Message.Contains('[Vulkan]')){$r.validationCallbackLog=@($entry)};return $r
}
Check 'between-iterations-healthy-info-remains-in-root-span' {$null=RmsCheckFixture (WithBetweenIterationLog) renderer completed 1}
Reject 'between-iterations-late-vulkan-error-cannot-hide' {$null=RmsCheckFixture (WithBetweenIterationLog BetweenIterations 0 3 '[Vulkan] late native validation error') renderer completed 1}
Reject 'between-iterations-log-cannot-claim-active-iteration' {$null=RmsCheckFixture (WithBetweenIterationLog BetweenIterations 1) renderer completed 1}
Reject 'iteration-logger-needs-active-iteration' {$null=RmsCheckFixture (WithBetweenIterationLog Iteration 0) renderer completed 1}
Reject 'host-logger-cannot-claim-active-iteration' {$null=RmsCheckFixture (WithBetweenIterationLog HostInitialize 1) renderer completed 1}
# The optional bootstrap is a separately observed native instance. These schema
# fixtures never claim actual driver residency or GPU acceptance from mock data.
function RendererHostContext([int]$Iteration,[string]$Role){return @{iteration=$Iteration;role=$Role;instanceHandle='2000';deviceHandle='3000';initialized=$true;validationActuallyEnabled=$true;hostLiveBefore=($Role -cne 'Startup');hostLiveAfter=$true;distinctFromHost=$true;shutdownAttempted=$true;contextReleased=$true;observationCheckpoint=$(if($Role -ceq 'Startup'){'ExistingStartupContextAfterHostCreation'}else{'ContextInitializedUnderHost'})}}
function RendererHostFixture([string]$Scenario='completed',[int]$Count=100){
	$r=Fixture $Scenario $Count;$r.rendererEvidenceVersion=3
	$r|Add-Member hostInstance @{requested=$true;createdCount=1;enumeratedCount=1;destroyedCount=1;createResult=0;enumerateCountResult=0;enumerateResult=0;physicalDeviceCount=1;instanceHandle='9000';cleanupResult='ReturnedNormally';handleCleared=$true;hostValidationCallbacks=$false;enabledExtensionCount=0;enabledLayerCount=0;engineOwnersShared=$false;driverModulePinned=$false;globalProcessInstanceCountClaim=$false;perIterationLastOwnedInstanceTeardownExercised=$false;coverageModel='RetainedFixtureHostInstance'}
	$contexts=@((RendererHostContext 0 Startup))
	foreach($row in $r.observations){
		$local=@((RendererHostContext $row.iteration Primary))
		if($Scenario -ceq 'restart'){$local+=,(RendererHostContext $row.iteration Restart)}
		$contexts+=$local
		$row|Add-Member hostInstance @{instanceHandle='9000';liveBeforeContextInitialization=$true;liveAfterContextCleanup=$true;contextLifecycles=$local}
	}
	$r|Add-Member rendererContexts @{observedInitializationCount=$contexts.Count;observedRetirementCount=$contexts.Count;finalRendererShutdownAttempted=$true;finalContextReleased=$true;windowLiveBeforeHostClose=$true;contexts=$contexts}
	$logs=@($r.allSessionLog[0],$r.allSessionLog[1],(Boundary 0 0 HostInstanceInitializeBegin),(Boundary 0 0 HostInstanceInitializeEnd),(Boundary 0 0 StartupRendererShutdownBegin),(Boundary 0 0 StartupRendererShutdownEnd))
	$logs+=@($r.allSessionLog|Where-Object iteration -GT 0)
	$logs+=@((Boundary 0 0 HostInstanceShutdownBegin),(Boundary 0 0 HostInstanceShutdownEnd),$r.allSessionLog[-2],$r.allSessionLog[-1])
	$r.allSessionLog=$logs
	SyncRendererHostLogs $r
	return ($r|ConvertTo-Json -Depth 100|ConvertFrom-Json)
}
function SyncRendererHostLogs($Receipt){
	$sequence=0;foreach($entry in $Receipt.allSessionLog){$sequence++;$entry.sequence=$sequence}
	foreach($row in $Receipt.observations){$row.allSessionLog=@($Receipt.allSessionLog|Where-Object iteration -EQ $row.iteration);$row.validationCallbackLog=@($row.allSessionLog|Where-Object eventKind -CEQ 'logger')}
	$Receipt.validationCallbackLog=@($Receipt.observations|ForEach-Object {$_.validationCallbackLog})
}
foreach($lane in @('completed','failed','cancelled','scene-clear','shutdown','restart')){
	Check "renderer-explicit-host-100-context-lifecycles-$lane" {$r=RendererHostFixture $lane;$null=RmsCheckFixture $r renderer $lane 100 -VulkanHostInstance;RmsRequire ($r.rendererContexts.observedInitializationCount -eq $(if($lane -ceq 'restart'){201}else{101})) 'Synthetic expected role count changed'}
}
Check 'renderer-default-false-v2-unchanged' {$null=RmsCheckFixture (Fixture completed) renderer completed 1 -VulkanHostInstance:$false}
Reject 'renderer-v3-requires-explicit-host-request' {RmsCheckFixture (RendererHostFixture) renderer completed 100}
Reject 'renderer-v2-cannot-satisfy-host-request' {RmsCheckFixture (Fixture completed 100) renderer completed 100 -VulkanHostInstance}
Reject 'renderer-host-still-requires100' {RmsRunFixture $null renderer @('completed','failed','cancelled','scene-clear','shutdown','restart') 99 -VulkanHostInstance}
Reject 'renderer-host-still-requires-six-lanes' {RmsRunFixture $null renderer @('completed') 100 -VulkanHostInstance}
foreach($field in @('hostInstance','rendererContexts')){Reject "renderer-host-missing-root-$field" {$r=RendererHostFixture;$r.PSObject.Properties.Remove($field);RmsCheckFixture $r renderer completed 100 -VulkanHostInstance}}
foreach($field in @('observedInitializationCount','observedRetirementCount','finalRendererShutdownAttempted','finalContextReleased','windowLiveBeforeHostClose','contexts')){Reject "renderer-host-missing-context-proof-$field" {$r=RendererHostFixture;$r.rendererContexts.PSObject.Properties.Remove($field);RmsCheckFixture $r renderer completed 100 -VulkanHostInstance}}
$hostFaults=@('leadingzero-host','leadingzero-context','startup-invented-host-before','wrong-observation-checkpoint','missing-observation-checkpoint','bootstrap-not-created','bootstrap-not-destroyed','wrong-root-handle','shared-engine-owner','module-pinning','missing-startup','startup-not-released','context-not-initialized','context-no-validation','context-not-shutdown','context-retained','context-host-dead-before','context-host-dead-after','context-not-distinct','context-actual-host-handle','null-device','wrong-initialization-count','wrong-retirement-count','final-shutdown-not-attempted','final-context-retained','window-closed-before-host','row-missing-host','row-wrong-handle','row-host-dead-before','row-host-dead-after','row-missing-contexts','row-forged-context','missing-restart','wrong-role','wrong-context-order','host-initialize-before-engine','startup-shutdown-after-first-context','bootstrap-destroy-before-last-context','bootstrap-destroy-after-engine-shutdown','missing-bootstrap-close-boundary','context-null-proof-lost','scratch-still-active','workers-drained-per-iteration','final-workers-retained','root-validation-error','row-validation-error','only99-iterations')
foreach($fault in $hostFaults){
	$lane=$(if($fault -eq 'missing-restart'){'restart'}else{'completed'});$r=RendererHostFixture $lane;$o=$r.observations[-1];$contexts=$r.rendererContexts
	switch($fault){
		'leadingzero-host'{$r.hostInstance.instanceHandle='09000';foreach($row in $r.observations){$row.hostInstance.instanceHandle='09000'}}
		'leadingzero-context'{$contexts.contexts[-1].instanceHandle='02000';$o.hostInstance.contextLifecycles[-1].instanceHandle='02000'}
		'startup-invented-host-before'{$contexts.contexts[0].hostLiveBefore=$true}
		'wrong-observation-checkpoint'{$contexts.contexts[-1].observationCheckpoint='ExistingStartupContextAfterHostCreation'}
		'missing-observation-checkpoint'{$contexts.contexts[-1].PSObject.Properties.Remove('observationCheckpoint')}
		'bootstrap-not-created'{$r.hostInstance.createdCount=0}
		'bootstrap-not-destroyed'{$r.hostInstance.destroyedCount=0}
		'wrong-root-handle'{$r.hostInstance.instanceHandle='9999'}
		'shared-engine-owner'{$r.hostInstance.engineOwnersShared=$true}
		'module-pinning'{$r.hostInstance.driverModulePinned=$true}
		'missing-startup'{$contexts.contexts=@($contexts.contexts|Select-Object -Skip 1)}
		'startup-not-released'{$contexts.contexts[0].contextReleased=$false}
		'context-not-initialized'{$contexts.contexts[-1].initialized=$false}
		'context-no-validation'{$contexts.contexts[-1].validationActuallyEnabled=$false}
		'context-not-shutdown'{$contexts.contexts[-1].shutdownAttempted=$false}
		'context-retained'{$contexts.contexts[-1].contextReleased=$false}
		'context-host-dead-before'{$contexts.contexts[-1].hostLiveBefore=$false}
		'context-host-dead-after'{$contexts.contexts[-1].hostLiveAfter=$false}
		'context-not-distinct'{$contexts.contexts[-1].distinctFromHost=$false}
		'context-actual-host-handle'{$contexts.contexts[-1].instanceHandle=$r.hostInstance.instanceHandle}
		'null-device'{$contexts.contexts[-1].deviceHandle='0'}
		'wrong-initialization-count'{$contexts.observedInitializationCount--}
		'wrong-retirement-count'{$contexts.observedRetirementCount--}
		'final-shutdown-not-attempted'{$contexts.finalRendererShutdownAttempted=$false}
		'final-context-retained'{$contexts.finalContextReleased=$false}
		'window-closed-before-host'{$contexts.windowLiveBeforeHostClose=$false}
		'row-missing-host'{$o.PSObject.Properties.Remove('hostInstance')}
		'row-wrong-handle'{$o.hostInstance.instanceHandle='9999'}
		'row-host-dead-before'{$o.hostInstance.liveBeforeContextInitialization=$false}
		'row-host-dead-after'{$o.hostInstance.liveAfterContextCleanup=$false}
		'row-missing-contexts'{$o.hostInstance.PSObject.Properties.Remove('contextLifecycles')}
		'row-forged-context'{$o.hostInstance.contextLifecycles[-1].deviceHandle='3333'}
		'missing-restart'{$contexts.contexts=@($contexts.contexts|Where-Object role -CNE 'Restart');$o.hostInstance.contextLifecycles=@($o.hostInstance.contextLifecycles|Where-Object role -CNE 'Restart');$contexts.observedInitializationCount=$contexts.contexts.Count;$contexts.observedRetirementCount=$contexts.contexts.Count}
		'wrong-role'{$contexts.contexts[-1].role='Startup'}
		'wrong-context-order'{$contexts.contexts=@($contexts.contexts[1],$contexts.contexts[0])+@($contexts.contexts|Select-Object -Skip 2)}
		'host-initialize-before-engine'{$logs=@($r.allSessionLog);$r.allSessionLog=@($logs[0],$logs[2],$logs[3],$logs[1])+@($logs|Select-Object -Skip 4)}
		'startup-shutdown-after-first-context'{$logs=@($r.allSessionLog);$r.allSessionLog=@($logs[0..3])+@($logs[6..7])+@($logs[4..5])+@($logs|Select-Object -Skip 8)}
		'bootstrap-destroy-before-last-context'{$logs=@($r.allSessionLog);$r.allSessionLog=@($logs[0..203])+@($logs[-4..-3])+@($logs[204..205])+@($logs[-2..-1])}
		'bootstrap-destroy-after-engine-shutdown'{$logs=@($r.allSessionLog);$r.allSessionLog=@($logs[0..205])+@($logs[-2..-1])+@($logs[-4..-3])}
		'missing-bootstrap-close-boundary'{$r.allSessionLog=@($r.allSessionLog|Where-Object phase -CNE 'HostInstanceShutdownEnd')}
		'context-null-proof-lost'{$o.rendererCleanup.contextReleased=$false}
		'scratch-still-active'{$o.rendererCleanup.activeAfterRetirement=$true}
		'workers-drained-per-iteration'{$o.rendererCleanup.workerCountAfterRetirement=0}
		'final-workers-retained'{$r.hostSession.threadPoolWorkersAfterCleanup=1}
		'root-validation-error'{$r.allSessionValidationErrors=1}
		'row-validation-error'{$o.allSessionValidationErrors=1}
		'only99-iterations'{$r.observations=@($r.observations|Select-Object -First 99)}
	}
	if($fault -in @('host-initialize-before-engine','startup-shutdown-after-first-context','bootstrap-destroy-before-last-context','bootstrap-destroy-after-engine-shutdown')){SyncRendererHostLogs $r}
	Reject "renderer-explicit-host-$fault" {RmsCheckFixture $r renderer $lane 100 -VulkanHostInstance}
}
Reject 'renderer-v2-cannot-hide-bootstrap-proof' {$r=Fixture completed;$r|Add-Member hostInstance (RendererHostFixture).hostInstance;RmsCheckFixture $r renderer completed 1}
Reject 'renderer-v2-cannot-hide-context-lifecycle-proof' {$r=Fixture completed;$r|Add-Member rendererContexts (RendererHostFixture).rendererContexts;RmsCheckFixture $r renderer completed 1}
Reject 'renderer-v2-cannot-hide-row-host-proof' {$r=Fixture completed;$r.observations[0]|Add-Member hostInstance (RendererHostFixture).observations[0].hostInstance;RmsCheckFixture $r renderer completed 1}
RmsWrite (Join-Path $rendererTestRoot 'result.json') @{schemaVersion=1;status='RendererProtocolPassed';caseCount=$script:rendererChecks;syntheticSchemaOnly=$true;nativeAcceptance=$false;performanceAcceptance=$false;sourceIntegrationReleased=$false}
Write-Output "$($script:rendererChecks) renderer protocol cases passed; synthetic schema only"
exit 0
