# RunAnimationSoak.ps1 - Runs resumable animation stability slices with durable evidence.
#
# Each runner slice receives its result path through environment variables and
# must write the source-controlled JSON contract. Use -ValidateOnly to verify the
# frozen inputs and protocol without launching a process or starting a long soak.

[CmdletBinding()]
param(
	[ValidateNotNullOrEmpty()]
	[string]$ManifestPath = "Tests/Fixtures/Animation/stress/workloads.json",

	[ValidateNotNullOrEmpty()]
	[string]$ResultSchemaPath = "Tests/Fixtures/Animation/stress/soak-result.schema.json",

	[ValidateNotNullOrEmpty()]
	[string]$OutputRoot = "out/validation/animation/soak",

	[string]$RunnerPath = "",

	[string[]]$RunnerArguments = @(),

	[ValidateRange(0.001, 168.0)]
	[double]$DurationHours = 24.0,

	[ValidateRange(1, 86400)]
	[int]$IterationTimeoutSeconds = 900,

	[string]$CancellationFile = "",

	[switch]$Resume,

	[switch]$FinalPhase,

	[switch]$ValidateOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../../.."))
$script:OutRoot = [System.IO.Path]::GetFullPath((Join-Path $script:RepoRoot "out"))
$script:BaselineRoot = [System.IO.Path]::GetFullPath((Join-Path $script:RepoRoot "out/validation/animation/baseline"))
$script:Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$script:ExpectedManifestSha256 = "b5c78e9f01dc0036db4fb4674f632ebf06a736e4a7c010ec56e1f3804b92131d"
$script:ExpectedResultSchemaSha256 = "daa3318a1b2b105ee05a740135ff0724fb1cec46a2467c414f7859a866b87514"
$script:PrivateGrowthLimitBytes = [UInt64]268435456
$script:GpuGrowthLimitBytes = [UInt64]134217728
$script:HandleGrowthLimit = [UInt64]64
$script:ThreadGrowthLimit = [UInt64]8
$script:RetainedHistoryLimit = [UInt64]36000

function Resolve-RepoPath {
	param([Parameter(Mandatory = $true)][string]$Path)
	if ([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }
	return [System.IO.Path]::GetFullPath((Join-Path $script:RepoRoot $Path))
}

function Test-ChildPath {
	param([Parameter(Mandatory = $true)][string]$Parent, [Parameter(Mandatory = $true)][string]$Child)
	$prefix = [System.IO.Path]::GetFullPath($Parent).TrimEnd("\", "/") + [System.IO.Path]::DirectorySeparatorChar
	return [System.IO.Path]::GetFullPath($Child).StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-EvidencePath {
	param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)][string]$Description)
	if (-not (Test-ChildPath $script:OutRoot $Path) -or (Test-ChildPath $script:BaselineRoot $Path)) {
		throw "$Description must remain below out/ and outside the immutable baseline tree."
	}
}

function Get-Sha256 {
	param([Parameter(Mandatory = $true)][string]$Path)
	if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Required file is missing: '$Path'." }
	return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Read-Json {
	param([Parameter(Mandatory = $true)][string]$Path)
	if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Required JSON is missing: '$Path'." }
	try { return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json }
	catch { throw "JSON is invalid at '$Path': $($_.Exception.Message)" }
}

function Write-DurableJson {
	param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)]$Value)
	$directory = Split-Path -Parent $Path
	[System.IO.Directory]::CreateDirectory($directory) | Out-Null
	$temporary = "$Path.pending"
	$backup = "$Path.previous"
	$text = ($Value | ConvertTo-Json -Depth 100).Replace("`r`n", "`n") + "`n"
	$bytes = $script:Utf8NoBom.GetBytes($text)
	$stream = New-Object System.IO.FileStream($temporary, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::Read, 4096, [System.IO.FileOptions]::WriteThrough)
	try {
		$stream.Write($bytes, 0, $bytes.Length)
		$stream.Flush($true)
	}
	finally {
		$stream.Dispose()
	}
	if (Test-Path -LiteralPath $Path -PathType Leaf) {
		[System.IO.File]::Replace($temporary, $Path, $backup, $true)
		if (Test-Path -LiteralPath $backup -PathType Leaf) { Remove-Item -LiteralPath $backup -Force }
	}
	else {
		[System.IO.File]::Move($temporary, $Path)
	}
}

