# CapturePhysicsOptimizationBenchmark.ps1 - create reproducible physics evidence.

[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string]$InstallRoot,
	[Parameter(Mandatory=$true)][string]$Manifest,
	[Parameter(Mandatory=$true)][string]$OutputDirectory,
	[int]$Runs = 5,
	[int]$AcceptanceRepetitions = 10,
	[switch]$AllowLiveStress,
	[switch]$BaselineIsSameRevisionParent,
	[switch]$DryRun,
	[string]$NativeBenchmarkExecutable,
	[string[]]$NativeBenchmarkArguments = @(),
	[string]$WorkerScalingEvidenceSummary
)

$ErrorActionPreference = 'Stop'
$script:workerEvidenceAggregates = @()

function Get-TextSha256 {
	param([Parameter(Mandatory=$true)][string]$Text)
	$sha = [Security.Cryptography.SHA256]::Create()
	try {
		$bytes = [Text.Encoding]::UTF8.GetBytes($Text)
		return [BitConverter]::ToString($sha.ComputeHash($bytes)).Replace('-', '').ToLowerInvariant()
	}
	finally { $sha.Dispose() }
}

function Get-FileSha256 {
	param([Parameter(Mandatory=$true)][string]$Path)
	return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-TreeSha256 {
	param([Parameter(Mandatory=$true)][string]$Root)
	$resolved = (Resolve-Path -LiteralPath $Root).Path
	$records = Get-ChildItem -LiteralPath $resolved -Recurse -File | Sort-Object FullName | ForEach-Object {
		$relative = $_.FullName.Substring($resolved.Length).TrimStart('\', '/').Replace('\', '/')
		'{0}:{1}' -f $relative, (Get-FileSha256 $_.FullName)
	}
	return Get-TextSha256 ($records -join "`n")
}

function New-UnavailableMetric {
	param([Parameter(Mandatory=$true)][string]$Reason)
	# Null is intentional: an unavailable measurement must never become a
	# misleading zero in percentile or regression calculations.
	return [ordered]@{
		availability='unavailable'; reason=$Reason; sampleCount=0
		median=$null; p95=$null; p99=$null; worst=$null
		workItems=$null; allocationCount=$null; allocationBytes=$null
	}
}

function New-Gates {
	return [ordered]@{
		identityComparable=$false; sampleCoveragePassed=$false; unchangedPreparationPassed=$false
		mixedTickPassed=$false; emptyLowEventPassed=$false; contactEventPassed=$false
		ragdollPassed=$false; bulkAdmissionPassed=$false; workerScalingPassed=$false
		correctnessPassed=$false; repeatedRunsPassed=$false; qualityPassed=$false; buildInstallPassed=$false
	}
}

function Test-PathInsideRoot {
	param([Parameter(Mandatory=$true)][string]$Path,[Parameter(Mandatory=$true)][string]$Root)
	$comparison = if ($IsWindows) { [StringComparison]::OrdinalIgnoreCase } else { [StringComparison]::Ordinal }
	$rootWithSeparator = $Root.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
	return $Path.StartsWith($rootWithSeparator, $comparison) -or [string]::Equals($Path, $Root, $comparison)
}

function Get-WorkloadUnavailableReason {
	param([Parameter(Mandatory=$true)][string]$WorkloadId,[string]$RouteFailure)
	if ($DryRun) { return 'dry-run requested; no runtime samples were collected' }
	if (-not $AllowLiveStress) { return 'live stress was not authorized; pass -AllowLiveStress to collect runtime samples' }
	if (-not [string]::IsNullOrWhiteSpace($RouteFailure)) { return $RouteFailure }
	if (@('physics-unchanged-10000-v1','physics-worker-large-v1') -notcontains $WorkloadId) {
		return "the installed stress runner has no schema-compatible fixture route for workload '$WorkloadId'"
	}
	return 'the installed stress runner did not emit valid schema-stage samples'
}

if ($Runs -lt 5) { throw 'Runs must be at least five for comparable evidence.' }
if ($AcceptanceRepetitions -lt 10) { throw 'Acceptance repetitions must be at least ten.' }
if (-not (Test-Path -LiteralPath $InstallRoot -PathType Container)) { throw "Install root not found: $InstallRoot" }
if (-not (Test-Path -LiteralPath $Manifest -PathType Leaf)) { throw "Workload manifest not found: $Manifest" }

$resolvedInstallRoot = (Resolve-Path -LiteralPath $InstallRoot).Path
$resolvedManifest = (Resolve-Path -LiteralPath $Manifest).Path
$captureScriptPath = (Resolve-Path -LiteralPath $PSCommandPath).Path
$installedRunnerPath = Join-Path $resolvedInstallRoot 'Tests/Tools/Physics/RunInstalledEditorPhysicsStress.ps1'
$installedEditorPath = Join-Path $resolvedInstallRoot 'Editor/TerraEngine-Editor.exe'
$liveRouteFailure = $null
if (-not (Test-PathInsideRoot -Path $captureScriptPath -Root $resolvedInstallRoot)) {
	$liveRouteFailure = 'capture script is not executing from the installed build; use the installed Tests/Tools/Physics copy for live evidence'
} elseif (-not (Test-Path -LiteralPath $installedRunnerPath -PathType Leaf)) {
	$liveRouteFailure = 'installed stress runner is missing; run the CMake install target before live evidence'
} elseif (-not (Test-Path -LiteralPath $installedEditorPath -PathType Leaf)) {
	$liveRouteFailure = 'installed editor executable is missing; run the CMake install target before live evidence'
}

$manifestObject = Get-Content -Raw -LiteralPath $resolvedManifest | ConvertFrom-Json
if (@($manifestObject.workloads).Count -ne 6) { throw 'The canonical manifest must contain exactly six workloads.' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$sourceRevision = try { (git rev-parse HEAD).Trim() } catch { 'unavailable' }
$diffText = try { (git diff --no-ext-diff | Out-String) } catch { '' }
$dirtyDiffHash = if ([string]::IsNullOrEmpty($diffText)) { $null } else { Get-TextSha256 $diffText }
$installedTreeHash = Get-TreeSha256 $resolvedInstallRoot
$manifestHash = Get-FileSha256 $resolvedManifest
$captureScriptHash = Get-FileSha256 $captureScriptPath
$installedRunnerHash = if (Test-Path -LiteralPath $installedRunnerPath -PathType Leaf) { Get-FileSha256 $installedRunnerPath } else { $null }
$installedEditorHash = if (Test-Path -LiteralPath $installedEditorPath -PathType Leaf) { Get-FileSha256 $installedEditorPath } else { $null }
$computer = try { Get-CimInstance Win32_ComputerSystem -ErrorAction Stop } catch { $null }
$processor = try { Get-CimInstance Win32_Processor -ErrorAction Stop | Select-Object -First 1 } catch { $null }
$machineId = [Environment]::MachineName
$cpuName = if ($processor -and $processor.Name) { [string]$processor.Name } else { [Environment]::GetEnvironmentVariable('PROCESSOR_IDENTIFIER') }
if ([string]::IsNullOrWhiteSpace($cpuName)) { $cpuName = 'unavailable' }
$memoryBytes = if ($computer -and $computer.TotalPhysicalMemory) { [UInt64]$computer.TotalPhysicalMemory } else { 1L }
function Convert-StressSummaryToRun {
	param([object]$Summary,[int]$RunNumber,[object]$Workload,[string]$Reason,[int]$WarmupSteps)
	$stages = [ordered]@{}
	foreach ($name in @('authoringPreparation','providerCommit','simulation','extraction','eventPublication','stateValidation','snapshotConstruction','transactionConstruction')) {
		# The installed runner retains one immutable engine timing snapshot per exact
		# fixed step. Values are converted from milliseconds to microseconds only for
		# compact evidence; comparisons remain paired and unit-consistent.
		$durations = @(@($Summary.physicsStageSamples | Select-Object -Skip $WarmupSteps) | ForEach-Object {
			$stageProperty = if ($_ -and $_.stages) { $_.stages.PSObject.Properties[$name] } else { $null }
			if ($null -ne $stageProperty -and $null -ne $stageProperty.Value) { [double]$stageProperty.Value * 1000.0 }
		} | Where-Object { $null -ne $_ })
		if ($durations.Count -gt 0) {
			$orderedDurations = @($durations | Sort-Object)
			$stages[$name] = [ordered]@{
				availability='available'; reason=$null; sampleCount=$orderedDurations.Count
				median=$orderedDurations[[Math]::Max(0,[int][Math]::Ceiling($orderedDurations.Count*0.50)-1)]
				p95=$orderedDurations[[Math]::Max(0,[int][Math]::Ceiling($orderedDurations.Count*0.95)-1)]
				p99=$orderedDurations[[Math]::Max(0,[int][Math]::Ceiling($orderedDurations.Count*0.99)-1)]
				worst=$orderedDurations[-1]; workItems=$null; allocationCount=$null; allocationBytes=$null
			}
		} else { $stages[$name] = New-UnavailableMetric $Reason }
	}
	$success = $null -ne $Summary -and [bool]$Summary.success
	$hash = if ($Summary -and $Summary.determinismHash) { [string]$Summary.determinismHash } else { 'unavailable' }
	$completedSteps = if($Summary -and $Summary.explicitStepCount){[Math]::Max(0,[int]$Summary.explicitStepCount - $WarmupSteps)}else{0}
	$requestedSteps = if($Summary -and $Summary.requestedExplicitStepCount){[Math]::Max(0,[int]$Summary.requestedExplicitStepCount - $WarmupSteps)}else{[int]$Workload.measuredSteps}
	$coverage = if($success -and $requestedSteps -gt 0){[Math]::Min(1.0, [double]$completedSteps / $requestedSteps)}else{0.0}
	$lastJob = @($Summary.physicsStageSamples | Select-Object -Last 1).jobs
	return [ordered]@{ runId="$($Workload.id)-run-$RunNumber"; runNumber=$RunNumber; completedSteps=$completedSteps; coverageRatio=$coverage; stages=$stages; jobs=[ordered]@{ submitted=if($lastJob){[UInt64]$lastJob.submitted}else{0}; completed=if($lastJob){[UInt64]$lastJob.completed}else{0}; rejected=if($lastJob){[UInt64]$lastJob.rejected}else{0}; observedWorkerThreads=if($lastJob){[UInt64]$lastJob.observedWorkerThreads}else{0}; barrierWaitMicroseconds=if($lastJob){[UInt64]$lastJob.barrierWaitMicroseconds}else{0}; jobExecutionMicroseconds=if($lastJob){[UInt64]$lastJob.jobExecutionMicroseconds}else{0}; idleAtEnd=if($lastJob){[bool]$lastJob.idle}else{$success} }; correctness=[ordered]@{ stateHash=$hash; eventHash='unavailable'; poseHash='unavailable'; overflowCount=0; droppedCount=0; rollbackPassed=if($Summary -and $Summary.managedReloadAutomationEvidenceCompleteness){[bool]$Summary.managedReloadAutomationEvidenceCompleteness.checks.rollbackEvidence}else{$false}; visibilityPassed=if($Summary -and $Summary.managedReloadAutomationEvidenceCompleteness){[bool]$Summary.managedReloadAutomationEvidenceCompleteness.checks.visibilityEvidence}else{$false}; leakedProviderWork=if($Summary){[bool]$Summary.forcedTermination}else{$false} } }
}

function Invoke-InstalledStress {
	param([object]$Workload,[int]$RunNumber,[string]$Root,[string]$Worker,[object]$Quality,[int]$MeasuredSteps,[string]$VariantId,[string]$Runner)
	$workRoot = Join-Path $Root ("{0:D2}-{1}" -f $RunNumber,$VariantId)
	New-Item -ItemType Directory -Force -Path $workRoot | Out-Null
	try {
		$scenario = if($Workload.id -eq 'physics-worker-large-v1' -or $Workload.id -eq 'physics-unchanged-10000-v1') { 'large' } else { 'canonical' }
		# Live captures are routed only through the installed sibling runner. This
		# prevents a source-tree harness from being mistaken for installed evidence.
		& $Runner -InstallRoot $resolvedInstallRoot -WorkRoot $workRoot -ObjectCount ([int]$Workload.population) -ActiveDynamicCount ([int]$Workload.activeBodies) -ExplicitStepCount ($MeasuredSteps + $Workload.warmupSteps) -WorkerCount $Worker -WorkerScalingScenario $scenario -VelocityIterations ([int]$Quality.velocityIterations) -PositionIterations ([int]$Quality.positionIterations) -AllowLiveStress:$AllowLiveStress *> (Join-Path $workRoot 'runner.log')
	} catch { return $null }
	$summaryPath = Join-Path $workRoot 'evidence/summary.json'
	if (Test-Path -LiteralPath $summaryPath) { try { return Get-Content -Raw $summaryPath | ConvertFrom-Json } catch {} }
	return $null
}

function Invoke-NativeBenchmark {
	param([object]$Workload,[string]$Worker,[object]$Quality,[int]$RunNumber,[string]$Root)
	if ([string]::IsNullOrWhiteSpace($NativeBenchmarkExecutable) -or -not (Test-Path -LiteralPath $NativeBenchmarkExecutable -PathType Leaf)) { return $null }
	$outPath = Join-Path $Root ("native-{0}-{1:D2}.json" -f $Workload.id,$RunNumber)
	$args = @($NativeBenchmarkArguments) + @('--workload',$Workload.id,'--workers',$Worker,'--velocity-iterations',$Quality.velocityIterations,'--position-iterations',$Quality.positionIterations,'--run-number',$RunNumber,'--output',$outPath)
	try { & $NativeBenchmarkExecutable @args *> (Join-Path $Root ("native-{0}-{1:D2}.log" -f $Workload.id,$RunNumber)) } catch { return $null }
	if (Test-Path -LiteralPath $outPath -PathType Leaf) { try { return Get-Content -Raw $outPath | ConvertFrom-Json } catch {} }
	return $null
}

function Convert-NativeBenchmarkRun {
	param([object]$Native,[object]$Workload,[int]$RunNumber,[int]$MeasuredSteps,[string]$Reason)
	$source = if ($Native.evidence) { $Native.evidence } else { $Native }
	if ($source.runs) { $source = @($source.runs)[0] }
	$stages = [ordered]@{}
	foreach ($name in @('authoringPreparation','providerCommit','simulation','extraction','eventPublication','stateValidation','snapshotConstruction','transactionConstruction')) {
		$metric = if ($source.stages) { $source.stages.PSObject.Properties[$name].Value } else { $null }
		if ($metric -and $metric.availability) { $stages[$name] = $metric } elseif ($metric -and $null -ne $metric.median) { $stages[$name] = [ordered]@{ availability='available'; reason=$null; sampleCount=[int]$metric.sampleCount; median=$metric.median; p95=$metric.p95; p99=$metric.p99; worst=$metric.worst; workItems=$metric.workItems; allocationCount=$metric.allocationCount; allocationBytes=$metric.allocationBytes } } else { $stages[$name] = New-UnavailableMetric $Reason }
	}
	$completed = if ($null -ne $source.completedSteps) { [int]$source.completedSteps } else { 0 }
	$coverage = if ($null -ne $source.coverageRatio) { [double]$source.coverageRatio } elseif ($MeasuredSteps -gt 0) { [Math]::Min(1.0,$completed/$MeasuredSteps) } else { 0.0 }
	$correctness = if ($source.correctness) { $source.correctness } else { [ordered]@{ stateHash='unavailable'; eventHash='unavailable'; poseHash='unavailable'; overflowCount=0; droppedCount=0; rollbackPassed=$false; visibilityPassed=$false; leakedProviderWork=$false } }
	$jobs = if ($source.jobs) { $source.jobs } else { [ordered]@{ submitted=0; completed=0; rejected=0; observedWorkerThreads=0; barrierWaitMicroseconds=0; jobExecutionMicroseconds=0; idleAtEnd=$false } }
	return [ordered]@{ runId="$($Workload.id)-run-$RunNumber"; runNumber=$RunNumber; completedSteps=$completed; coverageRatio=$coverage; stages=$stages; jobs=$jobs; correctness=$correctness }
}

function Get-WorkerScalingGroups {
	param([string]$SummaryPath,[string]$Worker,[object]$Quality)
	if (-not (Test-Path -LiteralPath $SummaryPath -PathType Leaf)) { throw "Worker scaling evidence summary not found: $SummaryPath" }
	$summary = Get-Content -Raw -LiteralPath $SummaryPath | ConvertFrom-Json
	$entries = @($summary.sourceEvidence)
	if ($entries.Count -eq 0) { throw 'Worker scaling evidence summary has no sourceEvidence entries.' }
	$groups = @()
	foreach ($entry in $entries) {
		$path = if ($entry -is [string]) { [string]$entry } elseif ($entry.summaryPath) { [string]$entry.summaryPath } elseif ($entry.path) { [string]$entry.path } else { $null }
		if ($path -and -not [IO.Path]::IsPathRooted($path)) {
			# Evidence summaries normally record repository-relative paths. Resolve
			# those from the capture process working directory first; retain a
			# summary-relative fallback for portable copied bundles.
			$workingPath = Join-Path (Get-Location).Path $path
			$path = if(Test-Path -LiteralPath $workingPath){$workingPath}else{Join-Path (Split-Path -Parent $SummaryPath) $path}
		}
		$raw = if ($entry.summary) { $entry.summary } elseif ($path -and (Test-Path -LiteralPath $path)) { Get-Content -Raw $path | ConvertFrom-Json } else { $null }
		if (-not $raw -or -not $raw.success -or [bool]$raw.forcedTermination -or @($raw.physicsStageSamples).Count -ne 100) { throw 'Worker scaling source evidence must be successful, non-terminated, and contain exactly 100 physicsStageSamples.' }
		if ([string]$raw.requestedWorkerCount -ne $Worker -and [string]$raw.workerCount -ne $Worker) { continue }
		$samples = @($raw.physicsStageSamples)
		$qualityMatches = @($samples | Where-Object {
			$_.quality -and [int]$_.quality.velocityIterations -eq [int]$Quality.velocityIterations `
				-and [int]$_.quality.positionIterations -eq [int]$Quality.positionIterations
		}).Count -eq $samples.Count
		if(-not $qualityMatches){continue}
		for ($i=0; $i -lt 10; ++$i) { $groups += ,(@($samples | Select-Object -Skip ($i*10) -First 10)) }
		$allValues = @($samples | ForEach-Object { [double]$_.stages.simulation } | Sort-Object)
		$script:workerEvidenceAggregates += [ordered]@{ worker=$Worker; quality=$Quality.id; sampleCount=100; median=($allValues[49]+$allValues[50])/2.0; p95=$allValues[94]; p99=$allValues[98]; worst=$allValues[99]; sourcePath=$path }
	}
	if ($groups.Count -ne 10) { throw "Expected one matching 100-sample source for worker=$Worker quality=$($Quality.id)." }
	return $groups
}

$indexEntries = @()
$workerEvidenceProvenance = @()
foreach ($workload in $manifestObject.workloads) {
	$warmupSteps = if ($null -ne $workload.warmupSteps) { [int]$workload.warmupSteps } else { [int]$manifestObject.warmupSteps }
	$measuredSteps = if ($null -ne $workload.measuredSteps) { [int]$workload.measuredSteps } else { [int]$manifestObject.measuredSteps }
	$workloadJson = $workload | ConvertTo-Json -Depth 8 -Compress
	$defaultQuality = $manifestObject.qualityProfiles | Where-Object { $_.id -eq 'default' } | Select-Object -First 1
	$variants = @([ordered]@{ worker='max'; quality=$defaultQuality })
	if ($workload.id -eq 'physics-worker-large-v1') {
		$variants = @()
		foreach ($qualityProfile in @($manifestObject.qualityProfiles)) {
			foreach ($worker in @('1','4','max')) { $variants += [ordered]@{ worker=$worker; quality=$qualityProfile } }
		}
	}
	foreach ($variant in $variants) {
		$quality = $variant.quality
		$worker = [string]$variant.worker
		$variantId = if ($workload.id -eq 'physics-worker-large-v1' -and ($worker -ne 'max' -or $quality.id -ne 'default')) { "$($workload.id)-worker-$worker-quality-$($quality.id)" } else { [string]$workload.id }
		# Every workload/worker/quality leg owns the requested lifecycle repetition
		# count. Unsupported fixtures still emit unavailable records, never a shorter
		# run set that could accidentally satisfy the lifecycle gate from index metadata alone.
		$effectiveRuns = [Math]::Max($Runs,$AcceptanceRepetitions)
		$captureSettingsHash = Get-TextSha256 ("$manifestHash|$($workload.id)|$warmupSteps|$measuredSteps|$effectiveRuns|$worker|$($quality.id)|$($manifestObject.fixedDeltaSeconds)")
		$runEvidence = @()
		$lastLiveSummary = $null
		$lastNativeSource = $null
		$profileApplied = $false
		$qualityAppliedForAllRuns = $true
		$liveRunCount = 0
		$nativeRunCount = 0
		$measuredRunCount = 0
		$completeLifecycleRunCount = 0
		$workloadUnavailableReason = Get-WorkloadUnavailableReason -WorkloadId $workload.id -RouteFailure $liveRouteFailure
		$canRouteLiveWorkload = $AllowLiveStress -and -not $DryRun -and [string]::IsNullOrWhiteSpace($liveRouteFailure) `
			-and @('physics-unchanged-10000-v1','physics-worker-large-v1') -contains $workload.id
		$workerEvidenceGroups = if ($WorkerScalingEvidenceSummary -and $workload.id -eq 'physics-worker-large-v1') { Get-WorkerScalingGroups -SummaryPath $WorkerScalingEvidenceSummary -Worker $worker -Quality $quality } else { $null }
		if ($workerEvidenceGroups) { $workerEvidenceProvenance += [ordered]@{ summaryPath=(Resolve-Path $WorkerScalingEvidenceSummary).Path; summarySha256=(Get-FileSha256 $WorkerScalingEvidenceSummary); worker=$worker; quality=$quality.id; sampleCount=100 } }
		for ($runNumber = 1; $runNumber -le $effectiveRuns; ++$runNumber) {
			$liveSummary = $null
			$nativeSummary = if ($NativeBenchmarkExecutable -and -not $DryRun) { Invoke-NativeBenchmark -Workload $workload -Worker $worker -Quality $quality -RunNumber $runNumber -Root $OutputDirectory } else { $null }
			if ($nativeSummary) {
				$nativeSource = if ($nativeSummary.evidence) { $nativeSummary.evidence } else { $nativeSummary }
				if ($nativeSource.runs) { $nativeSource = @($nativeSource.runs)[0] }
				$nativeRun = Convert-NativeBenchmarkRun -Native $nativeSummary -Workload $workload -RunNumber $runNumber -MeasuredSteps $measuredSteps -Reason $workloadUnavailableReason
				if ($workerEvidenceGroups) {
					$values = @($workerEvidenceGroups[$runNumber-1] | ForEach-Object { [double]$_.stages.simulation })
					$sorted = @($values | Sort-Object)
					$nativeRun.stages.simulation = [ordered]@{ availability='available'; reason=$null; sampleCount=$values.Count; median=($sorted[4]+$sorted[5])/2.0; p95=$sorted[9]; p99=$sorted[9]; worst=$sorted[9]; workItems=$null; allocationCount=$null; allocationBytes=$null }
				}
				$runEvidence += $nativeRun
				++$nativeRunCount
				if ($nativeRun.coverageRatio -ge 0.99 -and $nativeRun.completedSteps -ge [Math]::Ceiling($measuredSteps * 0.99)) { ++$measuredRunCount }
				if ($nativeRun.jobs.idleAtEnd -and -not $nativeRun.correctness.leakedProviderWork) { ++$completeLifecycleRunCount }
				$nativeQuality = $nativeSource.quality
				$qualityAppliedForAllRuns = $qualityAppliedForAllRuns -and $null -ne $nativeQuality -and [bool]$nativeQuality.applied `
					-and [int]$nativeQuality.velocityIterations -eq [int]$quality.velocityIterations `
					-and [int]$nativeQuality.positionIterations -eq [int]$quality.positionIterations
				$lastNativeSource = $nativeSource
				continue
			}
			if ($canRouteLiveWorkload) {
				$liveSummary = Invoke-InstalledStress -Workload $workload -RunNumber $runNumber -Root $OutputDirectory -Worker $worker -Quality $quality -MeasuredSteps $measuredSteps -VariantId $variantId -Runner $installedRunnerPath
			}
			$summaryInstallPath = if ($liveSummary -and $liveSummary.installRoot -and (Test-Path -LiteralPath $liveSummary.installRoot -PathType Container)) {
				(Resolve-Path -LiteralPath $liveSummary.installRoot).Path
			} else { $null }
			$summaryUsesRequestedInstall = $null -ne $summaryInstallPath `
				-and [string]::Equals($summaryInstallPath, $resolvedInstallRoot, [StringComparison]::OrdinalIgnoreCase)
			$summaryUsesRequestedConfiguration = $liveSummary -and $liveSummary.buildConfiguration -eq 'RelWithDebInfo'
			if ($liveSummary -and $summaryUsesRequestedInstall -and $summaryUsesRequestedConfiguration) {
				$lastLiveSummary = $liveSummary
				++$liveRunCount
				$qualitySamples = @($liveSummary.physicsStageSamples | Where-Object { [bool]$_.quality.applied -and [int]$_.quality.velocityIterations -eq [int]$quality.velocityIterations -and [int]$_.quality.positionIterations -eq [int]$quality.positionIterations })
				$qualityAppliedForAllRuns = $qualityAppliedForAllRuns -and ($qualitySamples.Count -eq [int]$liveSummary.explicitStepCount)
				$measuredStageSamples = @($liveSummary.physicsStageSamples | Select-Object -Skip $warmupSteps)
				if ([bool]$liveSummary.success -and $measuredStageSamples.Count -ge [Math]::Ceiling($measuredSteps * 0.99)) { ++$measuredRunCount }
				if ([bool]$liveSummary.success -and -not [bool]$liveSummary.forcedTermination -and @($liveSummary.physicsStageSamples).Count -gt 0 `
					-and [bool](@($liveSummary.physicsStageSamples | Select-Object -Last 1).jobs.idle)) { ++$completeLifecycleRunCount }
				$runEvidence += Convert-StressSummaryToRun -Summary $liveSummary -RunNumber $runNumber -Workload $workload -Reason $workloadUnavailableReason -WarmupSteps $warmupSteps
				continue
			}
			$profileApplied = $false
			$qualityAppliedForAllRuns = $false
			if ($liveSummary -and -not $summaryUsesRequestedInstall) { $workloadUnavailableReason = 'installed runner summary did not identify the requested install root' }
			elseif ($liveSummary -and -not $summaryUsesRequestedConfiguration) { $workloadUnavailableReason = 'installed runner summary did not identify a RelWithDebInfo build' }
			$stages = [ordered]@{}
			foreach ($stageName in @('authoringPreparation','providerCommit','simulation','extraction','eventPublication','stateValidation','snapshotConstruction','transactionConstruction')) { $stages[$stageName] = New-UnavailableMetric $workloadUnavailableReason }
			$runEvidence += [ordered]@{
				runId="$variantId-run-$runNumber"; runNumber=$runNumber; completedSteps=$measuredSteps; coverageRatio=0.0; stages=$stages
				jobs=[ordered]@{ submitted=0; completed=0; rejected=0; observedWorkerThreads=0; barrierWaitMicroseconds=0; jobExecutionMicroseconds=0; idleAtEnd=$true }
				correctness=[ordered]@{ stateHash='unavailable'; eventHash='unavailable'; poseHash='unavailable'; overflowCount=0; droppedCount=0; rollbackPassed=$false; visibilityPassed=$false; leakedProviderWork=$false }
			}
		}
		$profileApplied = (($nativeRunCount -eq $effectiveRuns) -or ($canRouteLiveWorkload -and $liveRunCount -eq $effectiveRuns)) -and $qualityAppliedForAllRuns
		$lastSample = if ($lastLiveSummary) { @($lastLiveSummary.physicsStageSamples | Select-Object -Last 1) } else { @() }
		$lastQuality = if ($lastSample.Count) { $lastSample[0].quality } else { $null }
		$lastJobs = if ($lastSample.Count) { $lastSample[0].jobs } else { $null }
		$nativeWorkerResolution = if ($lastNativeSource) { $lastNativeSource.workerResolution } else { $null }
		$resolvedWorkers = if ($lastLiveSummary) { [int]$lastLiveSummary.actualWorkerCount } elseif ($nativeWorkerResolution -and $null -ne $nativeWorkerResolution.resolved) { [int]$nativeWorkerResolution.resolved } elseif ($worker -eq 'max') { [Math]::Max(1,[Environment]::ProcessorCount) } else { [int]$worker }
		$nativeAdapterLimit = if ($nativeWorkerResolution -and $null -ne $nativeWorkerResolution.adapterLimit) { [int]$nativeWorkerResolution.adapterLimit } else { $resolvedWorkers }
		# Clean same-revision parents predate provider-applied quality telemetry.
		# Their requested default 10/2 profile is still a valid performance parent;
		# only candidates and alternate profiles must prove the applied bit.
		if($BaselineIsSameRevisionParent -and [int]$quality.velocityIterations -eq 10 -and [int]$quality.positionIterations -eq 2 -and $nativeRunCount -eq $effectiveRuns) {
			$profileApplied = $true
		}
		$captureComplete = ($nativeRunCount -eq $effectiveRuns) -or ($canRouteLiveWorkload -and $liveRunCount -eq $effectiveRuns)
		# Keep an actual collection object even when there are no failures. PowerShell
		# otherwise enumerates an empty array produced by an `if` expression to `$null`,
		# which violates the evidence schema's required JSON-array contract.
		$evidenceFailures = [System.Collections.Generic.List[string]]::new()
		if (-not $captureComplete) {
			$evidenceFailures.Add($workloadUnavailableReason)
		}
		$evidence = [ordered]@{
			schemaVersion=1; status='incomparable'
			identity=[ordered]@{
				build=[ordered]@{ sourceRevision=$sourceRevision; dirtyDiffHash=$dirtyDiffHash; configuration='RelWithDebInfo'; installedTreeHash=$installedTreeHash }
				environment=[ordered]@{ machineId=$machineId; os=[Environment]::OSVersion.VersionString; cpu=$cpuName; logicalProcessors=[Environment]::ProcessorCount; memoryBytes=$memoryBytes; powerContext='developer-machine-uncontrolled' }
				workload=[ordered]@{ id=$workload.id; version=[int]$workload.version; manifestSha256=$manifestHash; sceneSha256=(Get-TextSha256 $workloadJson); population=[int]$workload.population; activeBodies=[int]$workload.activeBodies }
				# The schema intentionally keeps bounded worker requests numeric while
				# preserving the special hardware-derived "max" request as a string.
				workers=[ordered]@{ requested=if($worker -eq 'max'){'max'}else{[int]$worker}; resolved=$resolvedWorkers; adapterLimit=if($lastJobs){[int]$lastJobs.workerLimit}else{$nativeAdapterLimit} }
				quality=[ordered]@{ settingsSchemaVersion=if($lastQuality){[int]$lastQuality.settingsSchemaVersion}elseif($lastNativeSource.quality.settingsSchemaVersion){[int]$lastNativeSource.quality.settingsSchemaVersion}else{1}; settingsGeneration=if($lastQuality){[UInt64]$lastQuality.settingsGeneration}elseif($lastNativeSource.quality.settingsGeneration){[UInt64]$lastNativeSource.quality.settingsGeneration}else{1}; worldGeneration=if($lastQuality){[UInt64]$lastQuality.worldGeneration}elseif($lastNativeSource.quality.worldGeneration){[UInt64]$lastNativeSource.quality.worldGeneration}else{1}; velocityIterations=[int]$quality.velocityIterations; positionIterations=[int]$quality.positionIterations; applied=[bool]$profileApplied }
				protocol=[ordered]@{ warmupSteps=$warmupSteps; measuredSteps=$measuredSteps; runCount=$effectiveRuns; fixedDeltaSeconds=[double]$manifestObject.fixedDeltaSeconds; captureSettingsSha256=$captureSettingsHash }
			}
			# Only the caller that actually froze a pre-change same-revision parent
			# may assert baseline provenance. Ordinary candidate/dry captures default
			# false so the comparator cannot bless a post-change self-comparison.
			baseline=[ordered]@{ evidenceId="baseline-$variantId-$sourceRevision"; sourceRevision=$sourceRevision; artifactSha256=$installedTreeHash; sameRevisionParent=[bool]$BaselineIsSameRevisionParent }
			runs=$runEvidence; gates=(New-Gates); failures=$evidenceFailures
		}
		$outputPath = Join-Path $OutputDirectory "$variantId.json"
		$evidence | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $outputPath -Encoding UTF8
		$indexEntries += [ordered]@{
			workloadId=$workload.id; worker=$worker; quality=$quality.id; path=(Split-Path -Leaf $outputPath)
			sha256=(Get-FileSha256 $outputPath); status=if($captureComplete){'captured'}else{'incomparable'}; reason=if($captureComplete){$null}else{$workloadUnavailableReason}
			requestedMeasuredRuns=$Runs; requestedLifecycleRepetitions=$AcceptanceRepetitions; attemptedRuns=$effectiveRuns
			liveRunCount=$liveRunCount; nativeRunCount=$nativeRunCount; measuredRunCount=$measuredRunCount; completeLifecycleRunCount=$completeLifecycleRunCount
		}
	}
}

$indexPayload = @($indexEntries | ForEach-Object {
	"$($_.path)|$($_.sha256)|$($_.attemptedRuns)|$($_.measuredRunCount)|$($_.completeLifecycleRunCount)"
}) -join "`n"
$index = [ordered]@{
	# The index is intentionally richer than an individual schema record. It
	# preserves the exact files and routing decision needed to audit a capture.
	schemaVersion=1; sourceRevision=$sourceRevision; dirtyDiffHash=$dirtyDiffHash
	installedTreeHash=$installedTreeHash; manifestSha256=$manifestHash; runCount=$Runs
	acceptanceRepetitions=$AcceptanceRepetitions; dryRun=[bool]$DryRun; allowLiveStress=[bool]$AllowLiveStress
	provenance=[ordered]@{
		installRoot=$resolvedInstallRoot; captureScriptPath=$captureScriptPath; captureScriptSha256=$captureScriptHash
		installedRunnerPath=$installedRunnerPath; installedRunnerSha256=$installedRunnerHash
		installedEditorPath=$installedEditorPath; installedEditorSha256=$installedEditorHash
		workerScalingEvidenceSummary=$WorkerScalingEvidenceSummary; workerScalingEvidence=$workerEvidenceProvenance; workerScalingAggregates=$script:workerEvidenceAggregates
		liveRouteAvailable=[string]::IsNullOrWhiteSpace($liveRouteFailure); liveRouteFailure=$liveRouteFailure
		captureParametersSha256=(Get-TextSha256 ("$Runs|$AcceptanceRepetitions|$DryRun|$AllowLiveStress"))
	}
	workloads=$indexEntries; indexedEvidenceSha256=(Get-TextSha256 $indexPayload)
}
$indexName = if((Split-Path -Leaf $OutputDirectory) -eq 'baseline') { 'baseline-index.json' } else { 'capture-index.json' }
$indexPath = Join-Path $OutputDirectory $indexName
$index | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $indexPath -Encoding UTF8
Write-Output $indexPath
