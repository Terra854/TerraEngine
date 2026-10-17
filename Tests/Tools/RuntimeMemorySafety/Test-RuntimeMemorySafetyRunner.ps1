<# Parser protocol tests. Mock logs/files prove parser behavior, never live acceptance.
Example: pwsh -File ./Test-RuntimeMemorySafetyRunner.ps1 -WorkRoot C:/Temp/fresh-rms-runner -MatrixPath C:/Frozen/Build/Tests/Tools/RuntimeMemorySafety/InvokeRuntimeMemorySafetyTimingMatrix.ps1
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$WorkRoot,[Parameter(Mandatory)][string]$MatrixPath)
Set-StrictMode -Version Latest;$ErrorActionPreference='Stop'
$testRoot=[IO.Path]::GetFullPath($WorkRoot);if(Test-Path $testRoot){throw 'Protocol WorkRoot must be fresh'};$null=[IO.Directory]::CreateDirectory($testRoot)
. (Join-Path $PSScriptRoot 'RunInstalledRuntimeMemorySafety.ps1')
$script:runnerChecks=0
function Check([string]$Name,[scriptblock]$Body){& $Body;$script:runnerChecks++;Write-Output "[PASS] $Name"}
function Reject([string]$Name,[scriptblock]$Body){$caught=$false;try{& $Body|Out-Null}catch{$caught=$true};if(!$caught){throw "Expected rejection: $Name"};$script:runnerChecks++;Write-Output "[PASS] $Name rejected"}
$case='Engine.RuntimeMemorySafety.TaskCache.CompletedNestedCapture'
$watch=[pscustomobject]@{pid=7000;exitCode=0;timedOut=$false;elapsedMilliseconds=15}
$facts=[pscustomobject]@{caseId=$case;actualWorkers=1;drainComplete=$true;recyclingOpen=$false;outstandingOwners=0;cachedTasks=0;destroyedCaptures=2;expectedDestroyedCaptures=2}
function NativeLog($Facts,[string]$CaseName=$case){return 'RMS_OBSERVATION '+($Facts|ConvertTo-Json -Compress)+"`n[PASS] $CaseName`n1 selected tests (54 registered), 0 failures`n"}
Check 'exact-native-selection-and-actual-cache-facts' {$r=RmsParseNative (NativeLog $facts) $watch 0 @($case) 1;RmsRequire ($r.selectedCases -eq 1 -and $r.observations[0].facts.destroyedCaptures -eq 2) 'Native facts lost'}
# Exact logger/harness interleaving observed in the parent-retained run:
# Retained completed-capture iteration 026 stdout; async logger split summary.
$splitSummaryLog=@'
RMS_OBSERVATION {"caseId":"Engine.RuntimeMemorySafety.TaskCache.CompletedNestedCapture","actualWorkers":1,"destroyedCaptures":2,"expectedDestroyedCaptures":2,"cacheGeneration":1,"outstandingOwners":0,"cachedTasks":0,"recyclingOpen":false,"drainComplete":true,"residualQueueReentry":false,"residualExternalFutureTerminal":false}
[PASS] Engine.RuntimeMemorySafety.TaskCache.CompletedNestedCapture
[2026-10-15 06:28:38][INFO][ThreadPool::workerThread:479] Worker thread no. 0 initialized and running.1
 selected tests (3024 registered), 0 failures
[2026-10-15 06:28:38][INFO][ThreadPool::workerThread:498] Worker thread no. 0 shutting down.
'@
Check 'real-async-logger-newline-summary' {$r=RmsParseNative $splitSummaryLog $watch 0 @($case) 1;RmsRequire ($r.selectedCases -eq 1 -and $r.observations.Count -eq 1 -and $r.passNames[0] -ceq $case) 'Split summary lost exact native proof'}
Reject 'newline-summary-still-enforces-floor' {RmsParseNative $splitSummaryLog $watch 0 @($case) 8}
Reject 'native-zero-selection' {RmsParseNative '0 selected tests (54 registered), 0 failures' $watch 0 @($case) 1}
Reject 'native-under-floor' {RmsParseNative (NativeLog $facts) $watch 0 @($case) 8}
Reject 'native-exact-name-missing' {RmsParseNative (NativeLog $facts) $watch 0 @('MisspelledCase') 1}
Reject 'native-missing-observation' {RmsParseNative "[PASS] $case`n1 selected tests (54 registered), 0 failures" $watch 0 @($case) 1}
Reject 'native-watchdog-kill' {RmsParseNative (NativeLog $facts) @{exitCode=124;timedOut=$true} 124 @($case) 1}
Reject 'native-nonzero-exit' {RmsParseNative (NativeLog $facts) $watch 1 @($case) 1}
Check 'case-key-normalization-preserves-raw' {$v=[pscustomobject]@{case=$case;actualWorkers=1;drainComplete=$true;recyclingOpen=$false;outstandingOwners=0;cachedTasks=0;destroyedCaptures=2;expectedDestroyedCaptures=2};$r=RmsParseNative (NativeLog $v) $watch 0 @($case) 1;RmsRequire ($r.observations[0].caseId -ceq $case -and $r.observations[0].rawJson.Contains('"case"')) 'Raw identity changed'}
Reject 'conflicting-native-identity' {RmsObservationIdentity ([pscustomobject]@{case=$case;caseId='different'})}
Reject 'unbalanced-cache-ownership' {$v=$facts|ConvertTo-Json|ConvertFrom-Json;$v.outstandingOwners=1;RmsParseNative (NativeLog $v) $watch 0 @($case) 1}
$admission='Engine.RuntimeMemorySafety.DependencyAdmission.HeldAncestorSibling'
$admissionFacts=[pscustomobject]@{caseId=$admission;actualWorkers=2;originalExceptionPreserved=$true;originalErrorTypePreserved=$true;originalErrorMessagePreserved=$true;originalErrorTokenPreserved=$true;heldAncestorsPending=$true;independentWorkCompleted=$true;drainComplete=$true;failureCheckpointCount=1;terminalAncestors=2;expectedTerminalAncestors=2;settledSiblingCount=1;heldSiblingCount=1;failedParentBodyCalls=0;ancestorBodyCalls=0;inputAliveWhileHeld=$true;inputAliveAtDrain=$true;inputSentinelsValid=$true;graphOwnershipBalanced=$true;inputExpiredAfterRetirement=$true;inputReadsBeforeGates=1;inputReadsAfterGates=1;inputDestructionCount=1}
Check 'actual-admission-worker-and-settlement' {$null=RmsParseNative (NativeLog $admissionFacts $admission) $watch 0 @($admission) 1 2}
Reject 'requested-worker-is-not-actual-worker' {RmsParseNative (NativeLog $admissionFacts $admission) $watch 0 @($admission) 1 8}
# Complete retained actual admission-8-90 stdout; original SHA256
# B0E84C8EC18B6B6D39A46F58A2D5CA35193C4EB307C293C6AE33F4DE5E6D3A4A.
# Parsing replay only: this neither repeats native work nor grants acceptance.
$retainedInterleavedLog=@'

