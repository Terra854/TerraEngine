# Shared, side-effect-free evidence validation. The installed runner and protocol
# regression script use the same checks, so malformed receipts cannot pass only
# because a test duplicated a weaker interpretation of the acceptance contract.
Set-StrictMode -Version Latest
function ConvertTo-AudioManifestTimestamp($Value) {
	# PowerShell versions may decode an ISO JSON timestamp as DateTime rather
	# than string. Preserve the typed value instead of culture-formatting it.
	if ($Value -is [DateTimeOffset]) { return $Value.ToUniversalTime().ToString('o',[Globalization.CultureInfo]::InvariantCulture) }
	if ($Value -is [DateTime]) {
		if ($Value.Kind -eq [DateTimeKind]::Unspecified) { throw 'Manifest timestamp has no UTC/offset identity.' }
		return ([DateTimeOffset]::new($Value)).ToUniversalTime().ToString('o',[Globalization.CultureInfo]::InvariantCulture)
	}
	if ($Value -isnot [string] -or $Value -notmatch '^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(\.\d{1,7})?(Z|[+-]\d{2}:\d{2})$') { throw 'Manifest timestamp must be an ISO instant.' }
	$parsed=[DateTimeOffset]::MinValue
	if (![DateTimeOffset]::TryParse($Value,[Globalization.CultureInfo]::InvariantCulture,[Globalization.DateTimeStyles]::None,[ref]$parsed)) { throw 'Manifest timestamp is invalid.' }
	return $parsed.ToUniversalTime().ToString('o',[Globalization.CultureInfo]::InvariantCulture)
}
function Assert-AudioFreshWorkRoot([string]$Path) {
	if (Test-Path -LiteralPath $Path) { throw 'WorkRoot must be fresh; previous evidence is never overwritten.' }
}
function Assert-AudioArtifact([string]$Path,$Entry) {
	if (!(Test-Path -LiteralPath $Path -PathType Leaf) -or (Get-Item -LiteralPath $Path).Length -ne [long]$Entry.length -or
		(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Entry.sha256) { throw "Missing/hash-mismatched artifact: $Path" }
}
function Assert-AudioEndpoint($Listeners,[int]$OwnedPid,[string]$ActualProject,[string]$ExpectedProject) {
	if (!@($Listeners).Count -or @($Listeners | Where-Object { $_.OwningProcess -ne $OwnedPid }).Count -or
		!$ActualProject -or [IO.Path]::GetFullPath($ActualProject) -ne [IO.Path]::GetFullPath($ExpectedProject)) { throw 'Wrong endpoint or project ownership.' }
}
function Assert-AudioSelection([int]$Count,[int]$Floor) {
	if ($Floor -lt 1 -or $Count -lt $Floor) { throw 'Missing/under-floor native selection.' }
}
function Assert-AudioOrderlyExit($Watch) {
	if ($Watch.timedOut -or $Watch.exitCode -ne 0 -or $Watch.activeProcessesAfterRootExit -ne 0) { throw 'Child terminated abnormally or required watchdog cleanup.' }
}
function Assert-AudioManagedEvidence($Value) {
	if (!$Value -or $Value.schemaVersion -ne 1 -or !$Value.passed -or $Value.failure -or
		!$Value.instanceId -or $Value.updates -lt 1 -or $Value.positiveDeltaFrames -lt 3) { throw 'Missing actual managed callbacks or positive-delta frames.' }
	$required=@('autoplay-observed','autoplay-advanced','fresh-facade-pause','pause-observed','fresh-facade-resume','resume-advanced','fresh-facade-stop','stop-observed','pending-play','pending-pause','pending-play-pause-observed','other-producer-pause','other-producer-controlled','stale-facade-rejected','replacement-pause','replacement-controlled')
	foreach ($name in $required) { if (@($Value.stages | Where-Object { $_.name -eq $name }).Count -ne 1) { throw "Missing/duplicate managed stage $name" } }
	$autoplay=($Value.stages | Where-Object name -eq 'autoplay-observed').state
	$advanced=($Value.stages | Where-Object name -eq 'autoplay-advanced').state
	$paused=($Value.stages | Where-Object name -eq 'pause-observed').state
	$resumed=($Value.stages | Where-Object name -eq 'resume-advanced').state
	$stopped=($Value.stages | Where-Object name -eq 'stop-observed').state
	if (!$autoplay.Audible -or $autoplay.PlaybackGeneration -eq 0 -or $advanced.LogicalFrame -le $autoplay.LogicalFrame -or
		$paused.PlaybackGeneration -ne $autoplay.PlaybackGeneration -or $paused.State -ne 2 -or
		$resumed.PlaybackGeneration -ne $paused.PlaybackGeneration -or $resumed.LogicalFrame -le $paused.LogicalFrame -or
		$stopped.State -ne 0 -or $stopped.LogicalFrame -ne 0) { throw 'Managed cursor/playback observations do not establish control continuity.' }
	$pauseCommand=($Value.stages | Where-Object name -eq 'fresh-facade-pause').command
	if ($pauseCommand.ResolvedPlaybackGeneration -ne $autoplay.PlaybackGeneration) { throw 'Fresh facade receipt did not resolve the observed autoplay instance.' }
	$stale=($Value.stages | Where-Object name -eq 'stale-facade-rejected').command
	if ($stale.Status -notin @(3,4)) { throw 'The stale attachment did not reject with missing/stale target.' }
}
function Assert-AudioContinuityEvidence($Value) {
	if (!$Value.passed -or @($Value.runs).Count -ne 3) { throw 'Incomplete continuity evidence.' }
	foreach ($producers in @(1,2,8)) {
		$runs=@($Value.runs | Where-Object producers -eq $producers)
		if ($runs.Count -ne 1) { throw 'Missing actual producer-count lane.' }; $run=$runs[0]
		if ($run.requests -lt 20000 -or $run.applied -ne $run.requests -or $run.duplicateApplications -ne 0 -or
			$run.pendingPeak -lt 1 -or $run.pendingPeak -gt 4096 -or $run.completedPeak -ne 4096 -or
			$run.evictions -ne ($run.requests-4096) -or $run.terminalOutcomes -ne $run.requests) { throw 'Incomplete retention/application evidence.' }
	}
}
function Assert-AudioSettingsEvidence($Value) {
	if (!$Value.passed -or $Value.frameCount -lt 120 -or $Value.frameCount -ne @($Value.frames).Count) { throw 'Missing/mismatched 120-frame source-setting evidence.' }
	$previousFrame=$null
	foreach ($frame in $Value.frames) {
		# JSON numbers must remain numbers: casts alone would admit strings or
		# NaN/Infinity and could let default-valued rows masquerade as the target.
		foreach ($field in @('frame','expectedVolume','actualVolume','expectedPitch','actualPitch')) {
			$number=$frame.$field
			if ($null -eq $number -or [Type]::GetTypeCode($number.GetType()) -notin @(
				[TypeCode]::Byte,[TypeCode]::SByte,[TypeCode]::Int16,[TypeCode]::UInt16,[TypeCode]::Int32,[TypeCode]::UInt32,
				[TypeCode]::Int64,[TypeCode]::UInt64,[TypeCode]::Single,[TypeCode]::Double,[TypeCode]::Decimal) -or
				![double]::IsFinite([double]$number)) { throw "Source-setting field $field is not a finite number." }
		}
		$currentFrame=[decimal]$frame.frame
		if ($currentFrame -lt 1 -or $currentFrame -ne [decimal]::Truncate($currentFrame) -or
			($null -ne $previousFrame -and $currentFrame -ne $previousFrame+1)) { throw 'Source-setting frames must be positive, strictly consecutive observations.' }
		$previousFrame=$currentFrame
		if ($frame.expectedVolume -ne 0.25 -or $frame.expectedPitch -ne 1.5) { throw 'Source-setting rows did not observe the required changed target fields.' }
		if ($frame.expectedVolume -ne $frame.actualVolume -or $frame.expectedPitch -ne $frame.actualPitch) { throw 'Source settings changed during persistence observations.' }
	}
	foreach ($flag in @('rollbackVerified','authoredOverrideVerified','retirementVerified','independentFieldsVerified')) {
		if (!$Value.$flag) { throw "Missing setting acceptance proof: $flag" }
	}
}
function Assert-AudioAllocationEvidence($Value,[int]$Repetitions=100) {
	if (!$Value.normalExit -or $Repetitions -lt 100 -or $Value.repetitions -lt $Repetitions -or @($Value.lanes).Count -lt 10 -or
		$Value.checkpoints -lt (10*$Repetitions)) { throw 'Missing mandatory allocation lane/checkpoint/repetition evidence.' }
	foreach ($checkpoint in @('rejection','scratch','receipt-success','receipt-rejection','seal-null','seal-rejected','seal-valid','publish-success','publish-session','publish-stale')) {
		$records=@($Value.records | Where-Object lane -eq $checkpoint)
		if ($records.Count -ne $Value.repetitions -or @($records.iteration | Sort-Object -Unique).Count -ne $Value.repetitions) { throw "Allocation checkpoint incomplete: $checkpoint" }
		$outcomes=@{'rejection'=9;'scratch'=2;'receipt-success'=0;'receipt-rejection'=9;'seal-null'=4;'seal-rejected'=9;'seal-valid'=0;'publish-success'=0;'publish-session'=9;'publish-stale'=4}
		foreach ($record in $records) {
			if (!$record.healthyFollowup -or $record.outcome -ne $outcomes[$checkpoint] -or
				($record.outcome -ne 0 -and $record.rootBefore -ne $record.rootAfter)) { throw 'Inconsistent allocation outcome/root/recovery evidence.' }
			if ($checkpoint -eq 'scratch') {
				if ($record.attempts -lt 1 -or $record.failedAllocations -lt 1) { throw 'Scratch did not encounter a real allocation failure.' }
			} elseif ($record.attempts -ne 0 -or $record.failedAllocations -ne 0) { throw 'An allocation-free checkpoint attempted allocation.' }
		}
	}
	if (@($Value.records | Where-Object { $_.failedAllocations -gt 0 }).Count -lt $Repetitions) { throw 'No real allocation failure was observed.' }
}

