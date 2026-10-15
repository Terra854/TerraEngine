<#
CPU-only protocol checks using real journals and installed standalone tools.
Configure/build/install first; pass -InstallRoot and a fresh absolute -WorkRoot.
These decoder rejection checks are never renderer timing acceptance evidence.
#>
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$InstallRoot,[Parameter(Mandatory=$true)][string]$WorkRoot)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$decoderRoot=[IO.Path]::GetFullPath($InstallRoot)
$decoderWork=[IO.Path]::GetFullPath($WorkRoot)
if(Test-Path -LiteralPath $decoderWork){throw 'Decoder work root must be fresh.'}
$decoder=Join-Path $decoderRoot 'Tests/Tools/RuntimeMemorySafety/TerraEngine-RuntimeMemorySafetyTimingInspector.exe'
$writer=Join-Path $decoderRoot 'Tests/Tools/RuntimeMemorySafety/TerraEngine-RuntimeMemorySafetyTimingJournalFixture.exe'
$fixtureText=& $writer --work-root $decoderWork
if($LASTEXITCODE -ne 0){throw 'Real timing journal fixture writer failed.'}
$fixtures=@(foreach($entry in (($fixtureText -join "`n")|ConvertFrom-Json)){$entry})
$expectedNames=@('valid','valid-with-child','rounding-boundary','valid-partial-stop','missing-phase','duplicate-phase',
	'wrong-module','incomplete-phase','nonfinite-duration','zero-duration','aggregate-occurrence','duplicate-sample-id',
	'misordered-phases','overlapping-phases','out-of-frame','missing-lane','wrong-lane-source','duplicate-lane',
	'gapped-frames','incomplete-frame','incomplete-terminal','wrong-capture-settings','wrong-capture-detail','truncated-terminal','corrupted-payload')
if($fixtures.Count -ne $expectedNames.Count -or [string]::Join('|',[string[]]@($fixtures.name|Sort-Object)) -cne [string]::Join('|',[string[]]@($expectedNames|Sort-Object))){throw 'Decoder fixture set differs.'}
$phaseNames=@('RenderSystem: Protected Publication Capture','RenderSystem: Geometry Route Preparation','RenderSystem: Virtual Geometry Preparation')
$passed=0
# Windows PowerShell wraps expected stderr as error records; preserve native exit
# status while allowing that local stderr only. Rejection must emit no JSON.
function InvokeTimingDecoder([string[]]$Arguments,[string]$ErrorLog){
	$ErrorActionPreference='Continue'
	& $decoder @Arguments 2> $ErrorLog
	$script:decoderExit=$LASTEXITCODE
}
foreach($fixture in $fixtures){
	$hash=(Get-FileHash -LiteralPath $fixture.path -Algorithm SHA256).Hash
	$result=InvokeTimingDecoder @('--journal',$fixture.path,'--offset','0','--count','360') (Join-Path $decoderWork ($fixture.name+'.stderr.log'))
	if(($decoderExit -eq 0) -ne [bool]$fixture.expectedInspectorExitZero){
		$errorLog=Join-Path $decoderWork ($fixture.name+'.stderr.log')
		$diagnostic=Get-Content -LiteralPath $errorLog -Raw
		throw "Unexpected decoder result: $($fixture.name); exit=$decoderExit; stderr=$errorLog; $diagnostic"
	}
	if($decoderExit -eq 0){
		$proof=($result -join "`n")|ConvertFrom-Json
		if($proof.formatIdentifier -cne 'profilerJournalV1' -or $proof.formatVersion -ne 1 -or !$proof.complete -or $proof.frames.Count -ne 360 -or $proof.metric -cne 'rendererPreparationScopeSumMs'){throw 'Decoder omitted fixed-window proof.'}
		if($proof.window.offset -ne 0 -or $proof.window.count -ne 360 -or $proof.window.warmupFrames -ne 60 -or $proof.window.measuredFrames -ne 300){throw 'Window differs.'}
		if($proof.captureSettings.captureLevel -cne 'detailed' -or $proof.captureSettings.sampleIntervalFrames -ne 1 -or $proof.captureSettings.proof -cne 'journalMetadata'){throw 'Persisted capture settings differ.'}
		if($null -ne $proof.captureSettings.PSObject.Properties['maxFrames'] -or $null -ne $proof.captureSettings.PSObject.Properties['cpuEngineEnabled']){throw 'Decoder fabricated settings absent from persisted journal metadata.'}
		for($ordinal=0;$ordinal -lt 360;++$ordinal){
			$frame=$proof.frames[$ordinal]
			if(!$frame.complete -or $frame.frameIndex -ne 1000+$ordinal -or $frame.frameStartMs -ne 10*$ordinal -or $frame.durationMs -ne 10 -or $frame.phaseSamples.Count -ne 3){throw 'Decoded frame identity differs.'}
			$expectedPreparation=2.5
			if($fixture.name -ceq 'rounding-boundary' -and $ordinal -eq 180){$expectedPreparation=9.650002}
			if([Math]::Abs($frame.preparationMs-$expectedPreparation) -gt 0.000000000001){throw 'Phase sum differs or child duration was double counted.'}
			for($phase=0;$phase -lt 3;++$phase){
				$sample=$frame.phaseSamples[$phase]
				if($sample.name -cne $phaseNames[$phase] -or $sample.moduleId -cne 'CpuEngine' -or $sample.sampleId -ne 10+$phase -or $sample.parentSampleId -ne 1 -or !$sample.complete -or $sample.occurrenceCount -ne 1 -or !$sample.threadLabel){throw 'Decoded sample provenance differs.'}
			}
			if($frame.engineClockLane.id -cne 'engine_cpu' -or $frame.engineClockLane.scope -cne 'EngineCpu' -or $frame.engineClockLane.source -cne 'ProfilerClock' -or $frame.engineClockLane.status -cne 'Valid' -or $frame.engineClockLane.milliseconds -ne 10){throw 'Clock provenance differs.'}
		}
	}elseif($result){throw 'Rejected journal emitted apparent successful JSON.'}
	if((Get-FileHash -LiteralPath $fixture.path -Algorithm SHA256).Hash -cne $hash){throw 'Decoder mutated journal bytes.'}
	++$passed
}
$valid=($fixtures|Where-Object name -eq 'valid').path
$badArguments=@(
	@('--journal','relative.teprof','--offset','0','--count','360'),
	@('--journal',$valid,'--offset','1','--count','360'),
	@('--journal',$valid,'--offset','0','--count','300'),
	@('--journal',$valid,'--offset','0','--offset','0','--count','360'),
	@('--journal',$valid,'--offset','0','--count','360','--unknown','1'),
	@('--journal',$valid,'--offset','0','--count'))
foreach($arguments in $badArguments){
	$result=InvokeTimingDecoder $arguments (Join-Path $decoderWork ('arguments-'+$passed+'.stderr.log'))
	if($decoderExit -eq 0 -or $result){throw 'Invalid decoder arguments were accepted.'}
	++$passed
}
if($passed -ne 31){throw 'Timing decoder cases were skipped.'}
Write-Output '31 runtime memory safety timing inspector cases passed; real CPU journals only'
# Expected-negative native invocations leave LASTEXITCODE nonzero. Publish the
# completed script's success only after every assertion has passed, so callers
# do not mistake that last deliberate rejection for a failed protocol run.
exit 0
