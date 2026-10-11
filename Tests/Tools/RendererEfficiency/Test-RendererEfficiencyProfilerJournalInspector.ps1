<#
Checks the installed decoder against genuine binary journals written by the
authoritative codec. These CPU fixtures validate rejection and are not runtime
performance evidence. Configure, build and install before invoking this script.
#>
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$InstallRoot,[Parameter(Mandatory=$true)][string]$WorkRoot)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$decoderRoot=[IO.Path]::GetFullPath($InstallRoot)
$decoderWork=[IO.Path]::GetFullPath($WorkRoot)
if(Test-Path -LiteralPath $decoderWork){throw 'Journal fixture root must be fresh.'}
$decoder=Join-Path $decoderRoot 'Tests/Tools/RendererEfficiency/TerraEngine-RendererEfficiencyProfilerJournalInspector.exe'
$fixtureWriter=Join-Path $decoderRoot 'Tests/Tools/RendererEfficiency/TerraEngine-RendererEfficiencyProfilerJournalFixture.exe'
$fixtureText=& $fixtureWriter --work-root $decoderWork
if($LASTEXITCODE -ne 0){throw 'Real journal fixture writer failed.'}
# PS5.1 returns the JSON root array as one pipeline object; explicitly enumerate
# it so both supported PowerShell runtimes validate the same actual journals.
$decoderFixtures=@(foreach($entry in (($fixtureText -join "`n")|ConvertFrom-Json)){$entry})
$decoderNames=@('valid','gap','zero-duration','incomplete-frame','incomplete-terminal','truncated-terminal','corrupted-payload','missing-lane','wrong-scope','wrong-source','unavailable-lane','mismatched-lane','duplicate-lane','partial-stop-outside-window')
if($decoderFixtures.Count -ne 14 -or [string]::Join('|',[string[]]@($decoderFixtures.name|Sort-Object)) -cne [string]::Join('|',[string[]]@($decoderNames|Sort-Object))){throw 'Real journal fixture set differs.'}
$decoderPassed=0
# Expected native rejection writes stderr. PS5.1 wraps those bytes as error
# records, so allow them locally while preserving the actual process exit code.
function InvokeJournalDecoder([string[]]$Arguments,[string]$ErrorLog) {
	$ErrorActionPreference='Continue'
	& $decoder @Arguments 2> $ErrorLog
	$script:decoderNativeExit=$LASTEXITCODE
}
foreach($decoderFixture in $decoderFixtures) {
	$decoderHash=(Get-FileHash -LiteralPath $decoderFixture.path -Algorithm SHA256).Hash
	$decoderText=InvokeJournalDecoder @('--journal',$decoderFixture.path,'--offset','0','--count','360') (Join-Path $decoderWork ($decoderFixture.name+'.stderr.log'))
	$decoderExit=$decoderNativeExit
	if(($decoderExit -eq 0) -ne [bool]$decoderFixture.expectedInspectorExitZero){throw "Unexpected decoder result: $($decoderFixture.name)"}
	if($decoderExit -eq 0) {
		$decoderResult=($decoderText -join "`n")|ConvertFrom-Json
		if($decoderResult.formatIdentifier -cne 'profilerJournalV1' -or $decoderResult.formatVersion -ne 1 -or !$decoderResult.complete -or $decoderResult.frames.Count -ne 360){throw 'Positive journal omitted decoded proof.'}
		for($decoderOrdinal=0;$decoderOrdinal -lt 360;++$decoderOrdinal) {
			$decoderFrame=$decoderResult.frames[$decoderOrdinal]
			if(!$decoderFrame.complete -or $decoderFrame.frameIndex -ne 1000+$decoderOrdinal -or $decoderFrame.durationMs -ne 10){throw 'Actual decoded frame differs from written journal.'}
		}
	} elseif($decoderText){throw 'Rejected journal emitted apparent successful JSON.'}
	if((Get-FileHash -LiteralPath $decoderFixture.path -Algorithm SHA256).Hash -cne $decoderHash){throw 'Decoder mutated its journal.'}
	++$decoderPassed
}
$decoderPartial=($decoderFixtures|Where-Object name -eq 'partial-stop-outside-window').path
$decoderPartialHash=(Get-FileHash -LiteralPath $decoderPartial -Algorithm SHA256).Hash
# Offset one includes the final partial stop frame; it must fail without JSON.
$decoderText=InvokeJournalDecoder @('--journal',$decoderPartial,'--offset','1','--count','360') (Join-Path $decoderWork 'partial-stop-selected.stderr.log')
if($decoderNativeExit -eq 0 -or $decoderText){throw 'Selected partial stop frame was accepted.'}
if((Get-FileHash -LiteralPath $decoderPartial -Algorithm SHA256).Hash -cne $decoderPartialHash){throw 'Decoder mutated partial-stop journal.'}
++$decoderPassed
$decoderValid=($decoderFixtures|Where-Object name -eq 'valid').path
# Exercise the real executable's argument parser without opening arbitrary files.
$decoderBadArguments=@(
	@('--journal','relative.teprof','--offset','0','--count','360'),
	@('--journal',$decoderValid,'--offset','-1','--count','360'),
	@('--journal',$decoderValid,'--offset','360','--count','1'),
	@('--journal',$decoderValid,'--offset','0','--count','0'),
	@('--journal',$decoderValid,'--offset','0','--offset','0','--count','360'),
	@('--journal',$decoderValid,'--offset','0','--count','360','--unknown','1'))
foreach($decoderArguments in $decoderBadArguments) {
	$decoderText=InvokeJournalDecoder $decoderArguments (Join-Path $decoderWork ('invalid-'+$decoderPassed+'.stderr.log'))
	if($decoderNativeExit -eq 0 -or $decoderText){throw 'Invalid decoder arguments were accepted.'}
	++$decoderPassed
}
if($decoderPassed -ne 21){throw 'Journal inspector cases were skipped.'}
Write-Output '21 profiler journal inspector cases passed; real CPU journal fixtures only'
