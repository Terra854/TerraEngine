# Installed CPU contracts: real binary journal codec/inspector, mocked MCP only.
# No editor or live profiler is launched, and these fixtures prove no speed gate.
# Example: pwsh -File Test-RendererEfficiencyBaselineTimingCapture.ps1 -InstallRoot C:/Build -WorkRoot C:/Temp/fresh-capture-tests
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$InstallRoot,[Parameter(Mandatory=$true)][string]$WorkRoot)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
function AssertOrdinaryPath([string]$Path) {
	$cursor=[IO.Path]::GetFullPath($Path)
	while($cursor) {
		try {if(([IO.File]::GetAttributes($cursor) -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw "Redirected test path: $cursor"}}
		catch [IO.FileNotFoundException] {} catch [IO.DirectoryNotFoundException] {}
		$parent=[IO.Path]::GetDirectoryName($cursor);if($parent -eq $cursor){break};$cursor=$parent
	}
}
$install=[IO.Path]::GetFullPath($InstallRoot).TrimEnd('\','/');$work=[IO.Path]::GetFullPath($WorkRoot).TrimEnd('\','/')
foreach($pair in @(@($install,$work),@($work,$install))) {
	if($pair[1] -ieq $pair[0] -or $pair[1].StartsWith($pair[0]+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)){throw 'WorkRoot and InstallRoot overlap'}
}
AssertOrdinaryPath $install;AssertOrdinaryPath $work
if([IO.Directory]::Exists($work) -or [IO.File]::Exists($work)){throw 'WorkRoot must be fresh'}
$tools=Join-Path $install 'Tests/Tools/RendererEfficiency'
$module=Join-Path $tools 'RendererEfficiencyBaselineTimingCapture.ps1'
$inspector=Join-Path $tools 'TerraEngine-RendererEfficiencyProfilerJournalInspector.exe'
$writer=Join-Path $tools 'TerraEngine-RendererEfficiencyProfilerJournalFixture.exe'
foreach($path in @($module,$inspector,$writer)){AssertOrdinaryPath $path;if(![IO.File]::Exists($path)){throw "Missing installed dependency: $path"}}
$pins=@($module,$inspector,$writer)|ForEach-Object {[pscustomobject]@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash.ToLowerInvariant()}}
. $module
$null=[IO.Directory]::CreateDirectory($work)
$fixtureText=& $writer --work-root (Join-Path $work 'journals')
if($LASTEXITCODE -ne 0){throw 'Real CPU journal fixture writer failed'}
$fixtures=($fixtureText -join "`n")|ConvertFrom-Json
function FindJournal([string]$Name) {
	$found=@($fixtures|Where-Object {$_.name -ceq $Name})
	if($found.Count -ne 1){throw "Missing exact real journal fixture $Name"};return $found[0].path
}
# One synchronous mutable case context avoids closure/module-scope differences
# between -File and CTest's -Command invocation on Windows PowerShell 5.1.
$cases=@(
	@{name='happy-real-journal';fault='';journal='valid';error=''},
	@{name='play-changes';fault='play';journal='valid';error='*stably paused*'},
	@{name='session-changes';fault='session';journal='valid';error='*project/session changed*'},
	@{name='epoch-changes';fault='epoch';journal='valid';error='*project/session changed*'},
	@{name='core-frames-rejected';fault='drop';journal='valid';error='*lossless recording*'},
	@{name='wrong-artifact';fault='artifact';journal='valid';error='*recorded session*'},
	@{name='wrong-job';fault='job';journal='valid';error='*job ownership changed*'},
	@{name='escaped-artifact-path';fault='path';journal='valid';error='*contained V1 artifact*'},
	@{name='missing-health';fault='missing';journal='valid';error='*Missing capture API field*'},
	@{name='failed-persistence';fault='failed';journal='valid';error='*job ended failed*'},
	@{name='incomplete-frame';fault='';journal='incomplete-frame';error='*journal*'},
	@{name='incomplete-terminal';fault='';journal='incomplete-terminal';error='*journal*'},
	@{name='gapped-frames';fault='';journal='gap';error='*journal*'},
	@{name='corrupted-journal';fault='';journal='corrupted-payload';error='*journal*'},
	@{name='sealed-journal-retained-writer';fault='';journal='valid';error='';held=$true},
	@{name='incomplete-journal-retained-writer';fault='';journal='incomplete-terminal';error='*journal*';held=$true}
)
$results=New-Object 'Collections.Generic.List[object]'
foreach($case in $cases) {
	$caseRoot=Join-Path $work $case.name;$project=Join-Path $caseRoot 'project'
	$null=[IO.Directory]::CreateDirectory((Join-Path $project 'Profiler'))
	$source=Join-Path $project 'Profiler/owned.teprof'
	[IO.File]::Copy((FindJournal $case.journal),$source,$false)
	$sourceHash=(Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant()
	$script:baselineCaseState=[pscustomobject]@{fault=$case.fault;active=$false;healthCalls=0;stops=0;calls=(New-Object 'Collections.Generic.List[string]')}
	$transport={param($name,$arguments)
		$c=$script:baselineCaseState;$c.calls.Add($name)
		switch($name) {
			'play.state' {return @{currentState=$(if($c.fault -ceq 'play' -and $c.active){'playing'}else{'paused'});pendingTransition=$null}}
			'profiler.capture.operation' {return @{projectEpoch=7;operations=@()}}
			'profiler.capture.start' {$c.active=$true;return @{state='succeeded';captureLevel='sampled';bounds=@{maxFrames=4096;sampleIntervalFrames=1}}}
			'profiler.capture.health' {
				$c.healthCalls++;$later=$c.healthCalls -ge 3
				$h=@{state=$(if($c.active){'Recording'}else{'Idle'});projectEpoch=$(if($later -and $c.fault -ceq 'epoch'){8}else{7});sessionArtifactId=$(if(!$c.active){'old'}elseif($later -and $c.fault -ceq 'session'){'foreign'}else{'owned'});terminalFailure=$false;destinationWritable=$true;rejectedCoreFrames=$(if($later -and $c.fault -ceq 'drop'){1}else{0});committedFrames=360;producedFrames=360}
				if($later -and $c.fault -ceq 'missing'){$h.Remove('committedFrames')};return $h
			}
			'profiler.capture.stop' {$c.active=$false;$c.stops++;return @{jobId='owned-job'}}
			'jobs.get' {return @{job=@{jobId=$(if($c.fault -ceq 'job'){'foreign-job'}else{'owned-job'});state=$(if($c.fault -ceq 'failed'){'failed'}else{'succeeded'});result=@{artifactId=$(if($c.fault -ceq 'artifact'){'foreign'}else{'owned'})};artifacts=@(@{kind='profiler_capture';artifactId='owned';metadata=@{name=$(if($c.fault -ceq 'path'){'../escaped.teprof'}else{'owned.teprof'});frameCount=360}})}}}
			default {throw "Unexpected transport command $name"}
		}
	}
	$receipt=$null;$caught=$null
	$heldWriter=$null
	try {
		# A flushed finalized journal may retain the engine's output handle. This
		# real Windows handle permits reads but requires readers to share writes.
		# No bytes are written, and an incomplete terminal must still be rejected.
		if($case.ContainsKey('held')){$heldWriter=[IO.FileStream]::new($source,[IO.FileMode]::Open,[IO.FileAccess]::Write,[IO.FileShare]::ReadWrite)}
		try {$receipt=Invoke-RendererEfficiencyBaselineTimingCapture -Tool $transport -InspectorPath $inspector -ProjectRoot $project -OutputRoot (Join-Path $caseRoot 'captured') -Metadata @{role='CPU mock';fixture=$case.name;nativeProvenance=$false}}
		catch {$caught=$_.Exception.Message}
	} finally {if($null -ne $heldWriter){$heldWriter.Dispose()}}
	if($case.error) {if($null -eq $caught -or $caught -notlike $case.error){throw "Expected rejection $($case.name), got: $caught"}}
	else {
		if($null -ne $caught){throw "Happy real journal failed: $caught"}
		if($receipt.frames.Count -ne 360 -or $receipt.nativeAcceptance -ne $false -or $receipt.advancesSimulation -ne $false -or $receipt.journal.sha256 -cne $sourceHash){throw 'Happy result omitted actual byte/frame proof'}
		for($i=0;$i -lt 360;$i++){if($receipt.frames[$i].frameIndex -ne 1000+$i -or $receipt.frames[$i].durationMs -ne 10 -or !$receipt.frames[$i].complete){throw 'Actual decoded journal frame differs'}}
		if($receipt.lifecycle.playBefore.currentState -cne 'paused' -or $receipt.lifecycle.playAfterInspection.currentState -cne 'paused'){throw 'Paused lifecycle missing'}
	}
	if((Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant() -cne $sourceHash){throw 'Source journal was modified'}
	if($script:baselineCaseState.calls.Contains('play.step') -or $script:baselineCaseState.calls.Contains('profiler.capture.load')){throw 'Helper issued forbidden mutation/load'}
	$expectedStops=1;if($case.fault -in @('session','epoch')){$expectedStops=0}
	if($script:baselineCaseState.stops -ne $expectedStops){throw "Capture cleanup ownership differs: $($case.name)"}
	$results.Add([pscustomobject]@{name=$case.name;status='Passed';expectedRejection=[bool]$case.error;error=$caught;sourceSha256=$sourceHash})
}
foreach($pin in $pins){if((Get-FileHash -LiteralPath $pin.path).Hash.ToLowerInvariant() -cne $pin.sha256){throw 'Installed test dependency changed'}}
if($results.Count -ne 16){throw 'Capture contract cases were skipped'}
$report=[pscustomobject]@{schemaVersion=1;status='CpuContractsPassed';caseCount=$results.Count;dependencies=$pins;cases=$results.ToArray();mockTransport=$true;realJournalCodec=$true;nativeProvenance=$false;performanceAcceptance=$false}
[IO.File]::WriteAllText((Join-Path $work 'result.json'),(ConvertTo-Json -InputObject $report -Depth 12),[Text.UTF8Encoding]::new($false))
Write-Output '16 baseline timing capture cases passed; real CPU journals and mocked MCP only'
exit 0
