# Runs an intentionally disposable, offline provider comparison.  It never
# downloads or copies provider source: the caller supplies each exact path.
[CmdletBinding()]
param(
	[ValidateSet("All", "Jolt55", "Jolt56", "PhysX58")]
	[string]$Candidate = "All",

	[string]$Jolt55Source = "",
	[string]$Jolt56Source = "",
	[string]$PhysX58Source = "",

	# The selected provider pin is explicit; retaining this argument makes old
	# reproduction commands compatible while the harness enforces the exact pin.
	[string]$Jolt56Revision = "e77f175595e64cb44218cc9d9d56fc365ad0e36a",

	[ValidateSet("Debug", "Release", "RelWithDebInfo", "ReleaseASAN")]
	[string]$Configuration = "Release",

	# Empty selects Ninja when it is available, otherwise lets CMake choose the
	# platform default. This avoids treating a global generator as a dependency.
	[string]$CMakeGenerator = "",

	[ValidateNotNullOrEmpty()]
	[string]$WorkRoot = "out/physics-backend-spike",

	[ValidatePattern("^[A-Za-z0-9][A-Za-z0-9._-]*$")]
	[string]$RunId = "backend-spike",

	# Hooks receive candidate/build/install paths through environment variables.
	[string]$FixtureCommand = "",
	[string[]]$FixtureArguments = @(),
	[string]$MeasurementCommand = "",
	[string[]]$MeasurementArguments = @(),
	[ValidateRange(1, 10)]
	[int]$FixtureRepeats = 3,
	[ValidateRange(10, 900)]
	[int]$FixtureTimeoutSeconds = 180,

	# Run a separate candidate invocation with this switch to capture Jolt's
	# compile-time double-position policy using an isolated build/install tree.
	[switch]$DoublePrecision,
	[switch]$DryRun,
	[switch]$ValidateHarnessOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
if (Test-Path variable:PSNativeCommandUseErrorActionPreference) {
	$PSNativeCommandUseErrorActionPreference = $false
}

$script:RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot "../../..")).Path
$script:HarnessSource = $PSScriptRoot

