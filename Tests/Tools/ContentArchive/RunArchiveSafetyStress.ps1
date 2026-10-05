<#
.SYNOPSIS
Runs bounded concurrent independent-reader and animation fault/recovery children.
.DESCRIPTION
Prerequisites: configure BUILD_TESTING=ON, compile and install current harnesses.
Example: ./RunArchiveSafetyStress.ps1 -InstallRoot ./out/build/RelWithDebInfo/Build -LogDirectory ./out/validation/archive-stress-fresh
Each cycle runs three isolated native processes. Readers never share a mount;
fixtures own unique temporary trees. The animation filter includes real pool
failures and recovery. This validates isolation, not same-mount thread safety.
Fresh logs, selected counts and per-process deadlines are mandatory.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string]$InstallRoot,
	[Parameter(Mandatory=$true)][string]$LogDirectory,
	[ValidateRange(1,10)][int]$Cycles=2,
	[ValidateRange(10,600)][int]$TimeoutSeconds=120,
	# Required archive coverage may be increased by callers, never reduced.
	[ValidateRange(30,1000)][int]$EngineMinimumCases=30,
	[ValidateRange(29,1000)][int]$EditorMinimumCases=29
)
$ErrorActionPreference='Stop'
$stressInstall=(Resolve-Path -LiteralPath $InstallRoot).Path
$stressLogs=[IO.Path]::GetFullPath($LogDirectory)
if(Test-Path -LiteralPath $stressLogs) { throw "Evidence directory must be fresh: $stressLogs" }
[IO.Directory]::CreateDirectory($stressLogs) | Out-Null
$stressWrapper=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '../Correctness/RunNativeCorrectnessSlice.ps1')).Path
$stressLanes=@(
	@{reader='Engine';filter='Engine.ContentArchiveSafety';minimum=$EngineMinimumCases},
	@{reader='Editor';filter='Editor.ContentArchiveSafety';minimum=$EditorMinimumCases},
	@{reader='Engine';filter='Animation.ProgramPublisher';minimum=9}
)
$stressResults=@()
for($stressCycle=1;$stressCycle -le $Cycles;++$stressCycle) {
	$stressJobs=@()
	try {
		foreach($stressLane in $stressLanes) {
			$stressExe=Join-Path $stressInstall "Tests/$($stressLane.reader)/TerraEngine-$($stressLane.reader)Tests.exe"
			$stressChildLog=Join-Path $stressLogs "cycle-$stressCycle-$($stressLane.filter)"
			# Each job invokes the existing owned-process watchdog; teardown of a
			# PowerShell job closes its native job handle and kills its owned tree.
			$stressJobs+=Start-Job -ScriptBlock {
				param($wrapper,$executable,$filter,$minimum,$timeout,$logs)
				$ErrorActionPreference='Stop'
				& $wrapper -Executable $executable -Filter $filter -MinimumCases $minimum -TimeoutSeconds $timeout -LogDirectory $logs
				if($LASTEXITCODE -ne 0) { throw "Native stress lane failed: $filter" }
			} -ArgumentList $stressWrapper,$stressExe,$stressLane.filter,$stressLane.minimum,$TimeoutSeconds,$stressChildLog
		}
		$stressJobs | Wait-Job -Timeout ($TimeoutSeconds+45) | Out-Null
		$stressJobFailure=$null
		foreach($stressJob in $stressJobs) {
			$stressJob | Receive-Job -ErrorAction Continue | Write-Output
			if($stressJob.State -ne 'Completed' -and $null -eq $stressJobFailure) { $stressJobFailure="Stress child did not complete: $($stressJob.State)" }
		}
		foreach($stressLane in $stressLanes) {
			$stressChildLog=Join-Path $stressLogs "cycle-$stressCycle-$($stressLane.filter)"
			# Failed/startup-timeout jobs may leave no complete receipt. Retain that
			# failure as evidence rather than aborting before outcome counts exist.
			$stressReceiptPath=Join-Path $stressChildLog 'watchdog.json'
			$stressOutputPath=Join-Path $stressChildLog 'stdout.log'
			$stressWatchdog=if(Test-Path -LiteralPath $stressReceiptPath) { Get-Content -LiteralPath $stressReceiptPath -Raw | ConvertFrom-Json } else { [pscustomobject]@{exitCode=125;timedOut=$false;elapsedMilliseconds=0} }
			$stressOutput=if(Test-Path -LiteralPath $stressOutputPath) { [IO.File]::ReadAllText($stressOutputPath) } else { '' }
			$stressSummary=[regex]::Match($stressOutput,'(?m)^(\d+) selected tests')
			# Preserve unavailable-case evidence independently of selected coverage;
			# skipped tests must not satisfy the mandatory executed-pass floor.
			$stressPassCount=[regex]::Matches($stressOutput,'(?m)^\[PASS\] ').Count
			$stressSkipCount=[regex]::Matches($stressOutput,'(?m)^\[SKIP\] ').Count
			$stressFailCount=[regex]::Matches($stressOutput,'(?m)^\[(FAIL|ERROR)\] ').Count
			$stressUnavailable=[regex]::Matches($stressOutput,'(?m)^\[UNAVAILABLE\] [^\r\n]*') | ForEach-Object { $_.Value }
			$stressResults+=@{cycle=$stressCycle;filter=$stressLane.filter;selectedCases=if($stressSummary.Success){[int]$stressSummary.Groups[1].Value}else{0};passedCases=$stressPassCount;skippedCases=$stressSkipCount;failedCases=$stressFailCount;unavailableLinkVariants=@($stressUnavailable);exitCode=$stressWatchdog.exitCode;timedOut=$stressWatchdog.timedOut;elapsedMilliseconds=$stressWatchdog.elapsedMilliseconds}
			# Write the failing lane too, so the evidence does not hide unavailable
			# or failed cases when the contract aborts the remaining cycle.
			$stressResults | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $stressLogs 'results.json') -Encoding UTF8
			if($stressWatchdog.exitCode -ne 0 -or $stressWatchdog.timedOut -or -not $stressSummary.Success -or [int]$stressSummary.Groups[1].Value -lt $stressLane.minimum -or $stressPassCount -lt $stressLane.minimum -or $stressFailCount -ne 0) { throw "Stress evidence failed for $($stressLane.filter)" }
		}
		$stressResults | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $stressLogs 'results.json') -Encoding UTF8
		if($null -ne $stressJobFailure) { throw $stressJobFailure }
	} finally {
		# Jobs are created by this invocation only; no shared process is stopped.
		$stressCleanupFailure=$null
		foreach($stressJob in $stressJobs) {
			try {
				# Blocked, suspended, starting and stopping jobs also retain owners.
				# Stop-Job has no timeout; the caller's outer watchdog bounds teardown.
				if($stressJob.State -notin @('Completed','Failed','Stopped')) { $stressJob | Stop-Job -ErrorAction Stop }
			} catch { if($null -eq $stressCleanupFailure) { $stressCleanupFailure=$_ } }
			finally {
				try { $stressJob | Remove-Job -Force -ErrorAction Stop }
				catch { if($null -eq $stressCleanupFailure) { $stressCleanupFailure=$_ } }
			}
		}
		if($null -ne $stressCleanupFailure) { throw $stressCleanupFailure }
	}
}
Write-Output "Concurrent isolated safety stress passed ($Cycles cycles); evidence: $stressLogs"