function Add-DurableProgress {
	param([Parameter(Mandatory = $true)][string]$Path, [Parameter(Mandatory = $true)]$Value)
	$directory = Split-Path -Parent $Path
	[System.IO.Directory]::CreateDirectory($directory) | Out-Null
	$text = (($Value | ConvertTo-Json -Compress -Depth 40) + "`n")
	$bytes = $script:Utf8NoBom.GetBytes($text)
	$stream = New-Object System.IO.FileStream($Path, [System.IO.FileMode]::Append, [System.IO.FileAccess]::Write, [System.IO.FileShare]::Read, 4096, [System.IO.FileOptions]::WriteThrough)
	try {
		$stream.Write($bytes, 0, $bytes.Length)
		$stream.Flush($true)
	}
	finally {
		$stream.Dispose()
	}
}

function Get-RequiredUInt64 {
	param([Parameter(Mandatory = $true)]$Object, [Parameter(Mandatory = $true)][string]$Name, [Parameter(Mandatory = $true)][string]$Context)
	# Fresh runs hold ordered dictionaries while resumed JSON becomes a
	# PSCustomObject. Read both representations explicitly so the second fresh
	# iteration enforces the same resource-growth checks as a resumed run.
	$value = $null
	$found = $false
	if ($Object -is [System.Collections.IDictionary] -and $Object.Contains($Name)) {
		$value = $Object[$Name]
		$found = $true
	}
	else {
		$property = $Object.PSObject.Properties[$Name]
		if ($null -ne $property) {
			$value = $property.Value
			$found = $true
		}
	}
	if (-not $found -or $null -eq $value) { throw "$Context omitted '$Name'." }
	try {
		$unsignedValue = [UInt64]$value
		if ([double]$value -lt 0) { throw "negative" }
		return $unsignedValue
	}
	catch { throw "$Context property '$Name' must be an unsigned integer." }
}

function Assert-IterationResult {
	param(
		[Parameter(Mandatory = $true)]$Result,
		[Parameter(Mandatory = $true)][string]$RunId,
		[Parameter(Mandatory = $true)][UInt64]$Iteration,
		[Parameter(Mandatory = $true)][UInt64]$PriorHeartbeat
	)
	if ($Result.schemaVersion -ne 1 -or [string]$Result.runId -cne $RunId -or
		[UInt64]$Result.iteration -ne $Iteration -or $Result.passed -isnot [bool] -or -not $Result.passed) {
		throw "Iteration $Iteration has an invalid identity or reports failure."
	}
	$heartbeat = Get-RequiredUInt64 $Result "heartbeatSequence" "Iteration $Iteration"
	if ($heartbeat -le $PriorHeartbeat) { throw "Iteration $Iteration did not advance its durable heartbeat." }
	foreach ($counter in @("deadlocks", "crashes", "stalePublications", "partialPublications", "unrecoverableRendererFailures", "unrecoverablePhysicsFailures")) {
		if ((Get-RequiredUInt64 $Result.counters $counter "Iteration $Iteration counters") -ne 0) {
			throw "Iteration $Iteration reported non-zero '$counter'."
		}
	}
	$resources = [ordered]@{
		privateBytes = Get-RequiredUInt64 $Result.resources "privateBytes" "Iteration $Iteration resources"
		gpuResidentBytes = Get-RequiredUInt64 $Result.resources "gpuResidentBytes" "Iteration $Iteration resources"
		handleCount = Get-RequiredUInt64 $Result.resources "handleCount" "Iteration $Iteration resources"
		threadCount = Get-RequiredUInt64 $Result.resources "threadCount" "Iteration $Iteration resources"
		retainedHistoryRecords = Get-RequiredUInt64 $Result.resources "retainedHistoryRecords" "Iteration $Iteration resources"
	}
	if ($resources.retainedHistoryRecords -gt $script:RetainedHistoryLimit) {
		throw "Iteration $Iteration exceeded the bounded diagnostic-history contract."
	}
	return [ordered]@{ heartbeat = $heartbeat; resources = $resources }
}

