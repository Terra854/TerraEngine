<#
.SYNOPSIS
Runs malformed trees and a valid deep tree in independent bounded native children.
.DESCRIPTION
Prerequisites: BUILD_TESTING=ON, configure/build and install both native harnesses.
Example: ./RunArchiveSafetyWatchdog.ps1 -InstallRoot ./out/build/RelWithDebInfo/Build -LogDirectory ./out/validation/archive-trees-fresh
Each case has its own process-tree watchdog and must select a real test. Evidence
directories must be fresh. This utility never deletes existing caller data.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string]$InstallRoot,
	[Parameter(Mandatory=$true)][string]$LogDirectory,
	[ValidateRange(1,120)][int]$TimeoutSeconds=30
)
$ErrorActionPreference='Stop'
$archiveInstall=(Resolve-Path -LiteralPath $InstallRoot).Path
$archiveLogs=[IO.Path]::GetFullPath($LogDirectory)
if(Test-Path -LiteralPath $archiveLogs) { throw "Evidence directory must be fresh: $archiveLogs" }
[IO.Directory]::CreateDirectory($archiveLogs) | Out-Null
$archiveWrapper=Join-Path $PSScriptRoot '../Correctness/RunNativeCorrectnessSlice.ps1'
$archiveCases=@('TreeSelfCycleRejected','TreeLongCycleRejected','TreeDisconnectedCycleRejected','TreeDeepValidExtractsOnce')
$archiveResults=@()
foreach($archiveReader in @('Engine','Editor')) {
	$archiveExe=Join-Path $archiveInstall "Tests/$archiveReader/TerraEngine-$($archiveReader)Tests.exe"
	foreach($archiveCase in $archiveCases) {
		$archiveFilter="$archiveReader.ContentArchiveSafety.$archiveCase"
		$archiveCaseLog=Join-Path $archiveLogs "$archiveReader-$archiveCase"
		& $archiveWrapper -Executable $archiveExe -Filter $archiveFilter -TimeoutSeconds $TimeoutSeconds -MinimumCases 1 -LogDirectory $archiveCaseLog
		$archiveExit=$LASTEXITCODE
		$archiveWatchdog=Get-Content -LiteralPath (Join-Path $archiveCaseLog 'watchdog.json') -Raw | ConvertFrom-Json
		$archiveSummary=[regex]::Match([IO.File]::ReadAllText((Join-Path $archiveCaseLog 'stdout.log')),'(?m)^(\d+) selected tests')
		# A selected editor test can return SKIP with exit zero. Require this
		# exact named contract to have executed successfully, not just been found.
		$archiveOutput=[IO.File]::ReadAllText((Join-Path $archiveCaseLog 'stdout.log'))
		$archivePassCount=[regex]::Matches($archiveOutput,('(?m)^\[PASS\] '+[regex]::Escape($archiveFilter)+'\r?$')).Count
		$archiveSkipCount=[regex]::Matches($archiveOutput,'(?m)^\[SKIP\] ').Count
		$archiveFailCount=[regex]::Matches($archiveOutput,'(?m)^\[(FAIL|ERROR)\] ').Count
		$archiveResults+=@{filter=$archiveFilter;exitCode=$archiveExit;selectedCases=if($archiveSummary.Success){[int]$archiveSummary.Groups[1].Value}else{0};passedCases=$archivePassCount;skippedCases=$archiveSkipCount;failedCases=$archiveFailCount;timedOut=$archiveWatchdog.timedOut;elapsedMilliseconds=$archiveWatchdog.elapsedMilliseconds}
		$archiveResults | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $archiveLogs 'results.json') -Encoding UTF8
		if($archiveExit -ne 0 -or -not $archiveSummary.Success -or [int]$archiveSummary.Groups[1].Value -ne 1 -or $archiveWatchdog.timedOut -or $archivePassCount -ne 1 -or $archiveSkipCount -ne 0 -or $archiveFailCount -ne 0) { throw "Archive tree case failed or did not select exactly one case: $archiveFilter; evidence: $archiveCaseLog" }
	}
}
Write-Output "Eight independent tree watchdog checks passed; evidence: $archiveLogs"
