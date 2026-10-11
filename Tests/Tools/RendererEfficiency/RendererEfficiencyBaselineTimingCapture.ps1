# Dot-source this module from the parent collector. Tool accepts (name, arguments)
# and returns the successful automation envelope's data, or throws on API failure.
# The parent owns play entry/pause, scene readiness, installed-tool trust and exit.
function Invoke-RendererEfficiencyBaselineTimingCapture {
	[CmdletBinding()]
	param(
		[Parameter(Mandatory=$true)][scriptblock]$Tool,
		[Parameter(Mandatory=$true)][string]$InspectorPath,
		[Parameter(Mandatory=$true)][string]$ProjectRoot,
		[Parameter(Mandatory=$true)][string]$OutputRoot,
		[Parameter(Mandatory=$true)]$Metadata,
		[ValidateRange(1,3600)][int]$CaptureTimeoutSeconds=120,
		[ValidateRange(1,3600)][int]$PersistenceTimeoutSeconds=120,
		[ValidateRange(25,1000)][int]$PollMilliseconds=100
	)
	Set-StrictMode -Version Latest
	$ErrorActionPreference='Stop'
	function RequireField($Object,[string]$Name) {
		if($null -eq $Object -or $null -eq $Object.PSObject.Properties[$Name]) {
			if($Object -is [Collections.IDictionary] -and $Object.Contains($Name)){return $Object[$Name]}
			throw "Missing capture API field: $Name"
		}
		return $Object.$Name
	}
	function NoRedirect([string]$Path) {
		$cursor=[IO.Path]::GetFullPath($Path)
		while($cursor) {
			try { if(([IO.File]::GetAttributes($cursor) -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw "Redirected capture path: $cursor"} }
			catch [IO.FileNotFoundException] {} catch [IO.DirectoryNotFoundException] {}
			$parent=[IO.Path]::GetDirectoryName($cursor);if($parent -eq $cursor){break};$cursor=$parent
		}
	}
	function JournalProof([string]$Path) {
		# Finalize flushes the complete terminal but the engine retains its write
		# handle until clear/new capture. Permit that handle without permitting
		# incomplete evidence: exact byte brackets and the inspector remain gates.
		$stream=[IO.FileStream]::new($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
		$sha=[Security.Cryptography.SHA256]::Create()
		try {
			$length=$stream.Length;$digest=$sha.ComputeHash($stream)
			if($length -le 0 -or $stream.Length -ne $length){throw 'Journal length changed during hashing'}
			return [pscustomobject]@{bytes=$length;sha256=([BitConverter]::ToString($digest)).Replace('-','').ToLowerInvariant()}
		} finally {$sha.Dispose();$stream.Dispose()}
	}
	function CopyJournal([string]$Source,[string]$Destination) {
		$inputStream=[IO.FileStream]::new($Source,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
		try {
			$outputStream=[IO.FileStream]::new($Destination,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
			try {$inputStream.CopyTo($outputStream,65536);$outputStream.Flush($true)} finally {$outputStream.Dispose()}
		} finally {$inputStream.Dispose()}
	}
	$project=[IO.Path]::GetFullPath($ProjectRoot);$output=[IO.Path]::GetFullPath($OutputRoot)
	$inspector=[IO.Path]::GetFullPath($InspectorPath)
	if(![IO.Path]::IsPathRooted($InspectorPath) -or ![IO.File]::Exists($inspector)){throw 'Require an absolute installed inspector executable'}
	if($null -eq $Metadata){throw 'Parent role/fixture metadata is required'}
	NoRedirect $project;NoRedirect $output;NoRedirect $inspector
	if(![IO.Directory]::Exists($project) -or [IO.Directory]::Exists($output) -or [IO.File]::Exists($output)){throw 'Require an existing project and fresh output root'}
	if($output -ieq $project -or $output.StartsWith($project.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'Capture output must be outside the project'}
	$null=[IO.Directory]::CreateDirectory($output)
	$events=New-Object 'Collections.Generic.List[object]'
	function Call([string]$Name,$Arguments) {
		$reply=& $Tool $Name $Arguments
		$events.Add([pscustomobject]@{utc=[DateTime]::UtcNow.ToString('o');tool=$Name;arguments=$Arguments;data=$reply})
		return $reply
	}
	function AssertPaused {
		$state=Call 'play.state' @{}
		if((RequireField $state 'currentState') -cne 'paused' -or $null -ne (RequireField $state 'pendingTransition')){throw 'Play state is not stably paused'}
		return $state
	}
	function AwaitJob([string]$Id) {
		if([string]::IsNullOrWhiteSpace($Id)){throw 'Missing owned profiler job ID'}
		$clock=[Diagnostics.Stopwatch]::StartNew()
		do {
			$response=Call 'jobs.get' @{jobId=$Id};$job=RequireField $response 'job'
			if((RequireField $job 'jobId') -cne $Id){throw 'Profiler job ownership changed'}
			$state=RequireField $job 'state'
			if($state -ceq 'succeeded'){return $job}
			if($state -in @('failed','canceled')){throw "Profiler job ended $state"}
			if($clock.Elapsed.TotalSeconds -ge $PersistenceTimeoutSeconds){throw 'Profiler persistence/load deadline exceeded'}
			Start-Sleep -Milliseconds $PollMilliseconds
		} while($true)
	}
	$ownsRecording=$false;$ownershipVerified=$false;$receipt=$null;$epoch=$null;$session=$null
	try {
		$before=AssertPaused
		$initial=Call 'profiler.capture.health' @{}
		if((RequireField $initial 'state') -notin @('Idle','Loaded','Finalizing')){throw 'Another profiler capture owns the session'}
		# The engine retains Finalizing after a successful stop. Its actual active
		# operation flags, rather than that historical label, exclude an old save.
		$operations=Call 'profiler.capture.operation' @{}
		if((RequireField $operations 'projectEpoch') -ne (RequireField $initial 'projectEpoch')){throw 'Profiler project changed before capture'}
		foreach($operation in @(RequireField $operations 'operations')){if((RequireField $operation 'active') -ne $false){throw 'A profiler operation is still active'}}
		$start=Call 'profiler.capture.start' @{captureLevel='sampled';maxFrames=4096;sampleIntervalFrames=1}
		$ownsRecording=$true
		# Capture identity comes from the newly published service snapshot, not
		# from a generated name or an older saved-capture catalog entry.
		$admission=Call 'profiler.capture.health' @{}
		$epoch=RequireField $admission 'projectEpoch';$session=RequireField $admission 'sessionArtifactId'
		if((RequireField $admission 'state') -cne 'Recording' -or $epoch -le 0 -or $epoch -ne (RequireField $initial 'projectEpoch') -or [string]::IsNullOrWhiteSpace($session) -or $session -ceq (RequireField $initial 'sessionArtifactId')){throw 'Fresh capture identity was not published'}
		$ownershipVerified=$true
		$bounds=RequireField $start 'bounds'
		if((RequireField $start 'state') -cne 'succeeded' -or (RequireField $start 'captureLevel') -cne 'sampled' -or (RequireField $bounds 'maxFrames') -ne 4096 -or (RequireField $bounds 'sampleIntervalFrames') -ne 1){throw 'Capture settings were not admitted exactly'}
		$lastCount=0;$clock=[Diagnostics.Stopwatch]::StartNew()
		do {
			$null=AssertPaused;$health=Call 'profiler.capture.health' @{}
			if((RequireField $health 'state') -cne 'Recording' -or (RequireField $health 'terminalFailure') -ne $false -or (RequireField $health 'destinationWritable') -ne $true -or (RequireField $health 'rejectedCoreFrames') -ne 0){throw 'Profiler health cannot prove lossless recording'}
			$currentEpoch=RequireField $health 'projectEpoch';$currentSession=RequireField $health 'sessionArtifactId'
			if($currentEpoch -le 0 -or [string]::IsNullOrWhiteSpace($currentSession)){throw 'Profiler capture identity is unavailable'}
			if($epoch -ne $currentEpoch -or $session -cne $currentSession){throw 'Profiler capture project/session changed'}
			$count=RequireField $health 'committedFrames'
			if($count -lt $lastCount -or (RequireField $health 'producedFrames') -lt $count){throw 'Profiler frame counters are inconsistent'}
			$lastCount=$count;if($count -ge 360){break}
			if($clock.Elapsed.TotalSeconds -ge $CaptureTimeoutSeconds){throw 'Capture did not commit 360 actual frames before deadline'}
			Start-Sleep -Milliseconds $PollMilliseconds
		} while($true)
		$stop=Call 'profiler.capture.stop' @{};$ownsRecording=$false
		$job=AwaitJob (RequireField $stop 'jobId')
		$artifactId=RequireField (RequireField $job 'result') 'artifactId'
		if($artifactId -cne $session){throw 'Saved artifact does not belong to the recorded session'}
		$artifacts=@((RequireField $job 'artifacts') | Where-Object {$_.kind -ceq 'profiler_capture' -and $_.artifactId -ceq $artifactId})
		if($artifacts.Count -ne 1){throw 'Persistence omitted its unique actual journal artifact'}
		$name=RequireField (RequireField $artifacts[0] 'metadata') 'name'
		if((RequireField (RequireField $artifacts[0] 'metadata') 'frameCount') -lt 360){throw 'Persisted artifact contains fewer than 360 frames'}
		if([IO.Path]::GetFileName($name) -cne $name -or [IO.Path]::GetExtension($name) -cne '.teprof'){throw 'Saved journal filename is not a contained V1 artifact'}
		$after=AssertPaused
		$source=Join-Path (Join-Path $project 'Profiler') $name;NoRedirect $source
		$sourceBefore=JournalProof $source;$length=$sourceBefore.bytes;$hash=$sourceBefore.sha256
		$destination=Join-Path $output $name
		NoRedirect $destination
		CopyJournal $source $destination
		$sourceAfter=JournalProof $source;$copied=JournalProof $destination
		if($sourceAfter.bytes -ne $length -or $copied.bytes -ne $length -or $sourceAfter.sha256 -cne $hash -or $copied.sha256 -cne $hash){throw 'Journal bytes changed during owned copy'}
		# The native inspector checks raw frames, engine_cpu lane and sealed tail;
		# no summary API or scalar duration substitutes for these 360 records.
		$inspectorHash=(Get-FileHash -LiteralPath $inspector).Hash.ToLowerInvariant()
		$decoded=& $inspector --journal $destination --offset 0 --count 360
		if($LASTEXITCODE -ne 0){throw 'Installed raw journal inspection failed'}
		$inspection=($decoded -join "`n") | ConvertFrom-Json
		$frames=@(RequireField $inspection 'frames')
		if((RequireField $inspection 'complete') -ne $true -or (RequireField $inspection 'formatIdentifier') -cne 'profilerJournalV1' -or $frames.Count -ne 360){throw 'Inspector omitted the complete selected range'}
		for($i=0;$i -lt 360;$i++) {
			$frame=$frames[$i];$duration=[double](RequireField $frame 'durationMs')
			if((RequireField $frame 'complete') -ne $true -or ([double]::IsNaN($duration) -or [double]::IsInfinity($duration)) -or $duration -le 0){throw 'Inspector returned an invalid raw frame'}
			if($i -gt 0 -and $frame.frameIndex -ne ($frames[$i-1].frameIndex+1)){throw 'Selected raw frames are not consecutive'}
		}
		NoRedirect $destination;NoRedirect $source
		$sourceFinal=JournalProof $source;$copiedFinal=JournalProof $destination
		if($sourceFinal.bytes -ne $length -or $copiedFinal.bytes -ne $length -or $sourceFinal.sha256 -cne $hash -or $copiedFinal.sha256 -cne $hash -or (Get-FileHash -LiteralPath $inspector).Hash.ToLowerInvariant() -cne $inspectorHash){throw 'Inspection input/tool changed'}
		$final=AssertPaused
		$receipt=[pscustomobject]@{schemaVersion=1;status='PausedPlayJournalCaptured';metadata=$Metadata;simulationPolicy='PausedPlayNoExplicitStep';advancesSimulation=$false;warmupFrames=60;measuredFrames=300;selectedOffset=0;frames=$frames;journal=[pscustomobject]@{path=$destination;sourcePath=$source;bytes=$length;sha256=$hash;artifactId=$artifactId};inspector=[pscustomobject]@{path=$inspector;sha256=$inspectorHash};lifecycle=[pscustomobject]@{playBefore=$before;playAfterPersistence=$after;playAfterInspection=$final;start=$start;stop=$stop;persistenceJob=$job};events=$events.ToArray();nativeAcceptance=$false}
		return $receipt
	} finally {
		# On failure only stop our capture; never resume/stop play or change scene.
		if($ownsRecording){try {$cleanupHealth=Call 'profiler.capture.health' @{};if($ownershipVerified -and $null -ne $session -and (RequireField $cleanupHealth 'sessionArtifactId') -ceq $session -and (RequireField $cleanupHealth 'projectEpoch') -eq $epoch -and (RequireField $cleanupHealth 'state') -in @('Recording','Paused')){$null=Call 'profiler.capture.stop' @{}}else{throw 'Cleanup cannot prove ownership of the active capture'}} catch {$events.Add([pscustomobject]@{cleanupError=$_.Exception.Message})}}
		try {[IO.File]::WriteAllText((Join-Path $output 'capture-events.json'),(ConvertTo-Json -InputObject $events.ToArray() -Depth 40),[Text.UTF8Encoding]::new($false))} catch {if($null -ne $receipt){throw}}
	}
}