function Assert-BoundedGrowth {
	param([Parameter(Mandatory = $true)]$Baseline, [Parameter(Mandatory = $true)]$Current, [Parameter(Mandatory = $true)][UInt64]$Iteration)
	$checks = @(
		@("privateBytes", $script:PrivateGrowthLimitBytes),
		@("gpuResidentBytes", $script:GpuGrowthLimitBytes),
		@("handleCount", $script:HandleGrowthLimit),
		@("threadCount", $script:ThreadGrowthLimit)
	)
	foreach ($check in $checks) {
		$name = [string]$check[0]
		$limit = [UInt64]$check[1]
		# Resume deserializes dictionaries as PSCustomObject. Read through the same
		# required-property helper so fresh and resumed runs enforce identical math.
		$baselineValue = Get-RequiredUInt64 $Baseline $name "Soak baseline resources"
		$currentValue = Get-RequiredUInt64 $Current $name "Current soak resources"
		if ($currentValue -gt $baselineValue -and ($currentValue - $baselineValue) -gt $limit) {
			throw "Iteration $Iteration exceeded the '$name' growth limit of $limit."
		}
	}
}

function Stop-RunnerTree {
	param([Parameter(Mandatory = $true)][int]$ProcessId)
	$killer = Start-Process -FilePath "taskkill.exe" -ArgumentList @("/PID", $ProcessId, "/T", "/F") -WindowStyle Hidden -Wait -PassThru
	if ($killer.ExitCode -ne 0 -and $killer.ExitCode -ne 128) {
		throw "taskkill failed for runner process $ProcessId with exit code $($killer.ExitCode)."
	}
}

