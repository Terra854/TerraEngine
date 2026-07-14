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

[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)]
	[ValidateNotNullOrEmpty()]
	[string] $InstallRoot,

	[ValidateNotNullOrEmpty()]
	[string] $WorkRoot = "out/validation/installed-editor-physics-stress",

	[ValidateRange(10000, 50000)]
	[int] $ObjectCount = 10000,

	[ValidateRange(1024, 65535)]
	[int] $McpPort = 43127,

	[ValidateRange(1, 100)]
	[int] $ExplicitStepCount = 100,

	# The committed acceptance manifest requires exactly 1,000 concurrently
	# active dynamic bodies. Callers may raise this for an explicit overload run,
	# but the default live gate stays comparable to that source-controlled fixture.
	[ValidateRange(1000, 10000)]
	[int] $ActiveDynamicCount = 1000,

	[ValidateRange(30, 600)]
	[int] $StartupTimeoutSeconds = 120,

	[ValidateSet("0", "1", "2", "4", "max")]
	[string] $WorkerCount = "max",

	# T113 adds public editor authoring operations before Play Mode. Keep this
	# optional so T105/T111 performance measurements remain directly comparable.
	[switch] $RunAuthoringWalkthrough,

	# T129 exercises the installed managed-reload and public physics automation
	# contracts. Its extra query and command calls remain outside the timed stress
	# interval so the T105/T111 performance measurements stay comparable.
	[switch] $RunUs6Loopback,

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

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../.."))
$outRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot "out"))
$resolvedInstallRoot = Get-AbsolutePath -Path $InstallRoot -BasePath $repoRoot
$resolvedWorkRoot = Get-AbsolutePath -Path $WorkRoot -BasePath $repoRoot
Assert-PathBelow -Parent $outRoot -Child $resolvedWorkRoot

$editorDirectory = Join-Path $resolvedInstallRoot "Editor"
$editorExecutable = Join-Path $editorDirectory "TerraEngine-Editor.exe"
if (-not (Test-Path -LiteralPath $editorExecutable -PathType Leaf)) {
	throw "Installed editor executable was not found: '$editorExecutable'. Run the CMake install target first."
}
if (-not (Test-LoopbackPortAvailable -Port $McpPort)) {
	throw "MCP port $McpPort is already occupied. Choose a free explicit -McpPort; the editor intentionally does not randomize this endpoint."
}

