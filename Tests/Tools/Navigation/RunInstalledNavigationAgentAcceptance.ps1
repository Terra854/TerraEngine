# Run from PowerShell 7 after the normal CMake build and install. This runner
# launches the installed editor and genuinely exported applications. Short runs
# are diagnostics only; -Qualification requires both 600-second headless cohorts.
[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$InstallRoot,
	[Parameter(Mandatory)][string]$OutputDirectory,
	[string[]]$Lanes = @('editor', 'export-rendered', 'export-headless'),
	[string[]]$Cohorts = @('direct', 'agent'),
	[ValidateSet('workload', 'clearance-parity')][string[]]$Scenarios = @('workload'),
	[ValidateRange(1, 1000)][int]$Population = 1000,
	[ValidateRange(1, 64)][int]$WorkerCount = 8,
	[ValidateRange(0, 300)][int]$WarmupSeconds = 30,
	[ValidateRange(1, 3600)][int]$MeasurementSeconds = 600,
	[ValidateRange(1, 120)][int]$SemanticSeconds = 10,
	[switch]$Qualification,
	[switch]$CacheParity,
	[switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$install = [IO.Path]::GetFullPath($InstallRoot)
$output = [IO.Path]::GetFullPath($OutputDirectory)
$installPrefix = $install.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if ($output.Equals($install, [StringComparison]::OrdinalIgnoreCase) -or
	$output.StartsWith($installPrefix, [StringComparison]::OrdinalIgnoreCase)) {
	throw 'Acceptance evidence and generated projects must remain outside the install tree.'
}
foreach ($lane in $Lanes) { if ($lane -notin @('editor', 'export-rendered', 'export-headless')) { throw "Unknown lane: $lane" } }
foreach ($cohort in $Cohorts) { if ($cohort -notin @('direct', 'agent')) { throw "Unknown cohort: $cohort" } }
if ($Lanes.Count -eq 0 -or $Cohorts.Count -eq 0) { throw 'At least one lane and cohort are required.' }
if ($Scenarios.Count -eq 0) { throw 'At least one validation scenario is required.' }
if ($Qualification -and ($Population -ne 1000 -or $WorkerCount -ne 8 -or
	$WarmupSeconds -lt 30 -or $MeasurementSeconds -lt 600 -or
	'editor' -notin $Lanes -or 'export-rendered' -notin $Lanes -or 'export-headless' -notin $Lanes -or
	'direct' -notin $Cohorts -or 'agent' -notin $Cohorts)) {
	throw 'Qualification requires all three lanes, both 1000-participant cohorts, eight workers, 30s warmup and 600s measurement in the headless lane.'
}
$manifestPath = Join-Path $install 'Tests/Fixtures/Navigation/AgentWorkload/manifest.json'
$generator = Join-Path $install 'Tests/Tools/Navigation/NewNavigationAgentWorkloadProject.ps1'
$sceneVerifier = Join-Path $install 'Tests/Tools/Navigation/VerifyNavigationWorkloadScene.ps1'
$editor = Join-Path $install 'Editor/TerraEngine-Editor.exe'
$exporter = Join-Path $install 'Editor/TerraEngine-Export.exe'
$buildMetadataPath = Join-Path $install 'Tests/Tools/Navigation/navigation-build-metadata.json'
foreach ($required in @($manifestPath, $generator, $sceneVerifier, $editor, $exporter, $buildMetadataPath)) {
	if (!(Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing installed input: $required" }
}
New-Item -ItemType Directory -Path $output -Force | Out-Null
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
$runId = 'navigation-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')
$work = Join-Path $output $runId
New-Item -ItemType Directory -Path $work | Out-Null
$processMeasurements = @{}

# Child environments are scoped to these processes. Never overwrite the user's
# persistent worker setting or run a measured cohort concurrently with another.
function Invoke-Application([string]$Executable, [string[]]$Arguments, [string]$Directory,
	[string]$LogPrefix, [int]$TimeoutSeconds) {
	$start = [Diagnostics.ProcessStartInfo]::new()
	$start.FileName = $Executable
	$start.WorkingDirectory = $Directory
	$start.UseShellExecute = $false
	$start.CreateNoWindow = $true
	$start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
	$start.RedirectStandardOutput = $true
	$start.RedirectStandardError = $true
	$start.Environment['TERRAENGINE_THREADPOOL_WORKERS'] = [string]$WorkerCount
	foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
	$process = [Diagnostics.Process]::new()
	$process.StartInfo = $start
	$startedAt = [DateTime]::UtcNow
	if (!$process.Start()) { throw "Could not start $Executable" }
	# Stream bounded process pipes directly to evidence files. Keeping the entire
	# ten-minute log in memory also hid startup progress until a timeout expired.
	$stdoutLog = [IO.FileStream]::new("$LogPrefix.stdout.log", [IO.FileMode]::Create,
		[IO.FileAccess]::Write, [IO.FileShare]::Read)
	$stderrLog = [IO.FileStream]::new("$LogPrefix.stderr.log", [IO.FileMode]::Create,
		[IO.FileAccess]::Write, [IO.FileShare]::Read)
	$stdout = $process.StandardOutput.BaseStream.CopyToAsync($stdoutLog)
	$stderr = $process.StandardError.BaseStream.CopyToAsync($stderrLog)
	$measurements = [ordered]@{ peakWorkingSetBytes = $null; peakPagedMemoryBytes = $null
		processorSeconds = $null; wallClockSeconds = $null; memorySamples = 0 }
	try {
		while (!$process.WaitForExit(1000)) {
			# A process can exit between the wait and the sample. Keep the last
			# successful OS high-water sample; unavailable is never reported as zero.
			try {
				$process.Refresh()
				$measurements.peakWorkingSetBytes = $process.PeakWorkingSet64
				$measurements.peakPagedMemoryBytes = $process.PeakPagedMemorySize64
				$measurements.processorSeconds = $process.TotalProcessorTime.TotalSeconds
				$measurements.memorySamples++
			} catch [InvalidOperationException] { }
			if (([DateTime]::UtcNow - $startedAt).TotalSeconds -gt $TimeoutSeconds) {
				# Only the exact process started above is terminated on a bounded timeout.
				$process.Kill($true)
				$process.WaitForExit()
				throw "Application exceeded $TimeoutSeconds seconds: $Executable"
			}
		}
		# Windows reports these high-water values for the exact child process;
		# they complement native navigation-owned allocation estimates.
		$measurements.wallClockSeconds = ([DateTime]::UtcNow - $startedAt).TotalSeconds
		$processMeasurements[$LogPrefix] = $measurements
		return $process.ExitCode
	} finally {
		[void]$stdout.GetAwaiter().GetResult()
		[void]$stderr.GetAwaiter().GetResult()
		$stdoutLog.Dispose()
		$stderrLog.Dispose()
		$process.Dispose()
	}
}

# Record measured metadata rather than copying the reference machine's claims.
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$computer = Get-CimInstance Win32_ComputerSystem
$operatingSystem = Get-CimInstance Win32_OperatingSystem
$powerScheme = (& powercfg /getactivescheme 2>&1 | Out-String).Trim()
$battery = @(Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue)
$record = [ordered]@{
	schemaVersion = 1; runId = $runId; qualificationRequested = [bool]$Qualification
	cacheParityRequested = [bool]($CacheParity -or $Qualification)
	clearanceParityRequested = [bool]($Qualification -or 'clearance-parity' -in $Scenarios)
	status = 'running'; sourceFree = $true
	build = (Get-Content -LiteralPath $buildMetadataPath -Raw | ConvertFrom-Json)
	fixtureSha256 = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
	machine = [ordered]@{ cpu = $cpu.Name; ramBytes = $computer.TotalPhysicalMemory
		os = $operatingSystem.Caption; osVersion = $operatingSystem.Version
		architecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
		powerMode = $powerScheme; batteryStatus = @($battery | ForEach-Object { $_.BatteryStatus })
		workerCount = $WorkerCount; buildType = 'RelWithDebInfo'
		engineSha256 = (Get-FileHash -LiteralPath (Join-Path $install 'Editor/TerraEngine.dll') -Algorithm SHA256).Hash }
	workloads = @(); errors = @()
}
$summaryPath = Join-Path $work 'navigation-agent-acceptance.json'
try {
	if ($Qualification -and $record.build.sourceRevision -eq 'unavailable') {
		throw 'Qualification requires source revision provenance from CMake configuration.'
	}
	if ($Qualification -or 'workload' -in $Scenarios) {
	foreach ($cohort in $Cohorts) {
		# Each variant loads through normal project/archive discovery. The baseline
		# captures genuine published tiles before blocker motion, never synthetic
		# cache bytes that merely happen to pass the container decoder.
		$cacheModes = if ($CacheParity -or $Qualification) { @('absent', 'current', 'stale', 'malformed') } else { @('absent') }
		$currentCache = Join-Path $work "$cohort-current.tenav"
		$staleCache = Join-Path $work "$cohort-stale.tenav"
		$referenceReachability = $null
		foreach ($cacheMode in $cacheModes) {
		$caseName = "$cohort-$cacheMode"
		$project = Join-Path $work "NavigationWorkload-$caseName"
		$package = Join-Path $work "Package-$caseName"
		if (!$DryRun) {
			& $generator -OutputDirectory $project -ManifestPath $manifestPath -InstallRoot $install -Cohort $cohort -Population $Population
			if (!$?) { throw "Fixture generation failed: $cohort" }
			# Validate the generated scene before export or any runtime process starts.
			# This catches presentation-authoring regressions (especially a light split
			# away from Sky) without changing navigation workload timing or denominator.
			$sceneInvariantPath = Join-Path $work "$caseName-scene-invariants.json"
			$sceneInvariantOutput = & $sceneVerifier -ProjectRoot $project 2>&1
			if(!$?) {
				$sceneInvariantOutput | Set-Content -LiteralPath $sceneInvariantPath -Encoding utf8
				throw "Generated scene invariant check failed: $project"
			}
			$sceneInvariantOutput | Set-Content -LiteralPath $sceneInvariantPath -Encoding utf8
			if ($cacheMode -ne 'absent') {
				$cacheDirectory = Join-Path $project 'Assets/Navigation'
				New-Item -ItemType Directory -Path $cacheDirectory -Force | Out-Null
				$cacheDestination = Join-Path $cacheDirectory 'ground.tenav'
				$cacheSource = if ($cacheMode -eq 'stale') { $staleCache } else { $currentCache }
				if (!(Test-Path -LiteralPath $cacheSource -PathType Leaf)) { throw "Baseline did not publish cache fixture: $cacheSource" }
				if ($cacheMode -eq 'malformed') {
					# A non-empty truncated container passes the optional export size
					# check but must be rejected atomically by real runtime decoding.
					[IO.File]::WriteAllBytes($cacheDestination, ([IO.File]::ReadAllBytes($cacheSource)[0..7]))
				} else { Copy-Item -LiteralPath $cacheSource -Destination $cacheDestination }
			}
			$profile = Join-Path $project 'BuildProfiles/navigation.json'
			$exportLog = Join-Path $work "$caseName-export"
			$exportExit = Invoke-Application $exporter @('--project', $project, '--profile', $profile,
				'--output', $package, '--evidence', (Join-Path $work "export-evidence-$caseName"),
				'--run-id', "$runId-$caseName") (Split-Path $exporter) $exportLog 900
			if ($exportExit -ne 0) { throw "Export failed ($exportExit); see $exportLog.stdout.log" }
		}
		# GetExportLauncherFileName derives the exported executable from this exact
		# generated project basename. Require the sibling archive as an identity check.
		$launcher = Join-Path $package ((Split-Path $project -Leaf) + '.exe')
		if (!$DryRun -and (!(Test-Path -LiteralPath $launcher) -or
			!(Test-Path -LiteralPath ([IO.Path]::ChangeExtension($launcher, '.tepak'))))) {
			throw "Export did not produce the expected launcher/archive pair: $launcher"
		}
		foreach ($lane in $Lanes) {
			$duration = if ($lane -eq 'export-headless' -and $cacheMode -eq 'absent') { $MeasurementSeconds } else { $SemanticSeconds }
			$optionsPath = Join-Path $work "$caseName-$lane-options.json"
			$reportPath = Join-Path $work "$caseName-$lane-report.json"
			$options = [ordered]@{ cohort = $cohort; warmupSeconds = $WarmupSeconds
				executionMode = $(if ($lane -eq 'editor') { 'editor-play' } else { 'export' })
				measurementSeconds = $duration; requiredWorkerCount = $WorkerCount
				population = $Population; changesPerSecond = $manifest.cohorts.destinationChangesPerSecond
				fixtureManifestPath = $manifestPath; requireRenderedPresentation = ($lane -ne 'export-headless')
				qualification = ([bool]$Qualification -and $lane -eq 'export-headless' -and $cacheMode -eq 'absent')
				expectedCacheMode = $cacheMode }
			if ($cacheMode -eq 'absent' -and ($CacheParity -or $Qualification)) {
				$options.cacheOutputPath = $currentCache
				$options.cacheStaleOutputPath = $staleCache
			}
			$options | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $optionsPath -Encoding utf8
			$executable = if ($lane -eq 'editor') { $editor } else { $launcher }
			$arguments = if ($lane -eq 'editor') {
				@('--project', $project, '--validate-editor-navigation-agents', $optionsPath, $reportPath)
			} else { @('--validate-export-navigation-agents', $optionsPath, $reportPath) }
			if ($lane -eq 'export-headless') { $arguments += '--headless-cpu-fallback' }
			$item = [ordered]@{ cohort = $cohort; cacheMode = $cacheMode; lane = $lane; executable = $executable
				options = $optionsPath; report = $reportPath; executedCaseCount = 0; status = 'not_run' }
			$record.workloads += $item
			if ($DryRun) { $item.status = 'dry_run'; continue }
			Write-Output "Running $cohort / $cacheMode / $lane ($WarmupSeconds s warmup + $duration s measurement)."
			$exitCode = Invoke-Application $executable $arguments (Split-Path $executable) (Join-Path $work "$caseName-$lane") ($WarmupSeconds + $duration + 180)
			$item.exitCode = $exitCode
			$item.processMeasurements = $processMeasurements[(Join-Path $work "$caseName-$lane")]
			if ($exitCode -ne 0 -or !(Test-Path -LiteralPath $reportPath)) {
				$item.status = 'failed'; throw "Application lane failed: $cohort / $lane ($exitCode)"
			}
			$report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
			$item.executedCaseCount = $report.executedCaseCount
			if (!$report.succeeded -or !$report.orderlyShutdown -or $report.executedCaseCount -le 0 -or
				$report.actualWorkerCount -ne $WorkerCount) {
				$item.status = 'failed'; throw "Incomplete or failed application evidence: $reportPath"
			}
			$settingsEvidence = [ordered]@{ limits = $report.settings; profile = $report.pinnedProfile }
			$settingsBytes = [Text.Encoding]::UTF8.GetBytes(($settingsEvidence | ConvertTo-Json -Depth 10 -Compress))
			$item.settingsSha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($settingsBytes))
			$item.sceneSha256 = (Get-FileHash -LiteralPath (Join-Path $project 'Assets/Scenes/Startup.tescene') -Algorithm SHA256).Hash
			if ($lane -ne 'export-headless' -and !$report.renderedPresentationObserved) {
				$item.status = 'failed'; throw "Rendered lane omitted actual presentation evidence: $reportPath"
			}
			if ($CacheParity -or $Qualification) {
				if (($cacheMode -eq 'current') -ne [bool]$report.cacheUsed -or
					($cacheMode -in @('stale', 'malformed') -and [string]::IsNullOrWhiteSpace($report.cacheRejectionReason))) {
					$item.status = 'failed'; throw "Cache consumption/rejection evidence failed: $reportPath"
				}
				if ($cacheMode -eq 'absent' -and !$report.cacheOutputWritten) {
					$item.status = 'failed'; throw "Baseline did not capture current coverage: $reportPath"
				}
				$reachability = $report.warmupReachability | ConvertTo-Json -Depth 20 -Compress
				if (@($report.warmupReachability).Count -ne $Population) {
					$item.status = 'failed'; throw "Incomplete warmup reachability denominator: $reportPath"
				}
				if ($null -eq $referenceReachability) { $referenceReachability = $reachability }
				elseif ($reachability -ne $referenceReachability) {
					$item.status = 'failed'; throw "Editor/export/cache reachability differs: $reportPath"
				}
			}
			if ($Qualification -and $lane -eq 'export-headless' -and $cacheMode -eq 'absent') {
				# Enforce the workload denominator independently of the native success
				# flag. Completion latency alone cannot hide a smaller or shorter run.
				if (!$report.qualification -or $report.measuredSeconds -lt 600 -or
					$report.population -ne 1000 -or $report.retainedPeak -lt 1000 -or
					$report.totalSubmitted -lt 61000 -or $report.measurementSubmitted -lt 60000 -or
					$report.measurementFinalized -ne $report.measurementSubmitted -or
					$report.admitted -le 0 -or $report.admittedFinalized -ne $report.admitted -or
					$report.pendingAdmitted -ne 0 -or
					!$report.admittedLatencyMilliseconds.comparableForQualification -or
					$report.admittedLatencyMilliseconds.sampleCount -ne $report.admitted -or
					$report.admittedLatencyMilliseconds.p95 -gt 100 -or
					$report.movingBlockerUpdates -lt 600 -or
					$report.remainingRequests -ne 0 -or $report.remainingRoutes -ne 0 -or
					$report.remainingAgents -ne 0 -or
					$item.processMeasurements.memorySamples -lt 1 -or
					!$report.eligibleStaticSourcesObserved -or $report.eligibleStaticSourceCount -lt 65 -or
					!$report.semanticChecks.navigationDoesNotMoveOwners) {
					$item.status = 'failed'; throw "Qualification denominator, safety or cleanup failed: $reportPath"
				}
			}
			$item.status = 'passed'
			$record | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $summaryPath -Encoding utf8
		}
		}
	}
	}
	# Clearance is a separate authored scene so walls never alter the pinned
	# performance workload. Each profile gets its own legitimately generated cache.
	if ($Qualification -or 'clearance-parity' -in $Scenarios) {
		foreach ($profileName in @('small', 'large')) {
			$clearanceCache = Join-Path $work "clearance-$profileName-current.tenav"
			$clearanceStaleCache = Join-Path $work "clearance-$profileName-stale.tenav"
			$referenceClearance = $null
			# Clearance parity must exercise the same optional-cache failure modes as
			# the workload lanes. Each non-current artifact is mounted into the normal
			# project path so the runtime must reject it and rebuild from authored data.
			foreach ($cacheMode in @('absent', 'current', 'stale', 'malformed')) {
				$caseName = "clearance-$profileName-$cacheMode"
				$project = Join-Path $work "NavigationWorkload-$caseName"
				$package = Join-Path $work "Export-$caseName"
				if (!$DryRun) {
					& $generator -OutputDirectory $project -ManifestPath $manifestPath -InstallRoot $install -Cohort direct -Population 1 -Scenario clearance-parity
					if (!$?) { throw "Clearance fixture generation failed: $profileName" }
					$sceneInvariantPath = Join-Path $work "$caseName-scene-invariants.json"
					$sceneInvariantOutput = & $sceneVerifier -ProjectRoot $project 2>&1
					if(!$?) {
						$sceneInvariantOutput | Set-Content -LiteralPath $sceneInvariantPath -Encoding utf8
						throw "Clearance scene invariant check failed: $project"
					}
					$sceneInvariantOutput | Set-Content -LiteralPath $sceneInvariantPath -Encoding utf8
					if ($cacheMode -eq 'current') {
						if (!(Test-Path -LiteralPath $clearanceCache -PathType Leaf)) { throw "Missing generated clearance cache: $clearanceCache" }
						Copy-Item -LiteralPath $clearanceCache -Destination (Join-Path $project 'Assets/Navigation/ground.tenav')
					}
					if ($cacheMode -in @('stale', 'malformed')) {
						$cacheDirectory = Join-Path $project 'Assets/Navigation'
						New-Item -ItemType Directory -Path $cacheDirectory -Force | Out-Null
						$cacheDestination = Join-Path $cacheDirectory 'ground.tenav'
						$cacheSource = if ($cacheMode -eq 'stale') { $clearanceStaleCache } else { $clearanceCache }
						if (!(Test-Path -LiteralPath $cacheSource -PathType Leaf)) { throw "Baseline did not publish clearance cache fixture: $cacheSource" }
						if ($cacheMode -eq 'malformed') {
							$cacheBytes = [IO.File]::ReadAllBytes($cacheSource)
							if ($cacheBytes.Length -lt 8) { throw "Clearance cache fixture is too small to truncate: $cacheSource" }
							[IO.File]::WriteAllBytes($cacheDestination, $cacheBytes[0..7])
						} else {
							Copy-Item -LiteralPath $cacheSource -Destination $cacheDestination
						}
					}
					$exportLog = Join-Path $work "$caseName-export"
					$exportExit = Invoke-Application $exporter @('--project', $project, '--profile', (Join-Path $project 'BuildProfiles/navigation.json'),
						'--output', $package, '--evidence', (Join-Path $work "export-evidence-$caseName"), '--run-id', "$runId-$caseName") (Split-Path $exporter) $exportLog 900
					if ($exportExit -ne 0) { throw "Clearance export failed ($exportExit); see $exportLog.stdout.log" }
				}
				$launcher = Join-Path $package ((Split-Path $project -Leaf) + '.exe')
				foreach ($lane in $Lanes) {
					$optionsPath = Join-Path $work "$caseName-$lane-options.json"
					$reportPath = Join-Path $work "$caseName-$lane-report.json"
					$options = [ordered]@{ scenario = 'clearance-parity'; clearanceProfile = $profileName; cohort = 'direct'
						executionMode = $(if ($lane -eq 'editor') { 'editor-play' } else { 'export' })
						fixtureManifestPath = $manifestPath; requiredWorkerCount = $WorkerCount; population = 1
						warmupSeconds = 0; measurementSeconds = $SemanticSeconds; timeoutSeconds = 120
						requireRenderedPresentation = ($lane -ne 'export-headless'); expectedCacheMode = $cacheMode }
					if ($cacheMode -eq 'absent') {
						$options.cacheOutputPath = $clearanceCache
						$options.cacheStaleOutputPath = $clearanceStaleCache
					}
					$options | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $optionsPath -Encoding utf8
					$executable = if ($lane -eq 'editor') { $editor } else { $launcher }
					$arguments = if ($lane -eq 'editor') {
						@('--project', $project, '--validate-editor-navigation-agents', $optionsPath, $reportPath)
					} else { @('--validate-export-navigation-agents', $optionsPath, $reportPath) }
					if ($lane -eq 'export-headless') { $arguments += '--headless-cpu-fallback' }
					$item = [ordered]@{ scenario = 'clearance-parity'; profile = $profileName; cacheMode = $cacheMode
						lane = $lane; executable = $executable; options = $optionsPath; report = $reportPath
						executedCaseCount = 0; status = 'not_run' }
					$record.workloads += $item
					if ($DryRun) { $item.status = 'dry_run'; continue }
					Write-Output "Running clearance / $profileName / $cacheMode / $lane."
					$logPrefix = Join-Path $work "$caseName-$lane"
					$item.exitCode = Invoke-Application $executable $arguments (Split-Path $executable) $logPrefix 180
					$item.processMeasurements = $processMeasurements[$logPrefix]
					if ($item.exitCode -ne 0 -or !(Test-Path -LiteralPath $reportPath)) { throw "Clearance application failed: $caseName / $lane" }
					$report = Get-Content -LiteralPath $reportPath -Raw | ConvertFrom-Json
					$item.executedCaseCount = $report.executedCaseCount
					$expectedOutcome = if ($profileName -eq 'small') { 'Complete' } else { 'Unreachable' }
					if (!$report.succeeded -or !$report.orderlyShutdown -or !$report.clearanceExpectationPassed -or
						$report.executedCaseCount -lt 1 -or $report.actualWorkerCount -ne $WorkerCount -or
						$report.clearanceRoute.outcome -ne $expectedOutcome -or
						[bool]$report.cacheUsed -ne ($cacheMode -eq 'current') -or
						($cacheMode -in @('stale', 'malformed') -and [string]::IsNullOrWhiteSpace($report.cacheRejectionReason)) -or
						($cacheMode -eq 'absent' -and !$report.cacheOutputWritten) -or
						($lane -ne 'export-headless' -and !$report.renderedPresentationObserved)) {
						throw "Incomplete clearance/cache evidence: $reportPath"
					}
					$observed = [ordered]@{ profile = $report.pinnedProfile; outcome = $report.clearanceRoute.outcome
						reason = $report.clearanceRoute.reason } | ConvertTo-Json -Depth 10 -Compress
					if ($null -eq $referenceClearance) { $referenceClearance = $observed }
					elseif ($referenceClearance -ne $observed) { throw "Editor/export/cache clearance differs: $reportPath" }
					$item.sceneSha256 = (Get-FileHash -LiteralPath (Join-Path $project 'Assets/Scenes/Startup.tescene') -Algorithm SHA256).Hash
					$item.status = 'passed'
					$record | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $summaryPath -Encoding utf8
				}
			}
		}
	}
	$record.status = if ($DryRun) { 'dry_run' } else { 'passed' }
} catch {
	$record.status = 'failed'
	$record.errors += $_.Exception.Message
} finally {
	$record | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $summaryPath -Encoding utf8
}
Write-Output "Acceptance summary: $summaryPath ($($record.status))"
if ($record.status -eq 'failed') { throw ($record.errors -join '; ') }