function Get-AbsolutePath {
	param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string]$BasePath)
	if ([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }
	return [System.IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

function Assert-ChildPath {
	param([Parameter(Mandatory = $true)][string]$Parent, [Parameter(Mandatory = $true)][string]$Child)
	$parentWithSeparator = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
	$childFullPath = [System.IO.Path]::GetFullPath($Child)
	if (-not $childFullPath.StartsWith($parentWithSeparator, [System.StringComparison]::OrdinalIgnoreCase)) {
		throw "Disposable output '$childFullPath' must stay under '$parentWithSeparator'."
	}
}

function Write-Utf8File {
	param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string]$Content)
	[System.IO.File]::WriteAllText($Path, $Content + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
}

function Get-FileDigestRecords {
	param([Parameter(Mandatory = $true)][string]$Directory)
	if (-not (Test-Path -LiteralPath $Directory -PathType Container)) { return @() }
	return @(Get-ChildItem -LiteralPath $Directory -File -Recurse | Sort-Object FullName | ForEach-Object {
		[ordered]@{
			path = $_.FullName.Substring($Directory.Length).TrimStart('\', '/') -replace '\\', '/'
			bytes = $_.Length
			sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
		}
	})
}

function Get-DirectoryDigest {
	param([Parameter(Mandatory = $true)][string]$Directory)
	$records = @(Get-FileDigestRecords -Directory $Directory)
	$canonical = (($records | ConvertTo-Json -Compress -Depth 4) + "`n")
	$bytes = [System.Text.Encoding]::UTF8.GetBytes($canonical)
	# Windows PowerShell 5.1 predates SHA256.HashData and Convert.ToHexString.
	# Use the instance API so the checked-in harness works on every supported
	# Windows development shell without requiring a newer PowerShell runtime.
	$sha256 = [System.Security.Cryptography.SHA256]::Create()
	try {
		$hash = $sha256.ComputeHash($bytes)
		return ([BitConverter]::ToString($hash) -replace '-', '').ToLowerInvariant()
	}
	finally { $sha256.Dispose() }
}

function Invoke-LoggedCommand {
	param(
		[Parameter(Mandatory = $true)][string]$Stage,
		[Parameter(Mandatory = $true)][string]$FilePath,
		[Parameter(Mandatory = $true)][string[]]$Arguments,
		[Parameter(Mandatory = $true)][string]$LogPath,
		[switch]$SkipExecution
	)
	$commandRecord = [ordered]@{ stage = $Stage; executable = $FilePath; arguments = $Arguments }
	Write-Utf8File -Path "$LogPath.command.json" -Content ($commandRecord | ConvertTo-Json -Depth 5)
	if ($SkipExecution) {
		Write-Utf8File -Path $LogPath -Content "DRY RUN: $FilePath $($Arguments -join ' ')"
		return [ordered]@{ elapsedMilliseconds = 0; exitCode = 0; dryRun = $true }
	}
	$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
	# Windows PowerShell 5.1 promotes any native stderr line to a non-terminating
	# ErrorRecord. With the script's strict Stop policy, harmless CMake warnings
	# would abort before LASTEXITCODE could be checked. Temporarily continue only
	# around the native process; the explicit exit-code check remains authoritative.
	$previousErrorActionPreference = $ErrorActionPreference
	try {
		$ErrorActionPreference = "Continue"
		& $FilePath @Arguments *> $LogPath
		$exitCode = $LASTEXITCODE
	}
	finally { $ErrorActionPreference = $previousErrorActionPreference }
	$stopwatch.Stop()
	if ($exitCode -ne 0) { throw "$Stage failed with exit code $exitCode. See '$LogPath'." }
	return [ordered]@{ elapsedMilliseconds = $stopwatch.ElapsedMilliseconds; exitCode = $exitCode; dryRun = $false }
}

function Invoke-FixtureProcess {
	param(
		[Parameter(Mandatory = $true)][string]$FilePath,
		[Parameter(Mandatory = $true)][string[]]$Arguments,
		[Parameter(Mandatory = $true)][string]$LogPath,
		[Parameter(Mandatory = $true)][int]$TimeoutSeconds
	)
	# ProviderFixture receives only simple --key=value arguments. Using the
	# process API gives this long-running stress probe a hard watchdog and captures
	# peak resident memory without changing general CMake command execution.
	$commandRecord = [ordered]@{ stage = "provider-fixture"; executable = $FilePath; arguments = $Arguments; timeoutSeconds = $TimeoutSeconds }
	Write-Utf8File -Path "$LogPath.command.json" -Content ($commandRecord | ConvertTo-Json -Depth 5)
	$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
	$startInfo.FileName = $FilePath
	$startInfo.Arguments = $Arguments -join ' '
	$startInfo.UseShellExecute = $false
	$startInfo.CreateNoWindow = $true
	$startInfo.RedirectStandardOutput = $true
	$startInfo.RedirectStandardError = $true
	$process = [System.Diagnostics.Process]::new()
	$process.StartInfo = $startInfo
	$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
	try {
		if (-not $process.Start()) { throw "Failed to start ProviderFixture '$FilePath'." }
		$stdoutTask = $process.StandardOutput.ReadToEndAsync()
		$stderrTask = $process.StandardError.ReadToEndAsync()
		# On the Windows PowerShell 5.1 runtime used by the supported editor
		# toolchain, Process.WorkingSet64 and PeakWorkingSet64 become null after a
		# child exits. Sample the live process while also applying the watchdog so
		# every short fixture launch retains a real resident-memory high-water mark.
		[long]$peakWorkingSetBytes = 0
		$timedOut = $false
		while (-not $process.WaitForExit(10)) {
			try {
				$process.Refresh()
				$currentWorkingSetBytes = [long]$process.WorkingSet64
				if ($currentWorkingSetBytes -gt $peakWorkingSetBytes) { $peakWorkingSetBytes = $currentWorkingSetBytes }
			}
			catch {
				# The child can exit between WaitForExit and Refresh. Its previously
				# sampled high-water mark remains valid in that race.
			}
			if ($stopwatch.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
				$timedOut = $true
				break
			}
		}
		if ($timedOut) {
			$process.Kill()
			$process.WaitForExit()
		}
		$stdout = $stdoutTask.GetAwaiter().GetResult()
		$stderr = $stderrTask.GetAwaiter().GetResult()
		Write-Utf8File -Path $LogPath -Content $stdout.TrimEnd()
		if (-not [string]::IsNullOrWhiteSpace($stderr)) { Write-Utf8File -Path "$LogPath.stderr" -Content $stderr.TrimEnd() }
		if ($timedOut) { throw "ProviderFixture exceeded the $TimeoutSeconds-second watchdog. Partial output is in '$LogPath'." }
		if ($process.ExitCode -ne 0) { throw "ProviderFixture failed with exit code $($process.ExitCode). See '$LogPath'." }
		return [ordered]@{ elapsedMilliseconds = $stopwatch.ElapsedMilliseconds; exitCode = $process.ExitCode; timedOut = $false; peakWorkingSetBytes = $peakWorkingSetBytes }
	}
	finally {
		$stopwatch.Stop()
		$process.Dispose()
	}
}

function Get-CMakeGeneratorArguments {
	if (-not [string]::IsNullOrWhiteSpace($CMakeGenerator)) { return @("-G", $CMakeGenerator) }
	if (Get-Command ninja -ErrorAction SilentlyContinue) { return @("-G", "Ninja") }
	return @()
}

function Get-SourceRevision {
	param([Parameter(Mandatory = $true)][string]$Source)
	if (-not (Test-Path -LiteralPath $Source -PathType Container)) { throw "Provider source directory does not exist: '$Source'." }
	$head = (& git -C $Source rev-parse HEAD 2>$null).Trim()
	if ($LASTEXITCODE -ne 0 -or $head -notmatch '^[0-9a-fA-F]{40}$') { throw "Provider source '$Source' must be a Git checkout with a full 40-character HEAD revision." }
	return $head.ToLowerInvariant()
}

function Get-DependencyClosureAudit {
	param(
		[Parameter(Mandatory = $true)][string]$FixtureExecutable,
		[Parameter(Mandatory = $true)][string]$InstallRoot,
		[Parameter(Mandatory = $true)][string]$ProviderBuild,
		[Parameter(Mandatory = $true)][string]$EvidenceRoot
	)
	# A static provider does not appear in the PE dependency table. Pair the PE
	# record with an installed-config/cache scan so a hidden global SDK path or a
	# provider DLL cannot be mistaken for a self-contained static closure.
	$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
	if ($null -eq $dumpbin) { $dumpbin = Get-Command dumpbin -ErrorAction SilentlyContinue }
	$peToolPath = $(if ($null -eq $dumpbin) { $null } else { $dumpbin.Source })
	$peToolArguments = @("/DEPENDENTS", $FixtureExecutable)
	if ($null -eq $peToolPath) {
		# Normal Codex/Desktop shells are not Visual Studio developer prompts.
		# CMake already records the exact linker it selected, and MSVC link.exe
		# exposes the same dependency table through /DUMP /DEPENDENTS.
		$linkerRecord = Select-String -LiteralPath (Join-Path $ProviderBuild "CMakeCache.txt") -Pattern '^CMAKE_LINKER:FILEPATH=(.+)$' -ErrorAction SilentlyContinue | Select-Object -First 1
		if ($null -ne $linkerRecord) {
			$candidateLinker = $linkerRecord.Matches[0].Groups[1].Value
			if (Test-Path -LiteralPath $candidateLinker -PathType Leaf) {
				$peToolPath = $candidateLinker
				$peToolArguments = @("/DUMP", "/DEPENDENTS", $FixtureExecutable)
			}
		}
	}
	$peOutput = ""
	$peLog = Join-Path $EvidenceRoot "fixture-pe-dependencies.raw.txt"
	if ($null -ne $peToolPath) {
		$peOutput = (& $peToolPath @peToolArguments 2>&1 | Out-String)
		Write-Utf8File -Path $peLog -Content $peOutput.TrimEnd()
	}
	# Scan only the active installed package metadata and configured cache. Jolt
	# also installs its source-side Jolt.cmake helper under include/; that file
	# contains inactive `if (JPH_USE_VK)` branches and is not a resolved dependency.
	$installedPackageDirectory = Join-Path $InstallRoot "lib/cmake/Jolt"
	$configInputs = @(
		Get-ChildItem -LiteralPath $installedPackageDirectory -File -Recurse -ErrorAction SilentlyContinue
		Get-Item -LiteralPath (Join-Path $ProviderBuild "CMakeCache.txt") -ErrorAction SilentlyContinue
	) | Where-Object { $null -ne $_ }
	$configMatches = @($configInputs | Select-String -Pattern 'VulkanSDK|VULKAN.*(PATH|DIR|ROOT)|D3D12.*(PATH|DIR)|DXGI.*(PATH|DIR)|METAL.*(PATH|DIR)|JPH_USE_(DX12|VK|MTL|CPU_COMPUTE):BOOL=ON|Jolt[^\r\n]*\.dll|PhysX[^\r\n]*\.dll' -CaseSensitive:$false)
	# Match dependency DLL names only. The dumper header includes the executable's
	# path (whose run directory contains "Jolt56") and is not a linked module.
	$peMatches = @($peOutput -split "`r?`n" | Where-Object { $_ -match '(?i)\b(?:jolt|physx|vulkan|d3d12|dxgi|metal|cpu[_ -]?compute)[^\s]*\.dll\b' })
	$audit = [ordered]@{
		schemaVersion = 1; platform = "windows"; fixture = $FixtureExecutable
		peToolAvailable = $null -ne $peToolPath; peTool = $peToolPath; peLog = $(if ($null -eq $peToolPath) { $null } else { $peLog })
		peUnexpectedMatches = $peMatches; configCacheUnexpectedMatches = @($configMatches | ForEach-Object { $_.Path + ':' + $_.LineNumber + ':' + $_.Line })
		staticLinkEvidence = "No provider DLL appears in PE dependencies; installed JoltConfig and provider CMake cache are scanned alongside the fixture."
	}
	if ($peMatches.Count -gt 0 -or $configMatches.Count -gt 0) {
		throw "Dependency-closure audit rejected global/provider SDK evidence. Inspect '$peLog' and the raw result."
	}
	return $audit
}

function Invoke-BackendSpikeCandidate {
	param([Parameter(Mandatory = $true)][hashtable]$Definition, [Parameter(Mandatory = $true)][string]$RunRoot)
	$source = Get-AbsolutePath -Path $Definition.source -BasePath $script:RepositoryRoot
	$observedRevision = Get-SourceRevision -Source $source
	if ($observedRevision -ne $Definition.revision) { throw "$($Definition.name) source HEAD is $observedRevision, expected $($Definition.revision)." }
	$sourceTree = (& git -C $source rev-parse 'HEAD^{tree}').Trim().ToLowerInvariant()
	$sourceTag = (& git -C $source describe --tags --exact-match HEAD 2>$null).Trim()
	$sourceUrl = (& git -C $source remote get-url origin 2>$null).Trim()
	$sourceStatus = @(& git -C $source status --porcelain --untracked-files=no)
	$sourceAcquisitionTimestampUtc = (Get-Item -LiteralPath $source).CreationTimeUtc.ToString("o")
	$runStartedUtc = [DateTime]::UtcNow.ToString("o")
	$sourceStats = Get-ChildItem -LiteralPath $source -File -Recurse | Measure-Object -Property Length -Sum
	$providerSource = Join-Path $source $Definition.cmakeSubdirectory
	if (-not (Test-Path -LiteralPath (Join-Path $providerSource "CMakeLists.txt") -PathType Leaf)) {
		throw "$($Definition.name) CMake entry point is missing: '$providerSource'."
	}
	$candidateRoot = Join-Path $RunRoot $Definition.id
	Assert-ChildPath -Parent $RunRoot -Child $candidateRoot
	if (Test-Path -LiteralPath $candidateRoot) { throw "Run output already exists: '$candidateRoot'. Choose a new -RunId; existing evidence is never overwritten." }
	$harnessBuild = Join-Path $candidateRoot "harness-build"
	$providerBuild = Join-Path $candidateRoot "provider-build"
	$fixtureBuild = Join-Path $candidateRoot "fixture-build"
	$installRoot = Join-Path $candidateRoot "install"
	$evidenceRoot = Join-Path $candidateRoot "evidence"
	$logsRoot = Join-Path $candidateRoot "logs"
	New-Item -ItemType Directory -Force -Path $harnessBuild, $providerBuild, $fixtureBuild, $installRoot, $evidenceRoot, $logsRoot | Out-Null

	# These knobs prevent FetchContent/package discovery from silently obtaining
	# dependencies. Loopback-only proxies fail any accidental network attempt.
	$oldEnvironment = @{}
	foreach ($name in @("HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "http_proxy", "https_proxy", "all_proxy", "NO_PROXY", "no_proxy")) {
		$oldEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, "Process")
		[Environment]::SetEnvironmentVariable($name, $(if ($name -match 'NO_PROXY|no_proxy') { "" } else { "http://127.0.0.1:9" }), "Process")
	}
	try {
		$commonOfflineArguments = @(
			"-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
			"-DFETCHCONTENT_UPDATES_DISCONNECTED=ON",
			"-DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE",
			"-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE",
			"-DCMAKE_FIND_PACKAGE_PREFER_CONFIG=TRUE"
		)
		$generatorArguments = @(Get-CMakeGeneratorArguments)
		$harnessConfigure = @("-S", $script:HarnessSource, "-B", $harnessBuild) + $generatorArguments + @("-DCMAKE_INSTALL_PREFIX=$installRoot", "-DBACKEND_SPIKE_PROVIDER=$($Definition.id)", "-DBACKEND_SPIKE_SOURCE_DIR=$source", "-DBACKEND_SPIKE_REVISION=$observedRevision", "-DBACKEND_SPIKE_EVIDENCE_DIR=$evidenceRoot") + $commonOfflineArguments
		$harness = [ordered]@{}
		$harness.configure = Invoke-LoggedCommand -Stage "harness-configure" -FilePath "cmake" -Arguments $harnessConfigure -LogPath (Join-Path $logsRoot "harness-configure.log") -SkipExecution:$DryRun
		$harness.build = Invoke-LoggedCommand -Stage "harness-build" -FilePath "cmake" -Arguments @("--build", $harnessBuild, "--config", $Configuration) -LogPath (Join-Path $logsRoot "harness-build.log") -SkipExecution:$DryRun
		$harness.install = Invoke-LoggedCommand -Stage "harness-install" -FilePath "cmake" -Arguments @("--install", $harnessBuild, "--config", $Configuration) -LogPath (Join-Path $logsRoot "harness-install.log") -SkipExecution:$DryRun

		$providerOptions = @($Definition.options)
		if ($Definition.id -eq "PhysX58") {
			# PhysX's public CMake entry point normally receives these values
			# from its networked Packman generator. Supplying them explicitly is
			# required for the direct, offline audit path used by this spike.
			# PhysX copies this cache value into a generated CMake script without
			# escaping backslashes, so normalize it before CMake sees the value.
			$physxRoot = (Join-Path $source 'physx') -replace '\\', '/'
			$physxOutput = $providerBuild -replace '\\', '/'
			$providerOptions += "-DPHYSX_ROOT_DIR=$physxRoot"
			$providerOptions += "-DTARGET_BUILD_PLATFORM=windows"
			$providerOptions += "-DPX_OUTPUT_LIB_DIR=$physxOutput"
			$providerOptions += "-DPX_OUTPUT_BIN_DIR=$physxOutput"
		}
		elseif ($DoublePrecision) {
			$providerOptions += "-DDOUBLE_PRECISION=ON"
		}
		$providerConfigure = @("-S", $providerSource, "-B", $providerBuild) + $generatorArguments + @("-DCMAKE_BUILD_TYPE=$Configuration", "-DCMAKE_INSTALL_PREFIX=$installRoot") + $commonOfflineArguments + $providerOptions
		$provider = [ordered]@{}
		$provider.configure = Invoke-LoggedCommand -Stage "provider-configure" -FilePath "cmake" -Arguments $providerConfigure -LogPath (Join-Path $logsRoot "provider-configure.log") -SkipExecution:$DryRun
		$provider.build = Invoke-LoggedCommand -Stage "provider-build" -FilePath "cmake" -Arguments @("--build", $providerBuild, "--config", $Configuration) -LogPath (Join-Path $logsRoot "provider-build.log") -SkipExecution:$DryRun
		$provider.install = Invoke-LoggedCommand -Stage "provider-install" -FilePath "cmake" -Arguments @("--install", $providerBuild, "--config", $Configuration) -LogPath (Join-Path $logsRoot "provider-install.log") -SkipExecution:$DryRun

		$builtInFixture = [ordered]@{ status = "not-supported" }
		if ($Definition.id -eq "Jolt55" -or $Definition.id -eq "Jolt56") {
			# Compile the checked-in probe against the just-installed package and
			# prohibit package fallback, proving the measured binary uses this run.
			$fixtureConfigure = @("-S", $script:HarnessSource, "-B", $fixtureBuild) + $generatorArguments + @(
				"-DCMAKE_BUILD_TYPE=$Configuration",
				"-DCMAKE_INSTALL_PREFIX=$installRoot",
				"-DBACKEND_SPIKE_PROVIDER=$($Definition.id)",
				"-DBACKEND_SPIKE_SOURCE_DIR=$source",
				"-DBACKEND_SPIKE_REVISION=$observedRevision",
				"-DBACKEND_SPIKE_EVIDENCE_DIR=$evidenceRoot",
				"-DBACKEND_SPIKE_BUILD_FIXTURE=ON",
				"-DBACKEND_SPIKE_PROVIDER_INSTALL_DIR=$installRoot") + $commonOfflineArguments
			$fixtureStages = [ordered]@{}
			$fixtureStages.configure = Invoke-LoggedCommand -Stage "fixture-configure" -FilePath "cmake" -Arguments $fixtureConfigure -LogPath (Join-Path $logsRoot "fixture-configure.log") -SkipExecution:$DryRun
			$fixtureStages.build = Invoke-LoggedCommand -Stage "fixture-build" -FilePath "cmake" -Arguments @("--build", $fixtureBuild, "--config", $Configuration, "--target", "BackendSpikeProviderFixture") -LogPath (Join-Path $logsRoot "fixture-build.log") -SkipExecution:$DryRun
			$fixtureStages.install = Invoke-LoggedCommand -Stage "fixture-install" -FilePath "cmake" -Arguments @("--install", $fixtureBuild, "--config", $Configuration) -LogPath (Join-Path $logsRoot "fixture-install.log") -SkipExecution:$DryRun

			$fixtureRuns = [ordered]@{}
			$fixtureAggregation = [ordered]@{ status = "not-run" }
			$dependencyClosure = [ordered]@{ status = "not-run" }
			if (-not $DryRun) {
				$fixtureExecutable = @(
					(Join-Path $fixtureBuild "ProviderFixture.exe"),
					(Join-Path (Join-Path $fixtureBuild $Configuration) "ProviderFixture.exe")
				) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
				if ([string]::IsNullOrWhiteSpace($fixtureExecutable)) { throw "Built ProviderFixture executable was not found under '$fixtureBuild'." }
				$workerCounts = @(1, 2, 4, [Environment]::ProcessorCount) | Sort-Object -Unique
				foreach ($workerCount in $workerCounts) {
					$repeatRuns = @()
					for ($repeat = 1; $repeat -le $FixtureRepeats; ++$repeat) {
						$fixtureLog = Join-Path $evidenceRoot "fixture-workers-$workerCount-repeat-$repeat.raw.json"
						$runTiming = Invoke-FixtureProcess -FilePath $fixtureExecutable -Arguments @("--workers=$workerCount") -LogPath $fixtureLog -TimeoutSeconds $FixtureTimeoutSeconds
						try { $fixtureResult = Get-Content -LiteralPath $fixtureLog -Raw | ConvertFrom-Json }
						catch { throw "ProviderFixture did not emit valid JSON for worker count $workerCount repeat $repeat. See '$fixtureLog'." }
						$repeatRuns += [ordered]@{ repeat = $repeat; timing = $runTiming; result = $fixtureResult }
					}
					$fixtureRuns["workers$workerCount"] = $repeatRuns
				}
				$allFixtureRuns = @($fixtureRuns.Values | ForEach-Object { $_ } | ForEach-Object { $_ })
				$stateHashes = @($allFixtureRuns | ForEach-Object { $_.result.determinism.backendStateHash } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object -Unique)
				$packetHashes = @($allFixtureRuns | ForEach-Object { $_.result.determinism.sortedEnginePacketHashFirst } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object -Unique)
				$queryHashes = @($allFixtureRuns | ForEach-Object { $_.result.queryBounds.denseOrderingHash } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object -Unique)
				$batchQueryHashes = @($allFixtureRuns | ForEach-Object { $_.result.queryBounds.batchOrderingHash } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object -Unique)
				$fixtureAggregation = [ordered]@{
					schemaVersion = 1; processLaunches = $allFixtureRuns.Count; workerLimits = $workerCounts
					backendStateHashes = $stateHashes; sortedPacketHashes = $packetHashes; queryOrderingHashes = $queryHashes; batchQueryOrderingHashes = $batchQueryHashes
					backendStateMatchesAcrossRepeatsAndWorkerLimits = ($stateHashes.Count -eq 1)
					sortedPacketMatchesAcrossRepeatsAndWorkerLimits = ($packetHashes.Count -eq 1)
					queryOrderingMatchesAcrossRepeatsAndWorkerLimits = ($queryHashes.Count -eq 1)
					batchQueryOrderingMatchesAcrossRepeatsAndWorkerLimits = ($batchQueryHashes.Count -eq 1)
					peakProcessWorkingSetBytes = [long](($allFixtureRuns | ForEach-Object { [long]$_.timing.peakWorkingSetBytes } | Measure-Object -Maximum).Maximum)
				}
				if (-not $fixtureAggregation.backendStateMatchesAcrossRepeatsAndWorkerLimits -or -not $fixtureAggregation.sortedPacketMatchesAcrossRepeatsAndWorkerLimits -or -not $fixtureAggregation.queryOrderingMatchesAcrossRepeatsAndWorkerLimits -or -not $fixtureAggregation.batchQueryOrderingMatchesAcrossRepeatsAndWorkerLimits) {
					throw "Cross-process determinism aggregation failed for $($Definition.name). Inspect the fixture raw JSON under '$evidenceRoot'."
				}
				$dependencyClosure = Get-DependencyClosureAudit -FixtureExecutable $fixtureExecutable -InstallRoot $installRoot -ProviderBuild $providerBuild -EvidenceRoot $evidenceRoot
			}
			$builtInFixture = [ordered]@{ schemaVersion = 3; status = $(if ($DryRun) { "dry-run" } else { "completed" }); stages = $fixtureStages; runs = $fixtureRuns; aggregation = $fixtureAggregation; dependencyClosure = $dependencyClosure }
		}

		$hookResults = [ordered]@{}
		$env:TERRA_BACKEND_SPIKE_CANDIDATE = $Definition.id
		$env:TERRA_BACKEND_SPIKE_SOURCE = $source
		$env:TERRA_BACKEND_SPIKE_BUILD = $providerBuild
		$env:TERRA_BACKEND_SPIKE_INSTALL = $installRoot
		$env:TERRA_BACKEND_SPIKE_EVIDENCE = $evidenceRoot
		foreach ($hook in @(@{ name = "fixture"; command = $FixtureCommand; arguments = $FixtureArguments }, @{ name = "measurement"; command = $MeasurementCommand; arguments = $MeasurementArguments })) {
			if (-not [string]::IsNullOrWhiteSpace($hook.command)) {
				$hookResults[$hook.name] = Invoke-LoggedCommand -Stage "$($hook.name)-hook" -FilePath $hook.command -Arguments @($hook.arguments) -LogPath (Join-Path $logsRoot "$($hook.name)-hook.log") -SkipExecution:$DryRun
			} else { $hookResults[$hook.name] = [ordered]@{ status = "not-supplied" } }
		}
	}
	finally {
		foreach ($entry in $oldEnvironment.GetEnumerator()) { [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, "Process") }
	}

	$installFiles = @(Get-FileDigestRecords -Directory $installRoot)
	$buildStats = Get-ChildItem -LiteralPath $providerBuild -File -Recurse | Measure-Object -Property Length -Sum
	$cmakeInputs = @(Get-ChildItem -LiteralPath $providerSource -File -Recurse | Where-Object { $_.Name -eq 'CMakeLists.txt' -or $_.Extension -eq '.cmake' })
	$networkPrimitiveMatches = @($cmakeInputs | Select-String -Pattern 'FetchContent|ExternalProject|file\s*\(\s*DOWNLOAD' -CaseSensitive:$false)
	$compilerCacheLines = @(Select-String -LiteralPath (Join-Path $providerBuild 'CMakeCache.txt') -Pattern '^CMAKE_(CXX_)?COMPILER(:FILEPATH)?=|^CMAKE_(CXX_)?COMPILER_VERSION' -ErrorAction SilentlyContinue | ForEach-Object { $_.Line })
	$processor = Get-CimInstance Win32_Processor | Select-Object -First 1
	$logFiles = @(Get-FileDigestRecords -Directory $logsRoot)
	# Get-FileDigestRecords intentionally returns ordered dictionaries so JSON
	# field order stays stable on Windows PowerShell 5.1. Measure-Object cannot
	# address dictionary keys as properties there, so project the numeric values
	# explicitly before summing the installed-tree byte count.
	$installBytes = [long](($installFiles | ForEach-Object { [long]$_['bytes'] } | Measure-Object -Sum).Sum)
	$rawEvidence = [ordered]@{
		schemaVersion = 3; candidate = $Definition.id; version = $Definition.version; revision = $observedRevision
		source = $source; configuration = $Configuration; doublePrecision = [bool]$DoublePrecision; platform = [System.Runtime.InteropServices.RuntimeInformation]::OSDescription
		architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString(); networkPolicy = "FetchContent disconnected; CMake registries disabled; loopback-only proxy"
		runStartedUtc = $runStartedUtc
		sourceManifest = @{ origin = $sourceUrl; tag = $sourceTag; commit = $observedRevision; gitTree = $sourceTree; acquisitionTimestampUtc = $sourceAcquisitionTimestampUtc; acquisitionTimestampBasis = "source-directory-creation-time"; cleanTrackedTree = ($sourceStatus.Count -eq 0); trackedDirtyEntries = $sourceStatus.Count; trackedStatus = $sourceStatus; fileCount = $sourceStats.Count; bytes = $sourceStats.Sum }
		toolchain = @{ cmake = (& cmake --version | Select-Object -First 1); compilerCache = $compilerCacheLines; cpu = $processor.Name; physicalCores = $processor.NumberOfCores; logicalProcessors = $processor.NumberOfLogicalProcessors }
		networkAudit = @{ loopbackProxyEnforced = $true; packageRegistriesDisabled = $true; cmakeNetworkPrimitiveMatches = $networkPrimitiveMatches.Count; note = "Source acquisition occurred before this measurement run." }
		providerPrivateThreadPoolAllowed = $false; lifecycle = @{ teardownRaceHook = "fixture-command"; result = "not-measured" }
		largeWorld = @{ floatPositionHook = "measurement-command"; doublePositionHook = "measurement-command"; result = "not-measured" }
		queryBounds = @{ maximumCastDistance = 100000; maximumShapeExtent = 1000; maximumHitsPerQuery = 64; maximumRequestsPerBatch = 10000; retainedResultTicks = 2; status = "reported-by-built-in-fixture" }
		# Hooks may add provider-specific raw packets, while these stable fields make
		# missing measurements visible rather than silently treating them as zero.
		performance = @{ tickP50Milliseconds = $null; tickP95Milliseconds = $null; tickP99Milliseconds = $null; allocations = $null; peakMemoryBytes = $null; binaryBytes = $null; schedulerContention = $null; status = "collect-through-measurement-hook" }
		determinism = @{ backendStateSha256 = $null; sortedPacketSha256 = $null; processLaunches = $null; workerCounts = @(); status = "built-in-fixture aggregation retained below" }
		timings = @{ harness = $harness; provider = $provider; hooks = $hookResults }
		fixture = $builtInFixture
		build = @{ fileCount = $buildStats.Count; bytes = $buildStats.Sum }
		logs = @{ files = $logFiles; deterministicSha256 = (Get-DirectoryDigest -Directory $logsRoot) }
		install = @{ fileCount = $installFiles.Count; files = $installFiles; bytes = $installBytes; deterministicSha256 = (Get-DirectoryDigest -Directory $installRoot) }
	}
	$rawPath = Join-Path $evidenceRoot "raw-result.json"
	Write-Utf8File -Path $rawPath -Content ($rawEvidence | ConvertTo-Json -Depth 12)
	$rawHash = (Get-FileHash -LiteralPath $rawPath -Algorithm SHA256).Hash.ToLowerInvariant()
	Write-Utf8File -Path (Join-Path $evidenceRoot "raw-result.sha256") -Content "$rawHash  raw-result.json"
	return $rawPath
}

if ($ValidateHarnessOnly) {
	# This path proves the CMake contract without source providers or network.
	$validationRoot = Join-Path (Get-AbsolutePath -Path $WorkRoot -BasePath $script:RepositoryRoot) "$RunId-validation"
	if (Test-Path -LiteralPath $validationRoot) { throw "Validation output already exists: '$validationRoot'." }
	New-Item -ItemType Directory -Force -Path $validationRoot | Out-Null
	$arguments = @("-S", $script:HarnessSource, "-B", (Join-Path $validationRoot "build")) + @(Get-CMakeGeneratorArguments) + @("-DCMAKE_INSTALL_PREFIX=$(Join-Path $validationRoot 'install')", "-DBACKEND_SPIKE_PROVIDER=Jolt56", "-DBACKEND_SPIKE_REVISION=e77f175595e64cb44218cc9d9d56fc365ad0e36a", "-DBACKEND_SPIKE_VALIDATE_ONLY=ON")
	Invoke-LoggedCommand -Stage "validation-configure" -FilePath "cmake" -Arguments $arguments -LogPath (Join-Path $validationRoot "configure.log") -SkipExecution:$DryRun | Out-Null
	Invoke-LoggedCommand -Stage "validation-build" -FilePath "cmake" -Arguments @("--build", (Join-Path $validationRoot "build")) -LogPath (Join-Path $validationRoot "build.log") -SkipExecution:$DryRun | Out-Null
	Invoke-LoggedCommand -Stage "validation-install" -FilePath "cmake" -Arguments @("--install", (Join-Path $validationRoot "build")) -LogPath (Join-Path $validationRoot "install.log") -SkipExecution:$DryRun | Out-Null
	Write-Host "Backend spike harness validation passed: $validationRoot"
	return
}

$definitions = @(
	@{ id = "Jolt55"; name = "Jolt Physics 5.5"; version = "5.5.0"; source = $Jolt55Source; revision = "23dadd0e603f1b321142d4c74df07fce85064989"; cmakeSubdirectory = "Build"; options = @("-DBUILD_SHARED_LIBS=OFF", "-DENABLE_INSTALL=ON", "-DTARGET_UNIT_TESTS=OFF", "-DTARGET_HELLO_WORLD=OFF", "-DTARGET_PERFORMANCE_TEST=OFF", "-DTARGET_SAMPLES=OFF", "-DTARGET_VIEWER=OFF", "-DENABLE_OBJECT_STREAM=OFF", "-DDEBUG_RENDERER_IN_DEBUG_AND_RELEASE=ON", "-DDEBUG_RENDERER_IN_DISTRIBUTION=OFF", "-DPROFILER_IN_DEBUG_AND_RELEASE=OFF", "-DOVERRIDE_CXX_FLAGS=OFF", "-DENABLE_ALL_WARNINGS=OFF") },
	@{ id = "Jolt56"; name = "Jolt Physics 5.6"; version = "5.6.0"; source = $Jolt56Source; revision = $Jolt56Revision; cmakeSubdirectory = "Build"; options = @("-DJPH_BUILD_SHARED_LIBS=OFF", "-DENABLE_INSTALL=ON", "-DTARGET_UNIT_TESTS=OFF", "-DTARGET_HELLO_WORLD=OFF", "-DTARGET_PERFORMANCE_TEST=OFF", "-DTARGET_SAMPLES=OFF", "-DTARGET_VIEWER=OFF", "-DENABLE_OBJECT_STREAM=OFF", "-DJPH_USE_DX12=OFF", "-DJPH_USE_VK=OFF", "-DJPH_USE_MTL=OFF", "-DJPH_USE_CPU_COMPUTE=OFF", "-DDEBUG_RENDERER_IN_DEBUG_AND_RELEASE=ON", "-DDEBUG_RENDERER_IN_DISTRIBUTION=OFF", "-DPROFILER_IN_DEBUG_AND_RELEASE=OFF", "-DOVERRIDE_CXX_FLAGS=OFF", "-DENABLE_ALL_WARNINGS=OFF") },
	@{ id = "PhysX58"; name = "PhysX 5.8"; version = "5.8.0"; source = $PhysX58Source; revision = "fc1018a3745664a1db2b95ce03fb5e91eb585f2e"; cmakeSubdirectory = "physx/compiler/public"; options = @("-DPX_GENERATE_GPU_PROJECTS=OFF", "-DPX_GENERATE_STATIC_LIBRARIES=ON") }
)
$selected = @($definitions | Where-Object { $Candidate -eq "All" -or $_.id -eq $Candidate })
foreach ($definition in $selected) {
	if ([string]::IsNullOrWhiteSpace($definition.source)) { throw "$($definition.id) requires its explicit -$($definition.id.Replace('Jolt', 'Jolt').Replace('PhysX58', 'PhysX58'))Source path." }
	if ([string]::IsNullOrWhiteSpace($definition.revision)) { throw "$($definition.id) requires an exact revision." }
}
if ($DoublePrecision -and ($selected | Where-Object { $_.id -eq "PhysX58" })) { throw "-DoublePrecision is supported only for Jolt candidates." }
$workRootPath = Get-AbsolutePath -Path $WorkRoot -BasePath $script:RepositoryRoot
$runRoot = Join-Path $workRootPath $RunId
Assert-ChildPath -Parent $workRootPath -Child $runRoot
if (Test-Path -LiteralPath $runRoot) { throw "Run output already exists: '$runRoot'. Choose a new -RunId; evidence is append-never-overwrite." }
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$results = foreach ($definition in $selected) { Invoke-BackendSpikeCandidate -Definition $definition -RunRoot $runRoot }
Write-Utf8File -Path (Join-Path $runRoot "run-index.json") -Content ([ordered]@{ schemaVersion = 1; candidates = $results; deterministicSha256 = (Get-DirectoryDigest -Directory $runRoot) } | ConvertTo-Json -Depth 6)
Write-Host "Backend spike completed. Raw evidence: $runRoot"
