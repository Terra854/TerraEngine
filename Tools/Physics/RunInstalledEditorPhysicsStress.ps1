# RunInstalledEditorPhysicsStress.ps1 - Opt-in installed-editor physics stress harness.
#
# This tool deliberately creates its project below out/ so an editor run cannot
# alter a developer's project or global editor preferences.  It uses only box
# colliders, which keeps the 10,000-object scene reproducible without downloads
# or asset cooking.  The editor is driven through its loopback MCP JSON-RPC
# contract; no keyboard/mouse automation or provider-private API is involved.
#
# Example:
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Physics/RunInstalledEditorPhysicsStress.ps1 `
#     -InstallRoot out/build/RelWithDebInfo/Build -AllowLiveStress
#
# readiness readiness matrix (one disposable project, five clean editor launches):
#   powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tools/Physics/RunInstalledEditorPhysicsStress.ps1 `
#     -InstallRoot out/build/RelWithDebInfo/Build -WorkRoot out/validation/readiness `
#     -RunReadinessMatrix -AllowLiveStress
# Individual -ReadinessMode cold|warm|stale|corrupt|forced-build runs retain
# their own evidence summary below WorkRoot/evidence/. Warm and later modes
# require the disposable project left by the preceding cold scenario.
# profiler-capture profiler checks are opt-in so the historical physics timing run remains
# comparable. They request a capture detail/version, then retain a bounded
# summary and recovery/metric-gate evidence under evidence/profiler-capture-*.json.

[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)]
	[ValidateNotNullOrEmpty()]
	[string] $InstallRoot,

	[ValidateNotNullOrEmpty()]
	[string] $WorkRoot = "out/validation/installed-editor-physics-stress",

	[ValidateRange(100, 50000)]
	[int] $ObjectCount = 10000,

	[ValidateRange(1024, 65535)]
	[int] $McpPort = 43127,

	[ValidateRange(1, 100)]
	[int] $ExplicitStepCount = 100,

	# The committed acceptance manifest requires exactly 1,000 concurrently
	# active dynamic bodies. Callers may raise this for an explicit overload run,
	# but the default live gate stays comparable to that source-controlled fixture.
	[ValidateRange(1, 10000)]
	[int] $ActiveDynamicCount = 1000,

	# canonical-runtime uses the same installed-editor harness for canonical, deliberately
	# dense, and small-scene marker evidence. Defaults retain the accepted
	# 10,000-body canonical physics identity.
	[ValidateSet("canonical", "dense", "small")]
	[string] $SceneViewScenario = "canonical",

	[ValidateRange(30, 600)]
	[int] $StartupTimeoutSeconds = 120,

	[ValidateSet("0", "1", "2", "4", "max")]
	[string] $WorkerCount = "max",

	# worker-scaling labels the workload used by the worker-scaling matrix.  The label is
	# persisted with each run so a comparison cannot accidentally mix scenarios.
	[ValidateSet("paused", "unchanged", "small", "large")]
	[string] $WorkerScalingScenario = "large",

	# Keep scheduler thresholds explicit and reproducible; the matrix can rerun
	# with an override without editing the generated project settings.
	[ValidateRange(1, 1000000)]
	[int] $ParallelPreparationThreshold = 512,

	[ValidateRange(1, 1000000)]
	[int] $ParallelExtractionThreshold = 512,

	# Solver quality is part of the workload identity. These values are written to
	# the disposable project before the editor starts and verified from copied
	# runtime diagnostics on every measured step.
	[ValidateRange(2, 255)]
	[int] $VelocityIterations = 10,

	[ValidateRange(1, 255)]
	[int] $PositionIterations = 2,

	# Runs the complete paused/unchanged/small/large x 1/2/4/max matrix and
	# writes one recommendation report below WorkRoot.
	[switch] $RunWorkerScalingMatrix,

	# Selects a source-controlled workload identity. Only the generic unchanged
	# and worker-large layouts can be authored by this box-only harness today;
	# specialised contact, ragdoll, and bulk-admission fixtures fail closed.
	[ValidateSet("physics-unchanged-10000-v1", "physics-contact-pairs-4096-v1", "physics-ragdoll-crowd-64x16-v1", "physics-ragdoll-crowd-128x16-v1", "physics-bulk-admission-10000-v1", "physics-worker-large-v1")]
	[string] $CanonicalFixture = "physics-unchanged-10000-v1",

	# Each leg owns a fresh installed editor process. The quality matrix proves
	# the settings copied into the disposable project are observed at runtime.
	[switch] $RunQualityProfileMatrix,

	# Acceptance repetition runs exactly the ten source-controlled attempts.
	# It emits a fail-closed aggregate-status record when the installed bridge
	# cannot supply all fields required by the strict evidence JSON schema.
	[switch] $RunRepeatedAcceptanceMatrix,

	# Internal diagnostic legs retain their measurements in the worker report,
	# but are never presented as a canonical acceptance fixture.
	[switch] $AllowDiagnosticFixture,

	# readiness readiness cases reuse one disposable project across clean editor
	# launches.  The modes retain source authority: only generated cache/output
	# artifacts are removed or damaged, except stale deliberately changes the
	# disposable source copy to invalidate its derived scene.
	[ValidateSet("none", "cold", "warm", "stale", "corrupt", "forced-build")]
	[string] $ReadinessMode = "none",

	# profiler-capture capture controls are additive to the existing profiler operations. The
	# editor may ignore these fields when an older installed bridge is used; the
	# retained evidence records that compatibility outcome explicitly.
	[ValidateSet("detailed", "sampled", "aggregate")]
	[string] $CaptureLevel = "detailed",

	[ValidateRange(5, 5)]
	[int] $CaptureDurationSeconds = 5,

	[switch] $RunProfilerCaptureChecks,

	# Runs the documented cold/warm/stale/corrupt/forced-build sequence and
	# writes a per-scenario report plus a matrix report below WorkRoot.
	[switch] $RunReadinessMatrix,

	# The authoring walkthrough adds public editor authoring operations before Play Mode. Keep this
	# optional so performance baseline performance measurements remain directly comparable.
	[switch] $RunAuthoringWalkthrough,

	# managed-reload-automation exercises the installed managed-reload and public physics automation
	# contracts. Its extra query and command calls remain outside the timed stress
	# interval so the performance baseline performance measurements stay comparable.
	[switch] $RunManagedReloadAutomationLoopback,

	# physics validation collider-cook adds an installed-editor matrix around the timed physics run.
	# Reparent/conflict probes execute while stopped, and stale/unchanged/active
	# probes use only published runtime snapshots so they never mutate live storage.
	[switch] $RunColliderCookIncrementalMatrix,

	# Memory regression runs opt into the maximum debug primitive budget while
	# preserving the normal two-tick retention setting. Default acceptance runs
	# keep visualization disabled so historical timings remain comparable.
	[switch] $EnablePhysicsDebugVisualization,

	# Process counters are sampled only when requested because querying Windows
	# process accounting after every step would contaminate the normal timing gate.
	[switch] $CaptureProcessMemory,

	# A memory-retention run can exclude the intentionally growing live profiler
	# document so process counters isolate physics/editor ownership more clearly.
	[switch] $SkipProfilerCapture,

	[switch] $AllowLiveStress
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $AllowLiveStress) {
	throw "Installed editor stress is opt-in. Pass -AllowLiveStress after the deterministic physics tests pass."
}

function Get-AbsolutePath {
	param([Parameter(Mandatory = $true)][string] $Path, [Parameter(Mandatory = $true)][string] $BasePath)
	if ([System.IO.Path]::IsPathRooted($Path)) { return [System.IO.Path]::GetFullPath($Path) }
	return [System.IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

function Assert-PathBelow {
	param([Parameter(Mandatory = $true)][string] $Parent, [Parameter(Mandatory = $true)][string] $Child)
	$parentPrefix = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\', '/') + [System.IO.Path]::DirectorySeparatorChar
	$childPath = [System.IO.Path]::GetFullPath($Child)
	if (-not $childPath.StartsWith($parentPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
		throw "Disposable work root must remain below '$parentPrefix'; received '$childPath'."
	}
}

function Get-StableGuid {
	param([Parameter(Mandatory = $true)][UInt64] $Value)
	# UUIDs are stable only within this disposable scene.  A deterministic format
	# makes a failed run reproducible while avoiding a dependency on randomness.
	return [string]::Format([Globalization.CultureInfo]::InvariantCulture, "{0:x32}", $Value)
}

function Test-LoopbackPortAvailable {
	param([Parameter(Mandatory = $true)][int] $Port)
	$listener = $null
	try {
		$listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $Port)
		$listener.Start()
		return $true
	}
	catch {
		return $false
	}
	finally {
		if ($null -ne $listener) { $listener.Stop() }
	}
}

function Write-JsonFile {
	param([Parameter(Mandatory = $true)][string] $Path, [Parameter(Mandatory = $true)] $Value, [int] $Depth = 32)
	$directory = Split-Path -Parent $Path
	if (-not [string]::IsNullOrWhiteSpace($directory)) { New-Item -ItemType Directory -Force -Path $directory | Out-Null }
	$Value | ConvertTo-Json -Depth $Depth | Set-Content -LiteralPath $Path -Encoding utf8
}

function Add-EvidenceRecord {
	param([Parameter(Mandatory = $true)][string] $Path, [Parameter(Mandatory = $true)] $Value)
	# JSON Lines keeps each request/response independently readable if the editor
	# crashes or the run is stopped midway through a long stress observation.
	$Value | ConvertTo-Json -Depth 32 -Compress | Add-Content -LiteralPath $Path -Encoding utf8
}

function Get-MonotonicMilliseconds {
	# Stopwatch is monotonic and is therefore safe for stage durations even when
	# the wall clock is adjusted by Windows or a time synchronisation service.
	return ([System.Diagnostics.Stopwatch]::GetTimestamp() - $script:monotonicStartTicks) * 1000.0 / $script:monotonicFrequency
}

function Start-Stage {
	param([Parameter(Mandatory = $true)][string] $Name)
	$script:stageStarts[$Name] = Get-MonotonicMilliseconds
}

function Complete-Stage {
	param([Parameter(Mandatory = $true)][string] $Name)
	if ($script:stageStarts.Contains($Name)) {
		$start = [double]$script:stageStarts[$Name]
		$script:stageRecords[$Name] = [ordered]@{ startedMs = $start; durationMs = [Math]::Max(0.0, (Get-MonotonicMilliseconds) - $start) }
	}
}

function Capture-EditorProcessMemory {
	param([Parameter(Mandatory = $true)][string] $Phase, [UInt64] $Tick = 0)
	if (-not $CaptureProcessMemory -or $null -eq $process) { return }
	$process.Refresh()
	if ($process.HasExited) { return }
	# Working set includes resident engine, renderer, and driver-backed pages;
	# private bytes is the stronger signal for retained process-owned allocations.
	$processMemorySamples.Add([ordered]@{
		timestamp = (Get-Date).ToString("o")
		phase = $Phase
		tick = $Tick
		workingSetBytes = [UInt64]$process.WorkingSet64
		privateBytes = [UInt64]$process.PrivateMemorySize64
	})
}

function Get-Percentile {
	param([Parameter(Mandatory = $true)] [double[]] $Values, [Parameter(Mandatory = $true)] [double] $Percentile)
	if ($Values.Count -eq 0) { return $null }
	$ordered = @($Values | Sort-Object)
	$rank = [Math]::Ceiling(($Percentile / 100.0) * $ordered.Count) - 1
	$index = [Math]::Max(0, [Math]::Min($ordered.Count - 1, [int]$rank))
	return [double]$ordered[$index]
}

function Get-LatencySummary {
	param([Parameter(Mandatory = $true)] $Samples)
	$groups = [ordered]@{}
	foreach ($sample in @($Samples)) {
		$key = [string]$sample.operation
		if (-not $groups.Contains($key)) { $groups[$key] = [System.Collections.Generic.List[double]]::new() }
		$groups[$key].Add([double]$sample.durationMs)
	}
	$result = [ordered]@{}
	foreach ($key in $groups.Keys) {
		$values = [double[]]$groups[$key].ToArray()
		$result[$key] = [ordered]@{ count = $values.Count; p50Ms = Get-Percentile -Values $values -Percentile 50; p95Ms = Get-Percentile -Values $values -Percentile 95; p99Ms = Get-Percentile -Values $values -Percentile 99 }
	}
	return $result
}

function Get-HardwareEvidence {
	# CIM can be unavailable in constrained environments; preserve an explicit
	# marker instead of failing the physics run or inventing machine metadata.
	$result = [ordered]@{ available = $true; os = $null; cpu = $null; logicalProcessors = $null; memoryBytes = $null; unavailable = @() }
	try { $os = Get-CimInstance Win32_OperatingSystem -ErrorAction Stop; $result.os = "$($os.Caption) $($os.Version)" } catch { $result.unavailable += "os" }
	try { $cpu = @(Get-CimInstance Win32_Processor -ErrorAction Stop); $result.cpu = ($cpu | Select-Object -First 1 -ExpandProperty Name); $result.logicalProcessors = ($cpu | Measure-Object -Property NumberOfLogicalProcessors -Sum).Sum } catch { $result.unavailable += "cpu" }
	try { $computer = Get-CimInstance Win32_ComputerSystem -ErrorAction Stop; $result.memoryBytes = [UInt64]$computer.TotalPhysicalMemory } catch { $result.unavailable += "memory" }
	if ($result.unavailable.Count -gt 0) { $result.available = $false }
	return $result
}

function Get-ArtifactInventory {
	param([Parameter(Mandatory = $true)][string] $Root)
	$items = [System.Collections.Generic.List[object]]::new()
	if (-not (Test-Path -LiteralPath $Root)) { return @() }
	foreach ($file in @(Get-ChildItem -LiteralPath $Root -Recurse -File -ErrorAction SilentlyContinue)) {
		$items.Add([ordered]@{ path = $file.FullName; bytes = [UInt64]$file.Length; extension = $file.Extension })
	}
	return @($items)
}

function Get-PropertyValue {
	param([Parameter(Mandatory = $true)] $Object, [Parameter(Mandatory = $true)][string] $Name)
	if ($null -eq $Object) { return $null }
	$property = $Object.PSObject.Properties[$Name]
	if ($null -eq $property) { return $null }
	return $property.Value
}

function Get-CanonicalFixtureEvidence {
	param([Parameter(Mandatory = $true)][string] $FixtureId)
	# The manifest is the authority for identity, population, and active-body
	# counts. Reading it here prevents command-line values from silently changing
	# what a named acceptance fixture means.
	$manifestPath = Join-Path $repoRoot "Tests/Fixtures/Performance/Physics/workloads.json"
	if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
		throw "Canonical physics workload manifest was not found: '$manifestPath'."
	}
	$manifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
	$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
	$fixture = @($manifest.workloads | Where-Object { [string]$_.id -eq $FixtureId } | Select-Object -First 1)
	if ($fixture.Count -ne 1) { throw "Canonical physics fixture '$FixtureId' is not declared by '$manifestPath'." }
	$implemented = $FixtureId -in @("physics-unchanged-10000-v1", "physics-worker-large-v1")
	return [ordered]@{
		id = $FixtureId; version = [int]$fixture[0].version; manifestPath = $manifestPath; manifestSha256 = $manifestHash
		population = [int]$fixture[0].population; activeBodies = [int]$fixture[0].activeBodies
		warmupSteps = [int]$fixture[0].warmupSteps; measuredSteps = [int]$fixture[0].measuredSteps
		seed = [int]$fixture[0].seed; description = [string]$fixture[0].description
		availability = if ($implemented) { "available" } else { "unavailable" }
		availabilityReason = if ($implemented) { $null } else { "The installed box-only harness cannot author the canonical contact, ragdoll, or fresh-world bulk-admission topology." }
	}
}

function Assert-CanonicalFixtureArguments {
	param([Parameter(Mandatory = $true)] $Fixture)
	if ($Fixture.availability -ne "available") {
		throw "Canonical fixture '$($Fixture.id)' is unavailable: $($Fixture.availabilityReason)"
	}
	if ($ObjectCount -ne [int]$Fixture.population -or $ActiveDynamicCount -ne [int]$Fixture.activeBodies -or $ExplicitStepCount -ne [int]$Fixture.measuredSteps) {
		throw "Canonical fixture '$($Fixture.id)' requires population=$($Fixture.population), activeBodies=$($Fixture.activeBodies), and measuredSteps=$($Fixture.measuredSteps); received $ObjectCount/$ActiveDynamicCount/$ExplicitStepCount."
	}
}

function Get-WorkerResolutionCheck {
	param([Parameter(Mandatory = $true)] $Record)
	$resolved = Get-PropertyValue -Object $Record.summary -Name "actualWorkerCount"
	if ($null -eq $resolved -or [int]$resolved -lt 1) {
		return [ordered]@{ available = $false; exact = $false; reason = "child did not report a positive resolved worker count"; resolved = $null }
	}
	$requested = [string]$Record.requestedWorkerCount
	# `max` is not a wildcard in acceptance evidence: the child must resolve it to
	# the exact hardware-concurrency value it records for the same process.
	$expected = if ($requested -eq "max") { [int][Math]::Max(1, [Environment]::ProcessorCount) } else { [int]$requested }
	$exact = [int]$resolved -eq $expected
	return [ordered]@{
		available = $true; exact = $exact; requested = $requested; resolved = [int]$resolved
		reason = if ($exact) { $null } else { "requested $requested workers (expected $expected) but the runtime resolved $resolved" }
	}
}

function Get-QualityEvidenceCheck {
	param([Parameter(Mandatory = $true)] $Summary, [Parameter(Mandatory = $true)][int] $ExpectedVelocityIterations, [Parameter(Mandatory = $true)][int] $ExpectedPositionIterations)
	$samples = @((Get-PropertyValue -Object $Summary -Name "physicsStageSamples"))
	if ($samples.Count -eq 0) {
		return [ordered]@{ available = $false; applied = $false; reason = "no measured physics diagnostics were retained"; verifiedSteps = 0 }
	}
	$invalid = @($samples | Where-Object {
		$quality = Get-PropertyValue -Object $_ -Name "quality"
		$null -eq $quality -or -not [bool](Get-PropertyValue -Object $quality -Name "applied") -or
		[int](Get-PropertyValue -Object $quality -Name "velocityIterations") -ne $ExpectedVelocityIterations -or
		[int](Get-PropertyValue -Object $quality -Name "positionIterations") -ne $ExpectedPositionIterations
	})
	return [ordered]@{
		available = $true; applied = ($invalid.Count -eq 0); verifiedSteps = $samples.Count
		reason = if ($invalid.Count -eq 0) { $null } else { "one or more measured diagnostics did not report the requested quality profile" }
	}
}

function Complete-ProfilerCaptureEvidence {
	param([Parameter(Mandatory = $true)] $Summary, [Parameter(Mandatory = $true)] $Artifact, [Parameter(Mandatory = $true)][string] $ArtifactRoot)
	if (-not $RunProfilerCaptureChecks) { return }
	$summaryData = Get-PropertyValue -Object $Summary -Name "summary"
	if ($null -eq $summaryData) { $summaryData = $Summary }
	$reportedFormat = Get-PropertyValue -Object $Summary -Name "formatIdentifier"
	if ($null -eq $reportedFormat) { $reportedFormat = Get-PropertyValue -Object $summaryData -Name "formatIdentifier" }
	$reportedBytes = Get-PropertyValue -Object $Summary -Name "artifactBytes"
	if ($null -eq $reportedBytes) { $reportedBytes = Get-PropertyValue -Object $summaryData -Name "artifactBytes" }
	if ($null -eq $reportedBytes) { $reportedBytes = Get-PropertyValue -Object $summaryData -Name "artifactSizeBytes" }
	if ($null -eq $reportedBytes) { $reportedBytes = Get-PropertyValue -Object $Artifact -Name "bytes" }
	if ($null -eq $reportedBytes) {
		$artifactFiles = @(Get-ChildItem -LiteralPath $ArtifactRoot -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -eq [string]$Artifact.name -or $_.BaseName -eq [string]$Artifact.artifactId })
		if ($artifactFiles.Count -gt 0) { $reportedBytes = [UInt64]($artifactFiles | Measure-Object -Property Length -Sum).Sum }
	}
	$profilerCapture.artifactBytes = if ($null -eq $reportedBytes) { $null } else { [UInt64]$reportedBytes }
	$profilerCapture.sizeGatePassed = $null -ne $reportedBytes -and [UInt64]$reportedBytes -le [UInt64]$profilerCapture.sizeGateBytes
	$summaryEngineDuration = Get-PropertyValue -Object $summaryData -Name "totalDurationMs"
	$profilerCapture.summaryEngineDurationMs = $summaryEngineDuration
	$observedDuration = if ($null -ne $profilerCaptureStartedAtMs -and $null -ne $profilerCaptureStoppedAtMs) {
		[double]$profilerCaptureStoppedAtMs - [double]$profilerCaptureStartedAtMs
	} else { $null }
	$profilerCapture.durationObservedMs = $observedDuration
	$profilerCapture.durationGatePassed = $null -ne $observedDuration -and [double]$observedDuration -ge ($CaptureDurationSeconds * 1000.0)
	$profilerCapture.formatAccepted = [string]$reportedFormat -eq 'TerraEngineProfilerJournalV1'
	$streamObject = Get-PropertyValue -Object $summaryData -Name "streams"
	if ($null -eq $streamObject) { $streamObject = Get-PropertyValue -Object $summaryData -Name "timingStreams" }
	$streamNames = @()
	if ($null -ne $streamObject) { $streamNames = @($streamObject | ForEach-Object { [string](Get-PropertyValue -Object $_ -Name "id") }) }
	$stageTimings = @(Get-PropertyValue -Object $summaryData -Name "stageTimings")
	$boundaryTimings = @(Get-PropertyValue -Object $summaryData -Name "protectedBoundaryTimings")
	$presentationTimings = @(Get-PropertyValue -Object $summaryData -Name "presentationWaitTimings")
	$responsivenessTimings = @(Get-PropertyValue -Object $summaryData -Name "responsivenessTimings")
	$workerHistories = @(Get-PropertyValue -Object $summaryData -Name "workerHistories")
	$workVolumes = Get-PropertyValue -Object $summaryData -Name "workVolumes"
	$stageIds = @($stageTimings | ForEach-Object { [string](Get-PropertyValue -Object $_ -Name "id") })
	$boundaryIds = @($boundaryTimings | ForEach-Object { [string](Get-PropertyValue -Object $_ -Name "id") })
	$presentationIds = @($presentationTimings | ForEach-Object { [string](Get-PropertyValue -Object $_ -Name "id") })
	$responsivenessIds = @($responsivenessTimings | ForEach-Object { [string](Get-PropertyValue -Object $_ -Name "id") })
	$metricAvailability = [ordered]@{
		physics = @($stageIds + $streamNames | Where-Object { $_ -like "physics.*" }).Count -gt 0
		sceneView = @($stageIds + $streamNames | Where-Object { $_ -like "scene_view.*" }).Count -gt 0
		scheduler = $workerHistories.Count -gt 0 -or @($streamNames | Where-Object { $_ -like "scheduler.*" }).Count -gt 0
		protectedBoundary = $boundaryIds.Count -gt 0
		responsiveness = $responsivenessIds.Count -gt 0
		presentation = $presentationIds.Count -gt 0
		workVolume = $null -ne $workVolumes -and [UInt64](Get-PropertyValue -Object $workVolumes -Name "measuredTickCount") -gt 0
	}
	$allEvidenceIds = @($streamNames + $stageIds + $boundaryIds + $presentationIds + $responsivenessIds)
	$profilerCapture.metricAvailability = [ordered]@{ streams = $metricAvailability; gatePassed = (@($metricAvailability.GetEnumerator() | Where-Object { -not $_.Value }).Count -eq 0); streamIds = $allEvidenceIds }
	if ($metricAvailability.physics) { [void]$unavailableMetrics.Remove("nativePhysicsStagePercentiles") }
	if ($metricAvailability.scheduler) { [void]$unavailableMetrics.Remove("workerUtilizationHistory") }
	if ($metricAvailability.protectedBoundary) { [void]$unavailableMetrics.Remove("protectedBoundaryStallHistory") }
	if ($metricAvailability.responsiveness) { [void]$unavailableMetrics.Remove("frameResponsiveness") }
	$profilerCapture.recoveryProbe = [ordered]@{ attempted = $false; supported = $false; status = "not-requested"; reason = "The public load operation is exercised by the orchestrator's truncated-tail fixture probe." }
	$baseline = Get-PropertyValue -Object $summaryData -Name "baseline"
	$profilerCapture.comparableBaseline = [ordered]@{
		captureLevel = $CaptureLevel; requestedFormat = 'TerraEngineProfilerJournalV1'
		frameCount = Get-PropertyValue -Object $summaryData -Name "frameCount"
		completeFrameCount = Get-PropertyValue -Object $summaryData -Name "completeFrameCount"
		incompleteFrameCount = Get-PropertyValue -Object $summaryData -Name "incompleteFrameCount"
		baselineReference = "Tests/Fixtures/Performance/physics-baseline-summary.json"
		baselineFrameP95Milliseconds = if ($null -ne $baseline) { Get-PropertyValue -Object $baseline -Name "frameP95Milliseconds" } else { $null }
	}
	$profilerCapture.compatibility = if ($profilerCapture.formatAccepted) { "accepted" } else { "unsupported" }
}