RMS_OBSERVATION {"caseId":"Engine.RuntimeMemorySafety.DependencyAdmission.MultipleAncestors","actualWorkers":8,"failureCheckpointCount":1,"originalExceptionPreserved":true,"originalErrorTypePreserved":true,"originalErrorMessagePreserved":true,"originalErrorTokenPreserved":true,"terminalAncestors":3,"expectedTerminalAncestors":3,"heldSiblingCount":2,"settledSiblingCount":2,"heldAncestorsPending":true,"failedParentBodyCalls":0,"ancestorBodyCalls":0,"independentWorkCompleted":true,"inputAliveWhileHeld":true,"inputAliveAtDrain":true,"inputReadsBeforeGates":2,"inputReadsAfterGates":2,"inputSentinelsValid":true,"graphOwnershipBalanced":true,"inputDestructionCount":1,"inputExpiredAfterRetirement":true,"drainComplete":true}
[2026-10-15 17:34:27][INFO][ThreadPool::workerThread::<lambda_1>::operator:576] Worker thread no. 0 initialized and running.
[2026-10-15 17:34:27][INFO][ThreadPool::workerThread::<lambda_1>::operator:576] Worker thread no. 4 initialized and running.
[2026-10-15 17:34:27][INFO][ThreadPool::workerThread::<lambda_1>::operator:576] Worker thread no. 3 initialized and running.
[PASS] [2026-10-15 17:34:27][INFO][ThreadPool::workerThread::<lambda_1>::operator:576] Worker thread no. 5 initialized and running.Engine.RuntimeMemorySafety.DependencyAdmission.MultipleAncestors

[2026-10-15 17:34:27][INFO][ThreadPool::workerThread::<lambda_1>::operator:576] Worker thread no. 2 initialized and running.
[2026-10-15 17:34:27][INFO][ThreadPool::workerThread::<lambda_5>::operator:595] Worker thread no. 0 shutting down.
[2026-10-15 17:34:27][INFO][ThreadPool::workerThread::<lambda_5>::operator:595] Worker thread no. 5 shutting down.
[2026-10-15 17:34:27][INFO][ThreadPool::workerThread::<lambda_1>::operator:576] Worker thread no. 6 initialized and running.
[2026-10-15 17:34:27][INFO][ThreadPool::workerThread::<lambda_5>::operator:595] Worker thread no. 4 shutting down.
[2026-10-15 17:34:27][INFO][ThreadPool::workerThread::<lambda_1>::operator:576] Worker thread no. 7 initialized and running.
[2026-10-15 17:34:27][INFO][ThreadPool::workerThread::<lambda_5>::operator:595] Worker thread no. 7 shutting down.
[2026-10-15 17:34:27][INFO][ThreadPool::workerThread::<lambda_5>::operator:595] Worker thread no. 2 shutting down.
[2026-10-15 17:34:27][INFO][ThreadPool::workerThread::<lambda_5>::operator:595] Worker thread no. 6 shutting down.
[2026-10-15 17:34:27][INFO][ThreadPool::workerThread::<lambda_5>::operator:595] Worker thread no. 3 shutting down.
[2026-10-15 17:34:27][INFO][ThreadPool::workerThread::<lambda_1>::operator:576] Worker thread no. 1 initialized and running.
[2026-10-15 17:34:27][INFO][ThreadPool::workerThread::<lambda_5>::operator:595] Worker thread no. 1 shutting down.

1 selected tests (3044 registered), 0 failures
'@
$retainedAdmission='Engine.RuntimeMemorySafety.DependencyAdmission.MultipleAncestors'
$retainedWatchdog=[pscustomobject]@{pid=40328;exitCode=0;timedOut=$false;elapsedMilliseconds=46}
$workerInterleaveInfo='[2026-10-15 17:34:27][INFO][ThreadPool::workerThread::<lambda_1>::operator:576] Worker thread no. 5 initialized and running.'
Check 'retained-actual-eight-worker-pass-name-interleave' {$r=RmsParseNative $retainedInterleavedLog $retainedWatchdog 0 @($retainedAdmission) 1 8;RmsRequire ($r.passNames[0] -ceq $retainedAdmission -and $r.observations[0].facts.actualWorkers -eq 8) 'Retained exact case or actual worker facts changed'}
Check 'worker-info-between-observation-prefix-and-json' {$interleaved=(NativeLog $admissionFacts $admission).Replace('RMS_OBSERVATION ',('RMS_OBSERVATION '+$workerInterleaveInfo));$r=RmsParseNative $interleaved $watch 0 @($admission) 1 2;RmsRequire ($r.observations.Count -eq 1 -and $r.observations[0].facts.actualWorkers -eq 2) 'Interleaved prefix lost exact whole JSON facts'}
Reject 'interleaved-log-still-rejects-missing-case' {RmsParseNative ([regex]::Replace($retainedInterleavedLog,'(?m)^\[PASS\][^\r\n]*','')) $retainedWatchdog 0 @($retainedAdmission) 1 8}
Reject 'interleaved-log-still-rejects-duplicate-case' {RmsParseNative ($retainedInterleavedLog+"`n[PASS] $retainedAdmission") $retainedWatchdog 0 @($retainedAdmission) 1 8}
Reject 'interleaved-log-still-rejects-raw-fail' {RmsParseNative ($retainedInterleavedLog.Replace('[PASS] ',('[PASS] [FAIL] '+$workerInterleaveInfo))) $retainedWatchdog 0 @($retainedAdmission) 1 8}
Reject 'interleaved-log-still-rejects-raw-error' {RmsParseNative ($retainedInterleavedLog.Replace('[PASS] ',('[PASS] [ERROR] '+$workerInterleaveInfo))) $retainedWatchdog 0 @($retainedAdmission) 1 8}
Reject 'interleaved-log-still-rejects-ansi-split-error-marker' {RmsParseNative ($retainedInterleavedLog.Replace('[PASS] ',("[PASS] [ER$([char]27)[0mROR] "+$workerInterleaveInfo))) $retainedWatchdog 0 @($retainedAdmission) 1 8}
Reject 'interleaved-unrelated-info-is-not-stripped' {RmsParseNative ($retainedInterleavedLog.Replace($workerInterleaveInfo,$workerInterleaveInfo.Replace('ThreadPool::workerThread::<lambda_1>::operator','ResourceManager::Initialize'))) $retainedWatchdog 0 @($retainedAdmission) 1 8}
Reject 'interleaved-unhealthy-worker-info-is-not-stripped' {RmsParseNative ($retainedInterleavedLog.Replace($workerInterleaveInfo,$workerInterleaveInfo.Replace('initialized and running.','unexpected termination.'))) $retainedWatchdog 0 @($retainedAdmission) 1 8}
Reject 'interleaved-malformed-worker-record-is-not-stripped' {RmsParseNative ($retainedInterleavedLog.Replace($workerInterleaveInfo,$workerInterleaveInfo.Replace('2026-10-15','2026/10/15'))) $retainedWatchdog 0 @($retainedAdmission) 1 8}
Reject 'interleaved-case-name-suffix-is-not-accepted' {RmsParseNative ($retainedInterleavedLog.Replace(('running.'+$retainedAdmission),('running.'+$retainedAdmission+'.Unexpected'))) $retainedWatchdog 0 @($retainedAdmission) 1 8}
Check 'bounded-cpu-repetition-scope-300-admission-faults' {
	$script:mockNativeCalls=New-Object 'Collections.Generic.List[object]'
	function RmsRunNative($Context,[string[]]$Cases,[int]$MinimumCases,[string]$Label,[int]$Workers=0){
		$script:mockNativeCalls.Add([pscustomobject]@{cases=$Cases;minimumCases=$MinimumCases;label=$Label;workers=$Workers})
		return [pscustomobject]@{status='ProtocolMockOnly';nativeAcceptance=$false}
	}
	$allCases=@('VoidParent','ValueParent','HeldAncestorSibling','MultipleAncestors','FirstFailurePreserved','ParentBodySuppressed','IndependentWorkSurvives','OneWorkerHeldSibling','TwoWorkerHeldSibling','EightWorkerHeldSibling')|ForEach-Object {'Engine.RuntimeMemorySafety.DependencyAdmission.'+$_}
	$context=@{registry=@{groups=@(@{prefix='Engine.RuntimeMemorySafety.DependencyAdmission';minimumRegisteredCases=10;cases=$allCases})}}
	$null=RmsRunCpu $context 100 @(1,2,8)
	RmsRequire ($script:mockNativeCalls.Count -eq 501 -and $script:mockNativeCalls[0].cases.Count -eq 10 -and $script:mockNativeCalls[0].minimumCases -eq 10) 'Initial complete admission selection or repetition count changed'
	foreach($cacheName in @('CompletedNestedCapture','NeverDispatchedNestedCapture')){
		$rows=@($script:mockNativeCalls|Where-Object {$_.label.StartsWith("cache-$cacheName-")})
		RmsRequire ($rows.Count -eq 100 -and @($rows|Where-Object {$_.cases.Count -ne 1 -or $_.cases[0] -cne "Engine.RuntimeMemorySafety.TaskCache.$cacheName" -or $_.minimumCases -ne 1}).Count -eq 0) 'Cache repetition scope changed'
	}
	foreach($workers in @(1,2,8)){
		$rows=@($script:mockNativeCalls|Where-Object {$_.workers -eq $workers})
		RmsRequire ($rows.Count -eq 100 -and @($rows|Where-Object {$_.cases.Count -ne 1 -or $_.cases[0] -cne 'Engine.RuntimeMemorySafety.DependencyAdmission.MultipleAncestors' -or $_.minimumCases -ne 1}).Count -eq 0) 'Admission matrix must repeat exactly MultipleAncestors100 times per worker count'
	}
}
Reject 'unverified-fixture-is-nonpassing' {RmsCheckFixture @{schemaVersion=1;mode='vulkan';scenario='fresh-acquisition';status='Unverified'} vulkan fresh-acquisition 100}
Reject 'missing-renderer-proof-is-nonpassing' {RmsCheckFixture @{schemaVersion=1;mode='renderer';scenario='completed';status='Passed';nativeAcceptance=$true;requestedIterations=1;attemptedIterations=1;completedIterations=1;observations=@(@{iteration=1;scenarioId='completed';status='Passed'})} renderer completed 1}

