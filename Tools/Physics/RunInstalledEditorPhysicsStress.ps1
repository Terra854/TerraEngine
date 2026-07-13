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
#     -InstallRoot out/build/Debug/Build -AllowLiveStress

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

	[ValidateRange(30, 600)]
	[int] $StartupTimeoutSeconds = 120,

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
$process = $null
$forcedTermination = $false
$runSucceeded = $false
$rpcId = 0

try {
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
		layers = @($layers)
	})

	$dynamicCount = [int][Math]::Floor($ObjectCount * 0.70)
	$kinematicCount = [int][Math]::Floor($ObjectCount * 0.10)
	$staticCount = $ObjectCount - $dynamicCount - $kinematicCount
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
			useGravity = $isDynamic; allowSleeping = $true; startAwake = -not $isStaticBody; continuousCollisionMode = "Discrete"
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
	}
	$scene = [ordered]@{
		name = "Installed Physics Stress"
		sceneSettings = [ordered]@{ indirectStrength = 1.0; ambientOcclusionStrength = 1.0; ambientOcclusionQuality = 2 }
		objects = @($objects)
	}
	Write-JsonFile -Path $scenePath -Value $scene -Depth 32
	$authoredSceneHashBefore = (Get-FileHash -LiteralPath $scenePath -Algorithm SHA256).Hash

	$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
	$startInfo.FileName = $editorExecutable
	$startInfo.Arguments = '--project "{0}"' -f $projectRoot.Replace('"', '\"')
	$startInfo.WorkingDirectory = $editorDirectory
	$startInfo.UseShellExecute = $false
	$startInfo.EnvironmentVariables["TERRAENGINE_EDITOR_PREFS_PATH"] = $preferencesPath
	$startInfo.EnvironmentVariables["TERRAENGINE_SUPPRESS_PROJECT_RECENTS"] = "1"
	$process = [System.Diagnostics.Process]::Start($startInfo)
	if ($null -eq $process) { throw "The installed TerraEngine editor process could not be started." }

	function Invoke-McpRpc {
		param([Parameter(Mandatory = $true)][string] $Method, $Params = @{})
		$script:rpcId++
		$request = [ordered]@{ jsonrpc = "2.0"; id = $script:rpcId; method = $Method; params = $Params }
		try {
			# Windows PowerShell 5.1 otherwise tries to initialize the retired Internet
			# Explorer parser and can throw before exposing a valid loopback response.
			$response = Invoke-WebRequest -UseBasicParsing -Uri ("http://127.0.0.1:{0}/mcp" -f $McpPort) -Method Post -ContentType "application/json" -Body ($request | ConvertTo-Json -Depth 32 -Compress) -TimeoutSec 30
			$parsed = $response.Content | ConvertFrom-Json
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

	# Verify the exact available capability surface before mutating Play Mode.
	$tools = Invoke-McpRpc -Method "tools/list"
	$toolNames = @($tools.result.tools | ForEach-Object { $_.name })
	foreach ($requiredTool in @("play.enter", "play.pause", "play.step", "play.exit", "physics.state", "physics.diagnostics.get", "diagnostics.get_summary")) {
		if ($toolNames -notcontains $requiredTool) { throw "Installed editor MCP does not advertise required tool '$requiredTool'." }
	}

	function Invoke-Tool {
		param([Parameter(Mandatory = $true)][string] $Name, $Arguments = @{})
		return Get-ToolEnvelope -RpcResponse (Invoke-McpRpc -Method "tools/call" -Params ([ordered]@{ name = $Name; arguments = $Arguments })) -Operation $Name
	}

	# A clean project performs one initial managed build after the MCP endpoint is
	# already available. Entering Play Mode during that build is valid, but the
	# script compiler then pauses gameplay for its reload boundary and races the
	# harness's explicit pause request. Poll the immutable diagnostics surface so
	# the following Play/Pause/Next checks measure physics after startup work.
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
	$initialPlayState = Invoke-Tool -Name "play.state"
	if ($initialPlayState.data.currentState -ne "stopped") {
		throw "The clean editor was not stopped after its initial managed build."
	}

	$enter = Invoke-Tool -Name "play.enter" -Arguments ([ordered]@{ expectedState = "stopped"; correlationId = "installed-physics-stress-enter" })
	$observations = [System.Collections.Generic.List[object]]::new()
	$physicsReady = $false
	$previousTick = [UInt64]0
	$deadline = (Get-Date).AddSeconds($StartupTimeoutSeconds)
	while ((Get-Date) -lt $deadline) {
		$process.Refresh()
		if ($process.HasExited) { throw "Editor exited while waiting for live physics observations. ExitCode=$($process.ExitCode)" }
		$state = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.state")
		$diagnostics = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.diagnostics.get")
		$bodyCount = [UInt64]$state.data.bodyCount
		$tick = [UInt64]$state.tick
		$observations.Add([ordered]@{ timestamp = (Get-Date).ToString("o"); tick = $tick; bodyCount = $bodyCount; diagnostics = $diagnostics.data })
		if ($tick -gt $previousTick -and $bodyCount -ge $ObjectCount) { $previousTick = $tick; $physicsReady = $true; break }
		$previousTick = $tick
		Start-Sleep -Milliseconds 250
	}
	if (-not $physicsReady) { throw "Physics did not publish advancing ticks with at least $ObjectCount bodies before the timeout." }

	$pause = Invoke-Tool -Name "play.pause" -Arguments ([ordered]@{ expectedState = "playing"; correlationId = "installed-physics-stress-pause" })
	$pausedState = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.state")
	$pausedTick = [UInt64]$pausedState.tick
	for ($step = 1; $step -le $ExplicitStepCount; ++$step) {
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
			Start-Sleep -Milliseconds 100
			$afterStep = Get-PhysicsSnapshot -Envelope (Invoke-Tool -Name "physics.state")
			$afterTick = [UInt64]$afterStep.tick
			if ($afterTick -gt $expectedTick) {
				throw "Explicit physics step $step advanced from tick $pausedTick to $afterTick; only $expectedTick was allowed."
			}
		}
		if ($afterTick -ne $expectedTick) {
			throw "Explicit physics step $step did not publish tick $expectedTick before the timeout (last tick $afterTick)."
		}
		$pausedTick = $afterTick
	}

	$exit = Invoke-Tool -Name "play.exit" -Arguments ([ordered]@{ expectedState = "paused"; correlationId = "installed-physics-stress-exit" })
	if (-not $exit.data.restoration.completed -or @($exit.data.restoration.partialFailures).Count -ne 0) {
		throw "Play Mode exit did not report complete authored-state restoration."
	}
	$playState = Invoke-Tool -Name "play.state"
	if ($playState.data.currentState -ne "stopped") { throw "Editor did not return to stopped Play Mode after restoration." }
	$authoredSceneHashAfter = (Get-FileHash -LiteralPath $scenePath -Algorithm SHA256).Hash
	if ($authoredSceneHashAfter -ne $authoredSceneHashBefore) {
		throw "The authored stress scene changed during Play Mode. Runtime transforms must not dirty the saved scene."
	}
	$process.Refresh()
	if ($process.HasExited) { throw "Editor exited unexpectedly before the harness could verify final process health." }

	$runSucceeded = $true
	Write-JsonFile -Path $summaryPath -Value ([ordered]@{
		success = $true; startedAt = $startTime.ToString("o"); completedAt = (Get-Date).ToString("o")
		installRoot = $resolvedInstallRoot; editorExecutable = $editorExecutable; projectRoot = $projectRoot
		objectCount = $ObjectCount; dynamicCount = $dynamicCount; kinematicCount = $kinematicCount; staticCount = $staticCount
		mcpEndpoint = "http://127.0.0.1:$McpPort/mcp"; explicitStepCount = $ExplicitStepCount
		firstLiveTick = $previousTick; finalPausedTick = $pausedTick; processId = $process.Id
		authoredSceneSha256Before = $authoredSceneHashBefore; authoredSceneSha256After = $authoredSceneHashAfter
		observations = @($observations); forcedTermination = $false
	})
}
catch {
	Write-JsonFile -Path $summaryPath -Value ([ordered]@{
		success = $false; startedAt = $startTime.ToString("o"); failedAt = (Get-Date).ToString("o")
		installRoot = $resolvedInstallRoot; workRoot = $resolvedWorkRoot; objectCount = $ObjectCount
		error = $_.Exception.Message; processId = if ($null -ne $process) { $process.Id } else { $null }; forcedTermination = $false
	})
	throw
}
finally {
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
	if (Test-Path -LiteralPath $summaryPath) {
		$summary = Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json
		$summary.forcedTermination = $forcedTermination
		Write-JsonFile -Path $summaryPath -Value $summary
	}
}
