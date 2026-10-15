<#
CPU protocol checks only; no editor/GPU launch or performance acceptance.
Installed invocation: & ./Test-RuntimeMemorySafetyTiming.ps1 -InstallRoot C:/Build -WorkRoot C:/Temp/fresh-rms-timing
Developer-only -ProtocolOnly exercises JSON/statistics without native binaries.
#>
[CmdletBinding()]
param([string]$InstallRoot='',[Parameter(Mandatory=$true)][string]$WorkRoot,[switch]$ProtocolOnly)
Set-StrictMode -Version Latest;$ErrorActionPreference='Stop'
$work=[IO.Path]::GetFullPath($WorkRoot)
if(Test-Path -LiteralPath $work){throw 'Timing test WorkRoot must be fresh'}
$tools=$PSScriptRoot
if(!$ProtocolOnly) {
	$install=[IO.Path]::GetFullPath($InstallRoot);$tools=Join-Path $install 'Tests/Tools/RuntimeMemorySafety'
	if([IO.Path]::GetFullPath($PSCommandPath) -cne (Join-Path $tools 'Test-RuntimeMemorySafetyTiming.ps1')){throw 'Invoke installed timing test'}
	if($work.StartsWith($install.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Timing test work must be outside install'}
}
. (Join-Path $tools 'CompareRuntimeMemorySafetyTiming.ps1')
Assert-RmsOrdinaryPath $work;$null=[IO.Directory]::CreateDirectory($work)
$script:rmsTestCount=0
function Check([string]$Name,[scriptblock]$CheckBody) {& $CheckBody;$script:rmsTestCount++;Write-Output "[PASS] $Name"}
function Reject([string]$Name,[scriptblock]$RejectionBody,[string]$Expected='*') {
	# Keep helper parameter names distinct from caller-owned $action. PowerShell
	# resolves scriptblock variables dynamically; an Action parameter would
	# shadow the caller's action and recursively invoke the check itself.
	$message=$null;try {& $RejectionBody|Out-Null}catch{$message=$_.Exception.Message}
	if($null -eq $message -or $message -notlike $Expected){throw "Expected rejection $Name ($Expected), got: $message"}
	$script:rmsTestCount++;Write-Output "[PASS] $Name rejected"
}
function Inspection([double]$Median=100,[double]$Tail=200,[switch]$Sequence) {
	$frames=New-Object 'Collections.Generic.List[object]'
	for($i=0;$i -lt 360;$i++) {
		$total=$(if($i -lt 60){1000.0}elseif($Sequence){[double]($i-59)+3.0}elseif($i -lt 344){$Median}else{$Tail})
		$d=$total/4.0
		$phases=@(
			[pscustomobject]@{sampleId=1;parentSampleId=0;moduleId='CpuEngine';name='RenderSystem: Protected Publication Capture';threadLabel='main';taskLabel='';startMs=1.0;durationMs=$d;depth=0;occurrenceCount=1;complete=$true;retainInAggregate=$true},
			[pscustomobject]@{sampleId=2;parentSampleId=0;moduleId='CpuEngine';name='RenderSystem: Geometry Route Preparation';threadLabel='main';taskLabel='';startMs=1.0+$d;durationMs=$d;depth=0;occurrenceCount=1;complete=$true;retainInAggregate=$true},
			[pscustomobject]@{sampleId=3;parentSampleId=0;moduleId='CpuEngine';name='RenderSystem: Virtual Geometry Preparation';threadLabel='main';taskLabel='';startMs=1.0+2.0*$d;durationMs=2.0*$d;depth=0;occurrenceCount=1;complete=$true;retainInAggregate=$true})
		$frames.Add([pscustomobject]@{frameIndex=1000+$i;frameStartMs=10000.0*$i;durationMs=10000.0;complete=$true;preparationMs=$total;phaseSamples=$phases;engineClockLane=@{id='engine_cpu';label='engine_cpu';scope='EngineCpu';source='ProfilerClock';status='Valid';milliseconds=10000.0;statusReason=''}})
	}
	return [pscustomobject]@{formatIdentifier='profilerJournalV1';formatVersion=1;complete=$true;metric='rendererPreparationScopeSumMs';window=@{offset=0;count=360;warmupFrames=60;measuredFrames=300};captureSettings=@{captureLevel='detailed';sampleIntervalFrames=1;proof='journalMetadata'};frames=$frames.ToArray()}
}
Check 'even-median-nearest-rank-p95-fixed-warmup' {
	$stats=Test-RuntimeMemorySafetyTimingFrames (Inspection -Sequence)
	Assert-Rms ($stats.median -eq 153.5 -and $stats.p95 -eq 288 -and $stats.measuredCount -eq 300) 'Statistics differ from fixed ranks'
}
foreach($fault in @('missing','duplicate','incomplete','wrong-module','nonfinite','zero','negative','misordered','overlap','out-of-frame','duplicate-id','wrong-occurrence','wrong-sum','gapped','window','sampled','clock')) {
	$value=Inspection
	switch($fault) {
		'missing' {$value.frames[0].phaseSamples=@($value.frames[0].phaseSamples|Select-Object -First 2)}
		'duplicate' {$value.frames[0].phaseSamples+=,$value.frames[0].phaseSamples[0]}
		'incomplete' {$value.frames[0].phaseSamples[0].complete=$false}
		'wrong-module' {$value.frames[0].phaseSamples[0].moduleId='CpuTask'}
		'nonfinite' {$value.frames[0].phaseSamples[0].durationMs=[double]::NaN}
		'zero' {$value.frames[0].phaseSamples[0].durationMs=0}
		'negative' {$value.frames[0].phaseSamples[0].startMs=-1}
		'misordered' {$value.frames[0].phaseSamples[0].name='RenderSystem: Geometry Route Preparation'}
		'overlap' {$value.frames[0].phaseSamples[1].startMs=1}
		'out-of-frame' {$value.frames[0].phaseSamples[2].durationMs=10001}
		'duplicate-id' {$value.frames[0].phaseSamples[1].sampleId=1}
		'wrong-occurrence' {$value.frames[0].phaseSamples[0].occurrenceCount=2}
		'wrong-sum' {$value.frames[0].preparationMs=1}
		'gapped' {$value.frames[1].frameIndex++}
		'window' {$value.window.offset=1}
		'sampled' {$value.captureSettings.captureLevel='sampled'}
		'clock' {$value.frames[0].engineClockLane.source='BackendGpuTimestamp'}
	}
	Reject "frame-$fault" {Test-RuntimeMemorySafetyTimingFrames $value}
}
Check 'two-nanosecond-boundary-only' {
	$value=Inspection;$value.frames[0].phaseSamples[1].startMs-=0.000002
	$null=Test-RuntimeMemorySafetyTimingFrames $value
}

# Synthetic JSON is deliberately labelled protocol-only. Pins and full matrix
# traversal are real; it does not claim any runtime, graphics or timing result.
$dependency=Join-Path $work 'synthetic-journal.teprof';[IO.File]::WriteAllText($dependency,'protocol synthetic bytes')
$fakeInspector=Join-Path $work 'synthetic-inspector';[IO.File]::WriteAllText($fakeInspector,'protocol synthetic decoder')
$parentPins=@{}
foreach($name in @('environment','settings','fixture')) {
	$path=Join-Path $work "synthetic-$name.json";Write-RmsFreshJson $path @{protocolOnly=$true;kind=$name};$parentPins[$name]=Get-RmsPin $path
}
$commonTools=@((Get-RmsPin (Join-Path $tools 'CaptureRuntimeMemorySafetyTiming.ps1')),(Get-RmsPin (Join-Path $tools 'CompareRuntimeMemorySafetyTiming.ps1')))
$script:rmsTime=[DateTimeOffset]::Parse('2020-01-01T00:00:00Z')
$script:rmsProcessOrdinal=0
function Receipt([string]$Scale,[string]$Role,[int]$Ordinal,[double]$Median,[double]$Tail,[string]$Suffix='') {
	$script:rmsTime=$script:rmsTime.AddSeconds(2)
	$script:rmsProcessOrdinal++
	$prefix=Join-Path $work "$Scale-$Role-$Ordinal$Suffix"
	$processPath=$prefix+'-process.json';Write-RmsFreshJson $processPath @{pid=30000+$script:rmsProcessOrdinal;parentPid=42;startUtc=$script:rmsTime.AddSeconds(-1).ToString('o');executablePath=$fakeInspector}
	$runtimePath=$prefix+'-runtime.json';Write-RmsFreshJson $runtimePath @{sourceIdentity=$(if($Role -ceq 'C'){'candidate'}else{'reference'});installIdentity=$(if($Role -ceq 'C'){'candidate-install'}else{'reference-install'});files=@((Get-RmsPin $fakeInspector))}
	$closePath=$prefix+'-lifecycle.json';Write-RmsFreshJson $closePath @{normalClose=$true;exitCode=0;timedOut=$false}
	$proof=@{process=(Get-RmsPin $processPath);runtime=(Get-RmsPin $runtimePath);environment=$parentPins.environment;settings=$parentPins.settings;fixture=$parentPins.fixture;orchestrator=$commonTools[0]}
	$metadata=[pscustomobject]@{scale=$Scale;role=$Role;ordinal=$Ordinal;fixtureSha256=$parentPins.fixture.sha256;settingsSha256=$parentPins.settings.sha256;cameraLightSha256=('3'*64);environmentSha256=$parentPins.environment.sha256;sourceIdentity=$(if($Role -ceq 'C'){'candidate'}else{'reference'});installIdentity=$(if($Role -ceq 'C'){'candidate-install'}else{'reference-install'});parentEvidence=$proof;lifecyclePath=$closePath}
	$journalPin=Get-RmsPin $dependency;$journalPin|Add-Member -NotePropertyName artifactId -NotePropertyValue synthetic
	$paused=@{currentState='paused';pendingTransition=$null}
	$value=[pscustomobject]@{schemaVersion='runtime-memory-safety-timing-v1';status='Captured';createdUtc=$script:rmsTime.ToString('o');completedUtc=$script:rmsTime.AddSeconds(1).ToString('o');metadata=$metadata;simulationPolicy='PausedPlayNoExplicitStep';advancesSimulation=$false;lifecycle=@{start=@{state='succeeded';captureLevel='detailed';bounds=@{maxFrames=4096;sampleIntervalFrames=1}};playBefore=$paused;playAfterPersistence=$paused;playAfterInspection=$paused;captureIdentity=@{projectEpoch=7;sessionArtifactId='synthetic'};finalOperations=@{projectEpoch=7;operations=@()};stop=@{jobId='synthetic-job'};persistenceJob=@{jobId='synthetic-job';state='succeeded';result=@{artifactId='synthetic'};artifacts=@(@{kind='profiler_capture';artifactId='synthetic';metadata=@{name=[IO.Path]::GetFileName($dependency)}})};finalHealth=@{state='Finalizing';terminalFailure=$false;rejectedCoreFrames=0;destinationWritable=$true;projectEpoch=7;sessionArtifactId='';committedFrames=360;producedFrames=360;queuedFrames=0;pendingBytes=0;inFlightBytes=0;lastError=''}};inspection=(Inspection $Median $Tail);journal=$journalPin;inspector=(Get-RmsPin $fakeInspector);tools=$commonTools;performanceAcceptance=$false}
	$path=Join-Path $work "$Scale-$Role-$Ordinal$Suffix.json";Write-RmsFreshJson $path $value;return $path
}
function Retimestamp([string]$Path,[string]$OriginalPath) {
	$record=Get-Content $Path -Raw|ConvertFrom-Json;$original=Get-Content $OriginalPath -Raw|ConvertFrom-Json
	$record.createdUtc=$original.createdUtc;$record.completedUtc=$original.completedUtc
	$processPath=$record.metadata.parentEvidence.process.path;$process=Get-Content $processPath -Raw|ConvertFrom-Json
	$process.startUtc=(ConvertTo-RmsInstant $original.createdUtc).AddSeconds(-1).ToString('o')
	[IO.File]::WriteAllText($processPath,($process|ConvertTo-Json -Depth 10));$record.metadata.parentEvidence.process=Get-RmsPin $processPath
	[IO.File]::WriteAllText($Path,($record|ConvertTo-Json -Depth 60))
}
$references=@();$scaleIndex=0
foreach($scale in @('small','normal','stress')) {
	$scaleIndex++
	$noiseMedian=$(if($scale -ceq 'normal'){100}elseif($scale -ceq 'stress'){90}else{95})
	$noiseTail=$(if($scale -ceq 'normal'){200}elseif($scale -ceq 'stress'){260}else{240})
	for($i=1;$i -le 3;$i++){$references+=Receipt $scale A $i (100*$scaleIndex) (200*$scaleIndex);$references+=Receipt $scale Aprime $i ($noiseMedian*$scaleIndex) ($noiseTail*$scaleIndex)}
}
$calibration=$null
Check 'calibration-asymmetric-ratio-separate-metric-limits' {
	$script:rmsCalibration=New-RuntimeMemorySafetyCalibration $references
	Assert-Rms ($script:rmsCalibration.thresholds.Count -eq 6) 'Six limits required'
	foreach($t in $script:rmsCalibration.thresholds) {
		$expected=$(if($t.scale -ceq 'normal'){0.0}elseif($t.scale -ceq 'stress') {if($t.metric -ceq 'median'){[Math]::Abs(90.0/100.0-1)}else{260.0/200.0-1}}elseif($t.metric -ceq 'median'){[Math]::Abs(95.0/100.0-1)}else{240.0/200.0-1})
		Assert-Rms ($t.noise -eq $expected -and $t.limit -eq [Math]::Max(0.02,$expected)) 'Wrong asymmetric per-scale/per-metric calibration formula or two-percent floor'
	}
}
$calibration=$script:rmsCalibration;$calibrationPath=Join-Path $work 'calibration.json';Write-RmsFreshJson $calibrationPath $calibration
$script:rmsTime=[DateTimeOffset]::UtcNow.AddMinutes(1)
$candidates=@();$scaleIndex=0
foreach($scale in @('small','normal','stress')){$scaleIndex++;for($i=1;$i -le 3;$i++){$candidates+=Receipt $scale C $i (100*$scaleIndex) (200*$scaleIndex)}}
Check 'all-eighteen-comparisons' {
	$result=Compare-RuntimeMemorySafetyTiming $calibrationPath $candidates
	Assert-Rms ($result.timingPassed -and $result.comparisonCount -eq 18 -and !$result.performanceAcceptance) 'Full timing matrix failed or claimed parity'
}
Check 'inclusive-threshold-without-epsilon' {
	# 95/100 calibrates five percent median noise. The upper boundary uses the
	# reference multiplication, avoiding cancellation in C/A-1 at equality.
	$equal=@($candidates);$equal[0]=Receipt small C 1 105 240 '-equal'
	Retimestamp $equal[0] $candidates[0]
	$result=Compare-RuntimeMemorySafetyTiming $calibrationPath $equal
	Assert-Rms ($result.timingPassed -and $result.comparisons[0].candidate -eq $result.comparisons[0].boundaryMs) 'Inclusive threshold rejected equality'
}
Reject 'reference-missing' {New-RuntimeMemorySafetyCalibration @($references|Select-Object -First 17)} '*18 reference*'
$reordered=@($references);$reordered[0]=$references[1];$reordered[1]=$references[0]
Reject 'reference-order' {New-RuntimeMemorySafetyCalibration $reordered} '*order*'
$tampered=$calibration|ConvertTo-Json -Depth 60|ConvertFrom-Json;$tampered.thresholds[0].limit=1
$alteredPath=Join-Path $work 'altered-calibration.json';Write-RmsFreshJson $alteredPath $tampered
Reject 'altered-calibration' {Compare-RuntimeMemorySafetyTiming $alteredPath $candidates} '*thresholds*'
Check 'one-failing-run-cannot-hide-in-pooled-summary' {
	$bad=@($candidates);$bad[0]=Receipt small C 1 110 200 '-failing'
	# Keep capture order valid while modifying only the intended run's data.
	Retimestamp $bad[0] $candidates[0]
	$result=Compare-RuntimeMemorySafetyTiming $calibrationPath $bad
	Assert-Rms (!$result.timingPassed -and @($result.comparisons|Where-Object {!$_.passed}).Count -eq 1) 'Failing run was pooled away'
}
foreach($fault in @('simulation','playing','missing-parent','hash-parent','abnormal-close','runtime-identity','runtime-hash','admission-maxframes')) {
	$value=Get-Content $references[0] -Raw|ConvertFrom-Json
	switch($fault) {
		'simulation' {$value.advancesSimulation=$true}
		'playing' {$value.lifecycle.playBefore.currentState='playing'}
		'admission-maxframes' {$value.lifecycle.start.bounds.maxFrames=0}
		'missing-parent' {$value.metadata.PSObject.Properties.Remove('parentEvidence')}
		'hash-parent' {$value.metadata.fixtureSha256=('a'*64)}
		'abnormal-close' {
			$path=Join-Path $work 'abnormal-lifecycle.json';Write-RmsFreshJson $path @{normalClose=$false;exitCode=0;timedOut=$false};$value.metadata.lifecyclePath=$path
		}
		'runtime-identity' {
			$path=Join-Path $work 'wrong-runtime.json';Write-RmsFreshJson $path @{sourceIdentity='wrong';installIdentity='wrong';files=@((Get-RmsPin $fakeInspector))};$value.metadata.parentEvidence.runtime=Get-RmsPin $path
		}
		'runtime-hash' {$value.metadata.parentEvidence.runtime.sha256=('a'*64)}
	}
	$path=Join-Path $work "invalid-$fault.json";Write-RmsFreshJson $path $value
	Reject "parent-proof-$fault" {Test-RmsTimingReceipt $path -RequireParentEvidence}
}
$duplicate=Get-Content $references[1] -Raw|ConvertFrom-Json;$first=Get-Content $references[0] -Raw|ConvertFrom-Json
$duplicate.metadata.parentEvidence.process=$first.metadata.parentEvidence.process
$duplicatePath=Join-Path $work 'duplicate-process.json';Write-RmsFreshJson $duplicatePath $duplicate
$duplicateReferences=@($references);$duplicateReferences[1]=$duplicatePath
Reject 'reference-process-reused' {New-RuntimeMemorySafetyCalibration $duplicateReferences} '*fresh unique process*'
$duplicate=Get-Content $candidates[0] -Raw|ConvertFrom-Json;$duplicate.metadata.parentEvidence.process=$first.metadata.parentEvidence.process
$duplicatePath=Join-Path $work 'candidate-reference-process.json';Write-RmsFreshJson $duplicatePath $duplicate
$duplicateCandidates=@($candidates);$duplicateCandidates[0]=$duplicatePath
Reject 'candidate-process-overlaps-reference' {Compare-RuntimeMemorySafetyTiming $calibrationPath $duplicateCandidates} '*fresh process disjoint*'
Reject 'changed-journal-hash' {
	[IO.File]::AppendAllText($dependency,'changed');Test-RmsTimingReceipt $references[0]
} '*Changed timing artifact*'
[IO.File]::WriteAllText($dependency,'protocol synthetic bytes')
Reject 'reused-output' {Write-RmsFreshJson $calibrationPath $calibration} '*'

if(!$ProtocolOnly) {
	# Real binary-codec tests prove the native parser and child-scope behavior.
	& (Join-Path $tools 'Test-RuntimeMemorySafetyTimingInspector.ps1') -InstallRoot $install -WorkRoot (Join-Path $work 'native-inspector')
	if($LASTEXITCODE -ne 0){throw 'Installed native timing inspector checks failed'}
	$script:rmsTestCount++
	. (Join-Path $tools 'CaptureRuntimeMemorySafetyTiming.ps1')
	$inspector=Join-Path $tools 'TerraEngine-RuntimeMemorySafetyTimingInspector.exe'
	$writer=Join-Path $tools 'TerraEngine-RuntimeMemorySafetyTimingJournalFixture.exe'
	$written=& $writer --work-root (Join-Path $work 'capture-journals')
	if($LASTEXITCODE -ne 0){throw 'Installed timing journal writer failed'}
	$journals=($written -join "`n")|ConvertFrom-Json
	$expectedErrors=@{play='*stably paused*';session='*project/session changed*';epoch='*project/session changed*';drop='*lossless recording*';artifact='*recorded session*';job='*job ownership changed*';path='*contained V1 artifact*';missing='*Missing capture API field*';persistence='*job ended failed*';'incomplete-journal'='*journal inspection failed*'}
	$expectedErrors['late-drop']='*Final profiler health*';$expectedErrors['late-session']='*Final profiler capture project/session*';$expectedErrors['late-operation']='*final profiler operation is still active*'
	foreach($fault in @('happy','play','session','epoch','drop','artifact','job','path','missing','persistence','incomplete-journal','late-drop','late-session','late-operation')) {
		$caseRoot=Join-Path $work "capture-$fault";$project=Join-Path $caseRoot 'project'
		$null=[IO.Directory]::CreateDirectory((Join-Path $project 'Profiler'))
		$name=$(if($fault -ceq 'incomplete-journal'){'incomplete-terminal'}else{'valid'})
		$journal=@($journals|Where-Object {$_.name -ceq $name});Assert-Rms ($journal.Count -eq 1) 'Missing capture journal fixture'
		$source=Join-Path $project 'Profiler/owned.teprof';[IO.File]::Copy($journal[0].path,$source,$false)
		$sourcePin=Get-RmsPin $source
		$script:rmsMock=[pscustomobject]@{fault=$fault;active=$false;started=$false;healthCalls=0;stops=0;calls=(New-Object 'Collections.Generic.List[string]')}
		$bridge={param($name,$arguments)
			$c=$script:rmsMock;$c.calls.Add($name)
			switch($name) {
				'play.state' {return @{currentState=$(if($c.fault -ceq 'play' -and $c.active){'playing'}else{'paused'});pendingTransition=$null}}
				'profiler.capture.operation' {
					$operationList=@();if($c.started -and !$c.active -and $c.fault -ceq 'late-operation'){$operationList+=@{active=$true}}
					return @{projectEpoch=7;operations=$operationList}
				}
				'profiler.capture.start' {
					Assert-Rms ($arguments.captureLevel -ceq 'detailed' -and $arguments.sampleIntervalFrames -eq 1 -and $arguments.maxFrames -eq 4096 -and $arguments.Count -eq 3) 'Capture sent unsupported or changed settings'
					$c.active=$true;$c.started=$true;return @{state='succeeded';captureLevel='detailed';bounds=@{maxFrames=4096;sampleIntervalFrames=1}}
				}
				'profiler.capture.health' {
					$c.healthCalls++;$later=$c.healthCalls -ge 3
					$health=@{state=$(if($c.active){'Recording'}elseif($c.started){'Finalizing'}else{'Idle'});projectEpoch=$(if($later -and $c.fault -ceq 'epoch'){8}else{7});sessionArtifactId=$(if(!$c.started){'old'}elseif(($later -and $c.fault -ceq 'session') -or ($c.healthCalls -ge 4 -and $c.fault -ceq 'late-session')){'foreign'}elseif(!$c.active){''}else{'owned'});terminalFailure=$false;destinationWritable=$true;rejectedCoreFrames=$(if(($later -and $c.fault -ceq 'drop') -or ($c.healthCalls -ge 4 -and $c.fault -ceq 'late-drop')){1}else{0});committedFrames=360;producedFrames=360;queuedFrames=0;pendingBytes=0;inFlightBytes=0;lastError=''}
					if($later -and $c.fault -ceq 'missing'){$health.Remove('committedFrames')};return $health
				}
				'profiler.capture.stop' {$c.active=$false;$c.stops++;return @{jobId='owned-job'}}
				'jobs.get' {return @{job=@{jobId=$(if($c.fault -ceq 'job'){'foreign-job'}else{'owned-job'});state=$(if($c.fault -ceq 'persistence'){'failed'}else{'succeeded'});result=@{artifactId=$(if($c.fault -ceq 'artifact'){'foreign'}else{'owned'})};artifacts=@(@{kind='profiler_capture';artifactId='owned';metadata=@{name=$(if($c.fault -ceq 'path'){'../escaped.teprof'}else{'owned.teprof'});frameCount=360}})}}}
				default {throw "Unexpected capture call $name"}
			}
		}
		$metadata=@{scale='small';role='A';ordinal=1;fixtureSha256=('1'*64);settingsSha256=('2'*64);cameraLightSha256=('3'*64);environmentSha256=('4'*64);sourceIdentity='mock-protocol-only';installIdentity='mock-protocol-only'}
		$action={Invoke-RuntimeMemorySafetyTimingCapture -Tool $bridge -InspectorPath $inspector -ProjectRoot $project -OutputRoot (Join-Path $caseRoot 'capture') -Metadata $metadata}
		if($fault -ceq 'happy') {
			Check 'capture-real-journal-mocked-transport' {
				$receipt=& $action
				Assert-Rms ($receipt.statistics.median -eq 2.5 -and $receipt.statistics.p95 -eq 2.5 -and !$receipt.performanceAcceptance -and !$receipt.advancesSimulation) 'Capture discarded raw scope proof'
			}
		}else {Reject "capture-$fault" $action $expectedErrors[$fault]}
		Test-RmsPin $sourcePin
		Assert-Rms (!$script:rmsMock.calls.Contains('play.step') -and !$script:rmsMock.calls.Contains('profiler.capture.load')) 'Capture performed unauthorized simulation/load'
		$stops=$(if($fault -in @('session','epoch')){0}else{1})
		Assert-Rms ($script:rmsMock.stops -eq $stops) "Wrong capture cleanup ownership: $fault"
	}
}
$report=[pscustomobject]@{schemaVersion=1;status='TimingProtocolPassed';caseCount=$script:rmsTestCount;protocolOnly=[bool]$ProtocolOnly;nativeAcceptance=$false;performanceAcceptance=$false}
Write-RmsFreshJson (Join-Path $work 'result.json') $report
Write-Output "$($script:rmsTestCount) runtime memory safety timing protocol cases passed; no performance acceptance"
exit 0