# Exercise the REAL RmsRunNative orchestration with a file-only mock slice.
# No native executable or watchdog is launched, and these facts never establish
# native acceptance. The mock records exactly what the invoked slice can see.
$environmentWork=Join-Path $testRoot 'environment-protocol';$null=[IO.Directory]::CreateDirectory($environmentWork)
$environmentSlice=Join-Path $environmentWork 'MockNativeSlice.ps1'
$mockSliceText=@'
param([string]$Executable,[string]$Filter,[int]$MinimumCases,[int]$TimeoutSeconds,[string]$LogDirectory)
$null=[IO.Directory]::CreateDirectory($LogDirectory)
$name='TERRAENGINE_RUNTIME_MEMORY_SAFETY_WORKERS'
$observed=@{protocolMockOnly=$true;exists=[Environment]::GetEnvironmentVariables().Contains($name);value=[Environment]::GetEnvironmentVariable($name)}
[IO.File]::WriteAllText((Join-Path $LogDirectory 'environment-observed.json'),($observed|ConvertTo-Json -Compress))
if($Filter.EndsWith('.Throw')){throw 'RMS protocol mock slice exception'}
[IO.File]::WriteAllText((Join-Path $LogDirectory 'watchdog.json'),'{"protocolMockOnly":true,"pid":7001,"exitCode":0,"timedOut":false}')
[IO.File]::WriteAllText((Join-Path $LogDirectory 'stdout.log'),"[PASS] $Filter`n1 selected tests (1 registered), 0 failures`n")
[IO.File]::WriteAllText((Join-Path $LogDirectory 'stderr.log'),'Protocol mock slice only; no native executable launched.')
$global:LASTEXITCODE=0
'@
[IO.File]::WriteAllText($environmentSlice,$mockSliceText)
$environmentContext=[pscustomobject]@{work=$environmentWork;slice=$environmentSlice;engineTests='protocol-mock-not-executed.exe';editorTests='protocol-mock-not-executed.exe'}
$script:environmentCaseNumber=0
function EnvironmentState{
	$name='TERRAENGINE_RUNTIME_MEMORY_SAFETY_WORKERS'
	return [pscustomobject]@{exists=[Environment]::GetEnvironmentVariables().Contains($name);value=[Environment]::GetEnvironmentVariable($name)}
}
function RestoreEnvironmentState($State){
	if(!$State.exists){Remove-Item -LiteralPath 'Env:TERRAENGINE_RUNTIME_MEMORY_SAFETY_WORKERS' -ErrorAction SilentlyContinue}
	else{[Environment]::SetEnvironmentVariable('TERRAENGINE_RUNTIME_MEMORY_SAFETY_WORKERS',$State.value)}
}
function ExerciseNativeEnvironment([string]$PriorState,[int]$Workers,[bool]$Throw=$false){
	$original=EnvironmentState
	try{
		switch($PriorState){
			'absent'{Remove-Item -LiteralPath 'Env:TERRAENGINE_RUNTIME_MEMORY_SAFETY_WORKERS' -ErrorAction SilentlyContinue}
			'value'{[Environment]::SetEnvironmentVariable('TERRAENGINE_RUNTIME_MEMORY_SAFETY_WORKERS','caller-preserved-exact-value')}
			'empty'{[Environment]::SetEnvironmentVariable('TERRAENGINE_RUNTIME_MEMORY_SAFETY_WORKERS','')}
			default{throw 'Unknown protocol environment state'}
		}
		$before=EnvironmentState
		RmsRequire (($PriorState -eq 'absent' -and !$before.exists) -or ($PriorState -eq 'value' -and $before.exists -and $before.value -ceq 'caller-preserved-exact-value') -or ($PriorState -eq 'empty' -and $before.exists -and $before.value -ceq '')) 'Protocol could not establish requested caller environment state'
		$script:environmentCaseNumber++;$label='environment-'+$script:environmentCaseNumber
		$caseName='Engine.RuntimeMemorySafety.Protocol.Environment'+$(if($Throw){'.Throw'}else{'.Mock'})
		$caught=$false
		try{$null=RmsRunNative $environmentContext @($caseName) 1 $label $Workers}
		catch{if(!$Throw){throw};$caught=$true;RmsRequire ($_.Exception.Message -ceq 'RMS protocol mock slice exception') 'Slice exception changed or was swallowed'}
		RmsRequire ($caught -eq $Throw) 'Expected slice exception path was not exercised'
		$observed=Get-Content -LiteralPath (Join-Path $environmentWork "$label/environment-observed.json") -Raw|ConvertFrom-Json
		RmsRequire $observed.protocolMockOnly 'Environment observation must remain mock-only'
		if($Workers -eq 0){RmsRequire (!$observed.exists -and $null -eq $observed.value) 'Default native slice inherited a worker override (including empty)'}
		else{RmsRequire ($observed.exists -and $observed.value -ceq [string]$Workers) 'Explicit native slice did not see the exact requested worker count'}
		$after=EnvironmentState
		RmsRequire ($after.exists -eq $before.exists -and (($null -eq $after.value -and $null -eq $before.value) -or ($null -ne $after.value -and $null -ne $before.value -and $after.value -ceq $before.value))) 'RmsRunNative did not restore caller absence/value/empty state exactly'
	}finally{RestoreEnvironmentState $original}
}
Check 'native-default-absent-remains-absent' {ExerciseNativeEnvironment absent 0}
Check 'native-default-clears-and-restores-caller-value' {ExerciseNativeEnvironment value 0}
Check 'native-default-clears-and-restores-existing-empty' {ExerciseNativeEnvironment empty 0}
Check 'native-explicit-one-restores-absence' {ExerciseNativeEnvironment absent 1}
Check 'native-explicit-two-restores-caller-value' {ExerciseNativeEnvironment value 2}
Check 'native-explicit-eight-restores-existing-empty' {ExerciseNativeEnvironment empty 8}
Check 'native-exception-restores-absence' {ExerciseNativeEnvironment absent 8 $true}
Check 'native-exception-default-restores-caller-value' {ExerciseNativeEnvironment value 0 $true}
Check 'native-exception-restores-existing-empty' {ExerciseNativeEnvironment empty 1 $true}