function Get-ToolEnvelope {
	param([Parameter(Mandatory = $true)] $RpcResponse, [Parameter(Mandatory = $true)][string] $Operation)
	if ($null -eq $RpcResponse.result -or $null -eq $RpcResponse.result.content -or $RpcResponse.result.content.Count -lt 1) {
		throw "MCP '$Operation' returned no text result envelope."
	}
	$envelope = $RpcResponse.result.content[0].text | ConvertFrom-Json
	if (-not $envelope.ok) {
		# Strict mode rejects direct reads of optional JSON properties. Inspect the
		# property collection so older Windows PowerShell can report either shape.
		$errorProperty = $envelope.PSObject.Properties["error"]
		$messageProperty = $envelope.PSObject.Properties["message"]
		$errorText = if ($null -ne $errorProperty -and $null -ne $errorProperty.Value) {
			$errorProperty.Value | ConvertTo-Json -Compress
		} elseif ($null -ne $messageProperty) {
			$messageProperty.Value
		} else {
			"unknown automation failure"
		}
		throw "MCP '$Operation' failed: $errorText"
	}
	return $envelope
}

function Get-PhysicsSnapshot {
	param([Parameter(Mandatory = $true)] $Envelope)
	# Physics capability handlers place their snapshot under the normal automation
	# result's data member. Keep this helper explicit so a contract change fails
	# loudly instead of silently treating an empty response as healthy.
	if ($null -eq $Envelope.data -or $null -eq $Envelope.data.tick -or $null -eq $Envelope.data.data) {
		throw "The physics MCP response did not contain the required immutable snapshot fields."
	}
	return $Envelope.data
}

function Invoke-WorkerScalingMatrix {
	# Each child owns one editor process and one disposable project.  Keeping
	# scenarios isolated makes timings and hashes comparable and repeatable.
	$matrixRoot = Get-AbsolutePath -Path $WorkRoot -BasePath $repoRoot
	Assert-PathBelow -Parent $outRoot -Child $matrixRoot
	New-Item -ItemType Directory -Force -Path $matrixRoot | Out-Null
	$records = [System.Collections.Generic.List[object]]::new()
	# Keep the large leg representative of a genuinely parallel physics load while
	# retaining 1,000 kinematic bodies and at least one static body in the fixture.
	$largeActiveCount = [Math]::Min(8000, [Math]::Max(1, $ObjectCount - 1001))
	foreach ($scenario in @("paused", "unchanged", "small", "large")) {
		foreach ($workers in @("1", "2", "4", "max")) {
			$childRoot = Join-Path $matrixRoot ("{0}-workers-{1}" -f $scenario, $workers)
			$arguments = @(
				"-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $PSCommandPath,
				"-InstallRoot", $InstallRoot, "-WorkRoot", $childRoot,
				"-WorkerCount", $workers, "-WorkerScalingScenario", $scenario,
				"-ParallelPreparationThreshold", $ParallelPreparationThreshold,
				"-ParallelExtractionThreshold", $ParallelExtractionThreshold,
				"-VelocityIterations", $VelocityIterations,
				"-PositionIterations", $PositionIterations,
				"-ObjectCount", $(if ($scenario -eq "small") { 256 } else { $ObjectCount }),
				"-ActiveDynamicCount", $(if ($scenario -eq "small") { 64 } elseif ($scenario -eq "large") { $largeActiveCount } else { $ActiveDynamicCount }),
				"-AllowLiveStress"
			)
			# Only the 8,000-active-body leg has a canonical worker-scaling fixture.
			# Idle and small legs remain diagnostic observations, never worker-scaling input.
			if ($scenario -eq "large") {
				$arguments += @("-CanonicalFixture", "physics-worker-large-v1")
			} else {
				$arguments += "-AllowDiagnosticFixture"
			}
			$child = & powershell.exe @arguments
			$childExitCode = $LASTEXITCODE
			$summaryFile = Join-Path $childRoot "evidence/summary.json"
			if (Test-Path -LiteralPath $summaryFile) {
				$summary = Get-Content -LiteralPath $summaryFile -Raw | ConvertFrom-Json
				$records.Add([ordered]@{ scenario = $scenario; requestedWorkerCount = $workers; success = ([bool]$summary.success -and $childExitCode -eq 0); childExitCode = $childExitCode; summaryPath = $summaryFile; summary = $summary })
			} else {
				$records.Add([ordered]@{ scenario = $scenario; requestedWorkerCount = $workers; success = $false; childExitCode = $childExitCode; summaryPath = $summaryFile; error = "child summary was not produced" })
			}
		}
	}
	$successful = @($records | Where-Object { $_.success })
	function Get-ScalingMetricMs {
		param([Parameter(Mandatory = $true)] $Record)
		# The worker-scaling gate evaluates provider work. Use the engine-measured simulation
		# stage, matching the schema comparator, while the report retains every other
		# stage for mixed-world total-tick and per-stage regression analysis.
		$simulation = @($Record.summary.physicsStageSamples | ForEach-Object { [double]$_.stages.simulation })
		if ($simulation.Count -gt 0) { return Get-Percentile -Values ([double[]]$simulation) -Percentile 50 }
		# Paused/unchanged legs have no measured tick. Their recommendation is only
		# informational, so retain the existing end-to-end duration as a fallback.
		return [double]$Record.summary.monotonicDurationMs
	}
	$determinismByScenario = [ordered]@{}
	foreach ($scenarioName in @("paused", "unchanged", "small", "large")) {
		$scenarioHashes = @($successful | Where-Object { $_.scenario -eq $scenarioName } | ForEach-Object { [string]$_.summary.determinismHash } | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique)
		$determinismByScenario[$scenarioName] = [ordered]@{ hashes = $scenarioHashes; identical = ($scenarioHashes.Count -le 1 -and $scenarioHashes.Count -gt 0) }
	}
	$recommendationsByScenario = [ordered]@{}
	foreach ($scenarioName in @("paused", "unchanged", "small", "large")) {
		$fastest = $successful | Where-Object { $_.scenario -eq $scenarioName } | Sort-Object { Get-ScalingMetricMs -Record $_ } | Select-Object -First 1
		$recommendationsByScenario[$scenarioName] = if ($null -eq $fastest) { $null } else { [ordered]@{
			requestedWorkerCount = $fastest.requestedWorkerCount; resolvedWorkerCount = $fastest.summary.actualWorkerCount
			durationMs = Get-ScalingMetricMs -Record $fastest; reason = "fastest successful correctness-gated median physics-stage result for measured workloads"
		} }
	}
	# The large workload is the production stress recommendation. Per-scenario
	# results remain available so small/idle overhead is not compared as equivalent
	# work and cannot accidentally choose the global worker default.
	$recommendation = $successful | Where-Object { $_.scenario -eq "large" } | Sort-Object { Get-ScalingMetricMs -Record $_ } | Select-Object -First 1
	$largeOne = $successful | Where-Object { $_.scenario -eq "large" -and $_.requestedWorkerCount -eq "1" } | Select-Object -First 1
	$largeFour = $successful | Where-Object { $_.scenario -eq "large" -and $_.requestedWorkerCount -eq "4" } | Select-Object -First 1
	$largeMax = $successful | Where-Object { $_.scenario -eq "large" -and $_.requestedWorkerCount -eq "max" } | Select-Object -First 1
	$largeRecords = @($largeOne, $largeFour, $largeMax)
	$largeResolutionChecks = @($largeRecords | Where-Object { $null -ne $_ } | ForEach-Object { Get-WorkerResolutionCheck -Record $_ })
	$largeExactWorkerCounts = $largeResolutionChecks.Count -eq 3 -and @($largeResolutionChecks | Where-Object { -not $_.available -or -not $_.exact }).Count -eq 0
	$workerScalingGate = [ordered]@{
		available = ($null -ne $largeOne -and $null -ne $largeFour -and $null -ne $largeMax -and $largeExactWorkerCounts)
		metric = "medianSimulationStageMs"
		exactWorkerResolution = $largeExactWorkerCounts
		workerResolutionChecks = $largeResolutionChecks
		oneWorkerDurationMs = if($null -eq $largeOne){$null}else{Get-ScalingMetricMs -Record $largeOne}
		fourWorkerDurationMs = if($null -eq $largeFour){$null}else{Get-ScalingMetricMs -Record $largeFour}
		maximumWorkerDurationMs = if($null -eq $largeMax){$null}else{Get-ScalingMetricMs -Record $largeMax}
		speedupFourVsOne = $null
		maximumVsFourRatio = $null
		passed = $false
	}
	if($workerScalingGate.available -and $workerScalingGate.fourWorkerDurationMs -gt 0.0) {
		$workerScalingGate.speedupFourVsOne = $workerScalingGate.oneWorkerDurationMs / $workerScalingGate.fourWorkerDurationMs
		$workerScalingGate.maximumVsFourRatio = $workerScalingGate.maximumWorkerDurationMs / $workerScalingGate.fourWorkerDurationMs
		$workerScalingGate.passed = $workerScalingGate.speedupFourVsOne -ge 1.5 -and $workerScalingGate.maximumVsFourRatio -le 1.10
	}
	$report = [ordered]@{
		version = 1; feature = "worker-scaling"; thresholdOverride = [ordered]@{ parallelPreparation = $ParallelPreparationThreshold; parallelExtraction = $ParallelExtractionThreshold }
		scenarios = @("paused", "unchanged", "small", "large"); workerCounts = @("1", "2", "4", "max")
		deterministic = ($successful.Count -eq 16 -and @($determinismByScenario.Values | Where-Object { -not $_.identical }).Count -eq 0)
		determinismByScenario = $determinismByScenario; scalingComparable = ($successful.Count -eq 16 -and $largeExactWorkerCounts)
		workerScalingGate = $workerScalingGate
		recommendationsByScenario = $recommendationsByScenario
		recommended = if ($null -eq $recommendation) { $null } else { [ordered]@{ scenario = $recommendation.scenario; requestedWorkerCount = $recommendation.requestedWorkerCount; resolvedWorkerCount = $recommendation.summary.actualWorkerCount; durationMs = Get-ScalingMetricMs -Record $recommendation; reason = "fastest successful correctness-gated median physics-stage large-workload result" } }
		runs = @($records)
	}
	$reportPath = Join-Path $matrixRoot "worker-scaling-matrix-summary.json"
	Write-JsonFile -Path $reportPath -Value $report -Depth 64
	Write-Output ("worker-scaling matrix report: {0}" -f $reportPath)
	if (-not $report.deterministic -or -not $report.scalingComparable -or -not $report.workerScalingGate.passed) {
		throw "worker-scaling matrix failed determinism, completeness, or the required 1.5x / 10% limits."
	}
	exit 0
}

function Invoke-QualityProfileMatrix {
	# Profiles are separate processes because the project settings are loaded at
	# editor startup. Reusing one process would not prove that either profile was
	# applied by the provider rather than merely requested by the harness.
	$matrixRoot = Get-AbsolutePath -Path $WorkRoot -BasePath $repoRoot
	Assert-PathBelow -Parent $outRoot -Child $matrixRoot
	New-Item -ItemType Directory -Force -Path $matrixRoot | Out-Null
	$records = [System.Collections.Generic.List[object]]::new()
	foreach ($profile in @(
		[ordered]@{ id = "default"; velocityIterations = 10; positionIterations = 2 },
		[ordered]@{ id = "alternate"; velocityIterations = 20; positionIterations = 4 }
	)) {
		$childRoot = Join-Path $matrixRoot ("quality-" + $profile.id)
		$arguments = @(
			"-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $PSCommandPath,
			"-InstallRoot", $InstallRoot, "-WorkRoot", $childRoot,
			"-CanonicalFixture", $CanonicalFixture, "-ObjectCount", $ObjectCount,
			"-ActiveDynamicCount", $ActiveDynamicCount, "-ExplicitStepCount", $ExplicitStepCount,
			"-WorkerCount", $WorkerCount, "-WorkerScalingScenario", $WorkerScalingScenario,
			"-ParallelPreparationThreshold", $ParallelPreparationThreshold,
			"-ParallelExtractionThreshold", $ParallelExtractionThreshold,
			"-VelocityIterations", $profile.velocityIterations,
			"-PositionIterations", $profile.positionIterations, "-AllowLiveStress"
		)
		& powershell.exe @arguments
		$childExitCode = $LASTEXITCODE
		$summaryPath = Join-Path $childRoot "evidence/summary.json"
		if (Test-Path -LiteralPath $summaryPath) {
			$summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
			$quality = Get-QualityEvidenceCheck -Summary $summary -ExpectedVelocityIterations $profile.velocityIterations -ExpectedPositionIterations $profile.positionIterations
			$records.Add([ordered]@{ profile = $profile; success = ([bool]$summary.success -and $childExitCode -eq 0 -and $quality.applied); childExitCode = $childExitCode; quality = $quality; summaryPath = $summaryPath; summary = $summary })
		} else {
			$records.Add([ordered]@{ profile = $profile; success = $false; childExitCode = $childExitCode; quality = [ordered]@{ available = $false; applied = $false; reason = "child summary was not produced" }; summaryPath = $summaryPath })
		}
	}
	$report = [ordered]@{
		version = 1; feature = "quality-profile"; canonicalFixture = $CanonicalFixture
		profiles = @($records | ForEach-Object { $_.profile }); complete = (@($records | Where-Object { $_.success }).Count -eq 2)
		# Quality performance is informative here. Correctness requires each leg to
		# report the requested values on every measured provider diagnostics sample.
		qualityGate = [ordered]@{ available = (@($records | Where-Object { $_.quality.available }).Count -eq 2); passed = (@($records | Where-Object { $_.success }).Count -eq 2) }
		runs = @($records)
	}
	$reportPath = Join-Path $matrixRoot "quality-profile-matrix-summary.json"
	Write-JsonFile -Path $reportPath -Value $report -Depth 64
	Write-Output ("quality profile matrix report: {0}" -f $reportPath)
	if (-not $report.complete) { throw "quality profile matrix did not produce two successful runtime-verified profile legs." }
	exit 0
}

