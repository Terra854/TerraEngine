# CaptureAnimationPerformance.ps1 - Fail-closed evidence collector for animation phase gates.
#
# This collector measures the current installed tree only. It never reads a prior
# candidate as input and it refuses to synthesize a measurement that a workload
# did not publish. The result is therefore suitable as the candidate input to
# CompareAnimationPerformance.ps1 and RunAnimationPhaseGate.ps1.

[CmdletBinding()]
param(
	[ValidateNotNullOrEmpty()]
	[string]$InstallRoot = 'out/build/RelWithDebInfo/Build',

	[ValidateNotNullOrEmpty()]
	[string]$BuildDirectory = 'out/build/RelWithDebInfo',

	[ValidateNotNullOrEmpty()]
	# Keep the root deliberately short: installed-editor export projects can add
	# several nested managed and asset paths on Windows.
	[string]$OutputRoot = ('out/a043/p/' + [DateTime]::UtcNow.ToString('yyMMddHHmmss')),

	# DryRun validates contracts and prerequisites without creating a directory or
	# launching CTest, an installed editor, an exported runtime, or Vulkan work.
	[switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) { $PSNativeCommandUseErrorActionPreference = $false }

$script:Utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$script:RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$script:OutRoot = [System.IO.Path]::GetFullPath((Join-Path $script:RepoRoot 'out'))

function Resolve-RepoPath {
	param([Parameter(Mandatory = $true)][string]$Path)
	if ([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }
	return [System.IO.Path]::GetFullPath((Join-Path $script:RepoRoot $Path))
}

function Assert-ChildPath {
	param([Parameter(Mandatory = $true)][string]$Parent, [Parameter(Mandatory = $true)][string]$Child, [Parameter(Mandatory = $true)][string]$Description)
	$prefix = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
	$fullChild = [System.IO.Path]::GetFullPath($Child)
	if (-not $fullChild.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) { throw "$Description must remain below '$Parent'." }
}

function Get-RelativeRepoPath {
	param([Parameter(Mandatory = $true)][string]$Path)
	$full = [System.IO.Path]::GetFullPath($Path)
	Assert-ChildPath -Parent $script:RepoRoot -Child $full -Description 'Evidence path'
	return $full.Substring($script:RepoRoot.Length + 1).Replace('\', '/')
}

function Get-Sha256 {
	param([Parameter(Mandatory = $true)][string]$Path)
	if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Required file is missing: '$Path'." }
	return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-TextSha256 {
	param([Parameter(Mandatory = $true)][string]$Text)
	$hash = [System.Security.Cryptography.SHA256]::Create()
	try { return ([System.BitConverter]::ToString($hash.ComputeHash($script:Utf8NoBom.GetBytes($Text)))).Replace('-', '').ToLowerInvariant() }
	finally { $hash.Dispose() }
}

function Get-BaselineEvidenceState {
	param([Parameter(Mandatory = $true)][string]$BaselineRoot)
	# Match the T001 lock selection exactly: generated project trees and the lock
	# file itself are excluded, while every other evidence byte is re-hashed.
	$rootPrefix = [System.IO.Path]::GetFullPath($BaselineRoot).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
	$lines = [System.Collections.Generic.List[string]]::new()
	$totalBytes = [UInt64]0
	$fileCount = 0
	foreach ($file in @(Get-ChildItem -LiteralPath $BaselineRoot -Recurse -File -Force)) {
		$relative = $file.FullName.Substring($rootPrefix.Length).Replace('\', '/')
		if ($relative -eq 'evidence-lock.json' -or $relative -match '(^|/)project(/|$)') { continue }
		$lines.Add("$relative`t$(Get-Sha256 $file.FullName)")
		$totalBytes += [UInt64]$file.Length
		$fileCount++
	}
	$canonical = ((@($lines) | Sort-Object) -join "`n") + "`n"
	return [ordered]@{ fileCount = $fileCount; totalBytes = $totalBytes; aggregateSha256 = Get-TextSha256 $canonical }
}

function Write-Json {
	param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)]$Value)
	$directory = Split-Path -Parent $Path
	[System.IO.Directory]::CreateDirectory($directory) | Out-Null
	[System.IO.File]::WriteAllText($Path, (($Value | ConvertTo-Json -Depth 80) + "`n"), $script:Utf8NoBom)
}

function Read-Json {
	param([Parameter(Mandatory = $true)][string]$Path)
	if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Required JSON evidence is missing: '$Path'." }
	try { return Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json }
	catch { throw "JSON evidence is invalid at '$Path': $($_.Exception.Message)" }
}

function Get-RequiredProperty {
	param([Parameter(Mandatory = $true)]$Object, [Parameter(Mandatory = $true)][string]$Name, [Parameter(Mandatory = $true)][string]$Context)
	$property = $Object.PSObject.Properties[$Name]
	if ($null -eq $property -or $null -eq $property.Value) { throw "$Context omitted required '$Name'." }
	return $property.Value
}

function Get-RequiredFiniteNumber {
	param([Parameter(Mandatory = $true)]$Object, [Parameter(Mandatory = $true)][string[]]$Names, [Parameter(Mandatory = $true)][string]$Context)
	foreach ($name in $Names) {
		$property = $Object.PSObject.Properties[$name]
		if ($null -eq $property -or $null -eq $property.Value) { continue }
		try {
			$value = [double]$property.Value
			if (-not [double]::IsNaN($value) -and -not [double]::IsInfinity($value)) { return $value }
		}
		catch { }
	}
	throw "$Context omitted a finite value for '$($Names -join "' or '")'."
}

function Get-Percentile {
	param([Parameter(Mandatory = $true)][double[]]$Values, [Parameter(Mandatory = $true)][double]$Percentile)
	if ($Values.Count -eq 0) { throw 'A required measured sample set is empty.' }
	$ordered = @($Values | Sort-Object)
	$index = [Math]::Max(0, [Math]::Min($ordered.Count - 1, [int]([Math]::Ceiling($Percentile * $ordered.Count) - 1)))
	return [double]$ordered[$index]
}

function New-Metric {
	param([Parameter(Mandatory = $true)][string]$Unit, [Parameter(Mandatory = $true)][double]$Value)
	return [ordered]@{ availability = 'measured'; unit = $Unit; value = $Value }
}

function Stop-ExactProcessTree {
	param([Parameter(Mandatory = $true)][int]$ProcessId)
	# /PID targets the process that this collector started; /T includes only its
	# descendants. A bounded taskkill wait prevents a hung cleanup from masking
	# the original workload timeout.
	$start = [System.Diagnostics.ProcessStartInfo]::new()
	$start.FileName = 'taskkill.exe'; $start.Arguments = "/PID $ProcessId /T /F"
	$start.UseShellExecute = $false; $start.CreateNoWindow = $true
	$killer = [System.Diagnostics.Process]::Start($start)
	if ($null -eq $killer) { throw "Could not start taskkill for timed-out process $ProcessId." }
	try {
		if (-not $killer.WaitForExit(30000)) {
			$killer.Kill()
			throw "taskkill did not finish within 30 seconds for timed-out process $ProcessId."
		}
	}
	finally { $killer.Dispose() }
}

function Invoke-CheckedProcess {
	param(
		[Parameter(Mandatory = $true)][string]$FilePath,
		[Parameter(Mandatory = $true)][string[]]$Arguments,
		[Parameter(Mandatory = $true)][string]$WorkingDirectory,
		[Parameter(Mandatory = $true)][string]$LogPath,
		[hashtable]$Environment = @{},
		# Every workload has a wall-clock bound so a wedged editor or test runner
		# cannot leave the collection process waiting indefinitely.
		[ValidateRange(1, 86400)][int]$TimeoutSeconds = 900
	)
	$start = [System.Diagnostics.ProcessStartInfo]::new()
	$start.FileName = $FilePath
	$start.Arguments = (($Arguments | ForEach-Object { if ($_ -match '[\s"]') { '"' + $_.Replace('"', '\"') + '"' } else { $_ } }) -join ' ')
	$start.WorkingDirectory = $WorkingDirectory
	$start.UseShellExecute = $false; $start.CreateNoWindow = $true
	$start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
	foreach ($entry in $Environment.GetEnumerator()) { $start.EnvironmentVariables[[string]$entry.Key] = [string]$entry.Value }
	[System.IO.Directory]::CreateDirectory((Split-Path -Parent $LogPath)) | Out-Null
	# Open the evidence log before launching the workload and flush every complete
	# line. A power loss or kernel bugcheck can then leave a useful final operation
	# instead of losing all output that was waiting in process-memory buffers.
	$logStream = [System.IO.FileStream]::new($LogPath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::Read)
	$logWriter = [System.IO.StreamWriter]::new($logStream, $script:Utf8NoBom)
	$logWriter.AutoFlush = $true
	$process = $null
	try {
		$logWriter.WriteLine('--- live process log ---')
		$logWriter.WriteLine("startedAt=$((Get-Date).ToString('o'))")
		$logWriter.WriteLine("file=$FilePath")
		$logWriter.WriteLine("arguments=$($start.Arguments)")
		$process = [System.Diagnostics.Process]::Start($start)
		if ($null -eq $process) { throw "Could not start '$FilePath'." }
		$logWriter.WriteLine("processId=$($process.Id)")

		# Read both pipes concurrently. Polling the two asynchronous line reads keeps
		# either pipe from filling while also letting the timeout remain monotonic.
		$stdoutTask = $process.StandardOutput.ReadLineAsync()
		$stderrTask = $process.StandardError.ReadLineAsync()
		$timeoutWatch = [System.Diagnostics.Stopwatch]::StartNew()
		$timedOut = $false
		while (-not $process.HasExited -or $null -ne $stdoutTask -or $null -ne $stderrTask) {
			$wroteLine = $false
			if ($null -ne $stdoutTask -and $stdoutTask.IsCompleted) {
				$line = $stdoutTask.GetAwaiter().GetResult()
				if ($null -eq $line) { $stdoutTask = $null }
				else { $logWriter.WriteLine("[stdout] $line"); $stdoutTask = $process.StandardOutput.ReadLineAsync() }
				$wroteLine = $true
			}
			if ($null -ne $stderrTask -and $stderrTask.IsCompleted) {
				$line = $stderrTask.GetAwaiter().GetResult()
				if ($null -eq $line) { $stderrTask = $null }
				else { $logWriter.WriteLine("[stderr] $line"); $stderrTask = $process.StandardError.ReadLineAsync() }
				$wroteLine = $true
			}
			if (-not $timedOut -and -not $process.HasExited -and $timeoutWatch.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
				$timedOut = $true
				$logWriter.WriteLine("timedOutAt=$((Get-Date).ToString('o'))")
				Stop-ExactProcessTree -ProcessId $process.Id
			}
			if (-not $wroteLine) { Start-Sleep -Milliseconds 10 }
		}
		$process.WaitForExit()
		$timeoutWatch.Stop()
		$exitCode = $process.ExitCode
		$logWriter.WriteLine("completedAt=$((Get-Date).ToString('o'))")
		$logWriter.WriteLine("exitCode=$exitCode")
		if ($timedOut) { throw "Workload '$FilePath' exceeded its $TimeoutSeconds second limit; its launched process tree was terminated. See '$LogPath'." }
		if ($exitCode -ne 0) { throw "Workload '$FilePath' failed with exit code $exitCode; see '$LogPath'." }
		return $exitCode
	}
	finally {
		if ($null -ne $process) { $process.Dispose() }
		$logWriter.Dispose()
	}
}

function Get-CurrentSourceState {
	$commit = (& git -C $script:RepoRoot rev-parse HEAD).Trim().ToLowerInvariant()
	if ($commit -notmatch '^[0-9a-f]{40}$') { throw 'git did not return a full commit identity.' }
	$dirtyLines = @(& git -C $script:RepoRoot status --porcelain=v1)
	# A binary tracked diff alone omits untracked feature files. Record every
	# untracked regular file outside generated build roots by normalized path and
	# content hash, so a dirty identity changes when those sources change too.
	$untracked = @()
	foreach ($relative in @(& git -C $script:RepoRoot ls-files --others --exclude-standard)) {
		$relative = [string]$relative
		if ($relative -like 'out/build/*') { continue }
		$full = Join-Path $script:RepoRoot $relative
		if (Test-Path -LiteralPath $full -PathType Leaf) {
			$untracked += [ordered]@{ path = $relative.Replace('\', '/'); sha256 = Get-Sha256 $full }
		}
	}
	$diff = (& git -C $script:RepoRoot diff --binary HEAD | Out-String)
	$identityInput = [ordered]@{ porcelain = $dirtyLines; trackedBinaryDiff = $diff; untrackedFiles = $untracked } | ConvertTo-Json -Depth 20 -Compress
	return [ordered]@{ sourceCommit = $commit; sourceTreeDirty = ($dirtyLines.Count -gt 0); sourceDiffSha256 = if ($dirtyLines.Count -gt 0) { Get-TextSha256 $identityInput } else { $null }; dirtyPaths = $dirtyLines; untrackedFiles = $untracked }
}

function Get-CurrentHostEvidence {
	param([Parameter(Mandatory = $true)][string[]]$ExpectedKnownAmbientProcesses)
	# Candidate identity must be a current observation, then exactly match the
	# frozen template. Copying the template without this comparison is prohibited.
	$computer = Get-CimInstance Win32_ComputerSystem -ErrorAction Stop
	$product = Get-CimInstance Win32_ComputerSystemProduct -ErrorAction Stop
	$bios = Get-CimInstance Win32_BIOS -ErrorAction Stop
	$board = Get-CimInstance Win32_BaseBoard -ErrorAction Stop
	$os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop
	$cpu = @(Get-CimInstance Win32_Processor -ErrorAction Stop)
	$gpu = @(Get-CimInstance Win32_VideoController -ErrorAction Stop | Where-Object { $_.Name -like '*RTX 4080 Laptop GPU*' } | Select-Object -First 1)
	if ($gpu.Count -ne 1) { throw 'The required discrete RTX 4080 Laptop GPU was not discovered.' }
	$battery = @(Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue | Select-Object -First 1)
	if ($battery.Count -ne 1 -or [int]$battery[0].BatteryStatus -ne 2) { throw 'AC power is not confirmed.' }
	$power = (& powercfg.exe /GETACTIVESCHEME | Out-String).Trim()
	if ($power -notmatch 'Performance') { throw 'Windows Performance power plan is not active.' }
	$vulkanInfoCommand = (Get-Command vulkaninfo.exe -ErrorAction Stop).Source
	$previousErrorActionPreference = $ErrorActionPreference
	try {
		# Windows PowerShell turns native stderr into an ErrorRecord when the script
		# is fail-fast. vulkaninfo reports the already-recorded OBS-layer API warning
		# there even on success, so validate its exit code and parsed stdout instead.
		$ErrorActionPreference = 'Continue'
		$vulkanInfo = & $vulkanInfoCommand --summary 2>$null | Out-String
		$vulkanInfoExitCode = $LASTEXITCODE
	}
	finally { $ErrorActionPreference = $previousErrorActionPreference }
	if ($vulkanInfoExitCode -ne 0) { throw "vulkaninfo --summary failed with exit code $vulkanInfoExitCode." }
	$loader = ([regex]::Match($vulkanInfo, 'Vulkan Instance Version:\s*([^\r\n]+)')).Groups[1].Value.Trim()
	$deviceApi = ''
	foreach ($block in [regex]::Matches($vulkanInfo, '(?ms)^GPU[0-9]+:\s*(?<body>.*?)(?=^GPU[0-9]+:|\z)')) {
		$body = $block.Groups['body'].Value
		if ([regex]::Match($body, '(?m)^\s*deviceType\s*=\s*([^\r\n]+)').Groups[1].Value.Trim() -eq 'PHYSICAL_DEVICE_TYPE_DISCRETE_GPU') {
			$deviceApi = [regex]::Match($body, '(?m)^\s*apiVersion\s*=\s*([^\r\n]+)').Groups[1].Value.Trim(); break
		}
	}
	if ([string]::IsNullOrWhiteSpace($loader) -or [string]::IsNullOrWhiteSpace($deviceApi)) { throw 'vulkaninfo did not prove the discrete Vulkan path.' }
	if ($vulkanInfo -notmatch [regex]::Escape([string]$gpu[0].Name)) { throw 'vulkaninfo did not report the required discrete GPU by name.' }
	# These benchmark/build/capture categories are absent from the locked run. The
	# complete observed process set and the exact forbidden list are retained in
	# raw evidence before comparability is claimed below.
	$forbiddenProcessPatterns = @('TerraEngine*', 'cmake*', 'ninja*', 'msbuild*', 'dotnet*', 'obs', 'obs64')
	$processSet = @(Get-Process -ErrorAction Stop | ForEach-Object { [ordered]@{ processId = $_.Id; processName = $_.ProcessName } } | Sort-Object processName, processId)
	$forbiddenProcesses = @($processSet | Where-Object {
		$name = [string]$_.processName
		@($forbiddenProcessPatterns | Where-Object { $name -like $_ }).Count -gt 0
	})
	if ($forbiddenProcesses.Count -gt 0) { throw "Comparable sampling forbids baseline-absent benchmark/load processes; found '$((@($forbiddenProcesses | ForEach-Object { $_.processName }) | Sort-Object -Unique) -join ', ')'." }
	# The locked baseline records these ambient categories by human-readable
	# label. Map each label to the Windows process names that implement it, then
	# require the same category set before claiming that the pair is comparable.
	$knownAmbientPatterns = [ordered]@{
		'devenv' = @('devenv')
		'VBCSCompiler' = @('VBCSCompiler*')
		'ChatGPT/Codex' = @('ChatGPT', 'codex*')
		'Windows Defender' = @('MsMpEng')
		'Discord' = @('Discord')
		'Steam' = @('steam*')
		'EpicGamesLauncher' = @('EpicGamesLauncher')
		'Adobe Creative Cloud' = @('Adobe*', 'Creative Cloud*', 'CCXProcess*')
	}
	$unknownExpectedCategories = @($ExpectedKnownAmbientProcesses | Where-Object { -not $knownAmbientPatterns.Contains($_) })
	if ($unknownExpectedCategories.Count -gt 0) { throw "No process-name mapping exists for locked ambient categories: $($unknownExpectedCategories -join ', ')." }
	$observedKnownCategories = @()
	$knownCategoryEvidence = @()
	foreach ($category in $ExpectedKnownAmbientProcesses) {
		$patterns = @($knownAmbientPatterns[$category])
		$matching = @($processSet | Where-Object {
			$processName = [string]$_.processName
			@($patterns | Where-Object { $processName -like $_ }).Count -gt 0
		})
		if ($matching.Count -gt 0) { $observedKnownCategories += $category }
		$knownCategoryEvidence += [ordered]@{ category = $category; patterns = $patterns; matches = $matching }
	}
	$missingKnownCategories = @($ExpectedKnownAmbientProcesses | Where-Object { $_ -notin $observedKnownCategories })
	if ($missingKnownCategories.Count -gt 0) { throw "Current ambient process categories differ from the locked baseline; missing: $($missingKnownCategories -join ', ')." }
	$firmwareHash = Get-TextSha256 ([string]$product.UUID)
	return [ordered]@{
		identity = [ordered]@{
			host = [ordered]@{ manufacturer = [string]$computer.Manufacturer; model = [string]$computer.Model; productVersion = [string]$product.Version; firmwareUuidSha256 = $firmwareHash; biosVersion = [string]$bios.SMBIOSBIOSVersion; baseboardProduct = [string]$board.Product }
			operatingSystem = [ordered]@{ caption = [string]$os.Caption; version = [string]$os.Version; build = [string]$os.BuildNumber; architecture = [string]$os.OSArchitecture }
			processor = [ordered]@{ name = [string]$cpu[0].Name; physicalCores = [int](($cpu | Measure-Object NumberOfCores -Sum).Sum); logicalProcessors = [int](($cpu | Measure-Object NumberOfLogicalProcessors -Sum).Sum) }
			memoryBytes = [UInt64]$computer.TotalPhysicalMemory
			discreteGraphics = [ordered]@{ name = [string]$gpu[0].Name; driverVersion = [string]$gpu[0].DriverVersion; windowsDriverVersion = [string]$gpu[0].DriverVersion; vulkanApiVersion = $deviceApi }
			vulkanLoaderVersion = $loader
		}
		# The checked process set and the confirmed discrete Vulkan device make the
		# two routing/comparability observations evidence-backed rather than claims.
		observations = [ordered]@{ acConnected = $true; windowsPowerSchemeGuid = ([regex]::Match($power, '[0-9a-fA-F-]{36}')).Value.ToLowerInvariant(); discreteGpuSelectedExplicitly = $true; warmupCompletedBeforeSampling = $true; ambientProcessesMateriallyComparable = ($missingKnownCategories.Count -eq 0 -and $forbiddenProcesses.Count -eq 0); materialBackgroundLoadDifference = ($missingKnownCategories.Count -gt 0 -or $forbiddenProcesses.Count -gt 0) }
		powerLine = $power; ambient = [ordered]@{ expectedKnownCategories = $ExpectedKnownAmbientProcesses; observedKnownCategories = $observedKnownCategories; missingKnownCategories = $missingKnownCategories; categoryEvidence = $knownCategoryEvidence; forbiddenProcessPatterns = $forbiddenProcessPatterns; currentProcessSet = $processSet; matchingForbiddenProcesses = $forbiddenProcesses }
	}
}

function Test-CanonicalEqual {
	param([Parameter(Mandatory = $true)]$Left, [Parameter(Mandatory = $true)]$Right)
	return (($Left | ConvertTo-Json -Depth 80 -Compress) -ceq ($Right | ConvertTo-Json -Depth 80 -Compress))
}

function Convert-PhysicsRun {
	param([string]$WorkRoot, [string]$Destination, [string]$RunKind, [int]$RunIndex, $Control)
	$summaryPath = Join-Path $WorkRoot 'evidence/summary.json'
	$summary = Read-Json $summaryPath
	if (-not [bool]$summary.success -or [bool]$summary.forcedTermination) { throw "Physics $RunKind run did not exit cleanly." }
	$steps = @($summary.explicitStepPublication | ForEach-Object { [double]$_.durationMs })
	$memory = @($summary.processMemorySamples)
	if ($memory.Count -eq 0) { throw 'Physics evidence omitted processMemorySamples.' }
	# The live MCP profiler summary supplies complete-frame percentiles. Persist a
	# transparent sidecar that contains only those measured fields, so the
	# normalized record has a stable, inspectable source without parsing .teprof.
	$profiler = Get-RequiredProperty -Object (Get-RequiredProperty -Object $summary -Name 'profilerCapture' -Context 'Physics summary') -Name 'summary' -Context 'Physics profiler capture'
	$profilerData = Get-RequiredProperty -Object $profiler -Name 'data' -Context 'Physics profiler capture'
	$profilerSummary = Get-RequiredProperty -Object $profilerData -Name 'summary' -Context 'Physics profiler capture'
	$timing = @{}
	foreach ($stream in @(Get-RequiredProperty -Object $profilerSummary -Name 'timingStreams' -Context 'Physics profiler summary')) { $timing[[string]$stream.id] = $stream }
	if (-not $timing.ContainsKey('engine_cpu') -or -not $timing.ContainsKey('viewport_cpu') -or -not $timing.ContainsKey('viewport_gpu')) { throw 'Physics profiler did not publish required engine/viewport streams.' }
	$metricsPath = Join-Path $WorkRoot 'evidence/profiler-metrics.json'
	$profilerMetrics = [ordered]@{
		frames = [ordered]@{
			medianMs = Get-RequiredFiniteNumber $profilerSummary @('frameMedianMs', 'medianFrameMs') 'Physics profiler summary'
			p95Ms = Get-RequiredFiniteNumber $profilerSummary @('frameP95Ms', 'p95FrameMs') 'Physics profiler summary'
			p99Ms = Get-RequiredFiniteNumber $profilerSummary @('frameP99Ms', 'p99FrameMs') 'Physics profiler summary'
			worstMs = Get-RequiredFiniteNumber $profilerSummary @('frameWorstMs', 'maximumFrameMs') 'Physics profiler summary'
		}
		timingStreams = [ordered]@{ engine_cpu = $timing['engine_cpu']; viewport_cpu = $timing['viewport_cpu']; viewport_gpu = $timing['viewport_gpu'] }
	}
	Write-Json $metricsPath $profilerMetrics
	# Re-read the transparent sidecar through the same JSON adapter used for all
	# other raw evidence. OrderedDictionary keys are not PSObject properties in
	# Windows PowerShell 5.1, while the parsed JSON fields are deliberately so.
	$profilerMetrics = Read-Json $metricsPath
	$normalizedProfiler = [ordered]@{
		frameMedianMs = Get-RequiredFiniteNumber $profilerMetrics.frames @('medianMs') 'Physics profiler metrics frames'
		frameP95Ms = Get-RequiredFiniteNumber $profilerMetrics.frames @('p95Ms') 'Physics profiler metrics frames'
		frameP99Ms = Get-RequiredFiniteNumber $profilerMetrics.frames @('p99Ms') 'Physics profiler metrics frames'
		frameWorstMs = Get-RequiredFiniteNumber $profilerMetrics.frames @('worstMs') 'Physics profiler metrics frames'
		engineMeanMs = Get-RequiredFiniteNumber $profilerMetrics.timingStreams.engine_cpu @('meanMs') 'Physics profiler metrics engine_cpu'
		viewportCpuP95Ms = Get-RequiredFiniteNumber $profilerMetrics.timingStreams.viewport_cpu @('p95Ms') 'Physics profiler metrics viewport_cpu'
		viewportGpuP95Ms = Get-RequiredFiniteNumber $profilerMetrics.timingStreams.viewport_gpu @('p95Ms') 'Physics profiler metrics viewport_gpu'
	}
	# A fresh root must contain exactly the capture named by the live MCP result.
	# Accepting the first matching file could silently bind a stale profiler run.
	$artifactFiles = @(Get-ChildItem -LiteralPath (Join-Path $WorkRoot 'project/Profiler') -Filter *.teprof -File)
	if ($artifactFiles.Count -ne 1) { throw "Physics profiler evidence must contain exactly one .teprof file; found $($artifactFiles.Count)." }
	$expectedArtifactName = [string](Get-RequiredProperty (Get-RequiredProperty $summary.profilerCapture 'artifact' 'Physics profiler capture') 'name' 'Physics profiler artifact')
	if ($artifactFiles[0].Name -cne $expectedArtifactName) { throw "Physics profiler artifact name '$($artifactFiles[0].Name)' does not match MCP artifact '$expectedArtifactName'." }
	$artifactPath = $artifactFiles[0].FullName
	$facts = [ordered]@{
		determinismHash = [string](Get-RequiredProperty $summary 'determinismHash' 'Physics summary')
		normalShutdown = -not [bool]$summary.forcedTermination
		profilerMetricGatePassed = [bool](Get-RequiredProperty $summary.us5Profiler.metricAvailability 'gatePassed' 'Physics profiler gates')
		profilerDurationGatePassed = [bool](Get-RequiredProperty $summary.us5Profiler 'durationGatePassed' 'Physics profiler gates')
		profilerSizeGatePassed = [bool](Get-RequiredProperty $summary.us5Profiler 'sizeGatePassed' 'Physics profiler gates')
	}
	$metrics = [ordered]@{
		durationMs = New-Metric 'milliseconds' ([double](Get-RequiredProperty $summary 'monotonicDurationMs' 'Physics summary'))
		frameMedianMs = New-Metric 'milliseconds' $normalizedProfiler.frameMedianMs; frameP95Ms = New-Metric 'milliseconds' $normalizedProfiler.frameP95Ms; frameP99Ms = New-Metric 'milliseconds' $normalizedProfiler.frameP99Ms; frameWorstMs = New-Metric 'milliseconds' $normalizedProfiler.frameWorstMs
		engineMeanMs = New-Metric 'milliseconds' $normalizedProfiler.engineMeanMs; viewportCpuP95Ms = New-Metric 'milliseconds' $normalizedProfiler.viewportCpuP95Ms; viewportGpuP95Ms = New-Metric 'milliseconds' $normalizedProfiler.viewportGpuP95Ms
		stepMedianMs = New-Metric 'milliseconds' (Get-Percentile $steps 0.50); stepP95Ms = New-Metric 'milliseconds' (Get-Percentile $steps 0.95); stepP99Ms = New-Metric 'milliseconds' (Get-Percentile $steps 0.99); stepWorstMs = New-Metric 'milliseconds' ((@($steps | Measure-Object -Maximum).Maximum))
		profilerBytes = New-Metric 'bytes' ([double](Get-RequiredProperty $summary.us5Profiler 'artifactBytes' 'Physics profiler gates'))
		maxWorkingSetBytes = New-Metric 'bytes' ([double](@($memory | Measure-Object workingSetBytes -Maximum).Maximum)); maxPrivateBytes = New-Metric 'bytes' ([double](@($memory | Measure-Object privateBytes -Maximum).Maximum))
	}
	$record = [ordered]@{ schemaVersion = 1; workloadId = $Control.id; runKind = $RunKind; runIndex = $RunIndex; succeeded = $true; settingsHash = $Control.settingsHash; sourceResultPaths = @((Get-RelativeRepoPath $summaryPath), (Get-RelativeRepoPath $metricsPath), (Get-RelativeRepoPath $artifactPath)); facts = $facts; metrics = $metrics }
	$path = Join-Path $Destination 'normalized.json'; Write-Json $path $record; return $path
}

function Convert-ExportRun {
	param([string]$WorkRoot, [string]$Destination, [string]$RunKind, [int]$RunIndex, $Control)
	$evidence = Join-Path $WorkRoot 'evidence'; $resultPath = Join-Path $evidence 'export-result.json'; $editorPath = Join-Path $evidence 'physics-editor-invariants.json'; $exportPath = Join-Path $evidence 'physics-export-invariants.json'
	$result = Read-Json $resultPath; $editor = Read-Json $editorPath; $exported = Read-Json $exportPath
	if (-not [bool]$result.success -or -not [bool]$editor.succeeded -or -not [bool]$exported.succeeded -or -not [bool]$exported.orderlyShutdown) { throw 'Export evidence did not report a successful orderly runtime.' }
	if (-not (Test-CanonicalEqual $editor $exported)) { throw 'Editor and exported physics invariant reports differ.' }
	$record = [ordered]@{ schemaVersion = 1; workloadId = $Control.id; runKind = $RunKind; runIndex = $RunIndex; succeeded = $true; settingsHash = $Control.settingsHash; sourceResultPaths = @((Get-RelativeRepoPath $resultPath), (Get-RelativeRepoPath $editorPath), (Get-RelativeRepoPath $exportPath)); facts = [ordered]@{ physicsParity = $true; orderlyRuntimeShutdown = $true }; metrics = [ordered]@{ elapsedSeconds = New-Metric 'seconds' ([double](Get-RequiredProperty $result 'elapsedSeconds' 'Export result')); packageBytes = New-Metric 'bytes' ([double](Get-RequiredProperty $result.package 'byteSize' 'Export result')); packageEntries = New-Metric 'count' ([double](Get-RequiredProperty $result.package 'entryCount' 'Export result')); errors = New-Metric 'count' ([double](Get-RequiredProperty $result.validation 'errorCount' 'Export result')); warnings = New-Metric 'count' ([double](Get-RequiredProperty $result.validation 'warningCount' 'Export result')) } }
	$path = Join-Path $Destination 'normalized.json'; Write-Json $path $record; return $path
}

function Get-WarmupExportProject {
	param([Parameter(Mandatory = $true)][string]$WarmupRoot)
	$resultPath = Join-Path $WarmupRoot 'evidence/export-result.json'
	$result = Read-Json $resultPath
	if (-not [bool](Get-RequiredProperty $result 'success' 'Warmup export result')) { throw 'Renderer smoke requires a successful export warmup.' }
	$projectRoot = [System.IO.Path]::GetFullPath([string](Get-RequiredProperty $result 'projectRoot' 'Warmup export result'))
	Assert-ChildPath -Parent $WarmupRoot -Child $projectRoot -Description 'Warmup export projectRoot'
	if (-not (Test-Path -LiteralPath $projectRoot -PathType Container)) { throw "Warmup export projectRoot is missing: '$projectRoot'." }
	return $projectRoot
}

function Convert-RendererRun {
	param([string]$EvidenceRoot, [string]$Destination, [string]$RunKind, [int]$RunIndex, $Control)
	$summaryPath = Join-Path $EvidenceRoot 'summary.json'; $driverPath = Join-Path $EvidenceRoot 'driver-events.txt'; $crashPath = Join-Path $EvidenceRoot 'crash-reports.txt'
	$summary = Read-Json $summaryPath
	foreach ($path in @($driverPath, $crashPath)) { if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Renderer evidence is missing '$path'." } }
	$driverCount = @((Get-Content -LiteralPath $driverPath | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })).Count
	$crashCount = @((Get-Content -LiteralPath $crashPath | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })).Count
	if (-not [bool](Get-RequiredProperty $summary 'processAlive' 'Renderer summary')) { throw 'Renderer did not remain alive through the paced smoke sequence.' }
	if ([string](Get-RequiredProperty $summary 'graphicsBackend' 'Renderer summary') -cne 'Vulkan') { throw 'Renderer evidence did not use the Vulkan backend.' }
	if ([string](Get-RequiredProperty $summary 'selectedGpu' 'Renderer summary') -cne 'NVIDIA GeForce RTX 4080 Laptop GPU') { throw 'Renderer evidence selected a GPU other than the locked RTX 4080 Laptop GPU.' }
	if (-not [bool](Get-RequiredProperty $summary 'discreteGpuSelectedExplicitly' 'Renderer summary')) { throw 'Renderer evidence did not prove explicit discrete-GPU selection.' }
	if ([int](Get-RequiredProperty $summary 'workerCount' 'Renderer summary') -ne 31) { throw 'Renderer evidence used a worker count other than 31.' }
	if ([int](Get-RequiredProperty $summary 'paceMilliseconds' 'Renderer summary') -ne 180) { throw 'Renderer evidence used a non-frozen pace.' }
	if ([string](Get-RequiredProperty $summary 'windowWidths' 'Renderer summary') -cne '960 + (operation mod 7) * 37' -or [string](Get-RequiredProperty $summary 'windowHeights' 'Renderer summary') -cne '600 + (operation mod 5) * 31') { throw 'Renderer evidence used non-frozen resize formula labels.' }
	$resizes = @(Get-RequiredProperty $summary 'resizeOperations' 'Renderer summary')
	if ($resizes.Count -ne 50) { throw "Renderer evidence must record exactly 50 resize operations; found $($resizes.Count)." }
	for ($operation = 0; $operation -lt 50; ++$operation) {
		$resize = $resizes[$operation]
		$expectedWidth = 960 + (($operation % 7) * 37); $expectedHeight = 600 + (($operation % 5) * 31)
		if ([int](Get-RequiredProperty $resize 'operation' "Renderer resize $operation") -ne $operation -or [int](Get-RequiredProperty $resize 'width' "Renderer resize $operation") -ne $expectedWidth -or [int](Get-RequiredProperty $resize 'height' "Renderer resize $operation") -ne $expectedHeight) { throw "Renderer resize record $operation does not match the frozen formula." }
	}
	if ([int](Get-RequiredProperty $summary 'driverEvents' 'Renderer summary') -ne $driverCount -or [int](Get-RequiredProperty $summary 'crashReports' 'Renderer summary') -ne $crashCount) { throw 'Renderer summary error counts disagree with the captured error files.' }
	$record = [ordered]@{ schemaVersion = 1; workloadId = $Control.id; runKind = $RunKind; runIndex = $RunIndex; succeeded = $true; settingsHash = $Control.settingsHash; sourceResultPaths = @((Get-RelativeRepoPath $summaryPath), (Get-RelativeRepoPath $driverPath), (Get-RelativeRepoPath $crashPath)); facts = [ordered]@{}; metrics = [ordered]@{ operations = New-Metric 'count' ([double](Get-RequiredProperty $summary 'operations' 'Renderer summary')); driverEvents = New-Metric 'count' ([double]$driverCount); crashReports = New-Metric 'count' ([double]$crashCount) } }
	if ($record.metrics.operations.value -ne 50 -or $record.metrics.driverEvents.value -ne 0 -or $record.metrics.crashReports.value -ne 0) { throw 'Renderer workload produced a non-comparable operation, driver, or crash count.' }
	$path = Join-Path $Destination 'normalized.json'; Write-Json $path $record; return $path
}

try {
	$workloadsPath = Join-Path $script:RepoRoot 'Tools/Animation/PerformanceWorkloads.json'
	$baselineRoot = Join-Path $script:RepoRoot 'out/validation/animation/baseline'
	$baselinePath = Join-Path $baselineRoot 'locked-baseline.json'
	$evidenceLockPath = Join-Path $baselineRoot 'evidence-lock.json'
	$comparatorPath = Join-Path $script:RepoRoot 'Tools/Animation/CompareAnimationPerformance.ps1'
	$config = Read-Json $workloadsPath; $baseline = Read-Json $baselinePath
	$install = Resolve-RepoPath $InstallRoot; $build = Resolve-RepoPath $BuildDirectory; $output = Resolve-RepoPath $OutputRoot
	Assert-ChildPath $script:OutRoot $install 'Install root'; Assert-ChildPath $script:OutRoot $build 'Build directory'; Assert-ChildPath $script:OutRoot $output 'Collector output root'
	# Exported managed projects add deep descendants. Reserve a conservative
	# MAX_PATH budget before any directory is created or workload is launched.
	if ($output.Length -gt 120) { throw "OutputRoot length $($output.Length) exceeds the 120-character nested-path budget: '$output'." }
	if ($output.StartsWith(([System.IO.Path]::GetFullPath((Join-Path $script:RepoRoot 'out/validation/animation/baseline'))), [System.StringComparison]::OrdinalIgnoreCase)) { throw 'Collector output may not use the immutable baseline directory.' }
	$editor = Join-Path $install 'Editor/TerraEngine-Editor.exe'; $engine = Join-Path $install 'Engine/TerraEngine.dll'
	foreach ($required in @($editor, $engine, $comparatorPath, $evidenceLockPath)) { if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Required installed/tool file is missing: '$required'." } }
	if ((Get-Sha256 $baselinePath) -ne [string]$config.authority.lockedBaseline.sha256) { throw 'The immutable locked baseline identity does not match PerformanceWorkloads.json.' }
	if ((Get-Sha256 $evidenceLockPath) -ne [string]$config.authority.lockedBaseline.evidenceLockSha256) { throw 'The immutable evidence-lock identity does not match PerformanceWorkloads.json.' }
	$controls = @($config.setupBaselineControls | Where-Object { $_.required })
	if ($controls.Count -ne 4) { throw 'Frozen workload file must declare exactly four required controls.' }
	$requiredIds = @('focused-control-matrix', 'installed-physics-profiler', 'installed-export-runtime', 'installed-renderer-smoke')
	foreach ($id in $requiredIds) { if (@($controls | Where-Object { $_.id -eq $id }).Count -ne 1) { throw "Frozen workload '$id' is missing or duplicated." } }
	# Static validation deliberately stops before host commands or workload
	# launches. Real collection performs those checks immediately before sampling.
	if ($DryRun) {
		if (Test-Path -LiteralPath $output) { throw "DryRun refuses an existing output root; choose a fresh OutputRoot." }
		[ordered]@{ schemaVersion = 1; tool = 'CaptureAnimationPerformance'; mode = 'dry-run'; passed = $true; hostChecksDeferred = $true; sourceSnapshotDeferred = $true; outputRoot = $output; controls = $requiredIds; installRoot = $install; buildDirectory = $build } | ConvertTo-Json -Depth 20 -Compress | Write-Output
		return
	}
	$evidenceLock = Read-Json $evidenceLockPath
	$baselineEvidence = Get-BaselineEvidenceState -BaselineRoot $baselineRoot
	$expectedAggregate = [string]$config.authority.lockedBaseline.evidenceAggregateSha256
	if ([string]$evidenceLock.aggregateSha256 -cne $expectedAggregate -or [string]$baselineEvidence.aggregateSha256 -cne $expectedAggregate -or [int]$baselineEvidence.fileCount -ne [int]$evidenceLock.fileCount -or [UInt64]$baselineEvidence.totalBytes -ne [UInt64]$evidenceLock.totalBytes) { throw 'The protected T001 evidence selection changed; candidate collection is incomparable.' }
	$hostEvidence = Get-CurrentHostEvidence -ExpectedKnownAmbientProcesses @($config.candidateTemplate.comparisonSettings.knownAmbientProcessesPresent)
	# Compare required facts individually because the raw host snapshot contains
	# additional diagnostic fields while candidate identity has the locked shape.
	$expectedIdentity = $config.candidateTemplate.comparisonIdentity
	if ($hostEvidence.identity.host.manufacturer -ne $expectedIdentity.host.manufacturer -or $hostEvidence.identity.host.model -ne $expectedIdentity.host.model -or $hostEvidence.identity.processor.name -ne $expectedIdentity.processor.name -or $hostEvidence.identity.processor.logicalProcessors -ne $expectedIdentity.processor.logicalProcessors -or $hostEvidence.identity.discreteGraphics.name -ne $expectedIdentity.discreteGraphics.name -or $hostEvidence.identity.discreteGraphics.windowsDriverVersion -ne $expectedIdentity.discreteGraphics.windowsDriverVersion -or $hostEvidence.identity.vulkanLoaderVersion -ne $expectedIdentity.vulkanLoaderVersion -or $hostEvidence.identity.discreteGraphics.vulkanApiVersion -ne $expectedIdentity.discreteGraphics.vulkanApiVersion) { throw 'Current host, driver, or Vulkan identity differs from the locked performance host.' }
	if (-not (Test-CanonicalEqual $hostEvidence.observations $config.candidateTemplate.requiredObservations)) { throw 'Current power, routing, warm-up, or ambient observations differ from the frozen contract.' }
	if (Test-Path -LiteralPath $output) { throw "OutputRoot must be fresh; evidence is never overwritten: '$output'." }
	[System.IO.Directory]::CreateDirectory($output) | Out-Null
	$source = Get-CurrentSourceState; $rawHostPath = Join-Path $output 'raw-host-and-settings.json'; $sourcePath = Join-Path $output 'source-state.json'
	Write-Json $rawHostPath ([ordered]@{ schemaVersion = 1; capturedAt = (Get-Date).ToString('o'); comparisonIdentity = $hostEvidence.identity; comparisonSettings = $config.candidateTemplate.comparisonSettings; observations = $hostEvidence.observations; powerLine = $hostEvidence.powerLine; ambient = $hostEvidence.ambient })
	Write-Json $sourcePath $source
	$normalized = @{}
	$ctest = (Get-Command ctest.exe -ErrorAction Stop).Source; $powershell = (Get-Process -Id $PID).Path
	$focused = $controls | Where-Object id -eq 'focused-control-matrix'; $physics = $controls | Where-Object id -eq 'installed-physics-profiler'; $export = $controls | Where-Object id -eq 'installed-export-runtime'; $renderer = $controls | Where-Object id -eq 'installed-renderer-smoke'
	$runSpecs = @(for ($index = 0; $index -le 5; ++$index) {
		[ordered]@{ index = $index; kind = if ($index -eq 0) { 'warmup' } else { 'measured' }; run = if ($index -eq 0) { 0 } else { $index }; label = if ($index -eq 0) { 'w' } else { "m$index" } }
	})
	# T001 measured each control as a contiguous warmup-plus-five batch. Preserve
	# that order so thermal state and cache reuse are paired with the locked run.
	foreach ($spec in $runSpecs) {
		$focusedRoot = Join-Path $output ("f/{0}" -f $spec.label); $focusedLog = Join-Path $focusedRoot 'ctest.log'
		$regex = '^(' + (($focused.settings.tests | ForEach-Object { [regex]::Escape([string]$_) }) -join '|') + ')$'
		$watch = [System.Diagnostics.Stopwatch]::StartNew(); [void](Invoke-CheckedProcess $ctest @('--test-dir', $build, '-C', 'RelWithDebInfo', '-R', $regex, '--output-on-failure') $script:RepoRoot $focusedLog @{ TERRAENGINE_THREADPOOL_WORKERS = '31' }); $watch.Stop()
		$focusedRecord = [ordered]@{ schemaVersion = 1; workloadId = $focused.id; runKind = $spec.kind; runIndex = $spec.run; succeeded = $true; settingsHash = $focused.settingsHash; sourceResultPaths = @((Get-RelativeRepoPath $focusedLog)); facts = [ordered]@{}; metrics = [ordered]@{ totalSeconds = New-Metric 'seconds' $watch.Elapsed.TotalSeconds } }
		$focusedNormalized = Join-Path $focusedRoot 'normalized.json'; Write-Json $focusedNormalized $focusedRecord; if ($null -eq $normalized[$focused.id]) { $normalized[$focused.id] = @() }; $normalized[$focused.id] += $focusedNormalized
	}

	# Child harnesses own their work directories and may delete or replace them at
	# startup. Keep the live process logs outside those roots so the collector can
	# hold each log open while the child safely prepares its own evidence folder.
	$workloadLogRoot = Join-Path $output 'logs'
	foreach ($spec in $runSpecs) {
		$physicsRoot = Join-Path $output ("p/{0}" -f $spec.label)
		$physicsLog = Join-Path $workloadLogRoot ("physics-{0}.log" -f $spec.label)
		[void](Invoke-CheckedProcess $powershell @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $script:RepoRoot 'Tools/Physics/RunInstalledEditorPhysicsStress.ps1'),'-InstallRoot',$install,'-WorkRoot',$physicsRoot,'-ObjectCount','10000','-McpPort','43127','-ExplicitStepCount','100','-ActiveDynamicCount','1000','-SceneViewScenario','canonical','-StartupTimeoutSeconds','120','-WorkerCount','max','-Us3Scenario','large','-ParallelPreparationThreshold','512','-ParallelExtractionThreshold','512','-CaptureProcessMemory','-RunUs5ProfilerChecks','-CaptureLevel','detailed','-CaptureVersion','v2','-CaptureDurationSeconds','5','-AllowLiveStress') $script:RepoRoot $physicsLog @{})
		$physicsDestination = Join-Path $physicsRoot 'normalized'; $physicsNormalized = Convert-PhysicsRun $physicsRoot $physicsDestination $spec.kind $spec.run $physics; if ($null -eq $normalized[$physics.id]) { $normalized[$physics.id] = @() }; $normalized[$physics.id] += $physicsNormalized
	}

	$rendererProject = $null
	foreach ($spec in $runSpecs) {
		$exportRoot = Join-Path $output ("e/{0}" -f $spec.label)
		$exportLog = Join-Path $workloadLogRoot ("export-{0}.log" -f $spec.label)
		[void](Invoke-CheckedProcess $powershell @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $script:RepoRoot 'Tools/ExportRuntimeBaseline/RunBaseline.ps1'),'-BuildDir',$build,'-InstallRoot',$install,'-WorkRoot',$exportRoot,'-FixtureRoot',(Join-Path $script:RepoRoot 'Tests/Fixtures/ExportRuntimeBaseline'),'-PhysicsFixtureRoot',(Join-Path $script:RepoRoot 'Tests/Fixtures/PhysicsExportRuntime'),'-RunId',("a043-{0}" -f $spec.label)) $script:RepoRoot $exportLog @{})
		$exportDestination = Join-Path $exportRoot 'normalized'; $exportNormalized = Convert-ExportRun $exportRoot $exportDestination $spec.kind $spec.run $export; if ($null -eq $normalized[$export.id]) { $normalized[$export.id] = @() }; $normalized[$export.id] += $exportNormalized
		if ($spec.index -eq 0) { $rendererProject = Get-WarmupExportProject -WarmupRoot $exportRoot }
	}
	if ([string]::IsNullOrWhiteSpace($rendererProject)) { throw 'Renderer smoke cannot run before the successful export warmup project is available.' }

	foreach ($spec in $runSpecs) {
		$rendererRoot = Join-Path $output ("r/{0}" -f $spec.label)
		$rendererLog = Join-Path $workloadLogRoot ("renderer-{0}.log" -f $spec.label)
		[void](Invoke-CheckedProcess $powershell @('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $script:RepoRoot 'Tools/RendererStability/RunLiveSmoke.ps1'),'-Executable',$editor,'-Project',$rendererProject,'-EvidenceDirectory',$rendererRoot,'-Operations','50','-PaceMilliseconds','180','-AllowLiveGpuStress') $script:RepoRoot $rendererLog @{ TERRAENGINE_THREADPOOL_WORKERS = '31' })
		$rendererDestination = Join-Path $rendererRoot 'normalized'; $rendererNormalized = Convert-RendererRun $rendererRoot $rendererDestination $spec.kind $spec.run $renderer; if ($null -eq $normalized[$renderer.id]) { $normalized[$renderer.id] = @() }; $normalized[$renderer.id] += $rendererNormalized
	}
	$manifestWorkloads = @(); foreach ($control in $controls) { $paths = @($normalized[$control.id]); if ($paths.Count -ne 6) { throw "Collector did not produce exactly one warmup plus five measured '$($control.id)' records." }; $manifestWorkloads += [ordered]@{ id = $control.id; settingsHash = $control.settingsHash; settings = $control.settings; warmupResultPath = Get-RelativeRepoPath $paths[0]; measuredResultPaths = @($paths[1..5] | ForEach-Object { Get-RelativeRepoPath $_ }) } }
	$reference = [ordered]@{}; foreach ($property in $config.candidateTemplate.reference.PSObject.Properties) { $reference[$property.Name] = $property.Value }; $reference.performanceWorkloadsSha256 = Get-Sha256 $workloadsPath
	$candidatePath = Join-Path $output 'candidate-manifest.json'
	Write-Json $candidatePath ([ordered]@{ schemaVersion = 1; feature = '043-complete-animation-system'; reference = $reference; comparisonIdentity = $config.candidateTemplate.comparisonIdentity; comparisonSettings = $config.candidateTemplate.comparisonSettings; observations = $config.candidateTemplate.requiredObservations; candidateBuild = [ordered]@{ sourceCommit = $source.sourceCommit; sourceTreeDirty = $source.sourceTreeDirty; sourceDiffSha256 = $source.sourceDiffSha256; buildConfiguration = 'RelWithDebInfo'; installedTreeRoot = Get-RelativeRepoPath $install; artifacts = @([ordered]@{ path = Get-RelativeRepoPath $editor; sha256 = Get-Sha256 $editor }, [ordered]@{ path = Get-RelativeRepoPath $engine; sha256 = Get-Sha256 $engine }) }; rawInputPaths = @((Get-RelativeRepoPath $rawHostPath), (Get-RelativeRepoPath $sourcePath)); workloads = $manifestWorkloads })
	$comparisonPath = Join-Path $output 'comparison.json'; & $powershell -NoProfile -ExecutionPolicy Bypass -File $comparatorPath -CandidateManifest (Get-RelativeRepoPath $candidatePath) -OutputPath (Get-RelativeRepoPath $comparisonPath); exit $LASTEXITCODE
}
catch {
	Write-Error $_.Exception.Message
	exit 4
}