# File-only mock install. Hash/source/containment protocol checks do not execute
# these fake binaries and cannot count as installed native or live acceptance.
$fakeInstall=Join-Path $testRoot 'mock-install';$fakeTools=Join-Path $fakeInstall 'Tests/Tools/RuntimeMemorySafety';$null=[IO.Directory]::CreateDirectory($fakeTools)
$artifact=Join-Path $fakeTools 'fake.exe';[IO.File]::WriteAllText($artifact,'mock bytes');$artifactPin=RmsPin $artifact
$mockRunner=Join-Path $fakeTools 'RunInstalledRuntimeMemorySafety.ps1';[IO.File]::WriteAllText($mockRunner,'mock installed runner path gate only');$runnerPin=RmsPin $mockRunner
$manifestPath=Join-Path $fakeTools 'runtime-memory-safety-artifacts.json'
RmsWrite $manifestPath @{schemaVersion='runtime-memory-safety-v1';files=@(@{path='Tests/Tools/RuntimeMemorySafety/fake.exe';sha256=$artifactPin.sha256;length=$artifactPin.bytes},@{path='Tests/Tools/RuntimeMemorySafety/RunInstalledRuntimeMemorySafety.ps1';sha256=$runnerPin.sha256;length=$runnerPin.bytes})}
Check 'installed-hash-manifest' {$inventory=RmsLoadInstall $fakeInstall;RmsRequire ((RmsInstalled $inventory 'Tests/Tools/RuntimeMemorySafety/fake.exe') -ieq $artifact) 'Wrong installed resolution'}
Check 'installed-invocation-fresh-root' {$inventory=RmsLoadInstall $fakeInstall;RmsAssertInvocationPaths $inventory (Join-Path $testRoot 'fresh-invocation') $mockRunner}
Reject 'source-invocation' {$inventory=RmsLoadInstall $fakeInstall;RmsAssertInvocationPaths $inventory (Join-Path $testRoot 'fresh-source') (Join-Path $PSScriptRoot 'RunInstalledRuntimeMemorySafety.ps1')}
Reject 'reused-work-root' {$inventory=RmsLoadInstall $fakeInstall;RmsAssertInvocationPaths $inventory $testRoot $mockRunner}
Reject 'work-root-inside-install' {$inventory=RmsLoadInstall $fakeInstall;RmsAssertInvocationPaths $inventory (Join-Path $fakeInstall 'fresh-child') $mockRunner}
Reject 'required-artifact-not-in-manifest' {$inventory=RmsLoadInstall $fakeInstall;RmsInstalled $inventory 'unsealed.exe'}
Reject 'changed-installed-artifact' {[IO.File]::AppendAllText($artifact,'changed');RmsLoadInstall $fakeInstall}
[IO.File]::WriteAllText($artifact,'mock bytes')
Reject 'reused-evidence-output' {RmsWrite $manifestPath @{}}
Reject 'missing-sealed-reference' {RmsPin (Join-Path $testRoot 'missing-calibration.json')}
Reject 'manifest-escape' {$escaped=Join-Path $testRoot 'escape-install';$d=Join-Path $escaped 'Tests/Tools/RuntimeMemorySafety';$null=[IO.Directory]::CreateDirectory($d);RmsWrite (Join-Path $d 'runtime-memory-safety-artifacts.json') @{schemaVersion='runtime-memory-safety-v1';files=@(@{path='../../outside.exe';sha256=('0'*64);length=1})};RmsLoadInstall $escaped}

