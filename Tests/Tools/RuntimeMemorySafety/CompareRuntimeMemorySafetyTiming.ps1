<#
Validation-only timing protocol. Dot-source for capture validation, or invoke:
& ./CompareRuntimeMemorySafetyTiming.ps1 -Mode Calibrate -Reference $ordered18ReceiptPaths -OutputPath ./fresh-calibration.json
& ./CompareRuntimeMemorySafetyTiming.ps1 -Mode Compare -Candidate $ordered9ReceiptPaths -CalibrationPath ./sealed-calibration.json -OutputPath ./fresh-result.json
Missing evidence returns 2; invalid evidence returns 3; a regression returns 1.
The caller owns installed-process execution and independent rendering parity.
#>
[CmdletBinding()]
param(
	[ValidateSet('Calibrate','Compare')][string]$Mode='Compare',
	[string[]]$Reference=@(),
	[string[]]$Candidate=@(),
	[string]$CalibrationPath='',
	[string]$OutputPath=''
)
$script:RuntimeMemorySafetyComparatorPath=$PSCommandPath

function Get-RmsField($Value,[string]$Name) {
	if($Value -is [Collections.IDictionary] -and $Value.Contains($Name)){return $Value[$Name]}
	if($null -eq $Value -or $null -eq $Value.PSObject.Properties[$Name]){throw "Missing timing field: $Name"}
	return $Value.$Name
}
function Assert-Rms([bool]$Condition,[string]$Message) {if(!$Condition){throw $Message}}
function ConvertTo-RmsInstant($Value) {
	if($Value -is [DateTimeOffset]){return $Value}
	if($Value -is [DateTime]){return [DateTimeOffset]::new($Value)}
	return [DateTimeOffset]::Parse([string]$Value,[Globalization.CultureInfo]::InvariantCulture,[Globalization.DateTimeStyles]::RoundtripKind)
}
function Get-RmsNumber($Value,[string]$Name,[switch]$Positive) {
	$n=Get-RmsField $Value $Name
	Assert-Rms ($n -is [ValueType] -and $n -isnot [bool] -and $n -isnot [DateTime]) "Non-numeric timing field: $Name"
	$d=[double]$n
	Assert-Rms (![double]::IsNaN($d) -and ![double]::IsInfinity($d) -and $d -ge 0 -and (!$Positive -or $d -gt 0)) "Invalid timing number: $Name"
	return $d
}
function Assert-RmsOrdinaryPath([string]$Path) {
	$cursor=[IO.Path]::GetFullPath($Path)
	while($cursor) {
		try {Assert-Rms (([IO.File]::GetAttributes($cursor) -band [IO.FileAttributes]::ReparsePoint) -eq 0) "Redirected timing path: $cursor"}
		catch [IO.FileNotFoundException] {} catch [IO.DirectoryNotFoundException] {}
		$parent=[IO.Path]::GetDirectoryName($cursor);if($parent -eq $cursor){break};$cursor=$parent
	}
}
function Get-RmsPin([string]$Path) {
	Assert-Rms ([IO.Path]::IsPathRooted($Path)) 'Timing evidence paths must be absolute'
	Assert-RmsOrdinaryPath $Path
	if(![IO.File]::Exists($Path)){throw "UNVERIFIED: Missing timing evidence: $Path"}
	$file=Get-Item -LiteralPath $Path
	return [pscustomobject]@{path=[IO.Path]::GetFullPath($Path);bytes=$file.Length;sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
}
function Test-RmsPin($Pin) {
	$actual=Get-RmsPin (Get-RmsField $Pin 'path')
	Assert-Rms ($actual.sha256 -ceq (Get-RmsField $Pin 'sha256') -and $actual.bytes -eq (Get-RmsField $Pin 'bytes')) "Changed timing artifact: $($actual.path)"
}
function Write-RmsFreshJson([string]$Path,$Value) {
	Assert-Rms ([IO.Path]::IsPathRooted($Path)) 'Timing output must be an absolute path'
	Assert-RmsOrdinaryPath $Path
	$parent=[IO.Path]::GetDirectoryName($Path);$null=[IO.Directory]::CreateDirectory($parent)
	$stream=[IO.FileStream]::new($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
	try {$bytes=[Text.UTF8Encoding]::new($false).GetBytes((ConvertTo-Json -InputObject $Value -Depth 60));$stream.Write($bytes,0,$bytes.Length);$stream.Flush($true)} finally {$stream.Dispose()}
}

function Test-RuntimeMemorySafetyTimingFrames($Inspection) {
	# Recheck decoded JSON independently. Only the native decoder establishes the
	# binary seal and lane provenance; these checks prevent receipt substitutions.
	Assert-Rms ((Get-RmsField $Inspection 'formatIdentifier') -ceq 'profilerJournalV1' -and (Get-RmsField $Inspection 'complete') -eq $true) 'Incomplete or wrong timing journal'
	Assert-Rms ((Get-RmsField $Inspection 'metric') -ceq 'rendererPreparationScopeSumMs') 'Wrong timing metric'
	$window=Get-RmsField $Inspection 'window';$settings=Get-RmsField $Inspection 'captureSettings'
	foreach($pair in @(@('offset',0),@('count',360),@('warmupFrames',60),@('measuredFrames',300))){Assert-Rms ((Get-RmsField $window $pair[0]) -eq $pair[1]) 'Changed timing window'}
	Assert-Rms ((Get-RmsField $settings 'captureLevel') -ceq 'detailed' -and (Get-RmsField $settings 'sampleIntervalFrames') -eq 1 -and (Get-RmsField $settings 'proof') -ceq 'journalMetadata') 'Changed timing capture settings'
	$names=@('RenderSystem: Protected Publication Capture','RenderSystem: Geometry Route Preparation','RenderSystem: Virtual Geometry Preparation')
	$frames=@(Get-RmsField $Inspection 'frames');Assert-Rms ($frames.Count -eq 360) 'Timing window must contain 360 frames'
	$values=New-Object 'Collections.Generic.List[double]';$previousIndex=$null;$previousStart=$null
	for($i=0;$i -lt 360;$i++) {
		$frame=$frames[$i];Assert-Rms ((Get-RmsField $frame 'complete') -eq $true) 'Incomplete timing frame'
		$index=Get-RmsNumber $frame 'frameIndex';$start=Get-RmsNumber $frame 'frameStartMs';$duration=Get-RmsNumber $frame 'durationMs' -Positive
		Assert-Rms ($index -eq [Math]::Truncate($index) -and ($null -eq $previousIndex -or $index -eq $previousIndex+1) -and ($null -eq $previousStart -or $start -ge $previousStart)) 'Nonconsecutive or reordered timing frames'
		$previousIndex=$index;$previousStart=$start
		$lane=Get-RmsField $frame 'engineClockLane'
		Assert-Rms ((Get-RmsField $lane 'id') -ceq 'engine_cpu' -and (Get-RmsField $lane 'scope') -ceq 'EngineCpu' -and (Get-RmsField $lane 'source') -ceq 'ProfilerClock' -and (Get-RmsField $lane 'status') -ceq 'Valid') 'Wrong engine clock lane'
		foreach($field in @('label','statusReason')){$null=Get-RmsField $lane $field}
		$laneDuration=Get-RmsNumber $lane 'milliseconds' -Positive
		Assert-Rms ($laneDuration -eq $duration) 'Engine lane and frame duration differ'
		$phases=@(Get-RmsField $frame 'phaseSamples');Assert-Rms ($phases.Count -eq 3) 'Missing or duplicate preparation phases'
		$ids=@{};$end=0.0;$sum=0.0
		for($p=0;$p -lt 3;$p++) {
			$sample=$phases[$p];$id=Get-RmsNumber $sample 'sampleId';$sampleStart=Get-RmsNumber $sample 'startMs';$sampleDuration=Get-RmsNumber $sample 'durationMs' -Positive
			foreach($field in @('parentSampleId','threadLabel','taskLabel','depth','retainInAggregate')){$null=Get-RmsField $sample $field}
			Assert-Rms ((Get-RmsField $sample 'name') -ceq $names[$p] -and (Get-RmsField $sample 'moduleId') -ceq 'CpuEngine' -and (Get-RmsField $sample 'complete') -eq $true -and (Get-RmsField $sample 'occurrenceCount') -eq 1) 'Wrong, incomplete, aggregate or misordered phase'
			Assert-Rms ($id -eq [Math]::Truncate($id) -and !$ids.ContainsKey([string]$id)) 'Duplicate or invalid phase sample ID';$ids[[string]$id]=$true
			Assert-Rms ($sampleStart + 0.000002 -ge $end -and $sampleStart+$sampleDuration -le $duration+0.000002) 'Overlapping or out-of-frame phase'
			$end=$sampleStart+$sampleDuration;$sum+=$sampleDuration
		}
		$recorded=Get-RmsNumber $frame 'preparationMs' -Positive
		Assert-Rms ($recorded -eq $sum) 'Preparation value does not equal exact phase sum'
		if($i -ge 60){$values.Add($sum)}
	}
	$ordered=@($values.ToArray() | Sort-Object)
	return [pscustomobject]@{median=([double]$ordered[149]+[double]$ordered[150])/2.0;p95=[double]$ordered[284];measuredCount=300}
}

function Test-RmsTimingReceipt([string]$Path,[switch]$RequireParentEvidence) {
	$pin=Get-RmsPin $Path;$receipt=Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
	Assert-Rms ((Get-RmsField $receipt 'schemaVersion') -ceq 'runtime-memory-safety-timing-v1' -and (Get-RmsField $receipt 'status') -ceq 'Captured') 'Invalid timing receipt'
	Assert-Rms ((Get-RmsField $receipt 'simulationPolicy') -ceq 'PausedPlayNoExplicitStep' -and (Get-RmsField $receipt 'advancesSimulation') -eq $false) 'Changed simulation policy'
	$lifecycle=Get-RmsField $receipt 'lifecycle'
	$admission=Get-RmsField $lifecycle 'start';$bounds=Get-RmsField $admission 'bounds'
	# V1 persists detail level/cadence but not maxFrames. Check the real API
	# admission instead of inventing that field in decoded journal metadata.
	Assert-Rms ((Get-RmsField $admission 'state') -ceq 'succeeded' -and (Get-RmsField $admission 'captureLevel') -ceq 'detailed' -and (Get-RmsField $bounds 'maxFrames') -eq 4096 -and (Get-RmsField $bounds 'sampleIntervalFrames') -eq 1) 'Changed capture admission settings'
	foreach($name in @('playBefore','playAfterPersistence','playAfterInspection')) {
		$play=Get-RmsField $lifecycle $name
		Assert-Rms ((Get-RmsField $play 'currentState') -ceq 'paused' -and $null -eq (Get-RmsField $play 'pendingTransition')) 'Capture lifecycle was not stably paused'
	}
	$finalHealth=Get-RmsField $lifecycle 'finalHealth'
	Assert-Rms ((Get-RmsField $finalHealth 'terminalFailure') -eq $false -and (Get-RmsField $finalHealth 'rejectedCoreFrames') -eq 0 -and (Get-RmsField $finalHealth 'destinationWritable') -eq $true) 'Invalid final capture health'
	$session=Get-RmsField (Get-RmsField $receipt 'journal') 'artifactId'
	$identity=Get-RmsField $lifecycle 'captureIdentity';$epoch=Get-RmsNumber $identity 'projectEpoch' -Positive
	$finalActiveSession=Get-RmsField $finalHealth 'sessionArtifactId'
	Assert-Rms ((Get-RmsField $identity 'sessionArtifactId') -ceq $session -and ![string]::IsNullOrWhiteSpace($session) -and (Get-RmsField $finalHealth 'projectEpoch') -eq $epoch -and (Get-RmsField $finalHealth 'state') -ceq 'Finalizing' -and ([string]::IsNullOrEmpty($finalActiveSession) -or $finalActiveSession -ceq $session)) 'Final capture session identity mismatch'
	$stop=Get-RmsField $lifecycle 'stop';$job=Get-RmsField $lifecycle 'persistenceJob'
	Assert-Rms (![string]::IsNullOrWhiteSpace((Get-RmsField $stop 'jobId')) -and (Get-RmsField $job 'jobId') -ceq (Get-RmsField $stop 'jobId') -and (Get-RmsField $job 'state') -ceq 'succeeded' -and (Get-RmsField (Get-RmsField $job 'result') 'artifactId') -ceq $session) 'Saved capture is not the owned successful stop job'
	$saved=@(@(Get-RmsField $job 'artifacts')|Where-Object {$_.kind -ceq 'profiler_capture' -and $_.artifactId -ceq $session})
	Assert-Rms ($saved.Count -eq 1) 'Owned stop job omitted its unique saved artifact'
	Assert-Rms ((Get-RmsField (Get-RmsField $saved[0] 'metadata') 'name') -ceq [IO.Path]::GetFileName((Get-RmsField (Get-RmsField $receipt 'journal') 'path'))) 'Saved artifact filename differs from copied journal'
	$operations=Get-RmsField $lifecycle 'finalOperations'
	Assert-Rms ((Get-RmsField $operations 'projectEpoch') -eq $epoch) 'Final profiler operation epoch differs'
	foreach($operation in @(Get-RmsField $operations 'operations')){Assert-Rms ((Get-RmsField $operation 'active') -eq $false) 'Final profiler operation is active'}
	$committed=Get-RmsNumber $finalHealth 'committedFrames'
	Assert-Rms ($committed -ge 360 -and (Get-RmsNumber $finalHealth 'producedFrames') -eq $committed -and (Get-RmsField $finalHealth 'lastError') -ceq '') 'Final capture counters or error are invalid'
	foreach($name in @('queuedFrames','pendingBytes','inFlightBytes')){Assert-Rms ((Get-RmsNumber $finalHealth $name) -eq 0) 'Final capture still has pending frames or bytes'}
	$metadata=Get-RmsField $receipt 'metadata'
	Assert-Rms ((Get-RmsField $metadata 'scale') -cin @('small','normal','stress') -and (Get-RmsField $metadata 'role') -cin @('A','Aprime','C')) 'Invalid scale or timing role'
	$ordinal=Get-RmsNumber $metadata 'ordinal' -Positive;Assert-Rms ($ordinal -in @(1,2,3)) 'Invalid timing ordinal'
	foreach($name in @('fixtureSha256','settingsSha256','cameraLightSha256','environmentSha256')) {Assert-Rms ((Get-RmsField $metadata $name) -cmatch '^[a-f0-9]{64}$') "Missing frozen identity: $name"}
	foreach($name in @('sourceIdentity','installIdentity')){Assert-Rms (![string]::IsNullOrWhiteSpace((Get-RmsField $metadata $name))) "Missing runtime identity: $name"}
	$created=ConvertTo-RmsInstant (Get-RmsField $receipt 'createdUtc');$completed=ConvertTo-RmsInstant (Get-RmsField $receipt 'completedUtc')
	Assert-Rms ($completed -ge $created) 'Reversed capture timestamps'
	$dependencies=@((Get-RmsField $receipt 'journal'),(Get-RmsField $receipt 'inspector'))+@(Get-RmsField $receipt 'tools')
	$commonTools=@($dependencies|Select-Object -Skip 1)
	Assert-Rms ($dependencies.Count -ge 4) 'Capture omitted tool pins'
	foreach($dependency in $dependencies){Test-RmsPin $dependency}
	$processIdentity=$null
	if($RequireParentEvidence) {
		# The collector cannot prove normal process closure while the editor is
		# still alive. Parent evidence is required only when sealing/comparing.
		$proof=Get-RmsField $metadata 'parentEvidence';$provenance=@{}
		foreach($name in @('process','environment','settings','fixture','runtime','orchestrator')) {
			$evidencePin=Get-RmsField $proof $name;Test-RmsPin $evidencePin
			$provenance[$name]=$evidencePin;$dependencies+=,$evidencePin
		}
		foreach($pair in @(@('environment','environmentSha256'),@('settings','settingsSha256'),@('fixture','fixtureSha256'))) {
			Assert-Rms ($provenance[$pair[0]].sha256 -ceq (Get-RmsField $metadata $pair[1])) "Parent evidence hash differs: $($pair[0])"
		}
		$process=Get-Content -LiteralPath $provenance.process.path -Raw|ConvertFrom-Json
		$processId=Get-RmsNumber $process 'pid' -Positive;$parentId=Get-RmsNumber $process 'parentPid' -Positive
		Assert-Rms ($processId -eq [Math]::Truncate($processId) -and $parentId -eq [Math]::Truncate($parentId) -and $processId -ne $parentId) 'Invalid parent process identity'
		$processStart=ConvertTo-RmsInstant (Get-RmsField $process 'startUtc');Assert-Rms ($processStart -le $created) 'Capture predates process start'
		$processIdentity=([string]$processId)+':'+$processStart.UtcDateTime.Ticks
		$executable=Get-RmsField $process 'executablePath';$executablePin=Get-RmsPin $executable
		$runtime=Get-Content -LiteralPath $provenance.runtime.path -Raw|ConvertFrom-Json
		foreach($name in @('sourceIdentity','installIdentity')){Assert-Rms ((Get-RmsField $runtime $name) -ceq (Get-RmsField $metadata $name)) "Parent runtime identity differs: $name"}
		$runtimeFiles=@(Get-RmsField $runtime 'files');Assert-Rms ($runtimeFiles.Count -gt 0) 'Missing parent runtime closure'
		$runtimePaths=@{};$foundExecutable=$false
		foreach($file in $runtimeFiles) {
			Test-RmsPin $file;$runtimePath=[IO.Path]::GetFullPath((Get-RmsField $file 'path'))
			Assert-Rms (!$runtimePaths.ContainsKey($runtimePath)) 'Duplicate parent runtime file';$runtimePaths[$runtimePath]=$true
			$dependencies+=,$file;if($runtimePath -ieq $executablePin.path){$foundExecutable=$true}
		}
		Assert-Rms $foundExecutable 'Owned process executable is absent from parent runtime closure'
		$closePin=Get-RmsPin (Get-RmsField $metadata 'lifecyclePath');$close=Get-Content -LiteralPath $closePin.path -Raw|ConvertFrom-Json
		Assert-Rms ((Get-RmsField $close 'normalClose') -eq $true -and (Get-RmsField $close 'exitCode') -eq 0 -and (Get-RmsField $close 'timedOut') -eq $false) 'Parent process did not close normally'
		$dependencies+=,$closePin
		foreach($dependency in $dependencies){Test-RmsPin $dependency}
	}
	$statistics=Test-RuntimeMemorySafetyTimingFrames (Get-RmsField $receipt 'inspection')
	Test-RmsPin $pin
	return [pscustomobject]@{pin=$pin;metadata=$metadata;createdUtc=$created.ToString('o');completedUtc=$completed.ToString('o');statistics=$statistics;dependencies=$dependencies;commonTools=$commonTools;processIdentity=$processIdentity}
}
function Assert-RmsMatchingInputs($First,$Second) {
	foreach($name in @('scale','fixtureSha256','settingsSha256','cameraLightSha256','environmentSha256')){Assert-Rms ((Get-RmsField $First $name) -ceq (Get-RmsField $Second $name)) "Changed comparison input: $name"}
}
function Get-RmsToolIdentity($Record) {
	# Paths may differ across immutable installs. Compare the ordered tools by
	# byte identity; journal bytes intentionally differ from run to run.
	return (@($Record.commonTools | ForEach-Object { $_.sha256+':'+$_.bytes }) -join '|')
}
function New-RuntimeMemorySafetyCalibration([string[]]$Reference) {
	Assert-Rms ($Reference.Count -eq 18) 'UNVERIFIED: Require all 18 reference captures'
	$records=New-Object 'Collections.Generic.List[object]';$thresholds=New-Object 'Collections.Generic.List[object]';$cursor=0;$lastCompletion=$null;$seen=@{};$processes=@{};$identity=$null
	foreach($scale in @('small','normal','stress')) {
		$pairs=New-Object 'Collections.Generic.List[object]';$scaleIdentity=$null
		for($ordinal=1;$ordinal -le 3;$ordinal++) {
			$pair=@()
			foreach($role in @('A','Aprime')) {
				$record=Test-RmsTimingReceipt $Reference[$cursor] -RequireParentEvidence;$cursor++
				Assert-Rms (!$processes.ContainsKey($record.processIdentity)) 'Reference requires a fresh unique process per capture';$processes[$record.processIdentity]=$true
				Assert-Rms (!$seen.ContainsKey($record.pin.path)) 'Repeated reference receipt';$seen[$record.pin.path]=$true
				Assert-Rms ($record.metadata.scale -ceq $scale -and $record.metadata.role -ceq $role -and $record.metadata.ordinal -eq $ordinal) 'Reference order must be A1,Aprime1,A2,Aprime2,A3,Aprime3 per small/normal/stress'
				if($null -ne $lastCompletion){Assert-Rms ((ConvertTo-RmsInstant $record.createdUtc) -ge $lastCompletion) 'Reference captures overlap or are reordered'}
				$lastCompletion=ConvertTo-RmsInstant $record.completedUtc
				if($null -ne $scaleIdentity){Assert-RmsMatchingInputs $scaleIdentity $record.metadata}else{$scaleIdentity=$record.metadata}
				if($null -ne $identity){Assert-Rms ($record.metadata.sourceIdentity -ceq $identity.metadata.sourceIdentity -and $record.metadata.installIdentity -ceq $identity.metadata.installIdentity -and (Get-RmsToolIdentity $record) -ceq (Get-RmsToolIdentity $identity)) 'Reference runtime or common tools changed'}else{$identity=$record}
				$records.Add($record);$pair+=,$record
			}
			$pairs.Add($pair)
		}
		foreach($metric in @('median','p95')) {
			$noise=0.0;foreach($pair in $pairs){$noise=[Math]::Max($noise,[Math]::Abs([double]$pair[1].statistics.$metric/[double]$pair[0].statistics.$metric-1.0))}
			$thresholds.Add([pscustomobject]@{scale=$scale;metric=$metric;noise=$noise;limit=[Math]::Max(0.02,$noise)})
		}
	}
	return [pscustomobject]@{schemaVersion='runtime-memory-safety-calibration-v1';status='Calibrated';createdUtc=[DateTime]::UtcNow.ToString('o');metric='rendererPreparationScopeSumMs';reference=$records.ToArray();thresholds=$thresholds.ToArray();comparator=(Get-RmsPin $script:RuntimeMemorySafetyComparatorPath);performanceAcceptance=$false}
}
function Compare-RuntimeMemorySafetyTiming([string]$CalibrationPath,[string[]]$Candidate) {
	Assert-Rms ($Candidate.Count -eq 9) 'UNVERIFIED: Require all nine candidate captures'
	$pin=Get-RmsPin $CalibrationPath;$sealed=Get-Content -LiteralPath $CalibrationPath -Raw|ConvertFrom-Json
	Assert-Rms ((Get-RmsField $sealed 'schemaVersion') -ceq 'runtime-memory-safety-calibration-v1' -and (Get-RmsField $sealed 'status') -ceq 'Calibrated') 'Invalid calibration receipt'
	Test-RmsPin (Get-RmsField $sealed 'comparator')
	Assert-Rms ((Get-RmsPin $script:RuntimeMemorySafetyComparatorPath).sha256 -ceq $sealed.comparator.sha256) 'Comparator changed after calibration'
	$refs=@(Get-RmsField $sealed 'reference');Assert-Rms ($refs.Count -eq 18) 'Invalid sealed reference count'
	foreach($ref in $refs){Test-RmsPin (Get-RmsField $ref 'pin')}
	# Recompute only from sealed reference bytes to detect tampered thresholds or
	# statistics. Candidate values never participate in noise calibration.
	$verified=New-RuntimeMemorySafetyCalibration @($refs | ForEach-Object {$_.pin.path})
	Assert-Rms ((ConvertTo-Json -InputObject $sealed.thresholds -Depth 8 -Compress) -ceq (ConvertTo-Json -InputObject $verified.thresholds -Depth 8 -Compress)) 'Altered calibration thresholds'
	for($r=0;$r -lt 18;$r++) {
		$old=$refs[$r];$fresh=$verified.reference[$r]
		# JSON readers differ in whether ISO timestamps become strings or dates.
		# Compare instants numerically while retaining exact pins and statistics.
		foreach($name in @('pin','metadata','statistics','dependencies','commonTools','processIdentity')) {
			Assert-Rms ((ConvertTo-Json -InputObject (Get-RmsField $old $name) -Depth 20 -Compress) -ceq (ConvertTo-Json -InputObject (Get-RmsField $fresh $name) -Depth 20 -Compress)) 'Altered calibration reference records'
		}
		foreach($name in @('createdUtc','completedUtc')) {Assert-Rms ((ConvertTo-RmsInstant (Get-RmsField $old $name)) -eq (ConvertTo-RmsInstant (Get-RmsField $fresh $name))) 'Altered calibration timestamps'}
	}
	$results=New-Object 'Collections.Generic.List[object]';$records=New-Object 'Collections.Generic.List[object]';$cursor=0;$passed=$true;$previousCompletion=$null;$runtime=$null;$seen=@{};$processes=@{}
	foreach($ref in $verified.reference){$processes[$ref.processIdentity]=$true}
	foreach($scale in @('small','normal','stress')) {
		for($ordinal=1;$ordinal -le 3;$ordinal++) {
			$c=Test-RmsTimingReceipt $Candidate[$cursor] -RequireParentEvidence;$cursor++
			Assert-Rms (!$processes.ContainsKey($c.processIdentity)) 'Candidate requires fresh process disjoint from reference';$processes[$c.processIdentity]=$true
			Assert-Rms (!$seen.ContainsKey($c.pin.path)) 'Repeated candidate receipt';$seen[$c.pin.path]=$true
			Assert-Rms ($c.metadata.scale -ceq $scale -and $c.metadata.role -ceq 'C' -and $c.metadata.ordinal -eq $ordinal) 'Candidate order must be C1,C2,C3 per small/normal/stress'
			$a=@($verified.reference|Where-Object {$_.metadata.scale -ceq $scale -and $_.metadata.role -ceq 'A' -and $_.metadata.ordinal -eq $ordinal})[0]
			Assert-RmsMatchingInputs $a.metadata $c.metadata
			Assert-Rms ((Get-RmsToolIdentity $a) -ceq (Get-RmsToolIdentity $c)) 'Candidate common tools changed'
			Assert-Rms ((ConvertTo-RmsInstant $c.createdUtc) -ge (ConvertTo-RmsInstant $sealed.createdUtc)) 'Candidate predates sealed calibration'
			if($null -ne $previousCompletion){Assert-Rms ((ConvertTo-RmsInstant $c.createdUtc) -ge $previousCompletion) 'Candidate captures overlap or are reordered'}
			$previousCompletion=ConvertTo-RmsInstant $c.completedUtc
			if($null -ne $runtime){Assert-Rms ($c.metadata.sourceIdentity -ceq $runtime.sourceIdentity -and $c.metadata.installIdentity -ceq $runtime.installIdentity) 'Candidate runtime identity changed'}else{$runtime=$c.metadata}
			foreach($metric in @('median','p95')) {
				$t=@($verified.thresholds|Where-Object {$_.scale -ceq $scale -and $_.metric -ceq $metric})[0]
				$regression=[double]$c.statistics.$metric/[double]$a.statistics.$metric-1.0
				# The normative ratio comparison uses unrounded doubles, including
				# threshold equality. Do not add a timing acceptance epsilon.
				$boundary=[double]$a.statistics.$metric*(1.0+[double]$t.limit)
				$ok=[double]$c.statistics.$metric -le $boundary;$passed=$passed -and $ok
				$results.Add([pscustomobject]@{scale=$scale;ordinal=$ordinal;metric=$metric;reference=$a.statistics.$metric;candidate=$c.statistics.$metric;noise=$t.noise;limit=$t.limit;boundaryMs=$boundary;regression=$regression;passed=$ok;elapsedSinceReferenceSeconds=((ConvertTo-RmsInstant $c.createdUtc)-(ConvertTo-RmsInstant $a.createdUtc)).TotalSeconds})
			}
			$records.Add($c)
		}
	}
	Test-RmsPin $pin
	return [pscustomobject]@{schemaVersion='runtime-memory-safety-timing-comparison-v1';status=$(if($passed){'Passed'}else{'Failed'});createdUtc=[DateTime]::UtcNow.ToString('o');calibration=$pin;candidate=$records.ToArray();comparisons=$results.ToArray();comparisonCount=$results.Count;timingPassed=$passed;parityAcceptance=$false;performanceAcceptance=$false}
}

if($MyInvocation.InvocationName -ne '.') {
	Set-StrictMode -Version Latest;$ErrorActionPreference='Stop';$exitCode=3;$result=$null
	try {
		Assert-Rms (![string]::IsNullOrWhiteSpace($OutputPath)) 'Require a fresh absolute OutputPath'
		if($Mode -ceq 'Calibrate'){$result=New-RuntimeMemorySafetyCalibration -Reference $Reference;$exitCode=0}
		else {$result=Compare-RuntimeMemorySafetyTiming -CalibrationPath $CalibrationPath -Candidate $Candidate;$exitCode=$(if($result.timingPassed){0}else{1})}
	} catch {$reason=$_.Exception.Message;$exitCode=$(if($reason.StartsWith('UNVERIFIED:')){2}else{3});$result=[pscustomobject]@{schemaVersion='runtime-memory-safety-timing-error-v1';status=$(if($exitCode -eq 2){'Unverified'}else{'Failed'});reason=$reason;performanceAcceptance=$false}}
	try {Write-RmsFreshJson $OutputPath $result} catch {Write-Error $_ -ErrorAction Continue;exit 3}
	Write-Output ($result|ConvertTo-Json -Depth 5 -Compress);exit $exitCode
}
