[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string]$Reference,
	[Parameter(Mandatory=$true)][string]$Candidate,
	[Parameter(Mandatory=$true)][string]$ArchiveClosureSha256,
	[Parameter(Mandatory=$true)][string]$JournalInspector,
	[Parameter(Mandatory=$true)][string]$JournalInspectorSha256,
	[Parameter(Mandatory=$true)][string]$Output,
	[string]$Manifest = ''
)

# Timing-only receipt v1, independent of native stage evidence:
# root: schemaVersion, role, metricDomain, clock, boundary, warmupFrames,
# measuredFrames, runsPerFixture, installClosureSha256, archiveClosureSha256, runs.
# runs: fixtureId, seed, preparation, runIndex, viewIds (manifest camera IDs),
# conditions {adapter,driver,resolution,cameraHash,sceneHash,inputHash,settings,
# simulationPolicy {deterministic,fixedDeltaTimeSeconds,temporalJitterEnabled,
# animationPolicy}; frozen policy instead has mode,advancesSimulation,deterministic,
# temporalJitterEnabled,animationPolicy}, journal {path,sha256,bytes,startOrdinal}, frames.
# Each frame is {frameIndex,durationMs,complete}. All 360 are consecutive and
# complete; the final 300 are measured. Neither thresholds nor summaries are inputs.
# The trusted, SHA-pinned CPU inspector must parse actual JournalV1 bytes using
# the native reader: --journal <path> --offset <ordinal> --count 360. Its JSON is
# {formatIdentifier,formatVersion,complete,frames}. Recovered or
# incomplete journals cannot pass. Raw receipt frames must equal this real index.
# Example: invoke the installed script with two closed timing receipts, the
# independently qualified archive closure SHA, and the installed inspector SHA.
# A Passed result is only a timing gate; stage pixels, budgets and full acceptance
# still require separate native evidence and parent review.

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$utf8 = [Text.UTF8Encoding]::new($false)
$boundary = 'Engine::Update.BeginFrame-EndFrame.ExcludingEditorWork'
$result = [ordered]@{schemaVersion=1;status='Invalid';scope='Whole engine-frame wall timing only';metricDomain='engine_cpu';clock='ProfilerClock';boundary=$boundary;maximumRatio=1.05;stageProof=$false;fullAcceptance=$false;comparisons=@();reason='';exitCode=3}