# Read and execute only the exact serializer function from the chosen matrix.
# Its surrounding launcher is never evaluated. These bounded regressions cover
# the wrapped scalar corruption found during real foundation calibration.
$matrixPin=RmsPin ([IO.Path]::GetFullPath($MatrixPath));$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($matrixPin.path,[ref]$tokens,[ref]$errors);RmsRequire ($errors.Count -eq 0) 'Matrix parse failed'
$canonical=@($ast.FindAll({param($node)$node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq 'Canonical'},$false));RmsRequire ($canonical.Count -eq 1) 'Missing unique frozen Canonical serializer'
. ([scriptblock]::Create($canonical[0].Extent.Text))
Check 'matrix-wrapped-join-path-string' {$value=Join-Path $testRoot 'Editor/TerraEngine-Editor.exe';$wrapped=[psobject]::AsPSObject($value);$json=ConvertTo-Json -InputObject (Canonical $wrapped) -Compress;RmsRequire (($json|ConvertFrom-Json) -ceq $value) 'Wrapped path became adapted Length object'}
Check 'matrix-generic-string-list' {$list=New-Object 'Collections.Generic.List[string]';$list.Add('one');$list.Add('two');$json=ConvertTo-Json -InputObject (Canonical $list) -Compress;RmsRequire ($json -ceq '["one","two"]') 'String list changed'}
Check 'matrix-pipeline-numeric-vector' {$vector=@(0,1,2|ForEach-Object {$_});$json=ConvertTo-Json -InputObject (Canonical $vector) -Compress;RmsRequire ($json -ceq '[0,1,2]') 'Numeric vector became empty objects'}
Check 'matrix-empty-native-object' {$empty='{}'|ConvertFrom-Json;$json=ConvertTo-Json -InputObject (Canonical $empty) -Compress;RmsRequire ($json -ceq '{}') 'Empty native object changed'}
Check 'matrix-mixed-primitives' {$value=@('path',0,1.5,$false,$null);$json=ConvertTo-Json -InputObject (Canonical $value) -Compress;RmsRequire ($json -ceq '["path",0,1.5,false,null]') 'Mixed primitives changed'}
RmsCheckPin $matrixPin
# Retained-window protocol fixtures are synthetic and never native acceptance.
# One hundred scope ordinals represent distinct constructed/destructed owners;
# reused decimal native handles model legal driver reuse, not shared owners.
function VkClone($Value){return ConvertTo-Json -InputObject $Value -Depth 100|ConvertFrom-Json}
function VkBoundary([int]$Sequence,[int]$Iteration,[string]$Phase){return @{sequence=$Sequence;iteration=$Iteration;phase=$Phase;eventKind='fixtureBoundary';level=$null;message=$Phase;succeeded=$true}}
function VkNative([int]$Sequence=1,[int]$Generation=1,[bool]$Healthy=$false){return @{
	generation=$Generation;sequence=$Sequence;imageIndex=0;frameSlot=0;swapchainImageCount=3;frameSlotCount=2;correlationId=1;acquired=$true;forcedEmpty=(!$Healthy);commandBufferCount=1;maintenanceEnabled=$true
	resetAttempted=$true;resetResult=0;submitAttempted=$true;submitResult=0;presentAttempted=$true;presentResult=0;waitIdleAttempted=$true;waitIdleResult=0;waitIdleCompletionObserved=$true
	recordEnded=(!$Healthy);fenceResetAccepted=$true;submitAccepted=(!$Healthy);fallbackCommandSubmitted=(!$Healthy);rejectedCheckpoint=0;recoverySuspended=$false;validationErrorsBefore=0;validationErrorsAfter=0;ordinarySubmitAccepted=$Healthy;ordinarySubmissionSerial=$(if($Healthy){$Sequence}else{0});ordinaryCompletionObserved=$Healthy
}}
function VkHealthy([int]$Generation=1,[int]$Start=2){return @($Start,($Start+1)|ForEach-Object {@{publicationFrameId=$_;rendererSessionGeneration=1;backendResourceGeneration=1;publicationAdmitted=$true;sceneCommandBufferSubmitted=$true;native=(VkNative $_ $Generation $true)}})}
function VkFixture([string]$Scenario='fresh-acquisition',[int]$Count=1){
	$op=VkNative;$healthyGeneration=1;$healthyStart=2
	$row=@{scenarioId=$Scenario;status='Passed';graphicsInstantiated=$true;validationActuallyEnabled=$true;contextCleanupCompleted=$true;contextShutdownAttempted=$true;contextInitializedAfterShutdown=$false;threadPoolCleanupCompleted=$true;threadPoolWorkerCount=8;maintenanceEnabled=$true;allSessionValidationErrors=0;adapter=@{hardwareName='PROTOCOL';driverVersion='MOCK';apiVersion='MOCK';renderSessionGeneration=1};operation=$op;allSessionLog=@();validationCallbackLog=@()}
	if($Scenario -ceq 'recreated-acquisition'){$op.generation=2;$healthyGeneration=2;$row.recreatedBeforeFirstAcquisition=$true}
	if($Scenario -ceq 'previously-presented'){$row.preludeFrames=@(VkNative);$op.sequence=2;$healthyStart=3}
	if($Scenario -ceq 'acquire-wait-consumed'){$op.acquireWaitConsumedBefore=$true;$op.acquireWaitConsumed=$true;$op.acquisitionDrainSubmitted=$true;$op.acquisitionDrainCompleted=$true}
	if($Scenario -ceq 'maintenance-disabled'){$row.maintenanceEnabled=$false;$op.maintenanceEnabled=$false}
	if($Scenario -in @('record-failure','fence-reset-failure','submit-failure')){
		$c=@{'record-failure'=1;'fence-reset-failure'=2;'submit-failure'=3}[$Scenario];$op.rejectedCheckpoint=$c;$op.recoverySuspended=$true;$op.acquisitionDrainSubmitted=$true;$op.acquisitionDrainCompleted=$true;$op.recordEnded=$c -ge 2;$op.resetAttempted=$c -eq 3;$op.resetResult=$(if($c -eq 3){0}else{$null});$op.submitAttempted=$false;$op.submitResult=$null;$op.submitAccepted=$false;$op.fallbackCommandSubmitted=$false;$op.presentAttempted=$false;$op.presentResult=$null;$healthyGeneration=2
	}
	$row.healthyFrames=VkHealthy $healthyGeneration $healthyStart
	if($Scenario -in @('candidate-rollback','maintenance-disabled')){
		$b=VkNative;$b.candidateRejected=$true;$b.candidateCheckpoint=1;$b.candidateOldSwapchainConsumed=$false
		$n=VkNative;$n.candidateRejected=$true;$n.candidateCheckpoint=2;$n.candidateOldSwapchainConsumed=$true;$n.candidateFallbackPassCreated=$true;$n.candidatePartialCleanupCompleted=$true;$n.recoverySuspended=$true;$n.maintenanceEnabled=$row.maintenanceEnabled
		$partial=@{checkpoint='AfterFallbackPass';replacementPublished=$false;native=$n;outcome='FailClosedTerminalShutdownRequired';healthyFrames=@();blockedBeginFrame=(VkClone $n)}
		$row.candidateSubcases=@(@{checkpoint='BeforeSwapchain';replacementPublished=$false;native=$b},$partial)
	}
	$rows=@();$logs=@();$seq=1;$logs+=,(VkBoundary $seq 0 WindowInitializeBegin);$seq++;$logs+=,(VkBoundary $seq 0 WindowInitializeEnd);$seq++
	for($i=1;$i -le $Count;$i++){
		$r=VkClone $row;$r|Add-Member iteration $i;$r|Add-Member sharedWindow @{windowIdentity='1000';windowRetainedForSession=$true;liveBeforeContextInitialization=$true;liveAfterContextCleanup=$true;hiddenBeforeContextInitialization=$true;hiddenAfterContextCleanup=$true}
		$r|Add-Member contextLifetime @{fixtureLifetimeId=$i;constructed=$true;destructionCompleted=$true;instanceHandle='2000';deviceHandle='3000'}
		$local=@();$local+=,(VkBoundary $seq $i IterationBegin);$seq++;$local+=,@{sequence=$seq;iteration=$i;phase='Iteration';eventKind='logger';level=1;message='[Vulkan] protocol-only information';nativeStage=0};$seq++;$local+=,(VkBoundary $seq $i IterationEnd);$seq++
		$r.allSessionLog=$local;$r.validationCallbackLog=@($local[1]);$logs+=$local;$rows+=,$r
	}
	$logs+=,(VkBoundary $seq 0 WindowShutdownBegin);$seq++;$logs+=,(VkBoundary $seq 0 WindowShutdownEnd)
	return VkClone @{schemaVersion=1;vulkanEvidenceVersion=2;mode='vulkan';scenario=$Scenario;status='Passed';nativeAcceptance=$true;requestedIterations=$Count;attemptedIterations=$Count;completedIterations=$Count;observations=$rows;windowSession=@{windowInitializeCount=1;windowShutdownCount=1;windowCleanupCompleted=$true;windowIdentity='1000';windowHidden=$true;handleCleared=$true};allSessionValidationErrors=0;allSessionLog=$logs;validationCallbackLog=@($rows|ForEach-Object {$_.validationCallbackLog})}
}
$vkLanes=@('fresh-acquisition','recreated-acquisition','previously-presented','acquire-wait-consumed','record-failure','fence-reset-failure','submit-failure','candidate-rollback','maintenance-enabled','maintenance-disabled')
foreach($lane in $vkLanes){Check "vulkan-window-session-$lane" {$null=RmsCheckFixture (VkFixture $lane) vulkan $lane 1}}
foreach($lane in @('previously-presented','maintenance-disabled')){Check "vulkan-one-window-100-independent-contexts-$lane" {$null=RmsCheckFixture (VkFixture $lane 100) vulkan $lane 100}}
$vkGood=VkFixture
$vkFaults=@('missing-version','wrong-version','missing-window-session','initialize-zero','initialize-many','shutdown-zero','shutdown-many','window-cleanup-false','window-cleanup-string','root-window-not-hidden','root-handle-not-cleared','root-null-handle','foreign-window','not-retained','window-dead-before','window-dead-after','visible-before','visible-after','old-window-cleanup-true','old-window-cleanup-false','missing-context-lifetime','wrong-context-scope','not-constructed','not-destroyed','null-instance','null-device','numeric-handle','context-attempt-false','context-still-initialized','context-cleanup-false','pool-cleanup-false','pool-never-started','root-validation-error','late-vulkan-error','missing-root-log','missing-root-callbacks','omitted-callback','forged-callback','missing-init-boundary','failed-window-shutdown','reordered-boundary','wrong-logger-phase','host-phase-in-iteration','row-omits-log','row-forges-log','row-omits-callback','missing-row-log','missing-healthy-frame','out-of-range-image','out-of-range-slot','duplicate-iteration','wrong-context-order','uncalled-result','fault-checkpoint-order','missing-disabled-subcase')
foreach($fault in $vkFaults){
	$lane=switch($fault){'uncalled-result'{'record-failure'}'fault-checkpoint-order'{'fence-reset-failure'}'missing-disabled-subcase'{'maintenance-disabled'}default{'fresh-acquisition'}}
	$v=VkFixture $lane $(if($fault -in @('duplicate-iteration','wrong-context-order')){2}else{1});$r=$v.observations[0]
	switch($fault){
		'missing-version'{$v.PSObject.Properties.Remove('vulkanEvidenceVersion')}
		'wrong-version'{$v.vulkanEvidenceVersion=1}
		'missing-window-session'{$v.PSObject.Properties.Remove('windowSession')}
		'initialize-zero'{$v.windowSession.windowInitializeCount=0}
		'initialize-many'{$v.windowSession.windowInitializeCount=100}
		'shutdown-zero'{$v.windowSession.windowShutdownCount=0}
		'shutdown-many'{$v.windowSession.windowShutdownCount=100}
		'window-cleanup-false'{$v.windowSession.windowCleanupCompleted=$false}
		'window-cleanup-string'{$v.windowSession.windowCleanupCompleted='true'}
		'root-window-not-hidden'{$v.windowSession.windowHidden=$false}
		'root-handle-not-cleared'{$v.windowSession.handleCleared=$false}
		'root-null-handle'{$v.windowSession.windowIdentity='0'}
		'foreign-window'{$r.sharedWindow.windowIdentity='9999'}
		'not-retained'{$r.sharedWindow.windowRetainedForSession=$false}
		'window-dead-before'{$r.sharedWindow.liveBeforeContextInitialization=$false}
		'window-dead-after'{$r.sharedWindow.liveAfterContextCleanup=$false}
		'visible-before'{$r.sharedWindow.hiddenBeforeContextInitialization=$false}
		'visible-after'{$r.sharedWindow.hiddenAfterContextCleanup=$false}
		'old-window-cleanup-true'{$r|Add-Member windowCleanupCompleted $true}
		'old-window-cleanup-false'{$r|Add-Member windowCleanupCompleted $false}
		'missing-context-lifetime'{$r.PSObject.Properties.Remove('contextLifetime')}
		'wrong-context-scope'{$r.contextLifetime.fixtureLifetimeId=2}
		'not-constructed'{$r.contextLifetime.constructed=$false}
		'not-destroyed'{$r.contextLifetime.destructionCompleted=$false}
		'null-instance'{$r.contextLifetime.instanceHandle='0'}
		'null-device'{$r.contextLifetime.deviceHandle='0'}
		'numeric-handle'{$r.contextLifetime.deviceHandle=3000}
		'context-attempt-false'{$r.contextShutdownAttempted=$false}
		'context-still-initialized'{$r.contextInitializedAfterShutdown=$true}
		'context-cleanup-false'{$r.contextCleanupCompleted=$false}
		'pool-cleanup-false'{$r.threadPoolCleanupCompleted=$false}
		'pool-never-started'{$r.threadPoolWorkerCount=0}
		'root-validation-error'{$v.allSessionValidationErrors=1}
		'late-vulkan-error'{$v.allSessionLog[-1].phase='WindowShutdown';$v.allSessionLog[-1].eventKind='logger';$v.allSessionLog[-1].level=3;$v.allSessionLog[-1].message='[Vulkan] late teardown error'}
		'missing-root-log'{$v.PSObject.Properties.Remove('allSessionLog')}
		'missing-root-callbacks'{$v.PSObject.Properties.Remove('validationCallbackLog')}
		'omitted-callback'{$v.validationCallbackLog=@()}
		'forged-callback'{$v.validationCallbackLog[0].message='[Vulkan] forged'}
		'missing-init-boundary'{$v.allSessionLog=@($v.allSessionLog|Select-Object -Skip 1)}
		'failed-window-shutdown'{$v.allSessionLog[-1].succeeded=$false}
		'reordered-boundary'{$v.allSessionLog[0].sequence=100}
		'wrong-logger-phase'{$v.allSessionLog[3].phase='Unknown'}
		'host-phase-in-iteration'{$v.allSessionLog[3].phase='WindowShutdown'}
		'row-omits-log'{$r.allSessionLog=@($r.allSessionLog|Select-Object -Skip 1)}
		'row-forges-log'{$r.allSessionLog[0].message='forged'}
		'row-omits-callback'{$r.validationCallbackLog=@()}
		'missing-row-log'{$r.PSObject.Properties.Remove('allSessionLog')}
		'missing-healthy-frame'{$r.healthyFrames=@($r.healthyFrames[0])}
		'out-of-range-image'{$r.operation.imageIndex=$r.operation.swapchainImageCount}
		'out-of-range-slot'{$r.operation.frameSlot=$r.operation.frameSlotCount}
		'duplicate-iteration'{$v.observations[1].iteration=1}
		'wrong-context-order'{$v.observations=@($v.observations[1],$v.observations[0])}
		'uncalled-result'{$r.operation.submitResult=0}
		'fault-checkpoint-order'{$r.operation.resetAttempted=$true;$r.operation.resetResult=0}
		'missing-disabled-subcase'{$r.candidateSubcases=@($r.candidateSubcases[0])}
	}
	Reject "vulkan-window-session-$fault" {RmsCheckFixture $v vulkan $lane $v.requestedIterations}
}
# Compatibility must be explicitly requested; it cannot qualify final native runs.
$historical=VkFixture;$historical.PSObject.Properties.Remove('vulkanEvidenceVersion');$historical.PSObject.Properties.Remove('windowSession');$historical.observations[0]|Add-Member windowCleanupCompleted $true
Check 'vulkan-explicit-historical-parser-replay' {$null=RmsCheckFixture $historical vulkan fresh-acquisition 1 -ProtocolReplayHistoricalReceipt}
Reject 'vulkan-historical-is-not-final-proof' {RmsCheckFixture $historical vulkan fresh-acquisition 1}
Reject 'vulkan-final-runner-still-requires-100' {RmsRunFixture $null vulkan $vkLanes 99}
Reject 'vulkan-final-runner-requires-all-ten-lanes' {RmsRunFixture $null vulkan @('fresh-acquisition') 100}
Check 'vulkan-between-iterations-information-retained' {
	$v=VkFixture;$entry=[pscustomobject]@{sequence=6;iteration=0;phase='BetweenIterations';eventKind='logger';level=1;message='[Vulkan] protocol-only settled background information';nativeStage=0};foreach($e in $v.allSessionLog|Where-Object {$_.sequence -ge 6}){$e.sequence++};$v.allSessionLog=@($v.allSessionLog[0..4]) + @($entry) + @($v.allSessionLog[5..6]);$v.validationCallbackLog+=,$entry;$null=RmsCheckFixture $v vulkan fresh-acquisition 1
}