function Invoke-RepeatedAcceptanceMatrix {
	# Preserve every child result, including failures. A repeated gate must not
	# average away a timeout, forced shutdown, or missing evidence file.
	$matrixRoot = Get-AbsolutePath -Path $WorkRoot -BasePath $repoRoot
	Assert-PathBelow -Parent $outRoot -Child $matrixRoot
	New-Item -ItemType Directory -Force -Path $matrixRoot | Out-Null
	$records = [System.Collections.Generic.List[object]]::new()
	for ($attempt = 1; $attempt -le 10; ++$attempt) {
		$childRoot = Join-Path $matrixRoot ("acceptance-run-{0:d2}" -f $attempt)
		$arguments = @(
			"-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $PSCommandPath,
			"-InstallRoot", $InstallRoot, "-WorkRoot", $childRoot,
			"-CanonicalFixture", $CanonicalFixture, "-ObjectCount", $ObjectCount,
			"-ActiveDynamicCount", $ActiveDynamicCount, "-ExplicitStepCount", $ExplicitStepCount,
			"-WorkerCount", $WorkerCount, "-WorkerScalingScenario", $WorkerScalingScenario,
			"-ParallelPreparationThreshold", $ParallelPreparationThreshold,
			"-ParallelExtractionThreshold", $ParallelExtractionThreshold,
			"-VelocityIterations", $VelocityIterations, "-PositionIterations", $PositionIterations,
			"-AllowLiveStress"
		)
		& powershell.exe @arguments
		$childExitCode = $LASTEXITCODE
		$summaryPath = Join-Path $childRoot "evidence/summary.json"
		if (Test-Path -LiteralPath $summaryPath) {
			$summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
			$quality = Get-QualityEvidenceCheck -Summary $summary -ExpectedVelocityIterations $VelocityIterations -ExpectedPositionIterations $PositionIterations
			$accepted = [bool]$summary.success -and $childExitCode -eq 0 -and -not [bool]$summary.forcedTermination -and $quality.applied
			$records.Add([ordered]@{ attempt = $attempt; accepted = $accepted; childExitCode = $childExitCode; summaryPath = $summaryPath; forcedTermination = [bool]$summary.forcedTermination; quality = $quality; error = Get-PropertyValue -Object $summary -Name "error"; summary = $summary })
		} else {
			$records.Add([ordered]@{ attempt = $attempt; accepted = $false; childExitCode = $childExitCode; summaryPath = $summaryPath; forcedTermination = $null; quality = [ordered]@{ available = $false; applied = $false; reason = "child summary was not produced" }; error = "child summary was not produced" })
		}
	}
	$failures = @($records | Where-Object { -not $_.accepted } | ForEach-Object {
		if (-not [string]::IsNullOrWhiteSpace([string]$_.error)) { "attempt $($_.attempt): $($_.error)" } else { "attempt $($_.attempt): acceptance evidence was incomplete" }
	})
	# The published evidence schema requires independent state, event, and pose
	# hashes. This public MCP harness currently has only its combined determinism
	# digest, so it deliberately writes an explicit non-schema status rather than
	# inventing three values. A future bridge can add those fields without changing
	# the ten-run collection or failure accounting below.
	$schemaCapabilities = [ordered]@{ stateHash = $false; eventHash = $false; poseHash = $false; supported = $false; reason = "installed public bridge does not expose independent state, event, and pose hashes" }
	$report = [ordered]@{
		version = 1; feature = "repeated-acceptance"; canonicalFixture = $CanonicalFixture
		requiredRuns = 10; completedRuns = $records.Count; acceptedRuns = @($records | Where-Object { $_.accepted }).Count
		passed = ($failures.Count -eq 0 -and $records.Count -eq 10); schemaValidAggregateAvailable = $false; schemaCapabilities = $schemaCapabilities
		failures = $failures; runs = @($records)
	}
	$reportPath = Join-Path $matrixRoot "physics-performance-aggregate-status.json"
	Write-JsonFile -Path $reportPath -Value $report -Depth 64
	Write-Output ("repeated acceptance aggregate status: {0}" -f $reportPath)
	if (-not $report.passed -or -not $report.schemaValidAggregateAvailable) {
		throw "repeated acceptance did not produce a schema-valid aggregate; inspect '$reportPath'."
	}
	exit 0
}

function Invoke-ReadinessMatrix {
	# One reusable project is intentional here: cold creates regenerated outputs,
	# warm proves they are reusable, and later cases perturb only the documented
	# disposable cache or source-copy boundary. Each child exits normally before
	# the next child starts, so no process shares live editor state.
	$matrixRoot = Get-AbsolutePath -Path $WorkRoot -BasePath $repoRoot
	Assert-PathBelow -Parent $outRoot -Child $matrixRoot
	if (Test-Path -LiteralPath $matrixRoot) { Remove-Item -LiteralPath $matrixRoot -Recurse -Force }
	New-Item -ItemType Directory -Force -Path $matrixRoot | Out-Null
	$records = [System.Collections.Generic.List[object]]::new()
	foreach ($mode in @("cold", "warm", "stale", "corrupt", "forced-build")) {
		$arguments = @(
			"-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $PSCommandPath,
			"-InstallRoot", $InstallRoot, "-WorkRoot", $matrixRoot,
			"-ReadinessMode", $mode,
			"-ObjectCount", $ObjectCount, "-ActiveDynamicCount", $ActiveDynamicCount,
			"-ExplicitStepCount", $ExplicitStepCount,
			"-WorkerCount", $WorkerCount,
			"-ParallelPreparationThreshold", $ParallelPreparationThreshold,
			"-ParallelExtractionThreshold", $ParallelExtractionThreshold,
			"-AllowLiveStress"
		)
		& powershell.exe @arguments
		$summaryFile = Join-Path $matrixRoot ("evidence/readiness-{0}-summary.json" -f $mode)
		if (Test-Path -LiteralPath $summaryFile) {
			$summary = Get-Content -LiteralPath $summaryFile -Raw | ConvertFrom-Json
			# Readiness evidence is accepted only when the editor completed its own
			# shutdown lifecycle. This prevents a force-stopped child from looking like
			# a successful cache/readiness scenario merely because earlier checks passed.
			$scenarioComplete = [bool]$summary.success -and [bool]$summary.readiness.normalShutdown
			$records.Add([ordered]@{ mode = $mode; success = $scenarioComplete; summaryPath = $summaryFile; summary = $summary })
		} else {
			$records.Add([ordered]@{ mode = $mode; success = $false; summaryPath = $summaryFile; error = "scenario summary was not produced" })
		}
	}
	$successful = @($records | Where-Object { $_.success })
	$report = [ordered]@{
		version = 1; feature = "readiness"; modes = @("cold", "warm", "stale", "corrupt", "forced-build")
		# A successful scenario writes its own source/cache/readiness evidence. The
		# matrix deliberately does not reinterpret provider-specific reason strings.
		complete = ($successful.Count -eq 5)
		runs = @($records)
	}
	$reportPath = Join-Path $matrixRoot "readiness-readiness-matrix-summary.json"
	Write-JsonFile -Path $reportPath -Value $report -Depth 64
	Write-Output ("readiness readiness matrix report: {0}" -f $reportPath)
	if (-not $report.complete) { throw "readiness readiness matrix did not produce five successful scenario summaries." }
	exit 0
}

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../.."))
$outRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot "out"))
$resolvedInstallRoot = Get-AbsolutePath -Path $InstallRoot -BasePath $repoRoot
$resolvedWorkRoot = Get-AbsolutePath -Path $WorkRoot -BasePath $repoRoot
Assert-PathBelow -Parent $outRoot -Child $resolvedWorkRoot

$matrixFlags = @([bool]$RunWorkerScalingMatrix, [bool]$RunReadinessMatrix, [bool]$RunQualityProfileMatrix, [bool]$RunRepeatedAcceptanceMatrix)
if (@($matrixFlags | Where-Object { $_ }).Count -gt 1) {
	throw "Run one matrix at a time so every child owns an unambiguous disposable project and evidence root."
}
if ($RunWorkerScalingMatrix) {
	Invoke-WorkerScalingMatrix
}
if ($RunReadinessMatrix) {
	Invoke-ReadinessMatrix
}
if ($RunQualityProfileMatrix) {
	Invoke-QualityProfileMatrix
}
if ($RunRepeatedAcceptanceMatrix) {
	Invoke-RepeatedAcceptanceMatrix
}

$editorDirectory = Join-Path $resolvedInstallRoot "Editor"
$editorExecutable = Join-Path $editorDirectory "TerraEngine-Editor.exe"
if (-not (Test-Path -LiteralPath $editorExecutable -PathType Leaf)) {
	throw "Installed editor executable was not found: '$editorExecutable'. Run the CMake install target first."
}
if (-not (Test-LoopbackPortAvailable -Port $McpPort)) {
	throw "MCP port $McpPort is already occupied. Choose a free explicit -McpPort; the editor intentionally does not randomize this endpoint."
}

if ((Test-Path -LiteralPath $resolvedWorkRoot) -and $ReadinessMode -eq "none") {
	# The path-below-out check above makes this recursive cleanup safe. The stress
	# project is disposable, including its generated 10,000-object scene.
	Remove-Item -LiteralPath $resolvedWorkRoot -Recurse -Force
}

$projectRoot = Join-Path $resolvedWorkRoot "project"
$assetsRoot = Join-Path $projectRoot "Assets"
$scenePath = Join-Path $assetsRoot "Scenes/PhysicsStress.tescene"
$preferencesPath = Join-Path $resolvedWorkRoot "editor-preferences.json"
$evidenceRoot = Join-Path $resolvedWorkRoot "evidence"
$rpcLogPath = Join-Path $evidenceRoot "mcp.jsonl"
$summaryPath = Join-Path $evidenceRoot "summary.json"
$canonicalFixtureEvidence = Get-CanonicalFixtureEvidence -FixtureId $CanonicalFixture
if (-not $AllowDiagnosticFixture) {
	# Produce an inspectable unavailable record before failing, so callers cannot
	# mistake an absent file for a passing canonical fixture.
	if ($canonicalFixtureEvidence.availability -ne "available") {
		New-Item -ItemType Directory -Force -Path $evidenceRoot | Out-Null
		Write-JsonFile -Path $summaryPath -Value ([ordered]@{
			success = $false; canonicalFixture = $canonicalFixtureEvidence; forcedTermination = $false
			error = "Canonical fixture '$CanonicalFixture' is unavailable: $($canonicalFixtureEvidence.availabilityReason)"
		}) -Depth 16
		throw "Canonical fixture '$CanonicalFixture' is unavailable: $($canonicalFixtureEvidence.availabilityReason)"
	}
	Assert-CanonicalFixtureArguments -Fixture $canonicalFixtureEvidence
} else {
	# Diagnostic-only matrix legs retain the requested fixture identity but cannot
	# be used by an acceptance gate when their population differs from the manifest.
	$canonicalFixtureEvidence.availability = "unavailable"
	$canonicalFixtureEvidence.availabilityReason = "diagnostic worker-scaling leg does not match a canonical fixture population or measured-step contract"
}
$projectSettingsPath = Join-Path $projectRoot "ProjectSettings/ProjectSettings.terra"
$physicsSettingsPath = Join-Path $projectRoot "ProjectSettings/PhysicsSettings.json"
$lastScenePath = Join-Path $projectRoot "Library/LastScene.txt"
$editorLayoutIniPath = Join-Path $projectRoot "UserSettings/EditorLayout.ini"
$editorLayoutStatePath = Join-Path $projectRoot "UserSettings/EditorLayout.terra"
$derivedScenesRoot = Join-Path $projectRoot "Library/DerivedScenes"
$managedOutputRoot = Join-Path $projectRoot "Build/Assembly-CSharp"
$managedReadinessManifestPath = Join-Path $managedOutputRoot "ManagedReadiness.manifest.json"
$projectAlreadyExists = Test-Path -LiteralPath $scenePath -PathType Leaf
$readinessScenarioSummaryPath = if ($ReadinessMode -eq "none") { $null } else { Join-Path $evidenceRoot ("readiness-{0}-summary.json" -f $ReadinessMode) }
$startTime = Get-Date
$monotonicFrequency = [double][System.Diagnostics.Stopwatch]::Frequency
$monotonicStartTicks = [System.Diagnostics.Stopwatch]::GetTimestamp()
$stageStarts = @{}
$stageRecords = [ordered]@{}
$latencySamples = [System.Collections.Generic.List[object]]::new()
$diagnosticSnapshots = [System.Collections.Generic.List[object]]::new()
$physicsStageSamples = [System.Collections.Generic.List[object]]::new()
$qualityEvidence = [ordered]@{
	requested = [ordered]@{ velocityIterations = $VelocityIterations; positionIterations = $PositionIterations }
	available = $false; applied = $false; verifiedSteps = 0; reason = "measured diagnostics have not been collected"
}
$processMemorySamples = [System.Collections.Generic.List[object]]::new()
$profilerCaptureStart = $null
$profilerCaptureStop = $null
$profilerCaptureJob = $null
$profilerCaptureArtifact = $null
$profilerCaptureSummary = $null
$profilerCaptureSucceeded = $false
$profilerCaptureStartedAtMs = $null
$profilerCaptureStoppedAtMs = $null
$profilerCaptureGateError = $null
$profilerCapture = [ordered]@{
	requested = [bool]$RunProfilerCaptureChecks; captureLevel = $CaptureLevel
	requestedFormat = 'TerraEngineProfilerJournalV1'; durationSeconds = $CaptureDurationSeconds
	sizeGateBytes = 10MB; sizeGatePassed = $null; artifactBytes = $null
	formatAccepted = $null; recoveryProbe = $null; metricAvailability = $null
	comparableBaseline = $null; compatibility = "unknown"
}
$authoringWalkthrough = [ordered]@{ requested = [bool]$RunAuthoringWalkthrough; completed = $false }
$readiness = [ordered]@{
	requested = ($ReadinessMode -ne "none"); mode = $ReadinessMode
	projectReused = $projectAlreadyExists; preparation = $null; startup = $null
	forcedCompile = $null; final = $null; sourceAuthority = $null
}
# Paused and unchanged probes deliberately issue no Next request after the editor
# reaches its protected paused boundary; active scenarios retain the requested loop.
$measuredExplicitStepCount = if ($WorkerScalingScenario -eq "paused" -or $WorkerScalingScenario -eq "unchanged") { 0 } else { $ExplicitStepCount }
$colliderCookMatrix = [ordered]@{
	requested = [bool]$RunColliderCookIncrementalMatrix; completed = $false
	reparent = $null; conflict = $null; stale = $null; unchanged = $null; active = $null
}
$workerScalingScenarioEvidence = [ordered]@{ scenario = $WorkerScalingScenario; paused = ($WorkerScalingScenario -eq "paused"); unchanged = $false; fromTick = $null; toTick = $null; nextRequests = $measuredExplicitStepCount; synchronization = $null }
$sceneViewEvidence = [ordered]@{ scenario = $SceneViewScenario; configured = $null; effective = $null; state = $null }
$managedReloadAutomationLoopback = [ordered]@{
	requested = [bool]$RunManagedReloadAutomationLoopback; completed = $false
	managedReload = $null; runtimeDiscovery = $null; customGravity = $null; events = $null
}
$managedReloadAutomationEvidenceCompleteness = [ordered]@{
	requested = [bool]$RunManagedReloadAutomationLoopback; complete = $false; checks = $null
}
$unavailableMetrics = [System.Collections.Generic.List[string]]::new()
$unavailableMetrics.Add("nativePhysicsStagePercentiles")
$unavailableMetrics.Add("workerUtilizationHistory")
$unavailableMetrics.Add("protectedBoundaryStallHistory")
$unavailableMetrics.Add("frameResponsiveness")
$unavailableMetrics.Add("profilerCaptureUnlessGeneratedByEditor")
$profilerCaptureRequiredMetrics = @("physics", "sceneView", "scheduler", "protectedBoundary", "responsiveness", "presentation", "workVolume")
$lastDiagnosticsCaptureMs = -5000.0
$hardwareEvidence = Get-HardwareEvidence
$resolvedWorkerCount = if ($WorkerCount -eq "max" -or $WorkerCount -eq "0") { [int][Math]::Max(1, [Environment]::ProcessorCount) } else { [int]$WorkerCount }
$workerIdentity = [ordered]@{
	requested = $WorkerCount; resolved = $resolvedWorkerCount
	mode = if ($WorkerCount -eq "max" -or $WorkerCount -eq "0") { "hardware-concurrency" } else { "explicit-override" }
	environmentOverride = if ($WorkerCount -eq "max" -or $WorkerCount -eq "0") { $null } else { $WorkerCount }
}
$determinismHash = $null
$process = $null
$forcedTermination = $false
$runSucceeded = $false
$rpcId = 0

