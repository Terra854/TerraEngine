# Bounded protocol regression only. Fixture numbers are NOT native performance evidence.
[CmdletBinding()]
param(
	[string]$RepoRoot = (Join-Path $PSScriptRoot '../../..'),
	[string]$WorkRoot,
	[string]$StressProducer,
	[string]$NavigationExecutable,
	[string]$NavigationManifest
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
if([string]::IsNullOrWhiteSpace($WorkRoot)) { $WorkRoot = Join-Path $script:RepoRoot ('out/validation/evidence-contract-fixtures/' + [guid]::NewGuid().ToString('N')) }
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot)
$prefix = (Join-Path $script:RepoRoot 'out').TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
if(-not $WorkRoot.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture work root must remain below repository out/.' }
[IO.Directory]::CreateDirectory($WorkRoot) | Out-Null
$script:Utf8NoBom = [Text.UTF8Encoding]::new($false)
$script:Cases = [Collections.Generic.List[object]]::new()
$script:FunctionProvenance = [Collections.Generic.List[object]]::new()
function Get-ActualProducerFunctions {
	param([string]$Path, [string[]]$Names)
	$tokens = $null; $errors = $null
	$ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
	if(@($errors).Count) { throw "Producer parse failed: $($errors[0].Message)" }
	$definitions = @()
	foreach($name in $Names) {
		$nodes = @($ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name}, $true))
		if($nodes.Count -ne 1) { throw "Required production callable seam '$name' was not found exactly once in '$Path'." }
		$definitions += $nodes[0].Extent.Text
		$sha=[Security.Cryptography.SHA256]::Create()
		try {$astHash=([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($nodes[0].Extent.Text)))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
		$script:FunctionProvenance.Add([ordered]@{ source=$Path; sourceSha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash; function=$name; astSha256=$astHash; startLine=$nodes[0].Extent.StartLineNumber; endLine=$nodes[0].Extent.EndLineNumber })
	}
	# Execute exact AST extents, with no source replacement or copied calculation.
	# Dot-source this returned block in caller scope; never dot-source the monolith.
	return [scriptblock]::Create(($definitions -join "`n"))
}
function Assert-Protocol([bool]$Condition,[string]$Message) { if(-not $Condition) { throw $Message } }
function Invoke-ProtocolCase([string]$Name,[scriptblock]$Body) {
	try { & $Body; $script:Cases.Add([ordered]@{name=$Name;status='passed'}); Write-Output "[PASS] $Name" }
	catch { $script:Cases.Add([ordered]@{name=$Name;status='failed';error=$_.Exception.Message}); Write-Output "[FAIL] $Name : $($_.Exception.Message)" }
}
function Complete-ProtocolTests {
	$failures = @($script:Cases | Where-Object {$_.status -ne 'passed'}).Count
	[IO.File]::WriteAllText((Join-Path $WorkRoot 'result.json'), ([ordered]@{fixtureProtocolOnly=$true;nativePerformanceMeasured=$false;selected=$script:Cases.Count;failures=$failures;cases=@($script:Cases.ToArray());functionProvenance=@($script:FunctionProvenance.ToArray())}|ConvertTo-Json -Depth 32),$script:Utf8NoBom)
	Write-Output "$($script:Cases.Count) selected protocol cases, $failures failures; native performance NOT measured."
	if($failures) { exit 1 }
}
# Match the actual benchmark script rather than inventing a stricter property contract.
Set-StrictMode -Off
$producer=Join-Path $script:RepoRoot 'Tests/Tools/Physics/CapturePhysicsOptimizationBenchmark.ps1'
. (Get-ActualProducerFunctions $producer @('New-UnavailableMetric','Invoke-InstalledStress','Convert-StressSummaryToRun','Get-ObservedPhysicsBenchmarkWorkerCount'))
foreach($case in @(
	@{name='Missing';live=$null;native=$null;expected=$null},
	@{name='NullLiveOverridesOtherProcess';live=[pscustomobject]@{actualWorkerCount=$null};native=[pscustomobject]@{resolved=8};expected=$null},
	@{name='ZeroIsUnavailable';live=[pscustomobject]@{actualWorkerCount=0};native=$null;expected=$null},
	@{name='ObservedLive';live=[pscustomobject]@{actualWorkerCount=3};native=[pscustomobject]@{resolved=8};expected=3},
	@{name='ObservedNative';live=$null;native=[pscustomobject]@{resolved=8};expected=8}
)) { Invoke-ProtocolCase ('WorkerIdentity.'+$case.name) {
	$actual=Get-ObservedPhysicsBenchmarkWorkerCount $case.live $case.native
	Assert-Protocol ($actual -eq $case.expected) 'Only an observed positive count may identify the measured process.'
} }
# Pin/execute the actual outer assignments and command, not a mirrored resolver.
$tokens=$null;$parseErrors=$null
$producerAst=[Management.Automation.Language.Parser]::ParseFile($producer,[ref]$tokens,[ref]$parseErrors)
if(@($parseErrors).Count){throw 'The real warmup producer must parse before AST callsite execution.'}
$actualBlocks=@{}
foreach($kind in @('warmupSteps','measuredSteps','installedStressCommand')) {
	$nodes=if($kind -eq 'installedStressCommand') {
		@($producerAst.FindAll({param($node) $node -is [Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Invoke-InstalledStress'},$true))
	}else{
		@($producerAst.FindAll({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left -is [Management.Automation.Language.VariableExpressionAst] -and $node.Left.VariablePath.UserPath -eq $kind},$true))
	}
	if($nodes.Count -ne 1){throw "Actual producer '$kind' AST must occur exactly once."}
	$node=$nodes[0];$actualBlocks[$kind]=[scriptblock]::Create($node.Extent.Text)
	$sha=[Security.Cryptography.SHA256]::Create()
	try{$nodeHash=([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($node.Extent.Text)))).Replace('-','')}finally{$sha.Dispose()}
	$script:FunctionProvenance.Add([ordered]@{source=$producer;sourceSha256=(Get-FileHash -LiteralPath $producer -Algorithm SHA256).Hash;astKind=$kind;astSha256=$nodeHash;startLine=$node.Extent.StartLineNumber;endLine=$node.Extent.EndLineNumber})
}

$script:resolvedInstallRoot=$WorkRoot
$script:AllowLiveStress=$false
$spy=Join-Path $WorkRoot 'ArgumentOnlyStressChild.ps1'
[IO.File]::WriteAllText($spy, @'
# Protocol spy: records child arguments and emits bounded raw fixture samples.
# This is not a physics runner, editor, native workload, or performance measurement.
param($InstallRoot,$WorkRoot,$CanonicalFixture,[int]$ObjectCount,[int]$ActiveDynamicCount,[int]$ExplicitStepCount,[int]$WarmupStepCount,$WorkerCount,$WorkerScalingScenario,$VelocityIterations,$PositionIterations,[switch]$AllowLiveStress)
[IO.Directory]::CreateDirectory((Join-Path $WorkRoot 'evidence'))|Out-Null
@{explicitStepCount=$ExplicitStepCount;warmupStepCount=$WarmupStepCount;canonicalFixture=$CanonicalFixture;fixtureProtocolOnly=$true}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $WorkRoot 'arguments.json')
$samples=@(for($i=1;$i -le $ExplicitStepCount;$i++){
	[ordered]@{tick=$i;stages=[ordered]@{authoringPreparation=$i;providerCommit=1;simulation=1;extraction=1;eventPublication=1;stateValidation=1;snapshotConstruction=1;transactionConstruction=1};jobs=[ordered]@{submitted=1;completed=1;rejected=0;observedWorkerThreads=0;barrierWaitMicroseconds=0;jobExecutionMicroseconds=0;idle=$true}}
})
[ordered]@{fixtureProtocolOnly=$true;nativePerformanceMeasured=$false;success=$true;explicitStepCount=$ExplicitStepCount;requestedExplicitStepCount=$ExplicitStepCount;physicsStageSamples=$samples;determinismHash='fixture-only';forcedTermination=$false;managedReloadAutomationEvidenceCompleteness=$null}|ConvertTo-Json -Depth 16|Set-Content -LiteralPath (Join-Path $WorkRoot 'evidence/summary.json')
'@,$script:Utf8NoBom)
foreach($case in @(
	[pscustomobject]@{name='ManifestFallbackWithoutWorkloadOverride';resolved=3;override=$null;measured=7;id='physics-worker-large-v1';expectedMedian=7},
	[pscustomobject]@{name='ExplicitWorkloadOverride';resolved=2;override=2;measured=7;id='physics-worker-large-v1';expectedMedian=6},
	[pscustomobject]@{name='ExplicitZeroWarmup';resolved=0;override=0;measured=7;id='physics-worker-large-v1';expectedMedian=4},
	[pscustomobject]@{name='CanonicalLargeThirtyWarmup';resolved=30;override=30;measured=100;id='physics-worker-large-v1';expectedMedian=80},
	[pscustomobject]@{name='CanonicalLargeZeroWarmup';resolved=0;override=0;measured=100;id='physics-worker-large-v1';expectedMedian=50},
	[pscustomobject]@{name='CanonicalUnchangedThirtyWarmup';resolved=30;override=30;measured=100;id='physics-unchanged-10000-v1';expectedMedian=80}
)) {
	Invoke-ProtocolCase "Warmup.$($case.name)" {
		$workload=[pscustomobject]@{id=$case.id;population=100;activeBodies=10;measuredSteps=$case.measured}
		if($null -ne $case.override) {$workload|Add-Member -NotePropertyName warmupSteps -NotePropertyValue $case.override}
		$caseRoot=Join-Path $WorkRoot $case.name
		$manifestObject=[pscustomobject]@{warmupSteps=3;measuredSteps=7}
		$runNumber=1;$OutputDirectory=$caseRoot;$worker='2'
		$quality=[pscustomobject]@{velocityIterations=10;positionIterations=2}
		$variantId='fixture';$installedRunnerPath=$spy
		. $actualBlocks['warmupSteps']
		. $actualBlocks['measuredSteps']
		Assert-Protocol ($warmupSteps -eq $case.resolved -and $measuredSteps -eq $case.measured) 'Actual outer assignments must resolve manifest fallback/overrides exactly.'
		# The exact actual main callsite invokes the actual production helper.
		# A forgotten caller forwarding argument remains observable after repair.
		$summary = . $actualBlocks['installedStressCommand']

		$arguments=Get-Content -LiteralPath (Join-Path $caseRoot '01-fixture/arguments.json') -Raw|ConvertFrom-Json
		Assert-Protocol ($arguments.warmupStepCount -eq $warmupSteps -and $arguments.canonicalFixture -eq $workload.id -and $arguments.explicitStepCount -eq ($case.measured + $warmupSteps)) 'Actual child invocation must include resolved warmup plus measured steps.'
		Assert-Protocol (@($summary.physicsStageSamples).Count -eq ($case.measured + $warmupSteps)) 'Raw warmup-inclusive child snapshots must remain intact.'
		$run=Convert-StressSummaryToRun -Summary $summary -RunNumber 1 -Workload $workload -Reason 'bounded protocol fixture, not native evidence' -WarmupSteps $warmupSteps
		Assert-Protocol ($run.completedSteps -eq $case.measured -and $run.coverageRatio -eq 1 -and $run.stages.authoringPreparation.sampleCount -eq $case.measured) 'Actual converter must retain the full measured window after warmup without dropping raw snapshots.'
		Assert-Protocol ($run.stages.authoringPreparation.median -eq (1000 * $case.expectedMedian)) 'Actual converter must exclude warmup samples from measured percentiles.'
	}
}
$installedProducer = Join-Path $script:RepoRoot 'Tests/Tools/Physics/RunInstalledEditorPhysicsStress.ps1'
. (Get-ActualProducerFunctions $installedProducer @('Get-PropertyValue','Get-PhysicsStepPlan','Assert-CanonicalFixtureArguments','Get-PhysicsRuntimeWorkerEvidence','Get-PhysicsRuntimeWorkerIdentity','Get-WorkerResolutionCheck','Get-WorkerScalingOutcome'))
foreach ($warmup in @(0,30,100)) {
	Invoke-ProtocolCase "CanonicalStepPlan.MeasuredHundredWarmup$warmup" {
		$ObjectCount=10000; $ActiveDynamicCount=8000; $ExplicitStepCount=100+$warmup; $WarmupStepCount=$warmup
		$fixture=[pscustomobject]@{id='physics-worker-large-v1';population=10000;activeBodies=8000;measuredSteps=100;availability='available'}
		Assert-CanonicalFixtureArguments -Fixture $fixture
		$plan=Get-PhysicsStepPlan -TotalSteps $ExplicitStepCount -WarmupSteps $WarmupStepCount
		Assert-Protocol ($plan.measuredSteps -eq 100 -and $plan.totalSteps -eq (100+$warmup)) 'Canonical100 measured steps must survive bounded warmup.'
	}
}
foreach ($measured in @(99,101)) {
	Invoke-ProtocolCase "CanonicalStepPlan.RejectsMeasured$measured" {
		$ObjectCount=10000; $ActiveDynamicCount=8000; $ExplicitStepCount=$measured+30; $WarmupStepCount=30
		$fixture=[pscustomobject]@{id='physics-worker-large-v1';population=10000;activeBodies=8000;measuredSteps=100;availability='available'}
		$rejected=$false;try { Assert-CanonicalFixtureArguments -Fixture $fixture } catch {$rejected=$true}
		Assert-Protocol $rejected 'Warmup cannot weaken the canonical100 measured-step contract or measured budget.'
	}
}
Invoke-ProtocolCase 'ChildWorkers.ActualHintAndCapacitySurviveBridgeCopy' {
	$slots=@(for($i=0;$i -lt 8;$i++){[pscustomobject]@{workerId=$i;dispatchCount=if($i -eq 0){1}else{0}}})
	$snapshot=[pscustomobject]@{monotonicMs=1;data=[pscustomobject]@{parallelFrame=[pscustomobject]@{executionTelemetry=[pscustomobject]@{detailIncluded=$true;workers=$slots;hardwareConcurrency=8;hardwareConcurrencySource='child-std-thread-hardware-concurrency'}}}}
	$copied=Get-PhysicsRuntimeWorkerEvidence -Snapshots @($snapshot)
	$identity=Get-PhysicsRuntimeWorkerIdentity -Requested 'max' -RuntimeEvidence $copied
	$check=Get-WorkerResolutionCheck -Record ([pscustomobject]@{requestedWorkerCount='max';summary=$identity})
	Assert-Protocol ($identity.actualWorkerCount -eq 8 -and $identity.workerIdentity.hardwareConcurrency -eq 8 -and $identity.workerIdentity.hardwareConcurrencySource -eq 'child-std-thread-hardware-concurrency' -and $check.available -and $check.exact) 'Actual child hint must be carried independently of requested max and parent processor count.'
}
# Exercise the real producer callable with the collector's actual dictionary shape.
function New-ProtocolWorkerSnapshot($Time, [int]$Dispatches) {
	return [ordered]@{monotonicMs=$Time;data=[ordered]@{parallelFrame=[ordered]@{executionTelemetry=[ordered]@{detailIncluded=$true;workers=@([ordered]@{workerId=7;dispatchCount=$Dispatches});hardwareConcurrency=8;hardwareConcurrencySource='child-std-thread-hardware-concurrency'}}}}
}
Invoke-ProtocolCase 'ChildWorkers.NewestDictionaryDispatchWins' {
	$copied=Get-PhysicsRuntimeWorkerEvidence -Snapshots @((New-ProtocolWorkerSnapshot 10 0),(New-ProtocolWorkerSnapshot 30 3),(New-ProtocolWorkerSnapshot 20 0))
	Assert-Protocol ($copied.snapshotMonotonicMs -eq 30 -and $copied.executionAvailability -eq 'observed' -and @($copied.executingWorkerIndices).Count -eq 1 -and $copied.executingWorkerIndices[0] -eq 7) 'Newest real dictionary snapshot must supply actual dispatch evidence, not the first inserted snapshot.'
}
Invoke-ProtocolCase 'ChildWorkers.NumericSnapshotTimeOrdering' {
	$copied=Get-PhysicsRuntimeWorkerEvidence -Snapshots @((New-ProtocolWorkerSnapshot '9' 0),(New-ProtocolWorkerSnapshot '100' 2),(New-ProtocolWorkerSnapshot '20' 0))
	Assert-Protocol ($copied.snapshotMonotonicMs -eq '100' -and $copied.executionAvailability -eq 'observed') 'Numeric string timestamps must sort numerically rather than lexically.'
}
Invoke-ProtocolCase 'ChildWorkers.RecentWindowPrecedesTimestampSelection' {
	$snapshots=@((New-ProtocolWorkerSnapshot 9999 4)) + @(for($i=1;$i -le 16;$i++){New-ProtocolWorkerSnapshot $i 0})
	$copied=Get-PhysicsRuntimeWorkerEvidence -Snapshots $snapshots
	Assert-Protocol ($copied.snapshotMonotonicMs -eq 16 -and $copied.executionAvailability -eq 'unavailable-no-runtime-worker-dispatch') 'An old observation outside the last sixteen must not reappear through timestamp sorting.'
}
foreach($hint in @($null,0,'invalid')) {
	Invoke-ProtocolCase "ChildWorkers.UnknownHintIsUnverified.$hint" {
		$summary=[pscustomobject]@{actualWorkerCount=8;workerIdentity=[pscustomobject]@{hardwareConcurrency=$hint}}
		$check=Get-WorkerResolutionCheck -Record ([pscustomobject]@{requestedWorkerCount='max';summary=$summary})
		$gate=[pscustomobject]@{available=$false;passed=$null;workerResolutionChecks=@($check)}
		$status=Get-WorkerScalingOutcome -Complete $true -Deterministic $true -Gate $gate
		Assert-Protocol (-not $check.available -and $status -eq 'Unverified') 'Unknown child hardware must not become failed or passing scaling evidence.'
	}
}
Invoke-ProtocolCase 'ChildWorkers.KnownWrongResolutionFails' {
	$record=[pscustomobject]@{requestedWorkerCount='max';summary=[pscustomobject]@{actualWorkerCount=4;workerIdentity=[pscustomobject]@{hardwareConcurrency=8}}}
	$check=Get-WorkerResolutionCheck -Record $record
	$gate=[pscustomobject]@{available=$false;passed=$null;workerResolutionChecks=@($check)}
	Assert-Protocol ($check.available -and -not $check.exact -and (Get-WorkerScalingOutcome -Complete $true -Deterministic $true -Gate $gate) -eq 'Failed') 'A known child resolution mismatch remains a real failure.'
}
foreach($passed in @($true,$false)) {
	Invoke-ProtocolCase "Scaling.KnownTimingOutcome.$passed" {
		$gate=[pscustomobject]@{available=$true;passed=$passed;workerResolutionChecks=@()}
		$expected=if($passed){'Passed'}else{'Failed'}
		Assert-Protocol ((Get-WorkerScalingOutcome -Complete $true -Deterministic $true -Gate $gate) -eq $expected) 'Available timing gates must retain their actual pass/fail result.'
	}
}

# Run the actual matrix entry point with its child-command boundary replaced in
# an isolated PowerShell process. Synthetic summaries prove classification only;
# no child below is an editor, a real worker pool, or measured physics evidence.
$matrixDriver = Join-Path $WorkRoot 'MatrixClassificationDriver.ps1'
[IO.File]::WriteAllText($matrixDriver, @'
param($Producer,$Root,$Mode)
$ErrorActionPreference='Stop'
$global:matrixProtocolMode=$Mode
$global:matrixProtocolCalls=0
function global:powershell.exe {
	$parameters=@{}
	for($i=0;$i -lt $args.Count-1;$i++) {
		$flag=[string]$args[$i]
		if($flag -in @('-WorkRoot','-WorkerCount','-WorkerScalingScenario','-ExplicitStepCount','-WarmupStepCount')) { $i+=1; $parameters[$flag]=$args[$i] }
	}
	# Validate the real matrix command's propagation, not a reconstructed plan.
	if($parameters['-ExplicitStepCount'] -ne 130 -or $parameters['-WarmupStepCount'] -ne 30) { throw 'Matrix dropped the requested 130/30 warmup split.' }
	$global:matrixProtocolCalls++
	$worker=[string]$parameters['-WorkerCount'];$scenario=[string]$parameters['-WorkerScalingScenario']
	$resolved=if($worker -eq 'max'){8}else{[int]$worker}
	$hint=if($global:matrixProtocolMode -eq 'unknown'){$null}else{8}
	if($global:matrixProtocolMode -eq 'wrong' -and $worker -eq 'max'){$resolved=4}
	$duration=if($worker -eq '1'){20}else{10}
	$summary=[ordered]@{fixtureProtocolOnly=$true;success=$true;actualWorkerCount=$resolved;workerIdentity=@{hardwareConcurrency=$hint};determinismHash="fixture-$scenario";monotonicDurationMs=1;physicsStageSamples=@(@{stages=@{simulation=$duration}})}
	$evidence=Join-Path $parameters['-WorkRoot'] 'evidence'
	[void][IO.Directory]::CreateDirectory($evidence)
	[IO.File]::WriteAllText((Join-Path $evidence 'summary.json'),($summary|ConvertTo-Json -Depth 12))
	$global:LASTEXITCODE=0
}
& $Producer -InstallRoot $Root -WorkRoot $Root -RunWorkerScalingMatrix -ExplicitStepCount 130 -WarmupStepCount 30 -AllowLiveStress
$code=$LASTEXITCODE
[IO.File]::WriteAllText((Join-Path $Root 'protocol-child-calls.txt'),[string]$global:matrixProtocolCalls)
exit $code
'@,$script:Utf8NoBom)
foreach($mode in @('unknown','wrong')) {
	Invoke-ProtocolCase "Scaling.ActualMatrixEntrypoint.$mode" {
		$matrixRoot=Join-Path $WorkRoot "matrix-$mode"
		$start=[Diagnostics.ProcessStartInfo]::new()
		$start.FileName=(Get-Process -Id $PID).Path
		$start.UseShellExecute=$false;$start.CreateNoWindow=$true
		$start.RedirectStandardOutput=$true;$start.RedirectStandardError=$true
		$argv=@('-NoProfile','-NonInteractive','-File',$matrixDriver,'-Producer',$installedProducer,'-Root',$matrixRoot,'-Mode',$mode)
		foreach($argument in $argv){$start.ArgumentList.Add($argument)}
		$child=[Diagnostics.Process]::new();$child.StartInfo=$start
		try {
			[void]$child.Start();$stdout=$child.StandardOutput.ReadToEndAsync();$stderr=$child.StandardError.ReadToEndAsync()
			$timedOut=-not $child.WaitForExit(15000)
			if($timedOut){$child.Kill($true);[void]$child.WaitForExit(5000)}
			$outText=if($stdout.Wait(5000)){$stdout.Result}else{'<stdout incomplete>'}
			$errText=if($stderr.Wait(5000)){$stderr.Result}else{'<stderr incomplete>'}
			$exitCode=if($timedOut){124}else{$child.ExitCode}
			[IO.File]::WriteAllText((Join-Path $WorkRoot "matrix-$mode-execution.json"),(@{executable=$start.FileName;arguments=$argv;exitCode=$exitCode;timedOut=$timedOut;stdout=$outText;stderr=$errText;fixtureProtocolOnly=$true}|ConvertTo-Json -Depth 8),$script:Utf8NoBom)
			Assert-Protocol (-not $timedOut) 'Actual matrix classification must complete under the external watchdog.'
			$report=Get-Content -Raw -LiteralPath (Join-Path $matrixRoot 'worker-scaling-matrix-summary.json')|ConvertFrom-Json
			$expected=if($mode -eq 'unknown'){'Unverified'}else{'Failed'}
			$expectedExit=if($mode -eq 'unknown'){2}else{1}
			Assert-Protocol ($exitCode -eq $expectedExit -and $report.status -eq $expected -and $report.runs.Count -eq 16) 'Actual report/exit must distinguish missing child facts from known incorrect resolution after all16 fixture child calls.'
			if($mode -eq 'unknown'){Assert-Protocol ($null -eq $report.workerScalingGate.passed) 'Unavailable timing comparison must not become a measured failure.'}
		} finally {$child.Dispose()}
	}
}
# Exercise exact producer functions: completion must preserve a real recovery
# observation, while an absent observation remains explicitly unavailable.
. (Get-ActualProducerFunctions $installedProducer @('Complete-ProfilerCaptureEvidence'))
foreach($observed in @($true,$false)) {
	Invoke-ProtocolCase "Profiler completion recovery observation=$observed" {
		$RunProfilerCaptureChecks=$true; $CaptureDurationSeconds=5; $CaptureLevel='Full'
		$profilerCaptureStartedAtMs=0; $profilerCaptureStoppedAtMs=5001
		$unavailableMetrics=[Collections.Generic.HashSet[string]]::new()
		$profilerCapture=[ordered]@{sizeGateBytes=10485760}
		if($observed){$profilerCapture.recoveryProbe=[ordered]@{attempted=$true;supported=$true;status='succeeded';artifactId='actual-observation'}}
		$summary=[ordered]@{formatIdentifier='TerraEngineProfilerJournalV1';artifactBytes=128;totalDurationMs=5001;frameCount=1;completeFrameCount=1;incompleteFrameCount=0;
			streams=@(@{id='physics.step'},@{id='scene_view.render'},@{id='scheduler.workers'});stageTimings=@();workerHistories=@();
			protectedBoundaryTimings=@(@{id='barrier'});presentationWaitTimings=@(@{id='present'});responsivenessTimings=@(@{id='loop'});workVolumes=@{measuredTickCount=1}}
		Complete-ProfilerCaptureEvidence -Summary $summary -Artifact ([ordered]@{name='probe.teprof';artifactId='actual-observation';bytes=128}) -ArtifactRoot $WorkRoot
		Assert-Protocol ($profilerCapture.sizeGatePassed -and $profilerCapture.durationGatePassed -and $profilerCapture.formatAccepted -and $profilerCapture.metricAvailability.gatePassed) 'All independent successful capture gates must execute.'
		if($observed){
			Assert-Protocol ($profilerCapture.recoveryProbe.attempted -and $profilerCapture.recoveryProbe.supported -and $profilerCapture.recoveryProbe.status -ceq 'succeeded' -and $profilerCapture.recoveryProbe.artifactId -ceq 'actual-observation') 'Completion must preserve exact observed recovery facts.'
		}else{
			Assert-Protocol (-not $profilerCapture.recoveryProbe.supported -and $profilerCapture.recoveryProbe.status -cne 'succeeded') 'Completion cannot invent recovery success without an observation.'
		}
	}
}
if($script:Cases.Count -ne 30){throw 'Resolved-warmup protocol must execute all25 bounded controls.'}
Complete-ProtocolTests