# Keep the shutdown boundary intact: a late callback must fail because it is
# an actual native error, even when the aggregate counter falsely remains zero.
foreach($phase in @('BetweenIterations','WindowShutdown')){
	Reject "vulkan-late-error-$phase-with-complete-boundaries" {
		$v=VkFixture;$entry=[pscustomobject]@{sequence=6;iteration=0;phase=$phase;eventKind='logger';level=3;message='[Vulkan] protocol-only late error';nativeStage=0};foreach($e in $v.allSessionLog|Where-Object {$_.sequence -ge 6}){$e.sequence++};$v.allSessionLog=@($v.allSessionLog[0..4])+@($entry)+@($v.allSessionLog[5..6]);$v.validationCallbackLog+=,$entry;$null=RmsCheckFixture $v vulkan fresh-acquisition 1
	}
}
Reject 'vulkan-logs-after-window-session-boundary' {$v=VkFixture;$v.allSessionLog+=,[pscustomobject]@{sequence=8;iteration=0;phase='WindowShutdown';eventKind='logger';level=1;message='outside captured boundary'};RmsCheckFixture $v vulkan fresh-acquisition 1}
Reject 'vulkan-row-reported-validation-error' {$v=VkFixture;$v.observations[0].allSessionValidationErrors=1;RmsCheckFixture $v vulkan fresh-acquisition 1}
# Host-instance protocol examples are synthetic schema coverage only. They do
# not execute Vulkan or prove GPU acceptance; every engine context still retires.
function VkHostFixture([string]$Scenario='fresh-acquisition',[int]$Count=100){
	$v=VkFixture $Scenario $Count
	$v.vulkanEvidenceVersion=3
	$v|Add-Member hostInstance @{requested=$true;createdCount=1;enumeratedCount=1;destroyedCount=1;createResult=0;enumerateCountResult=0;enumerateResult=0;physicalDeviceCount=1;instanceHandle='9000';cleanupResult='ReturnedNormally';handleCleared=$true;hostValidationCallbacks=$false;enabledExtensionCount=0;enabledLayerCount=0;engineOwnersShared=$false;driverModulePinned=$false;globalProcessInstanceCountClaim=$false;perIterationLastOwnedInstanceTeardownExercised=$false;coverageModel='RetainedFixtureHostInstance'}
	$logs=@($v.allSessionLog[0],$v.allSessionLog[1])
	$logs+=,(VkBoundary 0 0 HostInstanceInitializeBegin);$logs+=,(VkBoundary 0 0 HostInstanceInitializeEnd)
	$logs+=@($v.allSessionLog|Where-Object iteration -GT 0)
	$logs+=,(VkBoundary 0 0 HostInstanceShutdownBegin);$logs+=,(VkBoundary 0 0 HostInstanceShutdownEnd)
	$logs+=@($v.allSessionLog[-2],$v.allSessionLog[-1])
	$sequence=0;foreach($entry in $logs){$sequence++;$entry.sequence=$sequence}
	$v.allSessionLog=$logs
	foreach($row in $v.observations){
		$row|Add-Member hostInstance @{instanceHandle='9000';liveBeforeContextInitialization=$true;liveAfterContextCleanup=$true;distinctFromContextInstance=$true}
		$row.allSessionLog=@($logs|Where-Object iteration -EQ $row.iteration)
		$row.validationCallbackLog=@($row.allSessionLog|Where-Object eventKind -CEQ 'logger')
	}
	$v.validationCallbackLog=@($v.observations|ForEach-Object {$_.validationCallbackLog})
	return VkClone $v
}
foreach($lane in $vkLanes){Check "vulkan-explicit-host-100-contexts-$lane" {$null=RmsCheckFixture (VkHostFixture $lane) vulkan $lane 100 -VulkanHostInstance}}
Check 'vulkan-default-false-retains-v2-protocol' {$null=RmsCheckFixture (VkFixture) vulkan fresh-acquisition 1 -VulkanHostInstance:$false}
Reject 'vulkan-v3-requires-explicit-host-request' {RmsCheckFixture (VkHostFixture) vulkan fresh-acquisition 100}
Reject 'vulkan-v2-cannot-satisfy-explicit-host-request' {RmsCheckFixture (VkFixture fresh-acquisition 100) vulkan fresh-acquisition 100 -VulkanHostInstance}
Reject 'vulkan-host-cannot-use-historical-parser-bypass' {RmsCheckFixture (VkHostFixture) vulkan fresh-acquisition 100 -VulkanHostInstance -ProtocolReplayHistoricalReceipt}
Reject 'vulkan-host-still-requires-100-native-iterations' {RmsRunFixture $null vulkan $vkLanes 99 -VulkanHostInstance}
Reject 'vulkan-host-still-requires-all-ten-native-lanes' {RmsRunFixture $null vulkan @('fresh-acquisition') 100 -VulkanHostInstance}
foreach($field in @('requested','createdCount','enumeratedCount','destroyedCount','createResult','enumerateCountResult','enumerateResult','physicalDeviceCount','instanceHandle','cleanupResult','handleCleared','hostValidationCallbacks','enabledExtensionCount','enabledLayerCount','engineOwnersShared','driverModulePinned','globalProcessInstanceCountClaim','perIterationLastOwnedInstanceTeardownExercised','coverageModel')){
	Reject "vulkan-host-missing-$field" {$v=VkHostFixture;$v.hostInstance.PSObject.Properties.Remove($field);RmsCheckFixture $v vulkan fresh-acquisition 100 -VulkanHostInstance}
}
foreach($field in @('instanceHandle','liveBeforeContextInitialization','liveAfterContextCleanup','distinctFromContextInstance')){
	Reject "vulkan-host-row-missing-$field" {$v=VkHostFixture;$v.observations[-1].hostInstance.PSObject.Properties.Remove($field);RmsCheckFixture $v vulkan fresh-acquisition 100 -VulkanHostInstance}
}
$hostFaults=@('leadingzero-host','leadingzero-context','missing-root','requested-false','created-zero','created-many','enumerated-zero','enumerated-many','destroyed-zero','destroyed-many','create-failure','enumerate-count-failure','enumerate-failure','no-physical-devices','null-host-handle','numeric-host-handle','cleanup-not-returned','handle-not-cleared','host-validation-callbacks','host-extensions','host-layers','shared-engine-owners','pinned-driver-module','global-instance-claim','last-instance-teardown-claim','wrong-coverage-model','missing-row','foreign-row-handle','row-dead-before','row-dead-after','row-not-distinct','actual-context-is-host','missing-host-initialization','missing-host-shutdown','host-init-after-first-context','host-shutdown-before-last-context','host-shutdown-after-window','host-boundary-failed','context-not-destroyed','context-still-initialized','pool-not-cleaned','context-shutdown-not-attempted','context-cleanup-false','row-validation-error','validation-error','only99-contexts')
foreach($fault in $hostFaults){
	$v=VkHostFixture;$h=$v.hostInstance;$r=$v.observations[-1]
	switch($fault){
		'leadingzero-host'{$h.instanceHandle='09000';foreach($row in $v.observations){$row.hostInstance.instanceHandle='09000'}}
		'leadingzero-context'{$r.contextLifetime.instanceHandle='02000'}
		'missing-root'{$v.PSObject.Properties.Remove('hostInstance')}
		'requested-false'{$h.requested=$false}
		'created-zero'{$h.createdCount=0}
		'created-many'{$h.createdCount=100}
		'enumerated-zero'{$h.enumeratedCount=0}
		'enumerated-many'{$h.enumeratedCount=100}
		'destroyed-zero'{$h.destroyedCount=0}
		'destroyed-many'{$h.destroyedCount=100}
		'create-failure'{$h.createResult=-1}
		'enumerate-count-failure'{$h.enumerateCountResult=-1}
		'enumerate-failure'{$h.enumerateResult=-1}
		'no-physical-devices'{$h.physicalDeviceCount=0}
		'null-host-handle'{$h.instanceHandle='0'}
		'numeric-host-handle'{$h.instanceHandle=9000}
		'cleanup-not-returned'{$h.cleanupResult='NotCalled'}
		'handle-not-cleared'{$h.handleCleared=$false}
		'host-validation-callbacks'{$h.hostValidationCallbacks=$true}
		'host-extensions'{$h.enabledExtensionCount=1}
		'host-layers'{$h.enabledLayerCount=1}
		'shared-engine-owners'{$h.engineOwnersShared=$true}
		'pinned-driver-module'{$h.driverModulePinned=$true}
		'global-instance-claim'{$h.globalProcessInstanceCountClaim=$true}
		'last-instance-teardown-claim'{$h.perIterationLastOwnedInstanceTeardownExercised=$true}
		'wrong-coverage-model'{$h.coverageModel='PerIterationLastInstance'}
		'missing-row'{$r.PSObject.Properties.Remove('hostInstance')}
		'foreign-row-handle'{$r.hostInstance.instanceHandle='9999'}
		'row-dead-before'{$r.hostInstance.liveBeforeContextInitialization=$false}
		'row-dead-after'{$r.hostInstance.liveAfterContextCleanup=$false}
		'row-not-distinct'{$r.hostInstance.distinctFromContextInstance=$false}
		'actual-context-is-host'{$r.contextLifetime.instanceHandle=$h.instanceHandle}
		'missing-host-initialization'{$v.allSessionLog=@($v.allSessionLog|Where-Object phase -CNE 'HostInstanceInitializeEnd')}
		'missing-host-shutdown'{$v.allSessionLog=@($v.allSessionLog|Where-Object phase -CNE 'HostInstanceShutdownEnd')}
		'host-init-after-first-context'{$logs=@($v.allSessionLog);$v.allSessionLog=@($logs[0..2])+@($logs[4..6])+@($logs[3])+@($logs[7..($logs.Count-1)])}
		'host-shutdown-before-last-context'{$logs=@($v.allSessionLog);$lastBegin=4+3*99;$v.allSessionLog=@($logs[0..($lastBegin-1)])+@($logs[-4])+@($logs[$lastBegin..($logs.Count-5)])+@($logs[-3..-1])}
		'host-shutdown-after-window'{$logs=@($v.allSessionLog);$v.allSessionLog=@($logs[0..($logs.Count-4)])+@($logs[-2..-1])+@($logs[-3])}
		'host-boundary-failed'{$v.allSessionLog[-3].succeeded=$false}
		'context-not-destroyed'{$r.contextLifetime.destructionCompleted=$false}
		'context-still-initialized'{$r.contextInitializedAfterShutdown=$true}
		'pool-not-cleaned'{$r.threadPoolCleanupCompleted=$false}
		'context-shutdown-not-attempted'{$r.contextShutdownAttempted=$false}
		'context-cleanup-false'{$r.contextCleanupCompleted=$false}
		'row-validation-error'{$r.allSessionValidationErrors=1}
		'validation-error'{$v.allSessionValidationErrors=1}
		'only99-contexts'{$v.observations=@($v.observations|Select-Object -First 99)}
	}
	# Rebuild actual slices after permutation so ordering failures cannot rely
	# on stale copied rows or merely non-monotonic numeric sequence values.
	if($fault -in @('host-init-after-first-context','host-shutdown-before-last-context','host-shutdown-after-window')){
		$sequence=0;foreach($entry in $v.allSessionLog){$sequence++;$entry.sequence=$sequence}
		foreach($row in $v.observations){$row.allSessionLog=@($v.allSessionLog|Where-Object iteration -EQ $row.iteration);$row.validationCallbackLog=@($row.allSessionLog|Where-Object eventKind -CEQ 'logger')}
		$v.validationCallbackLog=@($v.observations|ForEach-Object {$_.validationCallbackLog})
	}
	Reject "vulkan-explicit-host-$fault" {RmsCheckFixture $v vulkan fresh-acquisition 100 -VulkanHostInstance}
}
Reject 'vulkan-v2-cannot-hide-root-host-proof' {$v=VkFixture;$v|Add-Member hostInstance (VkHostFixture).hostInstance;RmsCheckFixture $v vulkan fresh-acquisition 1}
Reject 'vulkan-v2-cannot-hide-row-host-proof' {$v=VkFixture;$v.observations[0]|Add-Member hostInstance (VkHostFixture).observations[0].hostInstance;RmsCheckFixture $v vulkan fresh-acquisition 1}