try {
	if ($RunManagedReloadAutomationLoopback -and -not $RunColliderCookIncrementalMatrix) {
		throw "managed-reload-automation evidence completeness requires -RunColliderCookIncrementalMatrix so rollback and protected-boundary visibility are proven in the same run."
	}
	Start-Stage -Name "projectAndSceneGeneration"
	New-Item -ItemType Directory -Force -Path (Split-Path -Parent $scenePath), (Split-Path -Parent $projectSettingsPath), (Split-Path -Parent $lastScenePath), (Split-Path -Parent $editorLayoutIniPath), $evidenceRoot | Out-Null
	if ($ReadinessMode -ne "none" -and -not $projectAlreadyExists -and $ReadinessMode -ne "cold") {
		throw "readiness '$ReadinessMode' requires the disposable source project created by the preceding cold scenario."
	}

	# These are the exact editor-wide preference keys consumed by ProjectManager.
	# Supplying them via TERRAENGINE_EDITOR_PREFS_SESSION_PATH prevents this live run from
	# changing the user's normal MCP enablement, port, or recent-project history.
	# Seed the complete Scene View document before process startup. Runtime writes
	# correctly require an interactive high-impact approval, while this disposable
	# file is already isolated below the validated work root and cannot affect the
	# developer's normal preferences.
	if (-not $projectAlreadyExists) {
	$sceneViewSettings = [ordered]@{
		largeSceneMode = if ($SceneViewScenario -eq "dense") { "on" } else { "auto" }
		largeSceneObjectThreshold = 2000
		visibleMarkerBudget = if ($SceneViewScenario -eq "dense") { 256 } else { 2048 }
		markerPresentationBatchSize = if ($SceneViewScenario -eq "dense") { 64 } else { 512 }
		densityRadiusPixels = if ($SceneViewScenario -eq "dense") { 64.0 } else { 18.0 }
		densityThreshold = if ($SceneViewScenario -eq "dense") { 2 } else { 4 }
		maximumMarkerDistance = 0.0
		minimumProjectedSizePixels = 1.0
		physicsMarkerMode = "clustered"
		densePrimitiveTier = if ($SceneViewScenario -eq "dense") { "point" } else { "simple" }
	}
	Write-JsonFile -Path $preferencesPath -Value ([ordered]@{
		recentProjects = @()
		theme = "Dark"
		externalScriptEditorCommand = ""
		autoBuildScripts = $false
		editorMcpEnabled = $true
		editorMcpWriteModeDefault = "writes_enabled"
		editorMcpRecentSession = "installed-physics-stress"
		editorMcpListenPort = $McpPort
		vulkanValidationLayers = $false
		renderDebugDiagnostics = $false
		rendererDiagnosticsPreferencesInitialized = $true
		sceneViewOverlay = $sceneViewSettings
	})
	Write-JsonFile -Path $projectSettingsPath -Value ([ordered]@{ projectDisplayName = "Installed Physics Stress" })
	Write-JsonFile -Path (Join-Path $projectRoot "game.json") -Value ([ordered]@{ name = "Installed Physics Stress"; version = "1.0.0" })
	# Pin the startup scene instead of depending on filesystem enumeration order.
	# The editor resolves this path relative to the project's Assets directory.
	Set-Content -LiteralPath $lastScenePath -Value "Scenes/PhysicsStress.tescene" -Encoding utf8
	# Give Scene View a real authoring viewport. The general editor template keeps
	# many first-run windows visible and can leave Scene View only one toolbar tall,
	# which would profile an empty clip rectangle instead of marker preparation.
	$stressLayoutIni = @"
[Window][TerraEngine DockSpace]
Pos=0,0
Size=1280,720
Collapsed=0

[Window][Scene View]
Pos=20,45
Size=1240,650
Collapsed=0

[Docking][Data]
DockSpace ID=0x81567634 Window=0x5B0EFE5B Pos=0,43 Size=1280,677 CentralNode=1
"@
	Set-Content -LiteralPath $editorLayoutIniPath -Value $stressLayoutIni -Encoding utf8
	Write-JsonFile -Path $editorLayoutStatePath -Value ([ordered]@{
		windows = @(
			[ordered]@{ title = "Scene Hierarchy"; visible = $false },
			[ordered]@{ title = "Inspector"; visible = $false },
			[ordered]@{ title = "Project Browser"; visible = $false },
			[ordered]@{ title = "Game View"; visible = $false },
			[ordered]@{ title = "Scene View"; visible = $true },
			[ordered]@{ title = "Project Settings"; visible = $false },
			[ordered]@{ title = "Preferences"; visible = $false },
			[ordered]@{ title = "Build Profiles"; visible = $false },
			[ordered]@{ title = "Profiler"; visible = $false },
			[ordered]@{ title = "Physics Diagnostics"; visible = $false },
			[ordered]@{ title = "Scheduler Diagnostics"; visible = $false },
			[ordered]@{ title = "Resource Residency"; visible = $false },
			[ordered]@{ title = "UI Builder"; visible = $false },
			[ordered]@{ title = "Engine Status"; visible = $false; managed = $true }
		)
	})

	# The settings file uses the project-owned 32-layer schema. Defaults omitted
	# by the serializer remain deterministic, while gravity is explicitly present
	# so the run proves normal dynamic-body behaviour rather than a zero-gravity
	# fixture accidentally passing the tick checks.
	$layers = for ($layer = 0; $layer -lt 32; ++$layer) {
		[ordered]@{ id = $layer; name = if ($layer -eq 0) { "Default" } else { "Layer{0:d2}" -f $layer }; collisionMask = [UInt32]::MaxValue; broadPhaseGroup = 0; enabled = $true }
	}
	Write-JsonFile -Path $physicsSettingsPath -Value ([ordered]@{
		schemaVersion = 1
		gravity = @(0.0, -9.81, 0.0)
		fixedTimeStep = 0.016666667
		transformPublicationChunkSize = 1024
		providerExtractionBatchSize = 1024
		parallelPreparationThreshold = $ParallelPreparationThreshold
		parallelExtractionThreshold = $ParallelExtractionThreshold
		velocityIterations = $VelocityIterations
		positionIterations = $PositionIterations
		aggregateDiagnosticsIntervalFrames = 30
		continuousCollisionEnabled = $false
		debugVisualizationEnabled = [bool]$EnablePhysicsDebugVisualization
		retainedDebugTicks = 2
		maxDebugPrimitives = 100000
		layers = @($layers)
	})
	}

	$dynamicCount = $ActiveDynamicCount
	$kinematicCount = if ($ObjectCount -ge 3000) { 1000 } else { [int][Math]::Max(1, [Math]::Floor($ObjectCount * 0.1)) }
	if (($dynamicCount + $kinematicCount) -ge $ObjectCount) {
		throw "ActiveDynamicCount plus the scenario's kinematic coverage must leave at least one static body."
	}
	$staticCount = $ObjectCount - $dynamicCount - $kinematicCount
	# Keep one high dynamic body free of built-in gravity during the optional managed-reload-automation
	# run. A mass-independent acceleration command is then the only source of its
	# first downward velocity, making idempotent custom-gravity behavior observable.
	$managedReloadAutomationTargetIndex = [int][Math]::Min($dynamicCount - 1, 1950)
	$managedReloadAutomationTargetId = Get-StableGuid -Value ([UInt64]($managedReloadAutomationTargetIndex + 1))
	$managedReloadAutomationTargetPosition = $null
	$objects = [System.Collections.Generic.List[object]]::new()
	$emptyGuid = "00000000000000000000000000000000"
	for ($index = 0; $index -lt $ObjectCount; ++$index) {
		$isDynamic = $index -lt $dynamicCount
		$isKinematic = $index -ge $dynamicCount -and $index -lt ($dynamicCount + $kinematicCount)
		$isStaticBody = -not $isDynamic -and -not $isKinematic
		$column = $index % 100
		$row = [Math]::Floor($index / 100)
		$position = if ($isDynamic) {
			# Spread dynamic cubes over several heights so gravity, contacts, sleeping,
			# and broad-phase updates all contribute to the live workload.
			@([double](($column - 50) * 1.1), [double](3 + (($row % 20) * 1.05)), [double](([Math]::Floor($row / 20) - 4) * 1.1))
		} elseif ($isKinematic) {
			# Kinematic bodies exercise the authored third motion type without
			# receiving gravity. Their elevated grid also participates in broad-phase
			# overlap while remaining controlled by gameplay commands.
			@([double](($column - 50) * 1.3), [double](8 + (($row % 10) * 1.2)), [double](([Math]::Floor($row / 10) - 75) * 1.3))
		} else {
			# Static obstacles form a low grid around the falling-body volume. The
			# first static object is a wide floor, while the rest are small blockers.
			if ($index -eq ($dynamicCount + $kinematicCount)) { @(0.0, -1.0, 0.0) }
			else { @([double](($column - 50) * 1.5), [double](($row % 3) * 0.75), [double](([Math]::Floor($row / 3) - 4) * 1.5)) }
		}
		$halfExtents = if ($index -eq ($dynamicCount + $kinematicCount)) { @(70.0, 0.5, 70.0) } else { @(0.45, 0.45, 0.45) }
		$body = [ordered]@{
			active = $true; schemaVersion = 1; motionType = if ($isDynamic) { "Dynamic" } elseif ($isKinematic) { "Kinematic" } else { "Static" }
			massMode = "ExplicitMass"; mass = 1.0; linearDamping = 0.02; angularDamping = 0.05
			useGravity = $isDynamic -and -not ($RunManagedReloadAutomationLoopback -and $index -eq $managedReloadAutomationTargetIndex); allowSleeping = $true; startAwake = -not $isStaticBody; continuousCollisionMode = "Discrete"
			initialLinearVelocity = @(0.0, 0.0, 0.0); initialAngularVelocity = @(0.0, 0.0, 0.0)
			maxLinearVelocity = 500.0; maxAngularVelocity = 100.0; allowedDegreesOfFreedom = 63
			interpolationMode = "Interpolate"; collisionLayerId = 0; useCollisionMaskOverride = $false
			collisionMaskOverride = [UInt32]::MaxValue; enabled = $true
		}
		$collider = [ordered]@{
			recordId = Get-StableGuid -Value ([UInt64]($ObjectCount + $index + 1)); recordVersion = 1
			localCenter = @(0.0, 0.0, 0.0); localRotation = @(1.0, 0.0, 0.0, 0.0)
			shape = [ordered]@{ type = "Box"; halfExtents = $halfExtents }
			materialId = $emptyGuid; useDefaultMaterial = $true; isTrigger = $false; useLayerOverride = $false
			collisionLayerId = 0; eventPolicy = "All"; queryFlags = 15; enabled = $true; hasDebugColorOverride = $false
		}
		$objects.Add([ordered]@{
			uuid = Get-StableGuid -Value ([UInt64]($index + 1))
			name = if ($isDynamic) { "Stress Dynamic {0:d5}" -f $index } elseif ($isKinematic) { "Stress Kinematic {0:d5}" -f ($index - $dynamicCount) } else { "Stress Static {0:d5}" -f ($index - $dynamicCount - $kinematicCount) }
			tag = "Untagged"; layer = "Default"; active = $true; isStatic = $isStaticBody
			components = [ordered]@{
				"Transform" = [ordered]@{ Position = $position; Rotation = @(1.0, 0.0, 0.0, 0.0); Scale = @(1.0, 1.0, 1.0) }
				"RigidBodyComponent" = $body
				"ColliderSetComponent" = [ordered]@{ active = $true; schemaVersion = 1; colliders = @($collider) }
			}
			children = @()
		})
		if ($index -eq $managedReloadAutomationTargetIndex) { $managedReloadAutomationTargetPosition = @($position) }
	}
	$scene = [ordered]@{
		name = "Installed Physics Stress"
		sceneSettings = [ordered]@{ indirectStrength = 1.0; ambientOcclusionStrength = 1.0; ambientOcclusionQuality = 2 }
		objects = @($objects)
	}
	if (-not $projectAlreadyExists) {
		Write-JsonFile -Path $scenePath -Value $scene -Depth 32
	}
	# The readiness matrix never deletes authored input. Cold clears only
	# disposable derived/readiness output, stale changes the disposable source
	# copy by whitespace only, and corrupt damages exactly one derived artifact.
	if ($ReadinessMode -ne "none") {
		Start-Stage -Name "readinessScenarioPreparation"
		$sourceHashBeforePreparation = (Get-FileHash -LiteralPath $scenePath -Algorithm SHA256).Hash
		$preparedArtifacts = @(Get-ArtifactInventory -Root $projectRoot)
		$mutation = "none"
		switch ($ReadinessMode) {
			"cold" {
				# The manifest is cache metadata, not project authority. Removing it
				# requests a real cold managed-readiness validation without deleting a
				# source file or manually changing the generated assembly bytes.
				foreach ($cachePath in @($derivedScenesRoot, $managedReadinessManifestPath)) {
					Assert-PathBelow -Parent $projectRoot -Child $cachePath
					if (Test-Path -LiteralPath $cachePath) { Remove-Item -LiteralPath $cachePath -Recurse -Force }
				}
				$mutation = "removed disposable derived-scene cache and managed-readiness manifest"
			}
			"stale" {
				# JSON permits trailing whitespace, so this changes the source identity
				# without changing the authored scene semantics or touching other inputs.
				Add-Content -LiteralPath $scenePath -Value "" -Encoding utf8
				$mutation = "appended whitespace to the disposable authored scene copy"
			}
			"corrupt" {
				$derivedArtifact = @(Get-ChildItem -LiteralPath $derivedScenesRoot -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1)
				if ($derivedArtifact.Count -ne 1) { throw "readiness corrupt requires an existing disposable derived-scene artifact from an earlier scenario." }
				[System.IO.File]::WriteAllBytes($derivedArtifact[0].FullName, [byte[]]@(0x55, 0x53, 0x34))
				$mutation = "damaged disposable derived artifact '$($derivedArtifact[0].FullName)'"
			}
		}
		$sourceHashAfterPreparation = (Get-FileHash -LiteralPath $scenePath -Algorithm SHA256).Hash
		$readiness.preparation = [ordered]@{
			mutation = $mutation; sourceSha256Before = $sourceHashBeforePreparation; sourceSha256After = $sourceHashAfterPreparation
			artifactInventoryBefore = $preparedArtifacts; artifactInventoryAfter = @(Get-ArtifactInventory -Root $projectRoot)
		}
		Complete-Stage -Name "readinessScenarioPreparation"
	}
	$authoredSceneHashBefore = (Get-FileHash -LiteralPath $scenePath -Algorithm SHA256).Hash
	Complete-Stage -Name "projectAndSceneGeneration"

	Start-Stage -Name "processStartupAndMcpReadiness"
	$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
	$startInfo.FileName = $editorExecutable
	$startInfo.Arguments = '--project "{0}"' -f $projectRoot.Replace('"', '\"')
	$startInfo.WorkingDirectory = $editorDirectory
	$startInfo.UseShellExecute = $false
	$startInfo.EnvironmentVariables["TERRAENGINE_EDITOR_PREFS_SESSION_PATH"] = $preferencesPath
	$startInfo.EnvironmentVariables["TERRAENGINE_SUPPRESS_PROJECT_RECENTS"] = "1"
	# The canonical run leaves worker discovery entirely to the native pool. Remove
	# an inherited override as well, so a parent shell cannot accidentally cap the
	# installed editor. Explicit 1/2/4 values remain available only for worker-scaling thresholds's
	# controlled worker-scaling comparison matrix.
	if ($WorkerCount -eq "max" -or $WorkerCount -eq "0") {
		$startInfo.EnvironmentVariables.Remove("TERRAENGINE_THREADPOOL_WORKERS")
	} else {
		$startInfo.EnvironmentVariables["TERRAENGINE_THREADPOOL_WORKERS"] = $WorkerCount
	}
	$process = [System.Diagnostics.Process]::Start($startInfo)
	if ($null -eq $process) { throw "The installed TerraEngine editor process could not be started." }

	function Invoke-McpRpc {
		param([Parameter(Mandatory = $true)][string] $Method, $Params = @{})
		$script:rpcId++
		$request = [ordered]@{ jsonrpc = "2.0"; id = $script:rpcId; method = $Method; params = $Params }
		$rpcStartMs = Get-MonotonicMilliseconds
		try {
			# Windows PowerShell 5.1 otherwise tries to initialize the retired Internet
			# Explorer parser and can throw before exposing a valid loopback response.
			$response = Invoke-WebRequest -UseBasicParsing -Uri ("http://127.0.0.1:{0}/mcp" -f $McpPort) -Method Post -ContentType "application/json" -Body ($request | ConvertTo-Json -Depth 32 -Compress) -TimeoutSec 30
			$parsed = $response.Content | ConvertFrom-Json
			$rpcDurationMs = [Math]::Max(0.0, (Get-MonotonicMilliseconds) - $rpcStartMs)
			if ($script:latencySamples.Count -lt 4096) { $script:latencySamples.Add([ordered]@{ operation = $Method; durationMs = $rpcDurationMs; timestamp = (Get-Date).ToString("o") }) }
			Add-EvidenceRecord -Path $rpcLogPath -Value ([ordered]@{ timestamp = (Get-Date).ToString("o"); request = $request; response = $parsed })
			# A successful JSON-RPC object omits the optional error member. Query the
			# property collection because direct access is an exception in strict mode.
			$errorProperty = $parsed.PSObject.Properties["error"]
			if ($null -ne $errorProperty -and $null -ne $errorProperty.Value) {
				throw "JSON-RPC error $($errorProperty.Value.code): $($errorProperty.Value.message)"
			}
			return $parsed
		}
		catch {
			$rpcDurationMs = [Math]::Max(0.0, (Get-MonotonicMilliseconds) - $rpcStartMs)
			if ($script:latencySamples.Count -lt 4096) { $script:latencySamples.Add([ordered]@{ operation = $Method; durationMs = $rpcDurationMs; timestamp = (Get-Date).ToString("o"); failed = $true }) }
			Add-EvidenceRecord -Path $rpcLogPath -Value ([ordered]@{ timestamp = (Get-Date).ToString("o"); request = $request; error = $_.Exception.Message })
			throw
		}
	}

	$ready = $false
	$deadline = (Get-Date).AddSeconds($StartupTimeoutSeconds)
	while ((Get-Date) -lt $deadline) {
		$process.Refresh()
		if ($process.HasExited) { throw "Editor exited before MCP became ready. ExitCode=$($process.ExitCode)" }
		try {
			$initialize = Invoke-McpRpc -Method "initialize" -Params ([ordered]@{ protocolVersion = "2025-03-26"; clientInfo = [ordered]@{ name = "installed-physics-stress"; version = "1" } })
			if ($initialize.result.serverInfo.name -eq "TerraEngine Editor") { $ready = $true; break }
		}
		catch { Start-Sleep -Milliseconds 500 }
	}
	if (-not $ready) { throw "Editor MCP did not become ready within $StartupTimeoutSeconds seconds." }
	Capture-EditorProcessMemory -Phase "mcp-ready"
	Complete-Stage -Name "processStartupAndMcpReadiness"

	# Verify the exact available capability surface before mutating Play Mode.
	$tools = Invoke-McpRpc -Method "tools/list"
	$toolNames = @($tools.result.tools | ForEach-Object { $_.name })
	$requiredTools = @("play.enter", "play.pause", "play.step", "play.exit", "physics.state", "physics.diagnostics.get", "diagnostics.get_summary", "profiler.capture.start", "profiler.capture.stop", "profiler.capture.list", "profiler.capture.load", "profiler.capture.summary", "jobs.get", "scene_view.state", "scene_view.settings.get", "scene_view.settings.set")
	if ($ReadinessMode -ne "none") {
		# These additive read surfaces are the contract evidence for source-versus-
		# derived startup decisions and managed-output reuse. Forced build also uses
		# the existing durable script.compile job contract.
		$requiredTools += @("project.state", "script.status")
		if ($ReadinessMode -eq "forced-build") { $requiredTools += "script.compile" }
	}
	if ($RunColliderCookIncrementalMatrix) {
		$requiredTools += @("project.state", "scene.list_objects", "scene.save", "transaction.execute", "history.state", "history.undo", "physics.body.inspect")
	}
	if ($RunAuthoringWalkthrough) {
		$requiredTools += @("object.create", "object.set_metadata", "scene.list_objects", "scene.save", "transaction.execute", "history.state", "history.undo", "history.redo", "prefab.create", "prefab.instantiate", "prefab.overrides.inspect", "prefab.overrides.apply", "prefab.overrides.revert")
	}
	if ($RunManagedReloadAutomationLoopback) {
		$requiredTools += @("scene.save", "script.compile", "physics.body.inspect", "physics.body.apply_force", "physics.raycast", "physics.query_job.get", "physics.contacts.list")
	}
	foreach ($requiredTool in $requiredTools) {
		if ($toolNames -notcontains $requiredTool) { throw "Installed editor MCP does not advertise required tool '$requiredTool'." }
	}

	function Invoke-Tool {
		param([Parameter(Mandatory = $true)][string] $Name, $Arguments = @{})
		$toolStartMs = Get-MonotonicMilliseconds
		try {
			$result = Get-ToolEnvelope -RpcResponse (Invoke-McpRpc -Method "tools/call" -Params ([ordered]@{ name = $Name; arguments = $Arguments })) -Operation $Name
			if ($script:latencySamples.Count -lt 4096) { $script:latencySamples.Add([ordered]@{ operation = "capability:$Name"; durationMs = [Math]::Max(0.0, (Get-MonotonicMilliseconds) - $toolStartMs); timestamp = (Get-Date).ToString("o") }) }
			return $result
		} catch {
			if ($script:latencySamples.Count -lt 4096) { $script:latencySamples.Add([ordered]@{ operation = "capability:$Name"; durationMs = [Math]::Max(0.0, (Get-MonotonicMilliseconds) - $toolStartMs); timestamp = (Get-Date).ToString("o"); failed = $true }) }
			throw
		}
	}

	function Invoke-ToolExpectFailure {
		param([Parameter(Mandatory = $true)][string] $Name, $Arguments = @{})
		# Expected-rejection probes must inspect the normal public envelope without
		# converting a correct conflict into a harness failure. The full RPC exchange
		# is still retained by Invoke-McpRpc in mcp.jsonl.
		$rpc = Invoke-McpRpc -Method "tools/call" -Params ([ordered]@{ name = $Name; arguments = $Arguments })
		if ($null -eq $rpc.result -or $null -eq $rpc.result.content -or $rpc.result.content.Count -lt 1) {
			throw "MCP '$Name' returned no envelope for an expected rejection."
		}
		$envelope = $rpc.result.content[0].text | ConvertFrom-Json
		if ([bool]$envelope.ok) { throw "MCP '$Name' unexpectedly accepted an operation that had to be rejected." }
		return $envelope
	}

	function Wait-StablePausedPhysicsState {
		# play.pause changes ownership immediately, but the immutable published
		# snapshot can still trail the already-completed simulation tick by one editor
		# frame. Require a quiet publication window before using the tick as the exact
		# baseline for Next so a late snapshot is not mistaken for two requested steps.
		$deadline = (Get-Date).AddSeconds([Math]::Min(30, $StartupTimeoutSeconds))
		$lastTick = $null
		$stableSinceMs = Get-MonotonicMilliseconds
		$latest = $null
		while ((Get-Date) -lt $deadline) {
			$latest = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.state")
			$tick = [UInt64]$latest.tick
			$nowMs = Get-MonotonicMilliseconds
			if ($null -eq $lastTick -or $tick -ne [UInt64]$lastTick) {
				$lastTick = $tick
				$stableSinceMs = $nowMs
			} elseif (($nowMs - $stableSinceMs) -ge 250.0) {
				return $latest
			}
			Start-Sleep -Milliseconds 16
		}
		throw "Paused physics publication did not stabilize before the exact-step timeout."
	}

	function Get-RejectionMessage {
		param([Parameter(Mandatory = $true)] $Envelope)
		# Error messages are additive in the automation contract. Older installed
		# envelopes may provide only the stable category, which remains sufficient
		# to prove the expected rejection without weakening the assertion.
		$messageProperty = $Envelope.error.PSObject.Properties["message"]
		if ($null -ne $messageProperty -and -not [string]::IsNullOrWhiteSpace([string]$messageProperty.Value)) {
			return [string]$messageProperty.Value
		}
		return [string]$Envelope.error.category
	}

	# Read back the startup document through the public automation surface. The
	# high-impact settings.set tool remains required and discoverable, but the
	# unattended harness must not bypass its interactive approval contract.
	$sceneViewGet = Invoke-Tool -Name "scene_view.settings.get"
	Start-Sleep -Milliseconds 500
	$sceneViewState = Invoke-Tool -Name "scene_view.state"
	$sceneViewEvidence.configured = $sceneViewGet.data.configured
	$sceneViewEvidence.effective = $sceneViewGet.data.effective
	$sceneViewEvidence.state = $sceneViewState.data.state
	$sceneViewEvidence.persisted = Test-Path -LiteralPath $preferencesPath
	$sceneViewEvidence.configurationSource = "isolated_preferences_file"

	function Wait-AutomationJob {
		param(
			[Parameter(Mandatory = $true)][string] $JobId,
			[Parameter(Mandatory = $true)][string] $Operation,
			[ValidateSet("jobs.get", "physics.query_job.get")][string] $PollTool = "jobs.get"
		)
		$deadlineMs = (Get-MonotonicMilliseconds) + ($StartupTimeoutSeconds * 1000.0)
		while ($true) {
			$jobEnvelope = Invoke-Tool -Name $PollTool -Arguments ([ordered]@{ jobId = $JobId })
			# Common jobs return data.job; physics query/cook polling is itself an
			# immutable physics snapshot and therefore returns data.data.job.
			$job = if ($PollTool -eq "physics.query_job.get") { $jobEnvelope.data.data.job } else { $jobEnvelope.data.job }
			$state = [string]$job.state
			if ($state -eq "succeeded") { return $job }
			if ($state -eq "failed" -or $state -eq "canceled") {
				throw "$Operation durable job '$JobId' ended in state '$state'."
			}
			if ((Get-MonotonicMilliseconds) -ge $deadlineMs) {
				throw "$Operation durable job '$JobId' did not complete before the timeout."
			}
			Start-Sleep -Milliseconds 100
		}
	}

	function Capture-ReadinessEvidence {
		param([Parameter(Mandatory = $true)][string] $Phase)
		# Keep raw additive contract data in the summary. This avoids guessing
		# provider-specific cache reasons while preserving derived-scene fallback,
		# readiness-manifest, loaded-output, and script lifecycle evidence.
		$projectState = Invoke-Tool -Name "project.state"
		$scriptStatus = Invoke-Tool -Name "script.status"
		if ($null -eq $projectState.data.derivedScene -or $null -eq $projectState.data.readiness) {
			throw "readiness project.state did not provide derived-scene and readiness evidence."
		}
		if ($null -eq $scriptStatus.data) { throw "readiness script.status did not provide readiness evidence." }
		return [ordered]@{
			phase = $Phase; timestamp = (Get-Date).ToString("o"); monotonicMs = Get-MonotonicMilliseconds
			projectState = $projectState.data; scriptStatus = $scriptStatus.data
			authoredSceneSha256 = (Get-FileHash -LiteralPath $scenePath -Algorithm SHA256).Hash
			artifactInventory = @(Get-ArtifactInventory -Root $projectRoot)
		}
	}

	function Capture-DiagnosticsSummary {
		param([Parameter(Mandatory = $true)][string] $Phase, [switch] $Force)
		# Keep snapshots bounded and sample at most once every five seconds so diagnostics
		# observation does not become the workload being measured.
		$nowMs = Get-MonotonicMilliseconds
		if (-not $Force -and ($nowMs - $script:lastDiagnosticsCaptureMs) -lt 5000.0) { return }
		if ($script:diagnosticSnapshots.Count -ge 16) {
			if (-not $Force) { return }
			$script:diagnosticSnapshots.RemoveAt(0)
		}
		try {
			$snapshot = Invoke-Tool -Name "diagnostics.get_summary" -Arguments ([ordered]@{ includeDetails = $true })
			$script:diagnosticSnapshots.Add([ordered]@{ phase = $Phase; monotonicMs = $nowMs; timestamp = (Get-Date).ToString("o"); data = $snapshot.data })
			$script:lastDiagnosticsCaptureMs = $nowMs
		} catch {
			# Diagnostics are optional evidence; transient unavailability must not
			# mask the physics correctness checks already in progress.
			if ($script:diagnosticSnapshots.Count -ge 16) { $script:diagnosticSnapshots.RemoveAt(0) }
			$script:diagnosticSnapshots.Add([ordered]@{ phase = $Phase; monotonicMs = $nowMs; timestamp = (Get-Date).ToString("o"); unavailable = $true; reason = $_.Exception.Message })
		}
	}

	# A clean project performs one initial managed build after the MCP endpoint is
	# already available. Entering Play Mode during that build is valid, but the
	# script compiler then pauses gameplay for its reload boundary and races the
	# harness's explicit pause request. Poll the immutable diagnostics surface so
	# the following Play/Pause/Next checks measure physics after startup work.
	Start-Stage -Name "scriptingReadiness"
	$deadline = (Get-Date).AddSeconds($StartupTimeoutSeconds)
	$scriptingReady = $false
	$coordinatorState = "Unknown"
	while ((Get-Date) -lt $deadline) {
		$process.Refresh()
		if ($process.HasExited) { throw "Editor exited while waiting for the initial managed build. ExitCode=$($process.ExitCode)" }
		$diagnosticsSummary = Invoke-Tool -Name "diagnostics.get_summary" -Arguments ([ordered]@{ includeDetails = $true; surface = "scripting" })
		$scriptingSurface = @($diagnosticsSummary.data.surfaces | Where-Object { $_.surface -eq "scripting" } | Select-Object -First 1)
		if ($scriptingSurface.Count -eq 1) {
			$coordinatorState = [string]$scriptingSurface[0].details.coordinatorState
			if ($coordinatorState -eq "Failed" -or $coordinatorState -eq "Canceled") {
				throw "The clean project's initial managed build ended in coordinator state '$coordinatorState'."
			}
			# Idle is only the pre-queue startup state for this clean project. Ready
			# proves the initial build and reload boundary both completed.
			if ($coordinatorState -eq "Ready") {
				$scriptingReady = $true
				break
			}
		}
		Start-Sleep -Milliseconds 250
	}
	if (-not $scriptingReady) {
		throw "The clean project's scripting coordinator did not become ready before the timeout (last state '$coordinatorState')."
	}
	Complete-Stage -Name "scriptingReadiness"
	$initialPlayState = Invoke-Tool -Name "play.state"
	if ($initialPlayState.data.currentState -ne "stopped") {
		throw "The clean editor was not stopped after its initial managed build."
	}
	if ($ReadinessMode -ne "none") {
		Start-Stage -Name "readinessEvidence"
		$readiness.startup = Capture-ReadinessEvidence -Phase "startup-ready"
		$derivedStartup = $readiness.startup.projectState.derivedScene
		$scriptReadinessStartup = $readiness.startup.scriptStatus.readiness
		$derivedExpectation = switch ($ReadinessMode) {
			"warm" { [bool]$derivedStartup.cacheHit }
			"cold" { -not [bool]$derivedStartup.cacheHit -and [bool]$derivedStartup.usedAuthoritativeSource -and [bool]$derivedStartup.regenerationPublished }
			"stale" { -not [bool]$derivedStartup.cacheHit -and [bool]$derivedStartup.usedAuthoritativeSource -and [bool]$derivedStartup.regenerationPublished }
			"corrupt" { -not [bool]$derivedStartup.cacheHit -and [bool]$derivedStartup.usedAuthoritativeSource -and [bool]$derivedStartup.regenerationPublished }
			default { $true }
		}
		$readiness.startupValidation = [ordered]@{
			derivedExpectationMet = $derivedExpectation
			cacheHit = [bool]$derivedStartup.cacheHit
			usedAuthoritativeSource = [bool]$derivedStartup.usedAuthoritativeSource
			regenerationPublished = [bool]$derivedStartup.regenerationPublished
			derivedReason = [string]$derivedStartup.reason
			managedReadiness = $scriptReadinessStartup
		}
		if (-not $derivedExpectation) { throw "readiness '$ReadinessMode' did not publish the required derived-scene cache/fallback/regeneration outcome." }
		if ($ReadinessMode -eq "forced-build") {
			if (-not [bool]$scriptReadinessStartup.cache.hit) { throw "readiness forced-build requires a warm managed-readiness hit before its explicit rebuild." }
			# A warm output is intentionally present from the previous scenario. This
			# public request must still rebuild and complete as a durable job.
			$compile = Invoke-Tool -Name "script.compile" -Arguments ([ordered]@{
				profile = "RelWithDebInfo"; correlationId = "readiness-forced-managed-build"
			})
			if (-not [bool]$compile.data.forcedBuild) { throw "readiness forced build was not acknowledged as cache-bypassing." }
			$compileJobId = [string]$compile.data.jobId
			if ([string]::IsNullOrWhiteSpace($compileJobId)) { throw "readiness forced build did not return a durable job ID." }
			$compileJob = Wait-AutomationJob -JobId $compileJobId -Operation "readiness forced managed build"
			$readiness.forcedCompile = [ordered]@{
				accepted = $compile.data; jobId = $compileJobId; durableResult = $compileJob
				postBuild = Capture-ReadinessEvidence -Phase "forced-build-complete"
			}
		}
		Complete-Stage -Name "readinessEvidence"
	}

	if ($RunColliderCookIncrementalMatrix) {
		Start-Stage -Name "collider-cookStoppedMutationMatrix"
		# Use two real physics objects from the generated stress scene. The exact
		# reparent is immediately undone before Play Mode so the performance fixture
		# remains byte-for-byte equivalent after its normalizing save.
		$childId = [string]$objects[0].uuid
		$childName = [string]$objects[0].name
		$parentIndex = $dynamicCount + $kinematicCount
		$parentId = [string]$objects[$parentIndex].uuid
		$projectStateBefore = Invoke-Tool -Name "project.state"
		$sceneGeneration = [UInt64]$projectStateBefore.data.sceneGeneration
		$historyBeforeReparent = Invoke-Tool -Name "history.state"
		$reparent = Invoke-Tool -Name "transaction.execute" -Arguments ([ordered]@{
			operations = @([ordered]@{ kind = "object.set_parent"; target = $childId; parent = $parentId })
			expectedSceneGeneration = $sceneGeneration
			label = "collider-cook matrix installed physics reparent"
			correlationId = "collider-cook-collider-cook-reparent"
			idempotencyKey = "collider-cook-collider-cook-reparent"
		})
		if ([int]$reparent.data.appliedOperationCount -ne 1) { throw "collider-cook matrix reparent did not commit exactly one typed operation." }
		$historyAfterReparent = Invoke-Tool -Name "history.state"
		$reparentUndo = Invoke-Tool -Name "history.undo" -Arguments ([ordered]@{ expectedSequence = [UInt64]$historyAfterReparent.data.sequence })
		$restoredRoots = Invoke-Tool -Name "scene.list_objects" -Arguments ([ordered]@{ search = $childName })
		$restoredChild = @($restoredRoots.data.objects | Where-Object { [string]$_.uuid -eq $childId -and $null -eq $_.parent })
		if ($restoredChild.Count -ne 1) { throw "collider-cook matrix reparent undo did not restore the physics child as an exact root." }

		# A deliberately stale scene generation must reject before any write. Search
		# both names afterward so the evidence proves the prior name survived and the
		# conflicting value never became partially visible.
		$conflictName = "collider-cook matrix MUST NOT COMMIT"
		$conflict = Invoke-ToolExpectFailure -Name "transaction.execute" -Arguments ([ordered]@{
			operations = @([ordered]@{ kind = "object.set_metadata"; target = $childId; name = $conflictName })
			expectedSceneGeneration = [UInt64]::MaxValue
			label = "collider-cook matrix installed stale conflict"
			correlationId = "collider-cook-collider-cook-conflict"
			idempotencyKey = "collider-cook-collider-cook-conflict"
		})
		if ([string]$conflict.error.category -ne "stale_target") { throw "collider-cook matrix stale transaction returned '$($conflict.error.category)' instead of stale_target." }
		$conflictLeak = Invoke-Tool -Name "scene.list_objects" -Arguments ([ordered]@{ search = $conflictName })
		$unchangedRoot = Invoke-Tool -Name "scene.list_objects" -Arguments ([ordered]@{ search = $childName })
		if (@($conflictLeak.data.objects).Count -ne 0 -or @($unchangedRoot.data.objects | Where-Object { [string]$_.uuid -eq $childId }).Count -ne 1) {
			throw "collider-cook matrix stale conflict changed live scene state before rejection."
		}

		[void](Invoke-Tool -Name "scene.save")
		$authoredSceneHashBefore = (Get-FileHash -LiteralPath $scenePath -Algorithm SHA256).Hash
		$colliderCookMatrix.reparent = [ordered]@{
			child = $childId; parent = $parentId; transactionId = [string]$reparent.data.transactionId
			visibilityBoundary = [string]$reparent.data.visibilityBoundary; undoHistoryId = [string]$reparentUndo.data.historyId
			initialHistoryDepth = [int]$historyBeforeReparent.data.undoDepth; restoredAsRoot = $true
		}
		$colliderCookMatrix.conflict = [ordered]@{
			expectedSceneGeneration = [UInt64]::MaxValue; actualSceneGeneration = $sceneGeneration
			category = [string]$conflict.error.category; message = Get-RejectionMessage -Envelope $conflict; partialVisibility = $false
		}
		Complete-Stage -Name "collider-cookStoppedMutationMatrix"
	}

	if ($RunManagedReloadAutomationLoopback) {
		Start-Stage -Name "managed-reload-automationManagedReload"
		# Use the public durable compilation job rather than relying solely on the
		# editor's implicit clean-project build. This proves the installed managed
		# assembly can cross a second reload boundary in RelWithDebInfo.
		$compile = Invoke-Tool -Name "script.compile" -Arguments ([ordered]@{ profile = "RelWithDebInfo"; correlationId = "managed-reload-automation-managed-reload" })
		$compileJobId = [string]$compile.data.jobId
		if ([string]::IsNullOrWhiteSpace($compileJobId)) { throw "managed-reload-automation script.compile did not return a durable job ID." }
		$compileJob = Wait-AutomationJob -JobId $compileJobId -Operation "managed-reload-automation managed reload"
		if ([string]$compileJob.correlationId -ne "managed-reload-automation-managed-reload") { throw "managed-reload-automation managed reload lost its correlation ID." }
		$managedReloadAutomationLoopback.managedReload = [ordered]@{
			jobId = $compileJobId; state = [string]$compileJob.state; correlationId = [string]$compileJob.correlationId
			result = $compileJob.result
		}
		if (-not $RunAuthoringWalkthrough) {
			# The hand-authored disposable JSON intentionally omits optional fields.
			# Normalize it through the installed serializer so a migration dirty flag
			# cannot turn normal window shutdown into an unsaved-changes prompt.
			[void](Invoke-Tool -Name "scene.save")
			$authoredSceneHashBefore = (Get-FileHash -LiteralPath $scenePath -Algorithm SHA256).Hash
		}
		Complete-Stage -Name "managed-reload-automationManagedReload"
	}

	if ($RunAuthoringWalkthrough) {
		Start-Stage -Name "authoringWalkthrough"
		# Normalize the generated scene through the installed serializer before the
		# walkthrough. Cleanup can then save the restored state and prove byte-for-byte
		# equality while also clearing the dirty flag for a normal window shutdown.
		[void](Invoke-Tool -Name "scene.save")
		$authoredSceneHashBefore = (Get-FileHash -LiteralPath $scenePath -Algorithm SHA256).Hash
		$initialHistory = Invoke-Tool -Name "history.state"
		$initialHistoryDepth = [int]$initialHistory.data.undoDepth
		# Exercise only public, self-advertised editor capabilities. The temporary
		# authoring objects do not carry physics components, so the stress body's
		# fixed 10,000-object performance workload remains unchanged.
		$source = Invoke-Tool -Name "object.create" -Arguments ([ordered]@{ name = "Authoring Walkthrough Prefab Source" })
		$sourceId = [string]$source.data.object.uuid
		if ([string]::IsNullOrWhiteSpace($sourceId)) { throw "Authoring walkthrough object.create did not return a stable source UUID." }

		$metadata = Invoke-Tool -Name "transaction.execute" -Arguments ([ordered]@{
			operations = @(
				[ordered]@{ kind = "object.set_metadata"; target = $sourceId; name = "Authoring Walkthrough Source" },
				[ordered]@{ kind = "object.set_metadata"; target = $sourceId; tag = "PhysicsWalkthrough" }
			)
			label = "authoring walkthrough metadata"
			correlationId = "authoring-walkthrough-metadata"
			idempotencyKey = "authoring-walkthrough-metadata"
		})
		$historyAfterCommit = Invoke-Tool -Name "history.state"
		if (-not $historyAfterCommit.data.canUndo) { throw "Authoring walkthrough metadata transaction did not publish an undo entry." }

		$undo = Invoke-Tool -Name "history.undo" -Arguments ([ordered]@{ expectedSequence = [UInt64]$historyAfterCommit.data.sequence })
		$afterUndo = Invoke-Tool -Name "scene.list_objects" -Arguments ([ordered]@{ search = "Authoring Walkthrough Prefab Source" })
		$undoMatches = @($afterUndo.data.objects | Where-Object { $_.uuid -eq $sourceId -and $_.name -eq "Authoring Walkthrough Prefab Source" })
		if ($undoMatches.Count -ne 1) { throw "Authoring walkthrough undo did not restore the exact source-object name." }

		$redo = Invoke-Tool -Name "history.redo" -Arguments ([ordered]@{ expectedSequence = [UInt64]$undo.data.sequence })
		$afterRedo = Invoke-Tool -Name "scene.list_objects" -Arguments ([ordered]@{ search = "Authoring Walkthrough Source" })
		$redoMatches = @($afterRedo.data.objects | Where-Object { $_.uuid -eq $sourceId -and $_.name -eq "Authoring Walkthrough Source" -and $_.tag -eq "PhysicsWalkthrough" })
		if ($redoMatches.Count -ne 1) { throw "Authoring walkthrough redo did not restore the exact committed metadata." }

		$prefabPath = "Assets/Prefabs/AuthoringWalkthrough.teprefab"
		$prefab = Invoke-Tool -Name "prefab.create" -Arguments ([ordered]@{ object = $sourceId; path = $prefabPath })
		$prefabId = [string]$prefab.data.prefabId
		if ([string]::IsNullOrWhiteSpace($prefabId)) { throw "Authoring walkthrough prefab.create did not return a stable prefab identity." }
		$instance = Invoke-Tool -Name "prefab.instantiate" -Arguments ([ordered]@{ prefab = $prefabId; name = "Authoring Walkthrough Prefab Instance" })
		$instanceId = [string]$instance.data.instanceId
		if ([string]::IsNullOrWhiteSpace($instanceId)) { throw "Authoring walkthrough prefab.instantiate did not return a stable instance identity." }

		[void](Invoke-Tool -Name "object.set_metadata" -Arguments ([ordered]@{ object = $instanceId; name = "Authoring Walkthrough Applied Override" }))
		$overridesBeforeApply = Invoke-Tool -Name "prefab.overrides.inspect" -Arguments ([ordered]@{ instance = $instanceId })
		$applyCount = @($overridesBeforeApply.data.overrides).Count
		if ($applyCount -lt 1) { throw "Authoring walkthrough instance metadata did not appear as a structured prefab override." }
		$applied = Invoke-Tool -Name "prefab.overrides.apply" -Arguments ([ordered]@{ instance = $instanceId; expectedPrefabGeneration = [UInt64]$overridesBeforeApply.data.prefabGeneration })
		if ([string]::IsNullOrWhiteSpace([string]$applied.data.historyId)) { throw "Authoring walkthrough prefab override apply did not publish one history identity." }

		# Author a second difference after apply so revert proves a real state change,
		# rather than merely confirming that the already-applied override list is empty.
		[void](Invoke-Tool -Name "object.set_metadata" -Arguments ([ordered]@{ object = $instanceId; name = "Authoring Walkthrough Revert Candidate" }))
		$overridesBeforeRevert = Invoke-Tool -Name "prefab.overrides.inspect" -Arguments ([ordered]@{ instance = $instanceId })
		$revertCount = @($overridesBeforeRevert.data.overrides).Count
		if ($revertCount -lt 1) { throw "Authoring walkthrough second instance edit did not create an override for revert." }
		$reverted = Invoke-Tool -Name "prefab.overrides.revert" -Arguments ([ordered]@{ instance = $instanceId })
		if ([int]$reverted.data.remainingOverrideCount -ne 0) { throw "Authoring walkthrough prefab revert left authored overrides behind." }
		$revertedInstance = Invoke-Tool -Name "scene.list_objects" -Arguments ([ordered]@{ search = "Authoring Walkthrough Applied Override" })
		$revertedMatches = @($revertedInstance.data.objects | Where-Object { $_.uuid -eq $instanceId -and $_.name -eq "Authoring Walkthrough Applied Override" })
		if ($revertedMatches.Count -ne 1) { throw "Authoring walkthrough prefab revert did not restore the current source state." }

		$authoringWalkthrough = [ordered]@{
			requested = $true; completed = $true; sourceObjectId = $sourceId; transactionId = [string]$metadata.data.transactionId
			undoHistoryId = [string]$undo.data.historyId; redoHistoryId = [string]$redo.data.historyId
			prefabId = $prefabId; prefabPath = $prefabPath; instanceId = $instanceId
			appliedOverrideCount = $applyCount; revertedOverrideCount = $revertCount; remainingOverrideCount = [int]$reverted.data.remainingOverrideCount
			initialHistoryDepth = $initialHistoryDepth; cleanupCompleted = $false
		}
		Complete-Stage -Name "authoringWalkthrough"
	}

	Start-Stage -Name "playEnter"
	$enter = Invoke-Tool -Name "play.enter" -Arguments ([ordered]@{ expectedState = "stopped"; correlationId = "installed-physics-stress-enter" })
	Complete-Stage -Name "playEnter"
	$observations = [System.Collections.Generic.List[object]]::new()
	$physicsReady = $false
	$previousTick = [UInt64]0
	Start-Stage -Name "physicsLive"
	$deadline = (Get-Date).AddSeconds($StartupTimeoutSeconds)
	while ((Get-Date) -lt $deadline) {
		$process.Refresh()
		if ($process.HasExited) { throw "Editor exited while waiting for live physics observations. ExitCode=$($process.ExitCode)" }
		$state = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.state")
		$diagnostics = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.diagnostics.get")
		Capture-DiagnosticsSummary -Phase "live"
		$bodyCount = [UInt64]$state.data.bodyCount
		$tick = [UInt64]$state.tick
		$observations.Add([ordered]@{ timestamp = (Get-Date).ToString("o"); tick = $tick; bodyCount = $bodyCount; diagnostics = $diagnostics.data })
		if ($tick -gt $previousTick -and $bodyCount -ge $ObjectCount) { $previousTick = $tick; $physicsReady = $true; break }
		$previousTick = $tick
		Start-Sleep -Milliseconds 250
	}
	if (-not $physicsReady) { throw "Physics did not publish advancing ticks with at least $ObjectCount bodies before the timeout." }
	Capture-EditorProcessMemory -Phase "physics-live" -Tick $previousTick
	Complete-Stage -Name "physicsLive"
	if ($RunColliderCookIncrementalMatrix) {
		# World generation is validated before body lookup, so this probe does not
		# depend on a provider-private handle or a guessed component generation.
		$currentWorldGeneration = [UInt64]$state.worldGeneration
		$staleWorldGeneration = if ($currentWorldGeneration -eq [UInt32]::MaxValue) { $currentWorldGeneration - 1 } else { $currentWorldGeneration + 1 }
		$stale = Invoke-ToolExpectFailure -Name "physics.body.inspect" -Arguments ([ordered]@{
			target = [ordered]@{ objectId = $childId; componentGeneration = [UInt64]1; worldGeneration = $staleWorldGeneration }
		})
		if ([string]$stale.error.category -ne "conflict") { throw "collider-cook matrix stale physics target returned '$($stale.error.category)' instead of conflict." }
		$colliderCookMatrix.stale = [ordered]@{
			objectId = $childId; currentWorldGeneration = $currentWorldGeneration; staleWorldGeneration = $staleWorldGeneration
			category = [string]$stale.error.category; message = Get-RejectionMessage -Envelope $stale; partialVisibility = $false
		}
	}

	if ($RunManagedReloadAutomationLoopback) {
		Start-Stage -Name "managed-reload-automationRuntimeDiscovery"
		$managedReloadAutomationWorldGeneration = [UInt64]$state.worldGeneration
		# Discover the target through a real bounded query job. The high, gravity-
		# disabled fixture body is the first hit along this vertical ray, and the hit
		# supplies the exact component generation needed by every later command.
		$raycast = Invoke-Tool -Name "physics.raycast" -Arguments ([ordered]@{
			worldGeneration = $managedReloadAutomationWorldGeneration
			origin = @([double]$managedReloadAutomationTargetPosition[0], 100.0, [double]$managedReloadAutomationTargetPosition[2])
			direction = @(0.0, -1.0, 0.0); distance = 200.0; maximumHits = 64
			batchSize = 1; layerMask = [UInt32]::MaxValue; includeTriggers = $false
			correlationId = "managed-reload-automation-target-raycast"
		})
		$raycastJobId = [string]$raycast.data.jobId
		if ([string]::IsNullOrWhiteSpace($raycastJobId)) { throw "managed-reload-automation physics.raycast did not return a durable query job ID." }
		$raycastJob = Wait-AutomationJob -JobId $raycastJobId -Operation "managed-reload-automation target raycast" -PollTool "physics.query_job.get"
		$targetHits = @($raycastJob.result.hits | Where-Object { [string]$_.objectId -eq $managedReloadAutomationTargetId })
		if ($targetHits.Count -ne 1) { throw "managed-reload-automation target raycast did not return the dedicated custom-gravity body exactly once." }
		$targetHit = $targetHits[0]
		$managedReloadAutomationTarget = [ordered]@{
			objectId = $managedReloadAutomationTargetId
			componentGeneration = [UInt64]$targetHit.componentGeneration
			worldGeneration = $managedReloadAutomationWorldGeneration
		}
		$bodyBefore = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.body.inspect" -Arguments ([ordered]@{ target = $managedReloadAutomationTarget }))
		$diagnosticsBefore = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.diagnostics.get" -Arguments ([ordered]@{ includeFailures = $true }))
		if (@($diagnosticsBefore.data.scheduler).Count -lt 1) { throw "managed-reload-automation physics diagnostics did not expose scheduler declarations." }
		$managedReloadAutomationLoopback.runtimeDiscovery = [ordered]@{
			worldGeneration = $managedReloadAutomationWorldGeneration; target = $managedReloadAutomationTarget; raycastJobId = $raycastJobId
			raycastState = [string]$raycastJob.state; raycastHitCount = @($raycastJob.result.hits).Count
			bodyBefore = $bodyBefore; schedulerDeclarationCount = @($diagnosticsBefore.data.scheduler).Count
		}
		Complete-Stage -Name "managed-reload-automationRuntimeDiscovery"
	}

	Start-Stage -Name "pause"
	$pause = Invoke-Tool -Name "play.pause" -Arguments ([ordered]@{ expectedState = "playing"; correlationId = "installed-physics-stress-pause" })
	$pausedState = Wait-StablePausedPhysicsState
	$pausedTick = [UInt64]$pausedState.tick
	if ($WorkerScalingScenario -eq "unchanged") {
		# Two immutable reads while paused prove the unchanged workload does not
		# advance state or enqueue a step request.
		$unchangedProbe = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.state")
		$workerScalingScenarioEvidence.fromTick = $pausedTick
		$workerScalingScenarioEvidence.toTick = [UInt64]$unchangedProbe.tick
		$workerScalingScenarioEvidence.unchanged = ($workerScalingScenarioEvidence.fromTick -eq $workerScalingScenarioEvidence.toTick)
		if (-not $workerScalingScenarioEvidence.unchanged) { throw "worker-scaling unchanged scenario advanced while paused." }
	}
	if ($WorkerScalingScenario -eq "paused") {
		$workerScalingScenarioEvidence.fromTick = $pausedTick
		$workerScalingScenarioEvidence.toTick = $pausedTick
	}
	if ($measuredExplicitStepCount -gt 0 -or $RunColliderCookIncrementalMatrix -or $RunManagedReloadAutomationLoopback) {
		# A playing-frame step can already be complete while its immutable snapshot is
		# still one publication behind. One untimed Next drains that late publication;
		# measured requests begin only after the new paused snapshot is stable.
		$synchronizationFromTick = $pausedTick
		$synchronizationStep = Invoke-Tool -Name "play.step" -Arguments ([ordered]@{ expectedState = "paused"; correlationId = "installed-physics-stress-synchronize" })
		if ([int]$synchronizationStep.data.stepsCompleted -ne 1 -or $synchronizationStep.data.currentState -ne "paused") {
			throw "The paused publication synchronization request was not accepted as exactly one Next request."
		}
		$synchronizationDeadline = (Get-Date).AddSeconds([Math]::Min(30, $StartupTimeoutSeconds))
		$synchronizationObservedTick = $synchronizationFromTick
		while ($synchronizationObservedTick -le $synchronizationFromTick -and (Get-Date) -lt $synchronizationDeadline) {
			Start-Sleep -Milliseconds 1
			$synchronizationState = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.state")
			$synchronizationObservedTick = [UInt64]$synchronizationState.tick
		}
		if ($synchronizationObservedTick -le $synchronizationFromTick) {
			throw "The paused publication synchronization request produced no newer physics snapshot."
		}
		$pausedState = Wait-StablePausedPhysicsState
		$pausedTick = [UInt64]$pausedState.tick
		$workerScalingScenarioEvidence.synchronization = [ordered]@{
			requestedNextCount = 1; fromTick = $synchronizationFromTick; toTick = $pausedTick
			observedTickAdvance = $pausedTick - $synchronizationFromTick; purpose = "drain pre-pause publication lag before measured exact steps"
		}
	}
	Capture-DiagnosticsSummary -Phase "paused" -Force
	Complete-Stage -Name "pause"
	if ($RunColliderCookIncrementalMatrix) {
		Start-Stage -Name "collider-cookUnchangedWarmup"
		# One untimed paused step establishes the steady unchanged-authoring path.
		# The separately timed/profiler-captured loop below still contains exactly
		# ExplicitStepCount active steps.
		$unchangedFromTick = $pausedTick
		$unchangedStep = Invoke-Tool -Name "play.step" -Arguments ([ordered]@{ expectedState = "paused"; correlationId = "collider-cook-collider-cook-unchanged" })
		if ([int]$unchangedStep.data.stepsCompleted -ne 1 -or $unchangedStep.data.currentState -ne "paused") { throw "collider-cook matrix unchanged warmup did not complete exactly one paused frame." }
		$unchangedExpectedTick = $unchangedFromTick + 1
		$unchangedTick = $unchangedFromTick
		$unchangedDeadline = (Get-Date).AddSeconds([Math]::Min(30, $StartupTimeoutSeconds))
		while ($unchangedTick -lt $unchangedExpectedTick -and (Get-Date) -lt $unchangedDeadline) {
			Start-Sleep -Milliseconds 1
			$unchangedState = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.state")
			$unchangedTick = [UInt64]$unchangedState.tick
			if ($unchangedTick -gt $unchangedExpectedTick) { throw "collider-cook matrix unchanged warmup advanced more than one physics tick." }
		}
		if ($unchangedTick -ne $unchangedExpectedTick) { throw "collider-cook matrix unchanged warmup did not publish tick $unchangedExpectedTick." }
		$unchangedDiagnostics = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.diagnostics.get" -Arguments ([ordered]@{ includeFailures = $true }))
		if ((-not [bool]$unchangedDiagnostics.data.authoring.snapshotReused) -or
			([UInt64]$unchangedDiagnostics.data.authoring.changedObjects -ne 0) -or
			([UInt64]$unchangedDiagnostics.data.preparation.estimatedWorkItems -ne 0)) {
			throw "collider-cook matrix unchanged frame rebuilt authoring or preparation work instead of reusing the retained snapshot."
		}
		$colliderCookMatrix.unchanged = [ordered]@{
			fromTick = $unchangedFromTick; toTick = $unchangedTick
			snapshotReused = [bool]$unchangedDiagnostics.data.authoring.snapshotReused
			changedObjects = [UInt64]$unchangedDiagnostics.data.authoring.changedObjects
			preparationWorkItems = [UInt64]$unchangedDiagnostics.data.preparation.estimatedWorkItems
			extractionChangedBodies = [UInt64]$unchangedDiagnostics.data.extraction.changedBodies
			publicationEntries = [UInt64]$unchangedDiagnostics.data.publication.entries
		}
		$pausedTick = $unchangedTick
		Complete-Stage -Name "collider-cookUnchangedWarmup"
	}
	if ($RunManagedReloadAutomationLoopback) {
		Start-Stage -Name "managed-reload-automationCustomGravity"
		$customGravityArguments = [ordered]@{
			target = $managedReloadAutomationTarget; worldGeneration = $managedReloadAutomationWorldGeneration
			vector = @(0.0, -9.81, 0.0); applicationMode = "Acceleration"; applyOnNextStep = $true
			idempotencyKey = "managed-reload-automation-custom-gravity-once"; correlationId = "managed-reload-automation-custom-gravity"
		}
		$customGravity = Invoke-Tool -Name "physics.body.apply_force" -Arguments $customGravityArguments
		$customGravityReplay = Invoke-Tool -Name "physics.body.apply_force" -Arguments $customGravityArguments
		if (-not [bool]$customGravity.data.accepted -or [string]$customGravity.data.idempotencyDisposition -ne "new") {
			throw "managed-reload-automation custom-gravity command was not accepted as a new typed command."
		}
		if (-not [bool]$customGravityReplay.data.accepted -or [string]$customGravityReplay.data.idempotencyDisposition -ne "replay") {
			throw "managed-reload-automation duplicate custom-gravity command did not replay the retained receipt."
		}
		if ([UInt64]$customGravity.data.commandId -ne [UInt64]$customGravityReplay.data.commandId) {
			throw "managed-reload-automation idempotent custom-gravity replay returned a different command ID."
		}

		# Consume the queued acceleration at exactly one protected paused boundary.
		$customGravityFromTick = $pausedTick
		$customGravityStep = Invoke-Tool -Name "play.step" -Arguments ([ordered]@{ expectedState = "paused"; correlationId = "managed-reload-automation-custom-gravity-step" })
		if ([int]$customGravityStep.data.stepsCompleted -ne 1 -or $customGravityStep.data.currentState -ne "paused") {
			throw "managed-reload-automation custom-gravity Next request did not complete exactly one paused frame."
		}
		$customGravityExpectedTick = $customGravityFromTick + 1
		$customGravityAfterTick = $customGravityFromTick
		$customGravityDeadline = (Get-Date).AddSeconds([Math]::Min(30, $StartupTimeoutSeconds))
		while ($customGravityAfterTick -lt $customGravityExpectedTick -and (Get-Date) -lt $customGravityDeadline) {
			Start-Sleep -Milliseconds 1
			$customGravityState = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.state")
			$customGravityAfterTick = [UInt64]$customGravityState.tick
			if ($customGravityAfterTick -gt $customGravityExpectedTick) { throw "managed-reload-automation custom-gravity step advanced more than one physics tick." }
		}
		if ($customGravityAfterTick -ne $customGravityExpectedTick) { throw "managed-reload-automation custom-gravity step did not publish its expected physics tick." }
		$bodyAfter = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.body.inspect" -Arguments ([ordered]@{ target = $managedReloadAutomationTarget }))
		$beforeVerticalVelocity = [double]$bodyBefore.data.linearVelocity[1]
		$afterVerticalVelocity = [double]$bodyAfter.data.linearVelocity[1]
		$verticalDelta = $afterVerticalVelocity - $beforeVerticalVelocity
		# One -9.81 m/s^2 command over a 1/60 s step is about -0.1635 m/s.
		# This deliberately broad deterministic envelope admits damping/float error
		# while rejecting no application and an accidental duplicate application.
		if ($verticalDelta -ge -0.05 -or $verticalDelta -le -0.25) {
			throw "managed-reload-automation custom gravity changed vertical velocity by $verticalDelta m/s; expected exactly one acceleration application."
		}
		$pausedTick = $customGravityAfterTick
		$managedReloadAutomationLoopback.customGravity = [ordered]@{
			applicationMode = "Acceleration"; commandId = [UInt64]$customGravity.data.commandId
			firstDisposition = [string]$customGravity.data.idempotencyDisposition; replayDisposition = [string]$customGravityReplay.data.idempotencyDisposition
			fromTick = $customGravityFromTick; toTick = $customGravityAfterTick
			verticalVelocityBefore = $beforeVerticalVelocity; verticalVelocityAfter = $afterVerticalVelocity; verticalVelocityDelta = $verticalDelta
		}
		Complete-Stage -Name "managed-reload-automationCustomGravity"
	}
	$stepPublicationSamples = [System.Collections.Generic.List[object]]::new()
	if (-not $SkipProfilerCapture) {
		$profilerStartArguments = [ordered]@{ correlationId = "installed-physics-stress-baseline" }
		if ($RunProfilerCaptureChecks) {
			# New recordings always request the sole fresh V1 journal format.
			$profilerStartArguments.captureLevel = $CaptureLevel
			$profilerStartArguments.formatIdentifier = 'TerraEngineProfilerJournalV1'
		}
		$profilerCaptureStart = Invoke-Tool -Name "profiler.capture.start" -Arguments $profilerStartArguments
		$profilerCaptureStartedAtMs = Get-MonotonicMilliseconds
	}
	# Start the budgeted interval after profiler setup. This keeps persistence and
	# automation bookkeeping out of the measurement while the profiler still spans
	# every explicit physics step.
	Start-Stage -Name "explicitStepping"
	$explicitSteppingStartTick = $pausedTick
	$finalPhysicsState = $pausedState
	for ($step = 1; $step -le $measuredExplicitStepCount; ++$step) {
		$stepStartMs = Get-MonotonicMilliseconds
		$stepResult = Invoke-Tool -Name "play.step" -Arguments ([ordered]@{ expectedState = "paused"; correlationId = "installed-physics-stress-step-$step" })
		if ([int]$stepResult.data.stepsCompleted -ne 1 -or $stepResult.data.currentState -ne "paused") {
			throw "Explicit physics step $step did not report exactly one completed paused frame."
		}
		# physics.state is an immutable worker-prepared snapshot. A request can be
		# prepared immediately before the frame that consumes the Next token, so its
		# first response may legitimately contain the prior tick. Poll until the next
		# published snapshot appears, while failing immediately if more than one tick
		# is ever observed.
		$expectedTick = $pausedTick + 1
		$afterTick = $pausedTick
		$stepDeadline = (Get-Date).AddSeconds([Math]::Min(30, $StartupTimeoutSeconds))
		while ($afterTick -lt $expectedTick -and (Get-Date) -lt $stepDeadline) {
			# The installed editor commonly publishes the requested tick on its next
			# frame. Poll promptly so this evidence measures editor responsiveness
			# instead of adding a fixed ten seconds across one hundred Next requests.
			Start-Sleep -Milliseconds 1
			$afterStep = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.state")
			$afterTick = [UInt64]$afterStep.tick
			if ($afterTick -gt $expectedTick) {
				throw "Explicit physics step $step advanced from tick $pausedTick to $afterTick; only $expectedTick was allowed."
			}
		}
		if ($afterTick -ne $expectedTick) {
			throw "Explicit physics step $step did not publish tick $expectedTick before the timeout (last tick $afterTick)."
		}
		$finalPhysicsState = $afterStep
		$stepPublicationSamples.Add([ordered]@{ step = $step; fromTick = $pausedTick; toTick = $afterTick; durationMs = [Math]::Max(0.0, (Get-MonotonicMilliseconds) - $stepStartMs) })
		# Read diagnostics after this exact tick has published. The RPC executes
		# between fixed steps, while every retained duration was measured inside its
		# engine stage and therefore excludes editor startup, MCP, and project setup.
		$physicsDiagnostics = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.diagnostics.get")
		if ([UInt64]$physicsDiagnostics.tick -ne [UInt64]$afterTick) {
			throw "Physics diagnostics for explicit step $step reported tick $($physicsDiagnostics.tick), expected $afterTick."
		}
		if (-not [bool]$physicsDiagnostics.data.quality.applied -or
			[int]$physicsDiagnostics.data.quality.velocityIterations -ne $VelocityIterations -or
			[int]$physicsDiagnostics.data.quality.positionIterations -ne $PositionIterations) {
			throw "Physics diagnostics for explicit step $step did not report the requested $VelocityIterations/$PositionIterations quality profile."
		}
		$timings = $physicsDiagnostics.data.timings
		$stageDurations = [ordered]@{
			authoringPreparation = [double]$timings.authoringPreparationMs
			providerCommit = [double]$timings.providerCommitMs
			simulation = [double]$timings.simulationMs
			extraction = [double]$timings.extractionMs
			eventPublication = [double]$timings.eventPublicationMs
			stateValidation = [double]$timings.stateValidationMs
			snapshotConstruction = [double]$timings.snapshotConstructionMs
			transactionConstruction = [double]$timings.transactionConstructionMs
		}
		$physicsStageSamples.Add([ordered]@{
			step = $step; tick = [UInt64]$physicsDiagnostics.tick
			totalDurationMs = [double](@($stageDurations.Values) | Measure-Object -Sum).Sum
			stages = $stageDurations; quality = $physicsDiagnostics.data.quality; jobs = $physicsDiagnostics.data.jobs
		})
		# Keep a compact acceptance fact beside the detailed samples. The aggregate
		# runner uses this only after every measured step verified the requested
		# profile, so a partial diagnostics response cannot look like a quality leg.
		$qualityEvidence.available = $true
		$qualityEvidence.verifiedSteps = [int]$qualityEvidence.verifiedSteps + 1
		if ($script:latencySamples.Count -lt 4096) { $script:latencySamples.Add([ordered]@{ operation = "physics.step.publication"; durationMs = $stepPublicationSamples[$stepPublicationSamples.Count - 1].durationMs; step = $step; timestamp = (Get-Date).ToString("o") }) }
		$pausedTick = $afterTick
		Capture-EditorProcessMemory -Phase "paused-step" -Tick $pausedTick
	}
	if ($measuredExplicitStepCount -gt 0) {
		$qualityEvidence.applied = [bool]$qualityEvidence.available -and $qualityEvidence.verifiedSteps -eq $measuredExplicitStepCount
		$qualityEvidence.reason = if ($qualityEvidence.applied) { $null } else { "not every measured step retained an exact quality diagnostic" }
	}
	Complete-Stage -Name "explicitStepping"
	if ($RunColliderCookIncrementalMatrix) {
		$activeDiagnostics = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.diagnostics.get" -Arguments ([ordered]@{ includeFailures = $true }))
		if (([UInt64]$activeDiagnostics.data.extraction.totalBodies -lt [UInt64]$ObjectCount) -or
			([UInt64]$activeDiagnostics.data.extraction.activeBodies -lt 1) -or
			([UInt64]$activeDiagnostics.data.extraction.extractedBodies -lt 1)) {
			throw "collider-cook matrix active matrix did not retain the complete body view and selected active extraction work."
		}
		$colliderCookMatrix.active = [ordered]@{
		stepCount = $measuredExplicitStepCount; fromTick = $explicitSteppingStartTick; toTick = $pausedTick
			totalBodies = [UInt64]$activeDiagnostics.data.extraction.totalBodies
			activeBodies = [UInt64]$activeDiagnostics.data.extraction.activeBodies
			examinedBodies = [UInt64]$activeDiagnostics.data.extraction.examinedBodies
			extractedBodies = [UInt64]$activeDiagnostics.data.extraction.extractedBodies
			changedBodies = [UInt64]$activeDiagnostics.data.extraction.changedBodies
			statePages = [UInt64]$activeDiagnostics.data.stateStore.pages
			pagesReused = [UInt64]$activeDiagnostics.data.stateStore.pagesReused
			publicationEntries = [UInt64]$activeDiagnostics.data.publication.entries
			publicationChunks = [UInt64]$activeDiagnostics.data.publication.chunks
			targetBoundary = [string]$activeDiagnostics.data.publication.targetBoundary
		}
		$colliderCookMatrix.completed = $null -ne $colliderCookMatrix.reparent -and $null -ne $colliderCookMatrix.conflict -and $null -ne $colliderCookMatrix.stale -and $null -ne $colliderCookMatrix.unchanged -and $null -ne $colliderCookMatrix.active
		if (-not $colliderCookMatrix.completed) { throw "collider-cook matrix collider-cook matrix did not retain evidence for every required scenario." }
	}
	# Detailed scheduler diagnostics can take several frames to serialize for a
	# 10,000-body scene. Capture them once after the timed interval so observation
	# never becomes a periodic stall in the performance workload itself.
	Capture-DiagnosticsSummary -Phase "stepping-complete" -Force
	if ($RunManagedReloadAutomationLoopback) {
		# Read the immutable event history after the timed interval. Managed event
		# ordering/reload isolation is covered by the focused ABI integration tests;
		# this installed call proves the same published packet is live through MCP.
		# The runtime intentionally retains only two result ticks, so a fully settled
		# stress scene may have no contact in the final retained window even though it
		# produced contacts earlier. In that case, perform one explicit untimed probe
		# after the measured interval instead of weakening the event assertion.
		$eventProbe = [ordered]@{ performed = $false; reason = "retained stress-window contact" }
		$contacts = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.contacts.list" -Arguments ([ordered]@{
			worldGeneration = $managedReloadAutomationWorldGeneration; maximumContacts = 64
			fromTick = [UInt64][Math]::Max(0, [double]$pausedTick - 31.0); toTick = $pausedTick
		}))
		$contactCount = @($contacts.data.contacts).Count
		if ($contactCount -lt 1) {
			Start-Stage -Name "managed-reload-automationContactEventProbe"
			# Reuse the exact body generations discovered by the installed raycast.
			# Place that body just above the wide static floor, then give it enough
			# downward velocity to cross the small gap without tunnelling through its
			# own height. This creates a real contact manifold; spawning the body deeply
			# inside a solid would not guarantee a provider contact-added callback.
			$eventProbePosition = @([double]$managedReloadAutomationTargetPosition[0], 0.1, [double]$managedReloadAutomationTargetPosition[2])
			$teleport = Invoke-Tool -Name "physics.body.teleport" -Arguments ([ordered]@{
				target = $managedReloadAutomationTarget; worldGeneration = $managedReloadAutomationWorldGeneration; position = $eventProbePosition
				applyOnNextStep = $true; idempotencyKey = "managed-reload-automation-contact-probe-teleport"; correlationId = "managed-reload-automation-contact-probe-teleport"
			})
			if (-not [bool]$teleport.data.accepted) { throw "managed-reload-automation contact-event probe teleport was not accepted." }
			$velocity = Invoke-Tool -Name "physics.body.set_velocity" -Arguments ([ordered]@{
				target = $managedReloadAutomationTarget; worldGeneration = $managedReloadAutomationWorldGeneration; vector = @(0.0, -20.0, 0.0)
				applyOnNextStep = $true; idempotencyKey = "managed-reload-automation-contact-probe-velocity"; correlationId = "managed-reload-automation-contact-probe-velocity"
			})
			if (-not [bool]$velocity.data.accepted) { throw "managed-reload-automation contact-event probe velocity was not accepted." }

			$eventProbeFromTick = $pausedTick
			$eventProbeTicks = [System.Collections.Generic.List[object]]::new()
			for ($probeStep = 1; $probeStep -le 3 -and $contactCount -lt 1; ++$probeStep) {
				$eventProbeStep = Invoke-Tool -Name "play.step" -Arguments ([ordered]@{ expectedState = "paused"; correlationId = "managed-reload-automation-contact-probe-step-$probeStep" })
				if ([int]$eventProbeStep.data.stepsCompleted -ne 1 -or $eventProbeStep.data.currentState -ne "paused") {
					throw "managed-reload-automation contact-event probe step $probeStep did not complete exactly one paused frame."
				}
				$eventProbeExpectedTick = $pausedTick + 1
				$eventProbeTick = $pausedTick
				$eventProbeDeadline = (Get-Date).AddSeconds([Math]::Min(30, $StartupTimeoutSeconds))
				while ($eventProbeTick -lt $eventProbeExpectedTick -and (Get-Date) -lt $eventProbeDeadline) {
					Start-Sleep -Milliseconds 1
					$eventProbeState = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.state")
					$eventProbeTick = [UInt64]$eventProbeState.tick
					if ($eventProbeTick -gt $eventProbeExpectedTick) { throw "managed-reload-automation contact-event probe advanced more than one physics tick." }
				}
				if ($eventProbeTick -ne $eventProbeExpectedTick) { throw "managed-reload-automation contact-event probe did not publish tick $eventProbeExpectedTick." }
				$eventProbeTicks.Add([ordered]@{ step = $probeStep; fromTick = $pausedTick; toTick = $eventProbeTick })
				$pausedTick = $eventProbeTick
				$contacts = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.contacts.list" -Arguments ([ordered]@{
					worldGeneration = $managedReloadAutomationWorldGeneration; target = $managedReloadAutomationTarget; maximumContacts = 64
					fromTick = $eventProbeTick; toTick = $eventProbeTick
				}))
				$contactCount = @($contacts.data.contacts).Count
			}
			$eventProbe = [ordered]@{
				performed = $true; reason = "settled stress window contained no retained contact"
				fromTick = $eventProbeFromTick; toTick = $pausedTick; teleportCommandId = [UInt64]$teleport.data.commandId
				velocityCommandId = [UInt64]$velocity.data.commandId; position = $eventProbePosition; velocity = @(0.0, -20.0, 0.0)
				steps = @($eventProbeTicks); contactCount = $contactCount
			}
			Complete-Stage -Name "managed-reload-automationContactEventProbe"
		}
		if ($contactCount -lt 1) { throw "managed-reload-automation installed event snapshot contained no contacts after the 10,000-body stepping interval." }
		$managedReloadAutomationLoopback.events = [ordered]@{
			worldGeneration = $managedReloadAutomationWorldGeneration; tick = [UInt64]$contacts.tick
			contactCount = $contactCount; truncated = [bool]$contacts.truncated; overflow = [bool]$contacts.overflow; probe = $eventProbe
		}
		$managedReloadAutomationLoopback.completed = $true
	}
	if (-not $SkipProfilerCapture) {
		if ($RunProfilerCaptureChecks -and $null -ne $profilerCaptureStartedAtMs) {
			# The artifact gate measures a real five-second recording interval. If the
			# deterministic explicit-step workload completes sooner, keep Play Mode
			# paused and let ordinary editor frames fill the remaining wall-clock time.
			$captureDeadlineMs = [double]$profilerCaptureStartedAtMs + ($CaptureDurationSeconds * 1000.0)
			while ((Get-MonotonicMilliseconds) -lt $captureDeadlineMs) {
				if ($process.HasExited) { throw "Editor exited before the five-second profiler interval completed." }
				Start-Sleep -Milliseconds 10
			}
		}
		$profilerCaptureStoppedAtMs = Get-MonotonicMilliseconds
		$profilerCaptureStop = Invoke-Tool -Name "profiler.capture.stop" -Arguments ([ordered]@{ correlationId = "installed-physics-stress-baseline-stop" })
		$profilerStopJobId = [string]$profilerCaptureStop.data.jobId
		if ([string]::IsNullOrWhiteSpace($profilerStopJobId)) { throw "Profiler capture stop did not return a durable job ID." }
		$profilerDeadline = (Get-MonotonicMilliseconds) + ($StartupTimeoutSeconds * 1000.0)
		while ($true) {
			$profilerCaptureJobResponse = Invoke-Tool -Name "jobs.get" -Arguments ([ordered]@{ jobId = $profilerStopJobId })
			$profilerCaptureJob = $profilerCaptureJobResponse.data.job
			$profilerState = [string]$profilerCaptureJob.state
			if ($profilerState -eq "succeeded" -or $profilerState -eq "failed" -or $profilerState -eq "canceled") { break }
			if ((Get-MonotonicMilliseconds) -ge $profilerDeadline) { throw "Profiler capture persistence job did not reach a terminal state before the timeout." }
			Start-Sleep -Milliseconds 100
		}
		if ([string]$profilerCaptureJob.state -ne "succeeded") { throw "Profiler capture persistence job ended in state '$($profilerCaptureJob.state)'." }
		$profilerCaptureJobResult = Get-PropertyValue -Object $profilerCaptureJob -Name "result"
		$profilerCaptureArtifactId = [string](Get-PropertyValue -Object $profilerCaptureJobResult -Name "artifactId")
		if ([string]::IsNullOrWhiteSpace($profilerCaptureArtifactId)) {
			$profilerCaptureJobArtifacts = @(Get-PropertyValue -Object $profilerCaptureJob -Name "artifacts")
			$profilerCaptureArtifactId = [string](
				$profilerCaptureJobArtifacts |
					Where-Object { [string](Get-PropertyValue -Object $_ -Name "kind") -eq "profiler_capture" } |
					Select-Object -First 1 -ExpandProperty artifactId)
		}
		if ([string]::IsNullOrWhiteSpace($profilerCaptureArtifactId)) {
			throw "Profiler capture persistence succeeded without publishing its artifact identity."
		}
		$profilerAfter = Invoke-Tool -Name "profiler.capture.list"
		$profilerCaptureArtifact = @($profilerAfter.data.artifacts | Where-Object { [string]$_.artifactId -eq $profilerCaptureArtifactId })
		if ($profilerCaptureArtifact.Count -ne 1) { throw "Profiler capture stop succeeded but its exact artifact identity was not listed once." }
		$profilerCaptureArtifact = $profilerCaptureArtifact[0]
		# Summary reads are intentionally served from the loaded immutable journal.
		# Load the exact artifact and wait for its durable job before asking for the
		# summary; otherwise a fresh editor process correctly reports no active V1.
		$load = Invoke-Tool -Name "profiler.capture.load" -Arguments ([ordered]@{ artifactId = [string]$profilerCaptureArtifact.artifactId; correlationId = "installed-profiler-capture-load" })
		$loadJobId = [string]$load.data.jobId
		if ([string]::IsNullOrWhiteSpace($loadJobId)) { throw "Profiler capture load did not return a durable job ID." }
		$loadState = "queued"
		$loadDeadline = (Get-MonotonicMilliseconds) + ($StartupTimeoutSeconds * 1000.0)
		while ($true) {
			$loadJob = (Invoke-Tool -Name "jobs.get" -Arguments ([ordered]@{ jobId = $loadJobId })).data.job
			$loadState = [string]$loadJob.state
			if ($loadState -in @("succeeded", "failed", "canceled")) { break }
			if ((Get-MonotonicMilliseconds) -ge $loadDeadline) { $loadState = "timeout"; break }
			Start-Sleep -Milliseconds 100
		}
		if ($loadState -ne "succeeded") { throw "Profiler capture load finished in '$loadState' instead of succeeded." }
		$profilerCapture.recoveryProbe = [ordered]@{ attempted = $true; supported = $true; status = $loadState; artifactId = [string]$profilerCaptureArtifact.artifactId }
		$profilerCaptureSummary = Invoke-Tool -Name "profiler.capture.summary" -Arguments ([ordered]@{ artifactId = [string]$profilerCaptureArtifact.artifactId })
		$profilerCaptureSucceeded = $true
		[void]$unavailableMetrics.Remove("profilerCaptureUnlessGeneratedByEditor")
		Complete-ProfilerCaptureEvidence -Summary $profilerCaptureSummary.data -Artifact $profilerCaptureArtifact -ArtifactRoot $projectRoot
		if ($RunProfilerCaptureChecks -and $null -ne $profilerCapture.artifactBytes -and -not $profilerCapture.sizeGatePassed) {
			$profilerCaptureGateError = "profiler-capture profiler artifact exceeded the 10 MiB five-second size gate ($($profilerCapture.artifactBytes) bytes)."
		}
		if ($RunProfilerCaptureChecks) {
			if (-not $profilerCapture.durationGatePassed) { $profilerCaptureGateError = "profiler-capture profiler capture did not span the required five-second wall-clock interval." }
			elseif (-not $profilerCapture.formatAccepted) { $profilerCaptureGateError = "profiler-capture format was not TerraEngineProfilerJournalV1." }
			elseif (-not $profilerCapture.metricAvailability.gatePassed) { $profilerCaptureGateError = "profiler-capture profiler summary omitted one or more required typed evidence streams." }
			elseif (-not $profilerCapture.recoveryProbe.supported -or $profilerCapture.recoveryProbe.status -ne "succeeded") { $profilerCaptureGateError = "profiler-capture profiler committed-artifact load/recovery probe did not succeed." }
		}
	}
	# Capture the post-workload Scene View state, not only the startup snapshot.
	# Project loading and active-membership publication can still be settling when
	# MCP first becomes reachable; the final sample proves the profile interval
	# exercised the mature marker cache, spatial query, and presentation stages.
	$sceneViewFinalState = Invoke-Tool -Name "scene_view.state"
	$sceneViewEvidence.startupState = $sceneViewEvidence.state
	$sceneViewEvidence.state = $sceneViewFinalState.data.state

	Start-Stage -Name "playExitAndRestore"
	$exit = Invoke-Tool -Name "play.exit" -Arguments ([ordered]@{ expectedState = "paused"; correlationId = "installed-physics-stress-exit" })
	Capture-EditorProcessMemory -Phase "play-exit" -Tick $pausedTick
	if (-not $exit.data.restoration.completed -or @($exit.data.restoration.partialFailures).Count -ne 0) {
		throw "Play Mode exit did not report complete authored-state restoration."
	}
	$playState = Invoke-Tool -Name "play.state"
	if ($playState.data.currentState -ne "stopped") { throw "Editor did not return to stopped Play Mode after restoration." }
	if ($RunAuthoringWalkthrough) {
		# Undo every walkthrough-owned history entry, then persist the restored scene.
		# This keeps the disposable proof self-cleaning and lets the editor close
		# normally without an unsaved-changes prompt masking lifecycle evidence.
		$cleanupUndoCount = 0
		$cleanupState = Invoke-Tool -Name "history.state"
		while ([int]$cleanupState.data.undoDepth -gt [int]$authoringWalkthrough.initialHistoryDepth) {
			[void](Invoke-Tool -Name "history.undo" -Arguments ([ordered]@{ expectedSequence = [UInt64]$cleanupState.data.sequence }))
			$cleanupUndoCount++
			if ($cleanupUndoCount -gt 32) { throw "Authoring walkthrough cleanup exceeded the bounded history depth." }
			$cleanupState = Invoke-Tool -Name "history.state"
		}
		$leftovers = Invoke-Tool -Name "scene.list_objects" -Arguments ([ordered]@{ search = "Authoring Walkthrough" })
		if (@($leftovers.data.objects).Count -ne 0) { throw "Authoring walkthrough cleanup left temporary authoring objects in the scene." }
		$sceneSave = Invoke-Tool -Name "scene.save"
		if ([bool]$sceneSave.data.dirty) { throw "Authoring walkthrough cleanup save did not clear the active scene dirty state." }
		$authoringWalkthrough.cleanupUndoCount = $cleanupUndoCount
		$authoringWalkthrough.cleanupCompleted = $true
	}
	$authoredSceneHashAfter = (Get-FileHash -LiteralPath $scenePath -Algorithm SHA256).Hash
	if ($authoredSceneHashAfter -ne $authoredSceneHashBefore) {
		throw "The authored stress scene changed during Play Mode. Runtime transforms must not dirty the saved scene."
	}
	$process.Refresh()
	if ($process.HasExited) { throw "Editor exited unexpectedly before the harness could verify final process health." }
	if ($ReadinessMode -ne "none") {
		$readiness.final = Capture-ReadinessEvidence -Phase "post-play-restoration"
		$sourceChangedOnlyWhenRequested = if ($ReadinessMode -eq "stale") {
			$readiness.preparation.sourceSha256Before -ne $readiness.preparation.sourceSha256After
		} else {
			$readiness.preparation.sourceSha256Before -eq $readiness.preparation.sourceSha256After
		}
		$sourcePreservedDuringEditorRun = $readiness.final.authoredSceneSha256 -eq $authoredSceneHashBefore
		$readiness.sourceAuthority = [ordered]@{
			sourceChangedOnlyForStale = $sourceChangedOnlyWhenRequested
			sourcePreservedDuringEditorRun = $sourcePreservedDuringEditorRun
			sourceSha256AtStartup = $authoredSceneHashBefore; sourceSha256AfterRun = $readiness.final.authoredSceneSha256
		}
		if (-not $sourceChangedOnlyWhenRequested -or -not $sourcePreservedDuringEditorRun) {
			throw "readiness source-authority evidence failed; inspect the per-scenario readiness summary."
		}
	}
	Complete-Stage -Name "playExitAndRestore"
	if (-not [string]::IsNullOrWhiteSpace($profilerCaptureGateError)) {
		# Defer profiler acceptance failure until Play Mode has restored authored
		# state. The finally block can then verify a normal editor shutdown even
		# when a strict evidence gate identifies an incomplete capture.
		throw $profilerCaptureGateError
	}

	$runSucceeded = $true
	# Installed startup can legitimately reach a different absolute tick before the
	# pause request. Hash the authoritative authored input plus exact post-pause step
	# deltas and published work volume; full simulation-state hashes remain covered
	# by the fresh-process PhysicsDeterminismStress worker matrix.
	$determinismEvidence = [ordered]@{
		version = 1; basis = "authored-scene-exact-step-publication"
		scenario = $WorkerScalingScenario; authoredSceneSha256 = $authoredSceneHashBefore
		objectCount = $ObjectCount; activeDynamicCount = $dynamicCount
		explicitStepCount = $measuredExplicitStepCount
		explicitStepDeltas = @($stepPublicationSamples | ForEach-Object { [UInt64]$_.toTick - [UInt64]$_.fromTick })
		publishedBodyCount = [UInt64]$finalPhysicsState.data.bodyCount
	}
	$determinismPayload = $determinismEvidence | ConvertTo-Json -Depth 12 -Compress
	$determinismHash = ([System.Security.Cryptography.SHA256]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($determinismPayload)) | ForEach-Object { $_.ToString("x2") }) -join ""
	Write-JsonFile -Path $summaryPath -Value ([ordered]@{
		success = $true; startedAt = $startTime.ToString("o"); completedAt = (Get-Date).ToString("o")
		monotonicDurationMs = Get-MonotonicMilliseconds; buildConfiguration = "RelWithDebInfo"; requestedWorkerCount = $WorkerCount; actualWorkerCount = $resolvedWorkerCount; workerIdentity = $workerIdentity; thresholdOverride = [ordered]@{ parallelPreparation = $ParallelPreparationThreshold; parallelExtraction = $ParallelExtractionThreshold }; determinismHash = $determinismHash; determinismEvidence = $determinismEvidence
		canonicalFixture = $canonicalFixtureEvidence; qualityEvidence = $qualityEvidence
		installRoot = $resolvedInstallRoot; editorExecutable = $editorExecutable; projectRoot = $projectRoot
		objectCount = $ObjectCount; activeDynamicCount = $dynamicCount; dynamicCount = $dynamicCount; kinematicCount = $kinematicCount; staticCount = $staticCount
		mcpEndpoint = "http://127.0.0.1:$McpPort/mcp"; explicitStepCount = $measuredExplicitStepCount; requestedExplicitStepCount = $ExplicitStepCount
		firstLiveTick = $previousTick; finalPausedTick = $pausedTick; processId = $process.Id
		authoredSceneSha256Before = $authoredSceneHashBefore; authoredSceneSha256After = $authoredSceneHashAfter
		observations = @($observations); diagnosticsSnapshots = @($diagnosticSnapshots); automationLatencySamples = @($latencySamples)
		authoringWalkthrough = $authoringWalkthrough
		colliderCookMatrix = $colliderCookMatrix
		workerScalingScenario = $workerScalingScenarioEvidence
		sceneView = $sceneViewEvidence
		managedReloadAutomationLoopback = $managedReloadAutomationLoopback
		managedReloadAutomationEvidenceCompleteness = $managedReloadAutomationEvidenceCompleteness
		readiness = $readiness
		stages = $stageRecords; automationLatency = Get-LatencySummary -Samples @($latencySamples)
		explicitStepPublication = @($stepPublicationSamples); physicsStageSamples = @($physicsStageSamples); hardware = $hardwareEvidence
		profilerCapture = [ordered]@{ start = $profilerCaptureStart; stop = $profilerCaptureStop; job = $profilerCaptureJob; artifact = $profilerCaptureArtifact; summary = $profilerCaptureSummary; succeeded = $profilerCaptureSucceeded }
		profilerEvidence = $profilerCapture
		processMemorySamples = @($processMemorySamples)
		unavailableMetrics = @($unavailableMetrics)
		artifactInventory = Get-ArtifactInventory -Root $projectRoot; forcedTermination = $false
	})
}
catch {
	Write-JsonFile -Path $summaryPath -Value ([ordered]@{
		success = $false; startedAt = $startTime.ToString("o"); failedAt = (Get-Date).ToString("o")
		monotonicDurationMs = Get-MonotonicMilliseconds; buildConfiguration = "RelWithDebInfo"; requestedWorkerCount = $WorkerCount; actualWorkerCount = $resolvedWorkerCount; workerIdentity = $workerIdentity; thresholdOverride = [ordered]@{ parallelPreparation = $ParallelPreparationThreshold; parallelExtraction = $ParallelExtractionThreshold }; determinismHash = $determinismHash
		canonicalFixture = $canonicalFixtureEvidence; qualityEvidence = $qualityEvidence
		installRoot = $resolvedInstallRoot; workRoot = $resolvedWorkRoot; objectCount = $ObjectCount
		error = $_.Exception.Message; processId = if ($null -ne $process) { $process.Id } else { $null }
		stages = $stageRecords; automationLatency = Get-LatencySummary -Samples @($latencySamples); automationLatencySamples = @($latencySamples); hardware = $hardwareEvidence
		diagnosticsSnapshots = @($diagnosticSnapshots); physicsStageSamples = @($physicsStageSamples); artifactInventory = @()
		authoringWalkthrough = $authoringWalkthrough
		colliderCookMatrix = $colliderCookMatrix
		workerScalingScenario = $workerScalingScenarioEvidence
		sceneView = $sceneViewEvidence
		managedReloadAutomationLoopback = $managedReloadAutomationLoopback
		managedReloadAutomationEvidenceCompleteness = $managedReloadAutomationEvidenceCompleteness
		readiness = $readiness
		profilerCapture = [ordered]@{ start = $profilerCaptureStart; stop = $profilerCaptureStop; job = $profilerCaptureJob; artifact = $profilerCaptureArtifact; summary = $profilerCaptureSummary; succeeded = $profilerCaptureSucceeded }
		profilerEvidence = $profilerCapture
		processMemorySamples = @($processMemorySamples)
		unavailableMetrics = @($unavailableMetrics); forcedTermination = $false
	})
	throw
}
finally {
	Start-Stage -Name "editorShutdown"
	if ($null -ne $process) {
		$process.Refresh()
		if (-not $process.HasExited) {
			# Prefer normal window shutdown so the editor can release jobs and restore
			# its own lifecycle state. A forced stop is recorded only as a last resort.
			[void]$process.CloseMainWindow()
			# A forced managed build can leave background compilation cleanup pending
			# after the window closes. Give the editor enough time to drain it normally.
			if (-not $process.WaitForExit(60000)) {
				$forcedTermination = $true
				Stop-Process -Id $process.Id -Force
				$process.WaitForExit()
			}
		}
	}
	Complete-Stage -Name "editorShutdown"
	if (Test-Path -LiteralPath $summaryPath) {
		$summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
		$summary.forcedTermination = $forcedTermination
		$summary.monotonicDurationMs = Get-MonotonicMilliseconds
		$summary.stages = $stageRecords
		$summary.automationLatency = Get-LatencySummary -Samples @($latencySamples)
		$summary.automationLatencySamples = @($latencySamples)
		$summary.diagnosticsSnapshots = @($diagnosticSnapshots)
		$summary.physicsStageSamples = @($physicsStageSamples)
		$summary.canonicalFixture = $canonicalFixtureEvidence
		$summary.qualityEvidence = $qualityEvidence
		$summary.colliderCookMatrix = $colliderCookMatrix
		$summary.workerScalingScenario = $workerScalingScenarioEvidence
		$artifactInventory = @(Get-ArtifactInventory -Root $projectRoot)
		$summary.artifactInventory = $artifactInventory
		$summary.profilerCapture = [ordered]@{ start = $profilerCaptureStart; stop = $profilerCaptureStop; job = $profilerCaptureJob; artifact = $profilerCaptureArtifact; summary = $profilerCaptureSummary; succeeded = $profilerCaptureSucceeded }
		$summary.profilerCapture = $profilerCapture
		$summary.processMemorySamples = @($processMemorySamples)
		$summary.unavailableMetrics = @($unavailableMetrics)
		if ($ReadinessMode -ne "none") {
			# Shutdown is recorded after CloseMainWindow/WaitForExit, so it distinguishes
			# a clean lifecycle from the emergency force-stop fallback.
			$readiness.normalShutdown = (-not $forcedTermination -and $null -ne $process -and $process.HasExited)
			$readiness.finalArtifactInventory = $artifactInventory
			$summary.readiness = $readiness
		}
		$completenessFailure = $false
		if ($RunManagedReloadAutomationLoopback -and [bool]$summary.success) {
			$stepDeltas = @($stepPublicationSamples | ForEach-Object { [UInt64]$_.toTick - [UInt64]$_.fromTick })
			$evidenceStepDeltas = @($determinismEvidence.explicitStepDeltas)
			$exactSteps = $measuredExplicitStepCount -gt 0 -and $stepDeltas.Count -eq $measuredExplicitStepCount -and @($stepDeltas | Where-Object { $_ -ne 1 }).Count -eq 0 -and ($stepDeltas -join ",") -eq ($evidenceStepDeltas -join ",")
			$stateEvidence = -not [string]::IsNullOrWhiteSpace([string]$determinismHash) -and $null -ne $determinismEvidence -and -not [string]::IsNullOrWhiteSpace([string]$determinismEvidence.authoredSceneSha256)
			$rollbackEvidence = [bool]$colliderCookMatrix.completed -and $null -ne $colliderCookMatrix.conflict -and -not [bool]$colliderCookMatrix.conflict.partialVisibility -and $null -ne $colliderCookMatrix.stale -and -not [bool]$colliderCookMatrix.stale.partialVisibility
			$visibilityEvidence = $null -ne $colliderCookMatrix.reparent -and -not [string]::IsNullOrWhiteSpace([string]$colliderCookMatrix.reparent.visibilityBoundary) -and $null -ne $colliderCookMatrix.active -and -not [string]::IsNullOrWhiteSpace([string]$colliderCookMatrix.active.targetBoundary)
			$normalShutdown = -not $forcedTermination -and $null -ne $process -and $process.HasExited
			$unavailableEvidence = $unavailableMetrics.Count -gt 0 -and @($unavailableMetrics | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count -eq 0
			$artifactEvidence = $artifactInventory.Count -gt 0
			$checks = [ordered]@{
				exactMeasuredStepDeltas = $exactSteps; measuredStepCount = $measuredExplicitStepCount; stepDeltas = $stepDeltas
				deterministicStateEvidence = $stateEvidence; determinismHash = $determinismHash
				rollbackEvidence = $rollbackEvidence; visibilityEvidence = $visibilityEvidence
				normalEditorShutdown = $normalShutdown; unavailableMetricEvidence = $unavailableEvidence
				unavailableMetrics = @($unavailableMetrics); artifactInventoryNonempty = $artifactEvidence; artifactCount = $artifactInventory.Count
				managedReloadAutomationLoopbackComplete = [bool]$managedReloadAutomationLoopback.completed
			}
			$managedReloadAutomationEvidenceCompleteness = [ordered]@{
				requested = $true
				complete = $exactSteps -and $stateEvidence -and $rollbackEvidence -and $visibilityEvidence -and $normalShutdown -and $unavailableEvidence -and $artifactEvidence -and [bool]$managedReloadAutomationLoopback.completed
				checks = $checks
			}
			$summary.managedReloadAutomationEvidenceCompleteness = $managedReloadAutomationEvidenceCompleteness
			if (-not $managedReloadAutomationEvidenceCompleteness.complete) {
				$completenessFailure = $true
				$summary.success = $false
				$summary | Add-Member -NotePropertyName failedAt -NotePropertyValue (Get-Date).ToString("o") -Force
				$summary | Add-Member -NotePropertyName error -NotePropertyValue "managed-reload-automation evidence completeness gate failed; inspect managedReloadAutomationEvidenceCompleteness.checks." -Force
			}
		}
		elseif ($RunManagedReloadAutomationLoopback) {
			$managedReloadAutomationEvidenceCompleteness = [ordered]@{ requested = $true; complete = $false; checks = $null; notEvaluatedReason = "run failed before final evidence validation" }
			$summary.managedReloadAutomationEvidenceCompleteness = $managedReloadAutomationEvidenceCompleteness
		}
		Write-JsonFile -Path $summaryPath -Value $summary
		if ($ReadinessMode -ne "none") {
			# Preserve each scenario even though the reusable project's normal summary
			# path is intentionally overwritten by the next matrix child.
			Write-JsonFile -Path $readinessScenarioSummaryPath -Value $summary -Depth 64
		}
		if ($completenessFailure) { throw "managed-reload-automation evidence completeness gate failed; inspect '$summaryPath'." }
	}
}