function Require([bool]$Condition, [string]$Reason) { if(!$Condition) { throw $Reason } }
function Keys($Value, [string[]]$Expected) {
	Require ($null -ne $Value -and $Value -is [pscustomobject]) 'Expected a JSON object'
	Require ((@($Value.PSObject.Properties.Name|Sort-Object) -join '|') -ceq (@($Expected|Sort-Object) -join '|')) 'Missing or unexpected receipt field'
}
function Sha([string]$Value) { Require ($Value -cmatch '^[0-9a-f]{64}$') 'SHA256 must be canonical lowercase hexadecimal' }
function NoRedirect([string]$Path) {
	# Check every existing ancestor freshly, including missing destination paths.
	$walk = [IO.Path]::GetFullPath($Path)
	while($walk) {
		try { Require (([IO.File]::GetAttributes($walk) -band [IO.FileAttributes]::ReparsePoint) -eq 0) 'Redirected evidence path' }
		catch [IO.FileNotFoundException] { }
		catch [IO.DirectoryNotFoundException] { }
		$walk = [IO.Path]::GetDirectoryName($walk)
	}
}
function Hash([string]$Path) {
	NoRedirect $Path
	$stream = [IO.File]::OpenRead($Path)
	$algorithm = [Security.Cryptography.SHA256]::Create()
	try { return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-','').ToLowerInvariant() }
	finally { $algorithm.Dispose(); $stream.Dispose() }
}
function ReadBound([string]$Path) {
	Require ([IO.FileInfo]$Path).Exists 'Missing timing receipt'
	Require (([IO.FileInfo]$Path).Length -le 16777216) 'Oversized timing receipt'
	$before = Hash $Path
	$value = [IO.File]::ReadAllText($Path) | ConvertFrom-Json
	Require ((Hash $Path) -ceq $before) 'Timing input changed while reading'
	return @{value=$value;sha256=$before;path=[IO.Path]::GetFullPath($Path)}
}
function Canonical($Value) {
	if($null -eq $Value) { return 'null' }
	if($Value -is [pscustomobject]) {
		$members=@();foreach($property in @($Value.PSObject.Properties|Sort-Object Name)) { $members+=($property.Name|ConvertTo-Json -Compress)+':'+(Canonical $property.Value) }
		return '{'+($members -join ',')+'}'
	}
	if($Value -is [array]) { return '['+((@($Value|ForEach-Object {Canonical $_})) -join ',')+']' }
	return ConvertTo-Json -InputObject $Value -Compress
}
function Positive($Value, [string]$Name) {
	Require ($Value -is [ValueType] -and $Value -isnot [bool]) "Nonnumeric $Name"
	$number=[double]$Value
	Require (![double]::IsNaN($number) -and ![double]::IsInfinity($number) -and $number -gt 0) "Nonfinite or nonpositive $Name"
	return $number
}
function Unsigned($Value, [string]$Name) {
	Require ($Value -is [int] -or $Value -is [long] -or $Value -is [uint64] -or $Value -is [uint32]) "Noninteger $Name"
	Require ([decimal]$Value -ge 0 -and [decimal]$Value -le 9007199254740991) "Invalid $Name"
	return [long]$Value
}
function Frames($Frames) {
	Require (@($Frames).Count -eq 360) 'Exactly 60 warmup and 300 measured frames are required'
	$previous=$null
	foreach($frame in $Frames) {
		Keys $frame @('frameIndex','durationMs','complete')
		$index=Unsigned $frame.frameIndex frameIndex
		Require ($frame.complete -is [bool] -and $frame.complete) 'Incomplete frame'
		$null=Positive $frame.durationMs durationMs
		if($null -ne $previous) { Require ($index -eq $previous+1) 'Duplicate or nonconsecutive frame index' }
		$previous=$index
	}
}
function Journal($Run, [string]$Directory) {
	Keys $Run.journal @('path','sha256','bytes','startOrdinal')
	Require ($Run.journal.path -is [string] -and ![IO.Path]::IsPathRooted($Run.journal.path)) 'Journal path must be relative'
	$path=[IO.Path]::GetFullPath((Join-Path $Directory $Run.journal.path))
	Require ($path.StartsWith($Directory.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) 'Journal escaped receipt directory'
	Sha $Run.journal.sha256
	$bytes=Unsigned $Run.journal.bytes journalBytes
	$offset=Unsigned $Run.journal.startOrdinal startOrdinal
	Require ($bytes -gt 0 -and ([IO.FileInfo]$path).Length -eq $bytes) 'Journal byte length mismatch'
	Require ((Hash $path) -ceq $Run.journal.sha256) 'Journal SHA mismatch'
	Require ((Hash $JournalInspector) -ceq $JournalInspectorSha256) 'Trusted journal inspector changed'
	# Only the caller-pinned native parser establishes journal membership. A file
	# hash paired with unrelated JSON durations is deliberately insufficient.
	$raw = & $JournalInspector --journal $path --offset $offset --count 360
	Require ($LASTEXITCODE -eq 0) 'Journal inspector failed'
	$parsed = ($raw -join "`n") | ConvertFrom-Json
	Keys $parsed @('formatIdentifier','formatVersion','complete','frames')
	Require ($parsed.formatIdentifier -is [string] -and $parsed.formatIdentifier -ceq 'profilerJournalV1' -and (Unsigned $parsed.formatVersion formatVersion) -eq 1 -and $parsed.complete -is [bool] -and $parsed.complete) 'Journal is not finalized complete V1'
	Frames $parsed.frames
	for($i=0;$i -lt 360;++$i) {
		Require ($parsed.frames[$i].frameIndex -eq $Run.frames[$i].frameIndex -and [double]$parsed.frames[$i].durationMs -eq [double]$Run.frames[$i].durationMs) 'Raw timings do not match actual journal index'
	}
	Require ((Hash $path) -ceq $Run.journal.sha256 -and (Hash $JournalInspector) -ceq $JournalInspectorSha256) 'Journal/parser changed while inspecting'
}
function Receipt($Document, [string]$Role, [string]$Directory, $Workloads) {
	Keys $Document @('schemaVersion','role','metricDomain','clock','boundary','warmupFrames','measuredFrames','runsPerFixture','installClosureSha256','archiveClosureSha256','runs')
	Require ((Unsigned $Document.schemaVersion schemaVersion) -eq 1 -and $Document.role -is [string] -and $Document.role -ceq $Role -and $Document.metricDomain -is [string] -and $Document.metricDomain -ceq 'engine_cpu' -and $Document.clock -is [string] -and $Document.clock -ceq 'ProfilerClock' -and $Document.boundary -is [string] -and $Document.boundary -ceq $boundary) 'Unsupported timing metric or boundary'
	Require ((Unsigned $Document.warmupFrames warmupFrames) -eq 60 -and (Unsigned $Document.measuredFrames measuredFrames) -eq 300 -and (Unsigned $Document.runsPerFixture runsPerFixture) -eq 3) 'Frozen timing window changed'
	Sha $Document.installClosureSha256;Sha $Document.archiveClosureSha256
	Require ($Document.archiveClosureSha256 -ceq $ArchiveClosureSha256) 'Wrong independent archive identity'
	Require (@($Document.runs).Count -eq 30) 'Five fixtures require three runs in both preparation modes'
	$indexed=@{};$journals=@{};$journalHashes=@{}
	foreach($run in $Document.runs) {
		Keys $run @('fixtureId','seed','preparation','runIndex','viewIds','conditions','journal','frames')
		$workload=@($Workloads|Where-Object id -CEQ $run.fixtureId)
		Require ($workload.Count -eq 1 -and (Unsigned $run.seed seed) -eq $workload[0].seed) 'Unknown fixture or wrong seed'
		Require ($run.preparation -is [string] -and $run.preparation -cin @('Serial','Concurrent') -and (Unsigned $run.runIndex runIndex) -lt 3) 'Wrong preparation/run'
		Require ((Canonical @($run.viewIds|Sort-Object)) -ceq (Canonical @($workload[0].cameraIds|Sort-Object))) 'Missing or unexpected views'
		Keys $run.conditions @('adapter','driver','resolution','cameraHash','sceneHash','inputHash','settings','simulationPolicy')
		foreach($field in @('adapter','driver')) { Require ($run.conditions.$field -is [string] -and ![string]::IsNullOrWhiteSpace($run.conditions.$field)) 'Missing adapter/driver' }
		foreach($field in @('cameraHash','sceneHash','inputHash')) { Sha $run.conditions.$field }
		Keys $run.conditions.resolution @('width','height')
		Require ((Unsigned $run.conditions.resolution.width width) -eq 1920 -and (Unsigned $run.conditions.resolution.height height) -eq 1080) 'Wrong representative resolution'
		Require ($run.conditions.settings -is [pscustomobject] -and @($run.conditions.settings.PSObject.Properties).Count -gt 0) 'Missing matched settings'
		$policy=$run.conditions.simulationPolicy
		# A frozen scene advances no simulation while complete engine frames are
		# timed. Wall-derived delta may remain nonzero: explicit advancement state
		# distinguishes a paused scene from fixed-step animation. Older receipts keep
		# their original exact key set for compatibility.
		$frozen = $null -ne $policy.PSObject.Properties['mode']
		if($frozen) {
			Keys $policy @('mode','advancesSimulation','deterministic','temporalJitterEnabled','animationPolicy')
			Require ($policy.mode -is [string] -and $policy.mode -ceq 'PausedFrozenPose') 'Unsupported simulation mode'
		} else { Keys $policy @('deterministic','fixedDeltaTimeSeconds','temporalJitterEnabled','animationPolicy') }
		Require ($policy.deterministic -is [bool] -and $policy.deterministic -and $policy.temporalJitterEnabled -is [bool] -and !$policy.temporalJitterEnabled -and $policy.animationPolicy -is [string] -and ![string]::IsNullOrWhiteSpace($policy.animationPolicy)) 'Missing deterministic simulation policy'
		if($frozen) {
			Require ($policy.advancesSimulation -is [bool] -and !$policy.advancesSimulation -and $policy.animationPolicy -ceq 'FrozenPose.NoStepOrSeekDuringCapture') 'Invalid frozen no-advance declaration'
		} else { $null=Positive $policy.fixedDeltaTimeSeconds fixedDeltaTimeSeconds }
		Frames $run.frames;Journal $run $Directory
		$journalPath=[IO.Path]::GetFullPath((Join-Path $Directory $run.journal.path))
		Require (!$journals.ContainsKey($journalPath) -and !$journalHashes.ContainsKey($run.journal.sha256)) 'A capture cannot stand in for multiple independent runs'
		$journals[$journalPath]=$true;$journalHashes[$run.journal.sha256]=$true
		$key=$run.fixtureId+'|'+$run.preparation+'|'+$run.runIndex
		Require (!$indexed.ContainsKey($key)) 'Duplicate matched run';$indexed[$key]=$run
	}
	return $indexed
}
try {
	Sha $ArchiveClosureSha256;Sha $JournalInspectorSha256
	Require ((Hash $JournalInspector) -ceq $JournalInspectorSha256) 'Wrong trusted inspector SHA'
	if(!$Manifest) { $Manifest=Join-Path $PSScriptRoot '../../Fixtures/RendererEfficiency/workloads.json' }
	$manifestInput=ReadBound $Manifest;$workloads=@($manifestInput.value.workloads|Where-Object representativeForSC008)
	Require ((@($workloads.id|Sort-Object) -join '|') -ceq 'depth-heavy|multiview-lights|recovery-capacity|shadow-batching|static-dynamic') 'Unexpected representative fixture contract'
	$r=ReadBound $Reference;$c=ReadBound $Candidate
	Require ($r.value.installClosureSha256 -ceq $ArchiveClosureSha256 -and $c.value.installClosureSha256 -cne $ArchiveClosureSha256) 'Baseline must use exact distinct archive bytes'
	$ri=Receipt $r.value Reference ([IO.Path]::GetDirectoryName($r.path)) $workloads
	$ci=Receipt $c.value Candidate ([IO.Path]::GetDirectoryName($c.path)) $workloads
	foreach($key in @($ri.Keys|Sort-Object)) {
		Require ($ci.ContainsKey($key)) 'Missing matched candidate run'
		$before=$ri[$key];$after=$ci[$key]
		Require ((Canonical $before.conditions) -ceq (Canonical $after.conditions)) 'Mismatched measurement conditions'
		$rt=@($before.frames[60..359].durationMs|Sort-Object);$ct=@($after.frames[60..359].durationMs|Sort-Object)
		$rm=([double]$rt[149]+[double]$rt[150])/2;$cm=([double]$ct[149]+[double]$ct[150])/2
		# Nearest-rank p95 is fixed: ceil(.95*300)-1, independently of summaries.
		$rp=[double]$rt[284];$cp=[double]$ct[284]
		$pass=$cm -le $rm*1.05 -and $cp -le $rp*1.05
		$result.comparisons+=@{fixtureId=$before.fixtureId;preparation=$before.preparation;runIndex=$before.runIndex;referenceMedianMs=$rm;candidateMedianMs=$cm;referenceP95Ms=$rp;candidateP95Ms=$cp;status=$(if($pass){'Passed'}else{'Failed'})}
	}
	foreach($input in @($r,$c,$manifestInput)) { Require ((Hash $input.path) -ceq $input.sha256) 'Control input changed during grading' }
	$result.referenceReceiptSha256=$r.sha256;$result.candidateReceiptSha256=$c.sha256;$result.manifestSha256=$manifestInput.sha256
	$result.status=if(@($result.comparisons|Where-Object status -CEQ Failed).Count){'Failed'}else{'Passed'}
	$result.exitCode=if($result.status -eq 'Passed'){0}else{1}
} catch { $result.reason=$_.Exception.Message }
NoRedirect $Output
Require (![IO.File]::Exists($Output) -and ![IO.Directory]::Exists($Output)) 'Comparison output must be fresh'
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Output))) | Out-Null
$stream=[IO.File]::Open($Output,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
try { $bytes=$utf8.GetBytes(($result|ConvertTo-Json -Depth 12));$stream.Write($bytes,0,$bytes.Length) } finally { $stream.Dispose() }
Write-Output ($result|ConvertTo-Json -Depth 12)
exit $result.exitCode