# Historical compatibility cannot discard a supplied host proof, even when old
# per-context cleanup fields are also supplied to satisfy its legacy branch.
foreach($kind in @('v3','v2-root','v2-row')){
	Reject "vulkan-historical-host-proof-without-request-$kind" {
		$v=VkHostFixture
		foreach($row in $v.observations){$row|Add-Member windowCleanupCompleted $true}
		if($kind -ne 'v3'){$v.vulkanEvidenceVersion=2}
		if($kind -eq 'v2-row'){$v.PSObject.Properties.Remove('hostInstance')}
		if($kind -eq 'v2-root'){foreach($row in $v.observations){$row.PSObject.Properties.Remove('hostInstance')}}
		RmsCheckFixture $v vulkan fresh-acquisition 100 -ProtocolReplayHistoricalReceipt
	}
}
Check 'host-switch-rejects-cpu-before-context-access' {
	$caught=$false
	try {RmsRunFixture $null cpu @('completed') 100 -VulkanHostInstance|Out-Null}
	catch {$caught=$true;RmsRequire ($_.Exception.Message -ceq 'Host-instance qualification is Vulkan/Renderer-only and requires at least100 independent Engine contexts') 'Wrong rejection: host switch reached unrelated CPU setup'}
	RmsRequire $caught 'Host-only switch was accepted by CPU'
}
$report=@{schemaVersion=1;status='RunnerProtocolPassed';caseCount=$script:runnerChecks;matrix=$matrixPin;mockSubprocessEvidence=$true;nativeAcceptance=$false;performanceAcceptance=$false;sourceIntegrationReleased=$false}
RmsWrite (Join-Path $testRoot 'result.json') $report
Write-Output "$($script:runnerChecks) runtime memory safety runner protocol cases passed; mocks only"
exit 0