if (Test-Path -LiteralPath $resolvedWorkRoot) {
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
$projectSettingsPath = Join-Path $projectRoot "ProjectSettings/ProjectSettings.terra"
$physicsSettingsPath = Join-Path $projectRoot "ProjectSettings/PhysicsSettings.json"
$lastScenePath = Join-Path $projectRoot "Library/LastScene.txt"
$startTime = Get-Date
$monotonicFrequency = [double][System.Diagnostics.Stopwatch]::Frequency
$monotonicStartTicks = [System.Diagnostics.Stopwatch]::GetTimestamp()
$stageStarts = @{}
$stageRecords = [ordered]@{}
$latencySamples = [System.Collections.Generic.List[object]]::new()
$diagnosticSnapshots = [System.Collections.Generic.List[object]]::new()
$processMemorySamples = [System.Collections.Generic.List[object]]::new()
$profilerCaptureStart = $null
$profilerCaptureStop = $null
$profilerCaptureJob = $null
$profilerCaptureArtifact = $null
$profilerCaptureSummary = $null
$profilerCaptureSucceeded = $false
$authoringWalkthrough = [ordered]@{ requested = [bool]$RunAuthoringWalkthrough; completed = $false }
$us6Loopback = [ordered]@{
	requested = [bool]$RunUs6Loopback; completed = $false
	managedReload = $null; runtimeDiscovery = $null; customGravity = $null; events = $null
}
$unavailableMetrics = [System.Collections.Generic.List[string]]::new()
$unavailableMetrics.Add("nativePhysicsStagePercentiles")
$unavailableMetrics.Add("workerUtilizationHistory")
$unavailableMetrics.Add("protectedBoundaryStallHistory")
$unavailableMetrics.Add("frameResponsiveness")
$unavailableMetrics.Add("profilerCaptureUnlessGeneratedByEditor")
$lastDiagnosticsCaptureMs = -5000.0
$hardwareEvidence = Get-HardwareEvidence
$resolvedWorkerCount = if ($WorkerCount -eq "max" -or $WorkerCount -eq "0") { [int][Math]::Max(1, [Environment]::ProcessorCount) } else { [Math]::Min([int]$WorkerCount, [int][Math]::Max(1, [Environment]::ProcessorCount)) }
$process = $null
$forcedTermination = $false
$runSucceeded = $false
$rpcId = 0

try {
	Start-Stage -Name "projectAndSceneGeneration"
	New-Item -ItemType Directory -Force -Path (Split-Path -Parent $scenePath), (Split-Path -Parent $projectSettingsPath), (Split-Path -Parent $lastScenePath), $evidenceRoot | Out-Null

	# These are the exact editor-wide preference keys consumed by ProjectManager.
	# Supplying them via TERRAENGINE_EDITOR_PREFS_PATH prevents this live run from
	# changing the user's normal MCP enablement, port, or recent-project history.
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
	})
	Write-JsonFile -Path $projectSettingsPath -Value ([ordered]@{ projectDisplayName = "Installed Physics Stress" })
	Write-JsonFile -Path (Join-Path $projectRoot "game.json") -Value ([ordered]@{ name = "Installed Physics Stress"; version = "1.0.0" })
	# Pin the startup scene instead of depending on filesystem enumeration order.
	# The editor resolves this path relative to the project's Assets directory.
	Set-Content -LiteralPath $lastScenePath -Value "Scenes/PhysicsStress.tescene" -Encoding utf8

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
		debugVisualizationEnabled = [bool]$EnablePhysicsDebugVisualization
		retainedDebugTicks = 2
		maxDebugPrimitives = 100000
		layers = @($layers)
	})

	$dynamicCount = $ActiveDynamicCount
	$kinematicCount = 1000
	if (($dynamicCount + $kinematicCount) -ge $ObjectCount) {
		throw "ActiveDynamicCount plus the 1,000-body kinematic coverage must leave at least one static body."
	}
	$staticCount = $ObjectCount - $dynamicCount - $kinematicCount
	# Keep one high dynamic body free of built-in gravity during the optional US6
	# run. A mass-independent acceleration command is then the only source of its
	# first downward velocity, making idempotent custom-gravity behavior observable.
	$us6TargetIndex = [int][Math]::Min($dynamicCount - 1, 1950)
	$us6TargetId = Get-StableGuid -Value ([UInt64]($us6TargetIndex + 1))
	$us6TargetPosition = $null
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
			useGravity = $isDynamic -and -not ($RunUs6Loopback -and $index -eq $us6TargetIndex); allowSleeping = $true; startAwake = -not $isStaticBody; continuousCollisionMode = "Discrete"
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
		if ($index -eq $us6TargetIndex) { $us6TargetPosition = @($position) }
	}
	$scene = [ordered]@{
		name = "Installed Physics Stress"
		sceneSettings = [ordered]@{ indirectStrength = 1.0; ambientOcclusionStrength = 1.0; ambientOcclusionQuality = 2 }
		objects = @($objects)
	}
	Write-JsonFile -Path $scenePath -Value $scene -Depth 32
	$authoredSceneHashBefore = (Get-FileHash -LiteralPath $scenePath -Algorithm SHA256).Hash
	Complete-Stage -Name "projectAndSceneGeneration"

	Start-Stage -Name "processStartupAndMcpReadiness"
	$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
	$startInfo.FileName = $editorExecutable
	$startInfo.Arguments = '--project "{0}"' -f $projectRoot.Replace('"', '\"')
	$startInfo.WorkingDirectory = $editorDirectory
	$startInfo.UseShellExecute = $false
	$startInfo.EnvironmentVariables["TERRAENGINE_EDITOR_PREFS_PATH"] = $preferencesPath
	$startInfo.EnvironmentVariables["TERRAENGINE_SUPPRESS_PROJECT_RECENTS"] = "1"
	# The native pool consumes this validated process-local override at startup;
	# zero requests the normal hardware-concurrency default.
	$startInfo.EnvironmentVariables["TERRAENGINE_THREADPOOL_WORKERS"] = if ($WorkerCount -eq "max") { "0" } else { $WorkerCount }
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
	$requiredTools = @("play.enter", "play.pause", "play.step", "play.exit", "physics.state", "physics.diagnostics.get", "diagnostics.get_summary", "profiler.capture.start", "profiler.capture.stop", "profiler.capture.list", "profiler.capture.summary", "jobs.get")
	if ($RunAuthoringWalkthrough) {
		$requiredTools += @("object.create", "object.set_metadata", "scene.list_objects", "scene.save", "transaction.execute", "history.state", "history.undo", "history.redo", "prefab.create", "prefab.instantiate", "prefab.overrides.inspect", "prefab.overrides.apply", "prefab.overrides.revert")
	}
	if ($RunUs6Loopback) {
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

	if ($RunUs6Loopback) {
		Start-Stage -Name "us6ManagedReload"
		# Use the public durable compilation job rather than relying solely on the
		# editor's implicit clean-project build. This proves the installed managed
		# assembly can cross a second reload boundary in RelWithDebInfo.
		$compile = Invoke-Tool -Name "script.compile" -Arguments ([ordered]@{ profile = "RelWithDebInfo"; correlationId = "t129-managed-reload" })
		$compileJobId = [string]$compile.data.jobId
		if ([string]::IsNullOrWhiteSpace($compileJobId)) { throw "T129 script.compile did not return a durable job ID." }
		$compileJob = Wait-AutomationJob -JobId $compileJobId -Operation "T129 managed reload"
		if ([string]$compileJob.correlationId -ne "t129-managed-reload") { throw "T129 managed reload lost its correlation ID." }
		$us6Loopback.managedReload = [ordered]@{
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
		Complete-Stage -Name "us6ManagedReload"
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
		$source = Invoke-Tool -Name "object.create" -Arguments ([ordered]@{ name = "T113 Prefab Source" })
		$sourceId = [string]$source.data.object.uuid
		if ([string]::IsNullOrWhiteSpace($sourceId)) { throw "T113 object.create did not return a stable source UUID." }

		$metadata = Invoke-Tool -Name "transaction.execute" -Arguments ([ordered]@{
			operations = @(
				[ordered]@{ kind = "object.set_metadata"; target = $sourceId; name = "T113 Authored Source" },
				[ordered]@{ kind = "object.set_metadata"; target = $sourceId; tag = "PhysicsWalkthrough" }
			)
			label = "T113 authored metadata"
			correlationId = "t113-authoring-metadata"
			idempotencyKey = "t113-authoring-metadata"
		})
		$historyAfterCommit = Invoke-Tool -Name "history.state"
		if (-not $historyAfterCommit.data.canUndo) { throw "T113 metadata transaction did not publish an undo entry." }

		$undo = Invoke-Tool -Name "history.undo" -Arguments ([ordered]@{ expectedSequence = [UInt64]$historyAfterCommit.data.sequence })
		$afterUndo = Invoke-Tool -Name "scene.list_objects" -Arguments ([ordered]@{ search = "T113 Prefab Source" })
		$undoMatches = @($afterUndo.data.objects | Where-Object { $_.uuid -eq $sourceId -and $_.name -eq "T113 Prefab Source" })
		if ($undoMatches.Count -ne 1) { throw "T113 undo did not restore the exact source-object name." }

		$redo = Invoke-Tool -Name "history.redo" -Arguments ([ordered]@{ expectedSequence = [UInt64]$undo.data.sequence })
		$afterRedo = Invoke-Tool -Name "scene.list_objects" -Arguments ([ordered]@{ search = "T113 Authored Source" })
		$redoMatches = @($afterRedo.data.objects | Where-Object { $_.uuid -eq $sourceId -and $_.name -eq "T113 Authored Source" -and $_.tag -eq "PhysicsWalkthrough" })
		if ($redoMatches.Count -ne 1) { throw "T113 redo did not restore the exact committed metadata." }

		$prefabPath = "Assets/Prefabs/T113PhysicsWalkthrough.teprefab"
		$prefab = Invoke-Tool -Name "prefab.create" -Arguments ([ordered]@{ object = $sourceId; path = $prefabPath })
		$prefabId = [string]$prefab.data.prefabId
		if ([string]::IsNullOrWhiteSpace($prefabId)) { throw "T113 prefab.create did not return a stable prefab identity." }
		$instance = Invoke-Tool -Name "prefab.instantiate" -Arguments ([ordered]@{ prefab = $prefabId; name = "T113 Prefab Instance" })
		$instanceId = [string]$instance.data.instanceId
		if ([string]::IsNullOrWhiteSpace($instanceId)) { throw "T113 prefab.instantiate did not return a stable instance identity." }

		[void](Invoke-Tool -Name "object.set_metadata" -Arguments ([ordered]@{ object = $instanceId; name = "T113 Applied Override" }))
		$overridesBeforeApply = Invoke-Tool -Name "prefab.overrides.inspect" -Arguments ([ordered]@{ instance = $instanceId })
		$applyCount = @($overridesBeforeApply.data.overrides).Count
		if ($applyCount -lt 1) { throw "T113 instance metadata did not appear as a structured prefab override." }
		$applied = Invoke-Tool -Name "prefab.overrides.apply" -Arguments ([ordered]@{ instance = $instanceId; expectedPrefabGeneration = [UInt64]$overridesBeforeApply.data.prefabGeneration })
		if ([string]::IsNullOrWhiteSpace([string]$applied.data.historyId)) { throw "T113 prefab override apply did not publish one history identity." }

		# Author a second difference after apply so revert proves a real state change,
		# rather than merely confirming that the already-applied override list is empty.
		[void](Invoke-Tool -Name "object.set_metadata" -Arguments ([ordered]@{ object = $instanceId; name = "T113 Revert Candidate" }))
		$overridesBeforeRevert = Invoke-Tool -Name "prefab.overrides.inspect" -Arguments ([ordered]@{ instance = $instanceId })
		$revertCount = @($overridesBeforeRevert.data.overrides).Count
		if ($revertCount -lt 1) { throw "T113 second instance edit did not create an override for revert." }
		$reverted = Invoke-Tool -Name "prefab.overrides.revert" -Arguments ([ordered]@{ instance = $instanceId })
		if ([int]$reverted.data.remainingOverrideCount -ne 0) { throw "T113 prefab revert left authored overrides behind." }
		$revertedInstance = Invoke-Tool -Name "scene.list_objects" -Arguments ([ordered]@{ search = "T113 Applied Override" })
		$revertedMatches = @($revertedInstance.data.objects | Where-Object { $_.uuid -eq $instanceId -and $_.name -eq "T113 Applied Override" })
		if ($revertedMatches.Count -ne 1) { throw "T113 prefab revert did not restore the current source state." }

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

	if ($RunUs6Loopback) {
		Start-Stage -Name "us6RuntimeDiscovery"
		$us6WorldGeneration = [UInt64]$state.worldGeneration
		# Discover the target through a real bounded query job. The high, gravity-
		# disabled fixture body is the first hit along this vertical ray, and the hit
		# supplies the exact component generation needed by every later command.
		$raycast = Invoke-Tool -Name "physics.raycast" -Arguments ([ordered]@{
			worldGeneration = $us6WorldGeneration
			origin = @([double]$us6TargetPosition[0], 100.0, [double]$us6TargetPosition[2])
			direction = @(0.0, -1.0, 0.0); distance = 200.0; maximumHits = 64
			batchSize = 1; layerMask = [UInt32]::MaxValue; includeTriggers = $false
			correlationId = "t129-target-raycast"
		})
		$raycastJobId = [string]$raycast.data.jobId
		if ([string]::IsNullOrWhiteSpace($raycastJobId)) { throw "T129 physics.raycast did not return a durable query job ID." }
		$raycastJob = Wait-AutomationJob -JobId $raycastJobId -Operation "T129 target raycast" -PollTool "physics.query_job.get"
		$targetHits = @($raycastJob.result.hits | Where-Object { [string]$_.objectId -eq $us6TargetId })
		if ($targetHits.Count -ne 1) { throw "T129 target raycast did not return the dedicated custom-gravity body exactly once." }
		$targetHit = $targetHits[0]
		$us6Target = [ordered]@{
			objectId = $us6TargetId
			componentGeneration = [UInt64]$targetHit.componentGeneration
			worldGeneration = $us6WorldGeneration
		}
		$bodyBefore = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.body.inspect" -Arguments ([ordered]@{ target = $us6Target }))
		$diagnosticsBefore = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.diagnostics.get" -Arguments ([ordered]@{ includeFailures = $true }))
		if (@($diagnosticsBefore.data.scheduler).Count -lt 1) { throw "T129 physics diagnostics did not expose scheduler declarations." }
		$us6Loopback.runtimeDiscovery = [ordered]@{
			worldGeneration = $us6WorldGeneration; target = $us6Target; raycastJobId = $raycastJobId
			raycastState = [string]$raycastJob.state; raycastHitCount = @($raycastJob.result.hits).Count
			bodyBefore = $bodyBefore; schedulerDeclarationCount = @($diagnosticsBefore.data.scheduler).Count
		}
		Complete-Stage -Name "us6RuntimeDiscovery"
	}

	Start-Stage -Name "pause"
	$pause = Invoke-Tool -Name "play.pause" -Arguments ([ordered]@{ expectedState = "playing"; correlationId = "installed-physics-stress-pause" })
	$pausedState = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.state")
	$pausedTick = [UInt64]$pausedState.tick
	Capture-DiagnosticsSummary -Phase "paused" -Force
	Complete-Stage -Name "pause"
	if ($RunUs6Loopback) {
		Start-Stage -Name "us6CustomGravity"
		$customGravityArguments = [ordered]@{
			target = $us6Target; worldGeneration = $us6WorldGeneration
			vector = @(0.0, -9.81, 0.0); applicationMode = "Acceleration"; applyOnNextStep = $true
			idempotencyKey = "t129-custom-gravity-once"; correlationId = "t129-custom-gravity"
		}
		$customGravity = Invoke-Tool -Name "physics.body.apply_force" -Arguments $customGravityArguments
		$customGravityReplay = Invoke-Tool -Name "physics.body.apply_force" -Arguments $customGravityArguments
		if (-not [bool]$customGravity.data.accepted -or [string]$customGravity.data.idempotencyDisposition -ne "new") {
			throw "T129 custom-gravity command was not accepted as a new typed command."
		}
		if (-not [bool]$customGravityReplay.data.accepted -or [string]$customGravityReplay.data.idempotencyDisposition -ne "replay") {
			throw "T129 duplicate custom-gravity command did not replay the retained receipt."
		}
		if ([UInt64]$customGravity.data.commandId -ne [UInt64]$customGravityReplay.data.commandId) {
			throw "T129 idempotent custom-gravity replay returned a different command ID."
		}

		# Consume the queued acceleration at exactly one protected paused boundary.
		$customGravityFromTick = $pausedTick
		$customGravityStep = Invoke-Tool -Name "play.step" -Arguments ([ordered]@{ expectedState = "paused"; correlationId = "t129-custom-gravity-step" })
		if ([int]$customGravityStep.data.stepsCompleted -ne 1 -or $customGravityStep.data.currentState -ne "paused") {
			throw "T129 custom-gravity Next request did not complete exactly one paused frame."
		}
		$customGravityExpectedTick = $customGravityFromTick + 1
		$customGravityAfterTick = $customGravityFromTick
		$customGravityDeadline = (Get-Date).AddSeconds([Math]::Min(30, $StartupTimeoutSeconds))
		while ($customGravityAfterTick -lt $customGravityExpectedTick -and (Get-Date) -lt $customGravityDeadline) {
			Start-Sleep -Milliseconds 1
			$customGravityState = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.state")
			$customGravityAfterTick = [UInt64]$customGravityState.tick
			if ($customGravityAfterTick -gt $customGravityExpectedTick) { throw "T129 custom-gravity step advanced more than one physics tick." }
		}
		if ($customGravityAfterTick -ne $customGravityExpectedTick) { throw "T129 custom-gravity step did not publish its expected physics tick." }
		$bodyAfter = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.body.inspect" -Arguments ([ordered]@{ target = $us6Target }))
		$beforeVerticalVelocity = [double]$bodyBefore.data.linearVelocity[1]
		$afterVerticalVelocity = [double]$bodyAfter.data.linearVelocity[1]
		$verticalDelta = $afterVerticalVelocity - $beforeVerticalVelocity
		# One -9.81 m/s^2 command over a 1/60 s step is about -0.1635 m/s.
		# This deliberately broad deterministic envelope admits damping/float error
		# while rejecting no application and an accidental duplicate application.
		if ($verticalDelta -ge -0.05 -or $verticalDelta -le -0.25) {
			throw "T129 custom gravity changed vertical velocity by $verticalDelta m/s; expected exactly one acceleration application."
		}
		$pausedTick = $customGravityAfterTick
		$us6Loopback.customGravity = [ordered]@{
			applicationMode = "Acceleration"; commandId = [UInt64]$customGravity.data.commandId
			firstDisposition = [string]$customGravity.data.idempotencyDisposition; replayDisposition = [string]$customGravityReplay.data.idempotencyDisposition
			fromTick = $customGravityFromTick; toTick = $customGravityAfterTick
			verticalVelocityBefore = $beforeVerticalVelocity; verticalVelocityAfter = $afterVerticalVelocity; verticalVelocityDelta = $verticalDelta
		}
		Complete-Stage -Name "us6CustomGravity"
	}
	$stepPublicationSamples = [System.Collections.Generic.List[object]]::new()
	$profilerBeforeIds = @()
	if (-not $SkipProfilerCapture) {
		$profilerBefore = Invoke-Tool -Name "profiler.capture.list"
		$profilerBeforeIds = @($profilerBefore.data.artifacts | ForEach-Object { [string]$_.artifactId })
		$profilerCaptureStart = Invoke-Tool -Name "profiler.capture.start" -Arguments ([ordered]@{ correlationId = "installed-physics-stress-baseline" })
	}
	# Start the budgeted interval after profiler setup. This keeps persistence and
	# automation bookkeeping out of the measurement while the profiler still spans
	# every explicit physics step.
	Start-Stage -Name "explicitStepping"
	for ($step = 1; $step -le $ExplicitStepCount; ++$step) {
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
		$stepPublicationSamples.Add([ordered]@{ step = $step; fromTick = $pausedTick; toTick = $afterTick; durationMs = [Math]::Max(0.0, (Get-MonotonicMilliseconds) - $stepStartMs) })
		if ($script:latencySamples.Count -lt 4096) { $script:latencySamples.Add([ordered]@{ operation = "physics.step.publication"; durationMs = $stepPublicationSamples[$stepPublicationSamples.Count - 1].durationMs; step = $step; timestamp = (Get-Date).ToString("o") }) }
		$pausedTick = $afterTick
		Capture-EditorProcessMemory -Phase "paused-step" -Tick $pausedTick
	}
	Complete-Stage -Name "explicitStepping"
	# Detailed scheduler diagnostics can take several frames to serialize for a
	# 10,000-body scene. Capture them once after the timed interval so observation
	# never becomes a periodic stall in the performance workload itself.
	Capture-DiagnosticsSummary -Phase "stepping-complete" -Force
	if ($RunUs6Loopback) {
		# Read the immutable event history after the timed interval. Managed event
		# ordering/reload isolation is covered by the focused ABI integration tests;
		# this installed call proves the same published packet is live through MCP.
		$contacts = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.contacts.list" -Arguments ([ordered]@{
			worldGeneration = $us6WorldGeneration; maximumContacts = 64
			fromTick = [UInt64][Math]::Max(0, [double]$pausedTick - 31.0); toTick = $pausedTick
		}))
		$contactCount = @($contacts.data.contacts).Count
		if ($contactCount -lt 1) { throw "T129 installed event snapshot contained no contacts after the 10,000-body stepping interval." }
		$us6Loopback.events = [ordered]@{
			worldGeneration = $us6WorldGeneration; tick = [UInt64]$contacts.tick
			contactCount = $contactCount; truncated = [bool]$contacts.truncated; overflow = [bool]$contacts.overflow
		}
		$us6Loopback.completed = $true
	}
	if (-not $SkipProfilerCapture) {
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
		$profilerAfter = Invoke-Tool -Name "profiler.capture.list"
		$profilerCaptureArtifact = @($profilerAfter.data.artifacts | Where-Object { $profilerBeforeIds -notcontains [string]$_.artifactId } | Select-Object -First 1)
		if ($profilerCaptureArtifact.Count -ne 1) { throw "Profiler capture stop succeeded but did not publish exactly one new artifact." }
		$profilerCaptureArtifact = $profilerCaptureArtifact[0]
		$profilerCaptureSummary = Invoke-Tool -Name "profiler.capture.summary" -Arguments ([ordered]@{ artifactId = [string]$profilerCaptureArtifact.artifactId })
		$profilerCaptureSucceeded = $true
		[void]$unavailableMetrics.Remove("profilerCaptureUnlessGeneratedByEditor")
	}

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
			if ($cleanupUndoCount -gt 32) { throw "T113 cleanup exceeded the bounded walkthrough history depth." }
			$cleanupState = Invoke-Tool -Name "history.state"
		}
		$leftovers = Invoke-Tool -Name "scene.list_objects" -Arguments ([ordered]@{ search = "T113" })
		if (@($leftovers.data.objects).Count -ne 0) { throw "T113 cleanup left temporary authoring objects in the scene." }
		$sceneSave = Invoke-Tool -Name "scene.save"
		if ([bool]$sceneSave.data.dirty) { throw "T113 cleanup save did not clear the active scene dirty state." }
		$authoringWalkthrough.cleanupUndoCount = $cleanupUndoCount
		$authoringWalkthrough.cleanupCompleted = $true
	}
	$authoredSceneHashAfter = (Get-FileHash -LiteralPath $scenePath -Algorithm SHA256).Hash
	if ($authoredSceneHashAfter -ne $authoredSceneHashBefore) {
		throw "The authored stress scene changed during Play Mode. Runtime transforms must not dirty the saved scene."
	}
	$process.Refresh()
	if ($process.HasExited) { throw "Editor exited unexpectedly before the harness could verify final process health." }
	Complete-Stage -Name "playExitAndRestore"

	$runSucceeded = $true
	Write-JsonFile -Path $summaryPath -Value ([ordered]@{
		success = $true; startedAt = $startTime.ToString("o"); completedAt = (Get-Date).ToString("o")
		monotonicDurationMs = Get-MonotonicMilliseconds; buildConfiguration = "RelWithDebInfo"; requestedWorkerCount = $WorkerCount; actualWorkerCount = $resolvedWorkerCount
		installRoot = $resolvedInstallRoot; editorExecutable = $editorExecutable; projectRoot = $projectRoot
		objectCount = $ObjectCount; activeDynamicCount = $dynamicCount; dynamicCount = $dynamicCount; kinematicCount = $kinematicCount; staticCount = $staticCount
		mcpEndpoint = "http://127.0.0.1:$McpPort/mcp"; explicitStepCount = $ExplicitStepCount
		firstLiveTick = $previousTick; finalPausedTick = $pausedTick; processId = $process.Id
		authoredSceneSha256Before = $authoredSceneHashBefore; authoredSceneSha256After = $authoredSceneHashAfter
		observations = @($observations); diagnosticsSnapshots = @($diagnosticSnapshots); automationLatencySamples = @($latencySamples)
		authoringWalkthrough = $authoringWalkthrough
		us6Loopback = $us6Loopback
		stages = $stageRecords; automationLatency = Get-LatencySummary -Samples @($latencySamples)
		explicitStepPublication = @($stepPublicationSamples); hardware = $hardwareEvidence
		profilerCapture = [ordered]@{ start = $profilerCaptureStart; stop = $profilerCaptureStop; job = $profilerCaptureJob; artifact = $profilerCaptureArtifact; summary = $profilerCaptureSummary; succeeded = $profilerCaptureSucceeded }
		processMemorySamples = @($processMemorySamples)
		unavailableMetrics = @($unavailableMetrics)
		artifactInventory = Get-ArtifactInventory -Root $projectRoot; forcedTermination = $false
	})
}
catch {
	Write-JsonFile -Path $summaryPath -Value ([ordered]@{
		success = $false; startedAt = $startTime.ToString("o"); failedAt = (Get-Date).ToString("o")
		monotonicDurationMs = Get-MonotonicMilliseconds; buildConfiguration = "RelWithDebInfo"; requestedWorkerCount = $WorkerCount; actualWorkerCount = $resolvedWorkerCount
		installRoot = $resolvedInstallRoot; workRoot = $resolvedWorkRoot; objectCount = $ObjectCount
		error = $_.Exception.Message; processId = if ($null -ne $process) { $process.Id } else { $null }
		stages = $stageRecords; automationLatency = Get-LatencySummary -Samples @($latencySamples); automationLatencySamples = @($latencySamples); hardware = $hardwareEvidence
		diagnosticsSnapshots = @($diagnosticSnapshots); artifactInventory = @()
		authoringWalkthrough = $authoringWalkthrough
		us6Loopback = $us6Loopback
		profilerCapture = [ordered]@{ start = $profilerCaptureStart; stop = $profilerCaptureStop; job = $profilerCaptureJob; artifact = $profilerCaptureArtifact; summary = $profilerCaptureSummary; succeeded = $profilerCaptureSucceeded }
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
			if (-not $process.WaitForExit(15000)) {
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
		$summary.artifactInventory = Get-ArtifactInventory -Root $projectRoot
		$summary.profilerCapture = [ordered]@{ start = $profilerCaptureStart; stop = $profilerCaptureStop; job = $profilerCaptureJob; artifact = $profilerCaptureArtifact; summary = $profilerCaptureSummary; succeeded = $profilerCaptureSucceeded }
		$summary.processMemorySamples = @($processMemorySamples)
		$summary.unavailableMetrics = @($unavailableMetrics)
		Write-JsonFile -Path $summaryPath -Value $summary
	}
}