function Quote-ProcessArgument {
	param([Parameter(Mandatory = $true)][AllowEmptyString()][string]$Value)
	if ($Value -notmatch '[\s"]') { return $Value }
	return '"' + $Value.Replace('\', '\').Replace('"', '\"') + '"'
}

function Invoke-RunnerSlice {
	param(
		[Parameter(Mandatory = $true)][string]$Executable,
		[Parameter(Mandatory = $true)][string[]]$Arguments,
		[Parameter(Mandatory = $true)][string]$RunId,
		[Parameter(Mandatory = $true)][UInt64]$Iteration,
		[Parameter(Mandatory = $true)][string]$ResultPath,
		[Parameter(Mandatory = $true)][string]$LogPath,
		[Parameter(Mandatory = $true)][string]$CancelPath,
		[Parameter(Mandatory = $true)][int]$TimeoutSeconds
	)
	$start = New-Object System.Diagnostics.ProcessStartInfo
	$start.FileName = $Executable
	$start.Arguments = (($Arguments | ForEach-Object { Quote-ProcessArgument ([string]$_) }) -join " ")
	$start.WorkingDirectory = $script:RepoRoot
	$start.UseShellExecute = $false
	$start.CreateNoWindow = $true
	# The runner's durable JSON is the evidence channel. Do not buffer arbitrary
	# stdout/stderr in the driver process, because a noisy failed runner could turn
	# a long soak into unbounded diagnostic memory growth.
	$start.RedirectStandardOutput = $false
	$start.RedirectStandardError = $false
	$start.EnvironmentVariables["TERRAENGINE_ANIMATION_SOAK_RUN_ID"] = $RunId
	$start.EnvironmentVariables["TERRAENGINE_ANIMATION_SOAK_ITERATION"] = [string]$Iteration
	$start.EnvironmentVariables["TERRAENGINE_ANIMATION_SOAK_RESULT"] = $ResultPath
	$process = [System.Diagnostics.Process]::Start($start)
	if ($null -eq $process) { throw "Could not start soak runner '$Executable'." }
	$watch = [System.Diagnostics.Stopwatch]::StartNew()
	$cancelled = $false
	$timedOut = $false
	try {
		while (-not $process.HasExited) {
			if (Test-Path -LiteralPath $CancelPath -PathType Leaf) {
				$cancelled = $true
				Stop-RunnerTree $process.Id
				break
			}
			if ($watch.Elapsed.TotalSeconds -ge $TimeoutSeconds) {
				$timedOut = $true
				Stop-RunnerTree $process.Id
				break
			}
			Start-Sleep -Milliseconds 250
		}
		$process.WaitForExit()
		$watch.Stop()
		$log = [ordered]@{
			schemaVersion = 1
			iteration = $Iteration
			executable = $Executable
			arguments = $Arguments
			exitCode = $process.ExitCode
			cancelled = $cancelled
			timedOut = $timedOut
			elapsedSeconds = $watch.Elapsed.TotalSeconds
			outputPolicy = "not-buffered-runner-result-json-is-authoritative"
		}
		Write-DurableJson $LogPath $log
		if ($cancelled) { return [ordered]@{ cancelled = $true; elapsedSeconds = $watch.Elapsed.TotalSeconds } }
		if ($timedOut) { throw "Runner iteration $Iteration exceeded $TimeoutSeconds seconds; this is a deadlock failure." }
		if ($process.ExitCode -ne 0) { throw "Runner iteration $Iteration exited with code $($process.ExitCode)." }
		return [ordered]@{ cancelled = $false; elapsedSeconds = $watch.Elapsed.TotalSeconds }
	}
	finally {
		$process.Dispose()
	}
}

$resolvedManifest = Resolve-RepoPath $ManifestPath
$resolvedSchema = Resolve-RepoPath $ResultSchemaPath
if ((Get-Sha256 $resolvedManifest) -cne $script:ExpectedManifestSha256) { throw "The animation stress manifest identity changed." }
if ((Get-Sha256 $resolvedSchema) -cne $script:ExpectedResultSchemaSha256) { throw "The soak result schema identity changed." }
$manifest = Read-Json $resolvedManifest
$schema = Read-Json $resolvedSchema
if ($manifest.performanceAcceptanceMode -ne "release" -or -not $manifest.performanceExecutionDeferred -or
	$schema.properties.schemaVersion.const -ne 1 -or $schema.properties.resources.properties.retainedHistoryRecords.maximum -ne 36000) {
	throw "The source-controlled soak contract is malformed or weakened."
}
if ($DurationHours -ge 24.0 -and -not $FinalPhase -and -not $ValidateOnly) {
	throw "A 24-hour completion attempt requires -FinalPhase; intermediate runs remain soak-safety evidence only."
}

if ($ValidateOnly) {
	[ordered]@{
		schemaVersion = 1
		tool = "RunAnimationSoak"
		mode = "protocol-readiness"
		passed = $true
		processLaunched = $false
		performanceExecuted = $false
		manifestSha256 = Get-Sha256 $resolvedManifest
		resultSchemaSha256 = Get-Sha256 $resolvedSchema
		targetHours = $DurationHours
		finalAcceptanceEligible = $FinalPhase -and $DurationHours -ge 24.0
		limits = [ordered]@{
			privateGrowthBytes = $script:PrivateGrowthLimitBytes
			gpuGrowthBytes = $script:GpuGrowthLimitBytes
			handleGrowth = $script:HandleGrowthLimit
			threadGrowth = $script:ThreadGrowthLimit
			retainedHistoryRecords = $script:RetainedHistoryLimit
		}
	} | ConvertTo-Json -Compress -Depth 20 | Write-Output
	return
}

if ([string]::IsNullOrWhiteSpace($RunnerPath)) { throw "RunnerPath is required unless -ValidateOnly is used." }
$resolvedRunner = Resolve-RepoPath $RunnerPath
if (-not (Test-Path -LiteralPath $resolvedRunner -PathType Leaf)) { throw "RunnerPath was not found: '$resolvedRunner'." }
$resolvedOutput = Resolve-RepoPath $OutputRoot
Assert-EvidencePath $resolvedOutput "OutputRoot"
$cancelPath = if ([string]::IsNullOrWhiteSpace($CancellationFile)) {
	Join-Path $resolvedOutput "cancel.request"
}
else {
	Resolve-RepoPath $CancellationFile
}
Assert-EvidencePath $cancelPath "CancellationFile"

$statePath = Join-Path $resolvedOutput "state.json"
$progressPath = Join-Path $resolvedOutput "progress.jsonl"
$finalPath = Join-Path $resolvedOutput "result.json"
$targetSeconds = [UInt64][Math]::Ceiling($DurationHours * 3600.0)
$configurationIdentity = [ordered]@{
	manifestSha256 = Get-Sha256 $resolvedManifest
	resultSchemaSha256 = Get-Sha256 $resolvedSchema
	runnerSha256 = Get-Sha256 $resolvedRunner
	runnerArguments = $RunnerArguments
	targetSeconds = $targetSeconds
	iterationTimeoutSeconds = $IterationTimeoutSeconds
	finalPhase = [bool]$FinalPhase
}

if ($Resume) {
	if (-not (Test-Path -LiteralPath $resolvedOutput -PathType Container)) { throw "Resume requires an existing OutputRoot." }
	$state = Read-Json $statePath
	if ($state.schemaVersion -ne 1 -or $state.status -notin @("active", "cancelled") -or
		($state.configuration | ConvertTo-Json -Compress -Depth 20) -cne ($configurationIdentity | ConvertTo-Json -Compress -Depth 20)) {
		throw "The durable soak state is not resumable with this exact configuration."
	}
}
else {
	if (Test-Path -LiteralPath $resolvedOutput) { throw "OutputRoot must be fresh unless -Resume is used." }
	[System.IO.Directory]::CreateDirectory($resolvedOutput) | Out-Null
	$state = [ordered]@{
		schemaVersion = 1
		runId = [Guid]::NewGuid().ToString("N")
		status = "active"
		configuration = $configurationIdentity
		nextIteration = [UInt64]0
		completedRuntimeSeconds = [double]0
		lastHeartbeatSequence = [UInt64]0
		baselineResources = $null
		highWaterResources = $null
		lastCompletedAtUtc = $null
		failure = $null
	}
	Write-DurableJson $statePath $state
	Add-DurableProgress $progressPath ([ordered]@{ type = "started"; schemaVersion = 1; runId = $state.runId; atUtc = [DateTime]::UtcNow.ToString("o") })
}

try {
	$state.status = "active"
	$state.failure = $null
	Write-DurableJson $statePath $state
	while ([double]$state.completedRuntimeSeconds -lt [double]$targetSeconds) {
		if (Test-Path -LiteralPath $cancelPath -PathType Leaf) {
			$state.status = "cancelled"
			Write-DurableJson $statePath $state
			Add-DurableProgress $progressPath ([ordered]@{ type = "cancelled"; schemaVersion = 1; runId = $state.runId; nextIteration = $state.nextIteration; atUtc = [DateTime]::UtcNow.ToString("o") })
			break
		}
		$iteration = [UInt64]$state.nextIteration
		$iterationRoot = Join-Path $resolvedOutput ("iterations/{0:d8}" -f $iteration)
		if (Test-Path -LiteralPath $iterationRoot) { throw "Iteration output already exists and may be stale: '$iterationRoot'." }
		[System.IO.Directory]::CreateDirectory($iterationRoot) | Out-Null
		$resultPath = Join-Path $iterationRoot "runner-result.json"
		$logPath = Join-Path $iterationRoot "runner-log.json"
		Add-DurableProgress $progressPath ([ordered]@{ type = "iteration-started"; schemaVersion = 1; runId = $state.runId; iteration = $iteration; atUtc = [DateTime]::UtcNow.ToString("o") })
		$invocation = Invoke-RunnerSlice $resolvedRunner $RunnerArguments $state.runId $iteration $resultPath $logPath $cancelPath $IterationTimeoutSeconds
		if ($invocation.cancelled) {
			$state.status = "cancelled"
			Write-DurableJson $statePath $state
			Add-DurableProgress $progressPath ([ordered]@{ type = "cancelled"; schemaVersion = 1; runId = $state.runId; iteration = $iteration; atUtc = [DateTime]::UtcNow.ToString("o") })
			break
		}
		$result = Read-Json $resultPath
		$validated = Assert-IterationResult $result $state.runId $iteration ([UInt64]$state.lastHeartbeatSequence)
		if ($null -eq $state.baselineResources) { $state.baselineResources = $validated.resources }
		else { Assert-BoundedGrowth $state.baselineResources $validated.resources $iteration }
		if ($null -eq $state.highWaterResources) { $state.highWaterResources = $validated.resources }
		else {
			foreach ($name in @("privateBytes", "gpuResidentBytes", "handleCount", "threadCount", "retainedHistoryRecords")) {
				$currentHighWater = Get-RequiredUInt64 $state.highWaterResources $name "Soak high-water resources"
				if ([UInt64]$validated.resources[$name] -gt $currentHighWater) {
					if ($state.highWaterResources -is [System.Collections.IDictionary]) {
						$state.highWaterResources[$name] = [UInt64]$validated.resources[$name]
					}
					else {
						$state.highWaterResources.$name = [UInt64]$validated.resources[$name]
					}
				}
			}
		}
		$state.completedRuntimeSeconds = [double]$state.completedRuntimeSeconds + [double]$invocation.elapsedSeconds
		$state.lastHeartbeatSequence = [UInt64]$validated.heartbeat
		$state.nextIteration = $iteration + 1
		$state.lastCompletedAtUtc = [DateTime]::UtcNow.ToString("o")
		Write-DurableJson $statePath $state
		Add-DurableProgress $progressPath ([ordered]@{ type = "iteration-completed"; schemaVersion = 1; runId = $state.runId; iteration = $iteration; heartbeatSequence = $validated.heartbeat; completedRuntimeSeconds = $state.completedRuntimeSeconds; resources = $validated.resources; atUtc = $state.lastCompletedAtUtc })
	}

	$completed = [double]$state.completedRuntimeSeconds -ge [double]$targetSeconds
	if ($completed) { $state.status = "completed"; Write-DurableJson $statePath $state }
	$final = [ordered]@{
		schemaVersion = 1
		tool = "RunAnimationSoak"
		runId = $state.runId
		status = $state.status
		passed = $completed
		cancelled = $state.status -eq "cancelled"
		resumable = $state.status -eq "cancelled"
		finalAcceptanceEligible = $completed -and $FinalPhase -and $DurationHours -ge 24.0
		targetSeconds = $targetSeconds
		completedRuntimeSeconds = $state.completedRuntimeSeconds
		completedIterations = $state.nextIteration
		lastHeartbeatSequence = $state.lastHeartbeatSequence
		baselineResources = $state.baselineResources
		highWaterResources = $state.highWaterResources
		configuration = $configurationIdentity
	}
	Write-DurableJson $finalPath $final
	$final | ConvertTo-Json -Compress -Depth 40 | Write-Output
}
catch {
	$state.status = "failed"
	$state.failure = $_.Exception.Message
	Write-DurableJson $statePath $state
	Add-DurableProgress $progressPath ([ordered]@{ type = "failed"; schemaVersion = 1; runId = $state.runId; nextIteration = $state.nextIteration; message = $_.Exception.Message; atUtc = [DateTime]::UtcNow.ToString("o") })
	throw
}
