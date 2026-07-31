[CmdletBinding()]
param(
	[string]$InstalledEditor = 'out/build/RelWithDebInfo/Build/Editor/TerraEngine-Editor.exe',
	[string]$Project = 'Tests/Fixtures/Animation/US1Import',
	[string]$Asset = '',
	[string]$Preferences = 'out/validation/virtual-geometry-phase0-live/editor-preferences.json',
	[string]$OutputDirectory = 'out/validation/virtual-geometry-phase0-live',
	[string]$Prefab = 'Assets/Models/US1Hero_Generated/US1Hero.teprefab',
	[string]$PrefabName = 'Feature 045 Phase 0 Hero',
	[string]$PrefabChildObject = '',
	[double]$PrefabScale = 1.0,
	[int]$PrefabInstanceCount = 1,
	[int]$PrefabRequestedAnimationMode = -1,
	[double]$PrefabSpacing = 0.0,
	[string]$ScenePath = '',
	[switch]$CreateValidationScene,
	[switch]$CleanupValidationScene,
	[string]$ValidationScenePath = 'Assets/Scenes/VirtualGeometryLive.tescene',
	[double[]]$ValidationCameraPosition = @(0.0, 0.5, 1.0),
	[int]$StreamingSettleMilliseconds = 250,
	[int]$McpPort = 43155,
	[int]$StartupTimeoutSeconds = 90,
	[int]$CaptureTimeoutMilliseconds = 60000,
	[string]$ValidationRoute = '',
	[ValidateSet('Game', 'Scene')]
	[string]$DiagnosticsViewType = 'Game',
	[switch]$EnterPlayMode,
	[switch]$ExpectAnimatedChange
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$utf8NoBom = [Text.UTF8Encoding]::new($false)

function Resolve-RepoPath([string]$Path)
{
	if([IO.Path]::IsPathRooted($Path))
	{
		return [IO.Path]::GetFullPath($Path)
	}
	return [IO.Path]::GetFullPath((Join-Path $repo $Path))
}

function Get-PropertyValue($Value, [string]$Name)
{
	if($null -eq $Value)
	{
		return $null
	}
	$property = $Value.PSObject.Properties[$Name]
	if($null -eq $property)
	{
		return $null
	}
	return $property.Value
}

$editorPath = Resolve-RepoPath $InstalledEditor
$projectPath = Resolve-RepoPath $Project
$preferencesPath = Resolve-RepoPath $Preferences
$outputRoot = Resolve-RepoPath $OutputDirectory
foreach($required in @($editorPath, $preferencesPath))
{
	if(-not (Test-Path -LiteralPath $required -PathType Leaf))
	{
		throw "Required installed live-test input is missing: $required"
	}
}
if(-not (Test-Path -LiteralPath $projectPath -PathType Container))
{
	throw "Required live-test project is missing: $projectPath"
}
if($PrefabInstanceCount -lt 1)
{
	throw 'PrefabInstanceCount must be at least one.'
}
$validationRouteName = $ValidationRoute.Trim().ToLowerInvariant()
if($validationRouteName -notin @('', 'hardware', 'compute', 'indexed', 'hybrid'))
{
	throw "ValidationRoute must be hardware, compute, indexed, hybrid, or empty; received '$ValidationRoute'."
}
[IO.Directory]::CreateDirectory($outputRoot) | Out-Null
# Live capture must be reproducible even when the developer's base preference
# file has MCP disabled. Preserve that file and write a run-local copy whose
# listener settings match the endpoint used below.
$runPreferences = Get-Content -Raw -LiteralPath $preferencesPath |
	ConvertFrom-Json
$runPreferences | Add-Member -NotePropertyName editorMcpEnabled `
	-NotePropertyValue $true -Force
$runPreferences | Add-Member -NotePropertyName editorMcpListenPort `
	-NotePropertyValue $McpPort -Force
$runPreferences | Add-Member -NotePropertyName editorMcpWriteModeDefault `
	-NotePropertyValue 'writes_enabled' -Force
$runPreferences | Add-Member -NotePropertyName editorMcpRecentSession `
	-NotePropertyValue 'virtual-geometry-live-visual' -Force
$preferencesPath = Join-Path $outputRoot 'editor-preferences.json'
[IO.File]::WriteAllText(
	$preferencesPath,
	($runPreferences | ConvertTo-Json -Depth 64),
	$utf8NoBom)
$transcriptPath = Join-Path $outputRoot 'mcp-transcript.jsonl'
$stdoutPath = Join-Path $outputRoot 'installed-editor.stdout.log'
$stderrPath = Join-Path $outputRoot 'installed-editor.stderr.log'
$endpoint = "http://127.0.0.1:$McpPort/mcp"
$sequence = 0
[IO.File]::WriteAllText($transcriptPath, '', $utf8NoBom)

function Add-Transcript($Value)
{
	[IO.File]::AppendAllText(
		$transcriptPath,
		($Value | ConvertTo-Json -Depth 64 -Compress) + "`n",
		$utf8NoBom)
}

function Invoke-McpRpc([string]$Method, $Params = @{})
{
	$script:sequence++
	$request = [ordered]@{
		jsonrpc = '2.0'
		id = $script:sequence
		method = $Method
		params = $Params
	}
	try
	{
		$reply = (
			Invoke-WebRequest `
				-UseBasicParsing `
				-Uri $endpoint `
				-Method Post `
				-ContentType 'application/json' `
				-Body ($request | ConvertTo-Json -Depth 64 -Compress) `
				-TimeoutSec 30
		).Content | ConvertFrom-Json
		Add-Transcript ([ordered]@{
			sequence = $script:sequence
			timestamp = [DateTime]::UtcNow.ToString('o')
			request = $request
			response = $reply
		})
		if($null -ne (Get-PropertyValue $reply 'error'))
		{
			throw "MCP returned an error for '$Method'."
		}
		return $reply
	}
	catch
	{
		Add-Transcript ([ordered]@{
			sequence = $script:sequence
			timestamp = [DateTime]::UtcNow.ToString('o')
			request = $request
			error = $_.Exception.Message
		})
		throw
	}
}

function Get-McpEnvelope($Reply, [string]$Tool)
{
	$result = Get-PropertyValue $Reply 'result'
	$content = @((Get-PropertyValue $result 'content'))
	$textItems = @($content | Where-Object {
		[string](Get-PropertyValue $_ 'type') -eq 'text'
	})
	if($textItems.Count -ne 1)
	{
		throw "MCP tool '$Tool' did not return exactly one typed envelope."
	}
	return [string](Get-PropertyValue $textItems[0] 'text') | ConvertFrom-Json
}

function Invoke-McpTool([string]$Name, $Arguments = @{})
{
	$reply = Invoke-McpRpc 'tools/call' ([ordered]@{
		name = $Name
		arguments = $Arguments
	})
	$envelope = Get-McpEnvelope $reply $Name
	if(-not [bool](Get-PropertyValue $envelope 'ok'))
	{
		throw "MCP tool '$Name' reported ok=false with status '$([string](Get-PropertyValue $envelope 'status'))'."
	}
	return $envelope
}

function Invoke-McpToolWithReadinessRetry([string]$Name, $Arguments = @{})
{
	# The listener can become available a few frames before Game-view statistics
	# publish their first completed target. Retry only that documented startup
	# race; every other MCP rejection still fails the live test immediately.
	for($attempt = 0; $attempt -lt 20; ++$attempt)
	{
		$reply = Invoke-McpRpc 'tools/call' ([ordered]@{
			name = $Name
			arguments = $Arguments
		})
		$envelope = Get-McpEnvelope $reply $Name
		if([bool](Get-PropertyValue $envelope 'ok'))
		{
			return $envelope
		}
		$errorValue = Get-PropertyValue $envelope 'error'
		$category = [string](Get-PropertyValue $errorValue 'category')
		if($Name -ne 'renderer.virtualGeometry.stats' -or
			$category -ne 'missing_target')
		{
			throw "MCP tool '$Name' reported ok=false with status '$([string](Get-PropertyValue $envelope 'status'))'."
		}
		Start-Sleep -Milliseconds 250
	}
	return Invoke-McpTool $Name $Arguments
}

function Invoke-McpPngCapture(
	[string]$Name,
	$Arguments,
	[string]$OutputPath)
{
	$reply = Invoke-McpRpc 'tools/call' ([ordered]@{
		name = $Name
		arguments = $Arguments
	})
	$envelope = Get-McpEnvelope $reply $Name
	if(-not [bool](Get-PropertyValue $envelope 'ok'))
	{
		throw "MCP capture '$Name' reported ok=false."
	}
	$content = @((Get-PropertyValue (Get-PropertyValue $reply 'result') 'content'))
	$images = @($content | Where-Object {
		[string](Get-PropertyValue $_ 'type') -eq 'image' -and
		[string](Get-PropertyValue $_ 'mimeType') -eq 'image/png'
	})
	if($images.Count -ne 1)
	{
		throw "MCP capture '$Name' did not return exactly one PNG."
	}
	$bytes = [Convert]::FromBase64String(
		[string](Get-PropertyValue $images[0] 'data'))
	if($bytes.Length -lt 8 -or $bytes[0] -ne 0x89 -or
		$bytes[1] -ne 0x50 -or $bytes[2] -ne 0x4e -or
		$bytes[3] -ne 0x47)
	{
		throw "MCP capture '$Name' returned invalid PNG bytes."
	}
	[IO.File]::WriteAllBytes($OutputPath, $bytes)
	return $envelope
}

$editor = $null
$stdoutTask = $null
$stderrTask = $null
$createdValidationScenePath = $null
$playEntered = $false
$playEnter = $null
$playExit = $null
$playSceneSave = $null
$validationRouteSet = $null
$validationRouteReset = $null
$validationRouteBootstrap = $null
$validationRouteOverrideActive = $false
try
{
	$start = [Diagnostics.ProcessStartInfo]::new()
	$start.FileName = $editorPath
	$start.Arguments = "--project `"$projectPath`""
	$start.WorkingDirectory = Split-Path -Parent $editorPath
	$start.UseShellExecute = $false
	$start.RedirectStandardOutput = $true
	$start.RedirectStandardError = $true
	$start.EnvironmentVariables['TERRAENGINE_EDITOR_PREFS_PATH'] =
		$preferencesPath
	$start.EnvironmentVariables['TERRAENGINE_SUPPRESS_PROJECT_RECENTS'] = '1'
	if(-not $EnterPlayMode)
	{
		# Static capture does not need project simulation readiness, so avoid
		# briefly rendering the developer's last scene before the disposable
		# validation scene becomes active.
		$start.EnvironmentVariables['TERRAENGINE_SUPPRESS_AUTOMATIC_SCENE_OPEN'] = '1'
	}
	# Play Mode readiness is established while the project opens its configured
	# authored scene. The harness replaces that scene with a disposable one after
	# MCP becomes ready, but suppressing the startup open would leave the project
	# readiness gate intentionally false for the entire run.
	$editor = [Diagnostics.Process]::Start($start)
	if($null -eq $editor)
	{
		throw 'Installed editor failed to start.'
	}

	$stdoutTask = $editor.StandardOutput.ReadToEndAsync()
	$stderrTask = $editor.StandardError.ReadToEndAsync()
	$deadline = [DateTime]::UtcNow.AddSeconds($StartupTimeoutSeconds)
	$tools = @()
	while([DateTime]::UtcNow -lt $deadline)
	{
		$editor.Refresh()
		if($editor.HasExited)
		{
			throw "Installed editor exited before MCP readiness with code $($editor.ExitCode)."
		}
		try
		{
			$ready = Invoke-McpRpc 'initialize' ([ordered]@{
				protocolVersion = '2025-03-26'
				clientInfo = [ordered]@{
					name = 'virtual-geometry-live-visual'
					version = '1'
				}
			})
			if([string]$ready.result.serverInfo.name -eq 'TerraEngine Editor')
			{
				$descriptors = @((Invoke-McpRpc 'tools/list').result.tools)
				# Tool names are advertised before an asynchronously opened
				# project is usable. Gate on live availability as well as presence
				# so the first scene mutation cannot race project publication.
				$tools = @(
					$descriptors |
						Where-Object { [bool]$_.availability.available } |
						ForEach-Object { [string]$_.name }
				)
				if($tools -contains 'camera.capture_custom' -and
					$tools -contains 'renderer.virtualGeometry.capabilityEvidence' -and
					$tools -contains 'renderer.virtualGeometry.stats' -and
					($CreateValidationScene -or
						$tools -contains 'prefab.instantiate') -and
					([string]::IsNullOrWhiteSpace($ScenePath) -or
						$tools -contains 'scene.open') -and
					([string]::IsNullOrWhiteSpace($Asset) -or
						$tools -contains 'asset.virtualGeometry.inspect') -and
					([string]::IsNullOrWhiteSpace($validationRouteName) -or
						($tools -contains 'renderer.virtualGeometry.validationRoute.set' -and
							$tools -contains 'renderer.virtualGeometry.validationRoute.reset')) -and
					(-not $CreateValidationScene -or
						$tools -contains 'scene.create'))
				{
					break
				}
			}
		}
		catch
		{
			# Startup probes are expected to fail until the local listener is bound.
		}
		Start-Sleep -Milliseconds 500
	}
	if($tools.Count -eq 0)
	{
		throw 'Installed editor MCP did not become ready.'
	}

	$validationScene = $null
	$validationCamera = $null
	if(-not [string]::IsNullOrWhiteSpace($ScenePath))
	{
		# Opening the supplied authored scene gives installed validation the same
		# animator and renderer state as the real project without saving any of
		# the disposable instances added below.
		$validationScene = Invoke-McpTool 'scene.open' ([ordered]@{
			path = $ScenePath
		})
	}
	if($CreateValidationScene)
	{
		# Some source fixtures use runtime-oriented scene JSON that the editor
		# intentionally does not open. Create a disposable editor-native scene
		# and camera so the normal RenderSystem path—not only custom readback—
		# advances page demand, decode, upload, and residency publication.
		Start-Sleep -Milliseconds 500
		$validationScene = Invoke-McpTool 'scene.create' ([ordered]@{
			path = $ValidationScenePath
			name = 'Virtual Geometry Live Validation'
			open = $true
		})
		$createdValidationScenePath = [IO.Path]::GetFullPath(
			(Join-Path $projectPath $ValidationScenePath))
		# Scene-scoped tools are advertised but unavailable until scene.create
		# reaches its protected publication. Refresh the availability snapshot
		# before choosing the atomic prefab path or issuing camera mutations.
		$sceneToolsDeadline = [DateTime]::UtcNow.AddSeconds(30)
		do
		{
			$sceneDescriptors = @((Invoke-McpRpc 'tools/list').result.tools)
			$tools = @(
				$sceneDescriptors |
					Where-Object { [bool]$_.availability.available } |
					ForEach-Object { [string]$_.name }
			)
			$sceneToolsReady =
				$tools -contains 'prefab.instantiate' -and
				$tools -contains 'object.create' -and
				$tools -contains 'component.add' -and
				$tools -contains 'component.set_property' -and
				(-not $EnterPlayMode -or $tools -contains 'scene.save')
			if(-not $sceneToolsReady)
			{
				Start-Sleep -Milliseconds 100
			}
		}
		while(-not $sceneToolsReady -and
			[DateTime]::UtcNow -lt $sceneToolsDeadline)
		if(-not $sceneToolsReady)
		{
			throw 'Disposable scene tools did not become available after scene publication.'
		}
		$validationCamera = Invoke-McpTool 'object.create' ([ordered]@{
			name = 'Feature 045 Validation Camera'
		})
		Invoke-McpTool 'component.add' ([ordered]@{
			object = 'Feature 045 Validation Camera'
			component = 'Transform'
		}) | Out-Null
		Invoke-McpTool 'component.add' ([ordered]@{
			object = 'Feature 045 Validation Camera'
			component = 'CameraComponent'
		}) | Out-Null
		Invoke-McpTool 'component.set_property' ([ordered]@{
			object = 'Feature 045 Validation Camera'
			component = 'Transform'
			property = 'Position'
			value = @($ValidationCameraPosition)
		}) | Out-Null
	}

	$instances = @()
	$instanceNames = @()
	for($instanceIndex = 0; $instanceIndex -lt $PrefabInstanceCount; ++$instanceIndex)
	{
		$instanceName = if($PrefabInstanceCount -eq 1)
		{
			$PrefabName
		}
		else
		{
			"$PrefabName $($instanceIndex + 1)"
		}
		$instanceNames += $instanceName
	}
	$usedBulkPrefabPublication =
		($PrefabInstanceCount -gt 1 -or $PrefabRequestedAnimationMode -ge 0) -and
		$tools -contains 'prefab.instantiate_many'
	if($usedBulkPrefabPublication)
	{
		# Large gates and forced-authority probes publish in one protected
		# object-storage generation. Even a single forced CPU instance must not
		# render one automatic GPU frame before a follow-up mode mutation.
		$bulkArguments = [ordered]@{
			prefab = $Prefab
			count = $PrefabInstanceCount
			namePrefix = $PrefabName
			spacing = $PrefabSpacing
		}
		if($PrefabRequestedAnimationMode -ge 0)
		{
			# The policy is serialized into every cloned AnimatorComponent before
			# the single protected publication, so no automatic GPU frame can run
			# between instantiation and a follow-up component mutation.
			$bulkArguments.requestedAnimationMode = $PrefabRequestedAnimationMode
		}
		$instances += Invoke-McpTool 'prefab.instantiate_many' $bulkArguments
	}
	else
	{
		for($instanceIndex = 0; $instanceIndex -lt $PrefabInstanceCount; ++$instanceIndex)
		{
			$instances += Invoke-McpTool 'prefab.instantiate' ([ordered]@{
				prefab = $Prefab
				name = $instanceNames[$instanceIndex]
			})
		}
	}

	for($instanceIndex = 0; $instanceIndex -lt $PrefabInstanceCount; ++$instanceIndex)
	{
		# Every prefab creates a distinct stable animation-instance identity. The
		# crowd profile therefore exercises real per-instance deformation work
		# while remaining transient because this harness never saves the scene.
		$instanceName = $instanceNames[$instanceIndex]
		if($PrefabRequestedAnimationMode -ge 0 -and -not $usedBulkPrefabPublication)
		{
			# A probe may force CPU animation while retaining the same real prefab
			# and renderer path. This isolates evaluator failures from canonical
			# page-deformation failures without changing or saving the asset.
			Invoke-McpTool 'component.set_property' ([ordered]@{
				object = $instanceName
				component = 'AnimatorComponent'
				property = 'requestedMode'
				value = $PrefabRequestedAnimationMode
			}) | Out-Null
		}
		if(-not $usedBulkPrefabPublication -and
			$PrefabSpacing -gt 0.0 -and $PrefabInstanceCount -gt 1)
		{
			# Arrange transient crowds on a centered square grid so every
			# independently animated instance has a distinct world transform.
			$gridWidth = [int][Math]::Ceiling([Math]::Sqrt($PrefabInstanceCount))
			$row = [Math]::Floor($instanceIndex / $gridWidth)
			$column = $instanceIndex % $gridWidth
			$halfWidth = ($gridWidth - 1) * 0.5
			$positionX = ($column - $halfWidth) * $PrefabSpacing
			$positionZ = ($row - $halfWidth) * $PrefabSpacing
			Invoke-McpTool 'component.set_property' ([ordered]@{
				object = $instanceName
				component = 'Transform'
				property = 'Position'
				value = @($positionX, 0.0, $positionZ)
			}) | Out-Null
		}
	}
	if(-not [string]::IsNullOrWhiteSpace($PrefabChildObject) -and
		$PrefabScale -ne 1.0)
	{
		if($PrefabInstanceCount -ne 1)
		{
			throw 'PrefabChildObject scaling requires exactly one validation instance.'
		}
		# Scaling only the disposable validation instance raises its projected
		# error enough to demand non-root detail pages without altering the
		# source asset, cook metadata, or production quality defaults.
		Invoke-McpTool 'component.set_property' ([ordered]@{
			object = $PrefabChildObject
			component = 'Transform'
			property = 'Scale'
			value = @($PrefabScale, $PrefabScale, $PrefabScale)
		}) | Out-Null
	}
	if(-not [string]::IsNullOrWhiteSpace($validationRouteName))
	{
		# Route availability is published by the first completed Vulkan frame.
		# Drive one explicitly ungraded bootstrap capture before asking the
		# renderer to validate a forced route; otherwise a fresh editor can
		# correctly report that its not-yet-published capability snapshot is
		# unavailable even though the adapter supports the route.
		$routeBootstrapPath = Join-Path $outputRoot 'validation-route-bootstrap.png'
		$validationRouteBootstrap = Invoke-McpPngCapture `
			'camera.capture_custom' `
			([ordered]@{
				name = 'Feature 045 Validation Route Bootstrap'
				position = @($ValidationCameraPosition)
				target = @(0.0, 0.5, 0.0)
				up = @(0.0, 1.0, 0.0)
				projection = 'perspective'
				verticalFovDegrees = 60.0
				nearPlane = 0.1
				farPlane = 100.0
				width = 480
				height = 270
				freshness = 'nextFrame'
				timeoutMs = $CaptureTimeoutMilliseconds
			}) `
			$routeBootstrapPath
		# Route parity must exercise the renderer's transient route-selection
		# contract before any graded frame. Hybrid deliberately requests a mixed
		# packet/material scene instead of inventing a fourth backend route.
		$validationRouteSet = Invoke-McpTool `
			'renderer.virtualGeometry.validationRoute.set' `
			([ordered]@{ route = $validationRouteName })
		$validationRouteOverrideActive = $true
	}
	$scene = Invoke-McpTool 'scene.list_objects' ([ordered]@{
		includeComponents = $true
	})
	if($EnterPlayMode)
	{
		if($tools -notcontains 'play.enter' -or
			$tools -notcontains 'play.exit')
		{
			throw 'Animated live capture requires play.enter and play.exit.'
		}
		if($CreateValidationScene)
		{
			# Project readiness is derived from a complete saved scene publication.
			# Persist only the disposable validation scene immediately before Play;
			# CleanupValidationScene removes this exact file after the editor exits.
			$playSceneSave = Invoke-McpTool 'scene.save'
		}
		$playEnter = Invoke-McpTool 'play.enter' ([ordered]@{
			expectedState = 'stopped'
			correlationId = 'feature-045-live-enter'
		})
		$playEntered = $true
		# Let the protected play transition publish its runtime scene and advance
		# animation at least one fixed tick before collecting diagnostics.
		Start-Sleep -Milliseconds $StreamingSettleMilliseconds
	}
	$animationDiagnostics = $null
	if($tools -contains 'animation.runtime.diagnostics')
	{
		# Capture the immutable execution-mode summary before requesting a GPU
		# frame. If the frame later stalls, this still proves which animation
		# authority and packet population were active at the failure boundary.
		$animationDiagnostics = Invoke-McpTool `
			'animation.runtime.diagnostics' `
			([ordered]@{
				includeDetails = $false
				maximumItems = 256
			})
	}
	$capabilities = Invoke-McpTool `
		'renderer.virtualGeometry.capabilityEvidence' `
		([ordered]@{ view = 'Game' })
	$cameraArguments = [ordered]@{
		name = 'Feature 045 Phase 0 Static Baseline'
		position = @($ValidationCameraPosition)
		target = @(0.0, 0.5, 0.0)
		up = @(0.0, 1.0, 0.0)
		projection = 'perspective'
		verticalFovDegrees = 60.0
		nearPlane = 0.1
		farPlane = 100.0
		width = 480
		height = 270
		freshness = 'nextFrame'
		timeoutMs = $CaptureTimeoutMilliseconds
	}
	$warmupPath = Join-Path $outputRoot 'live-vulkan-warmup.png'
	$firstPath = Join-Path $outputRoot 'live-vulkan-a.png'
	$secondPath = Join-Path $outputRoot 'live-vulkan-b.png'
	# Prefab publication is atomic, but animation binding and compact-page
	# residency complete asynchronously afterward. Grade only captures taken
	# after this ungraded warm-up frame has driven those worker-owned paths.
	$warmup = Invoke-McpPngCapture `
		'camera.capture_custom' `
		$cameraArguments `
		$warmupPath
	# The warm-up next-frame capture submits demand for any non-resident compact
	# pages. Record the immediate state before the worker decode and Vulkan
	# publication have another frame to complete.
	$postWarmupCaptureStats = Invoke-McpToolWithReadinessRetry `
		'renderer.virtualGeometry.stats' `
		([ordered]@{ viewType = $DiagnosticsViewType })
	$postWarmupCaptureAsset = $null
	if(-not [string]::IsNullOrWhiteSpace($Asset))
	{
		$postWarmupCaptureAsset = Invoke-McpTool `
			'asset.virtualGeometry.inspect' `
			([ordered]@{
				asset = $Asset
				includeRuntimeState = $true
				viewType = $DiagnosticsViewType
			})
	}
	Start-Sleep -Milliseconds $StreamingSettleMilliseconds
	$first = Invoke-McpPngCapture `
		'camera.capture_custom' `
		$cameraArguments `
		$firstPath
	$postFirstCaptureStats = Invoke-McpToolWithReadinessRetry `
		'renderer.virtualGeometry.stats' `
		([ordered]@{ viewType = $DiagnosticsViewType })
	$postFirstCaptureAsset = $null
	if(-not [string]::IsNullOrWhiteSpace($Asset))
	{
		$postFirstCaptureAsset = Invoke-McpTool `
			'asset.virtualGeometry.inspect' `
			([ordered]@{
				asset = $Asset
				includeRuntimeState = $true
				viewType = $DiagnosticsViewType
			})
	}
	Start-Sleep -Milliseconds $StreamingSettleMilliseconds
	$second = Invoke-McpPngCapture `
		'camera.capture_custom' `
		$cameraArguments `
		$secondPath
	# Query again after two next-frame captures. This gives the backend enough
	# completed frame slots to publish fence-delayed execution receipts without
	# treating the earlier capability snapshot as proof of execution.
	$postCaptureCapabilities = Invoke-McpTool `
		'renderer.virtualGeometry.capabilityEvidence' `
		([ordered]@{ view = $DiagnosticsViewType })
	$postCaptureStats = Invoke-McpToolWithReadinessRetry `
		'renderer.virtualGeometry.stats' `
		([ordered]@{ viewType = $DiagnosticsViewType })
	$postCaptureAsset = $null
	if(-not [string]::IsNullOrWhiteSpace($Asset))
	{
		$postCaptureAsset = Invoke-McpTool `
			'asset.virtualGeometry.inspect' `
			([ordered]@{
				asset = $Asset
				includeRuntimeState = $true
				viewType = $DiagnosticsViewType
			})
	}
	if($playEntered)
	{
		$playExit = Invoke-McpTool 'play.exit' ([ordered]@{
			expectedState = 'playing'
			correlationId = 'feature-045-live-exit'
		})
		$playEntered = $false
		if(-not [bool]$playExit.data.restoration.completed -or
			@($playExit.data.restoration.partialFailures).Count -ne 0)
		{
			throw 'Play Mode did not restore the authored validation scene atomically.'
		}
	}
	if($validationRouteOverrideActive)
	{
		$validationRouteReset = Invoke-McpTool `
			'renderer.virtualGeometry.validationRoute.reset'
		$validationRouteOverrideActive = $false
	}
	$completedStages = @(
		$postCaptureCapabilities.data.capabilities |
			Where-Object { $_.executed -eq $true -and $_.validated -eq $true } |
			ForEach-Object { [string]$_.stage }
	)
	$deviceAuthoredStages = @(
		'traversal',
		'compaction',
		'indirect-generation'
	)
	$deviceAuthoredRoute = @(
		$deviceAuthoredStages |
			Where-Object { $completedStages -notcontains $_ }
	).Count -eq 0
	$firstHash = (Get-FileHash -LiteralPath $firstPath -Algorithm SHA256).Hash.ToLowerInvariant()
	$secondHash = (Get-FileHash -LiteralPath $secondPath -Algorithm SHA256).Hash.ToLowerInvariant()
	$summary = [ordered]@{
		schemaVersion = 1
		feature = '045-virtual-geometry-alternative'
		route = if($deviceAuthoredRoute) {
			'vulkan-device-authored-indirect'
		} else {
			'processor-expanded-fallback'
		}
		capturedAt = [DateTime]::UtcNow.ToString('o')
		editor = $editorPath
		project = $projectPath
		mcpEndpoint = $endpoint
		sceneObjectCount = @($scene.data.objects).Count
		validationScene = $validationScene
		validationCamera = $validationCamera
		playSceneSave = $playSceneSave
		playEnter = $playEnter
		playExit = $playExit
		validationRoute = [ordered]@{
			requested = if([string]::IsNullOrWhiteSpace($validationRouteName)) {
				$null
			} else {
				$validationRouteName
			}
			bootstrap = $validationRouteBootstrap
			set = $validationRouteSet
			reset = $validationRouteReset
		}
		diagnosticsViewType = $DiagnosticsViewType
		instantiatedPrefab = $instances[0]
		instantiatedPrefabs = $instances
		animationDiagnostics = $animationDiagnostics
		capabilityEvidence = $capabilities
		postCaptureCapabilityEvidence = $postCaptureCapabilities
		postWarmupCaptureStats = $postWarmupCaptureStats
		postWarmupCaptureAsset = $postWarmupCaptureAsset
		postFirstCaptureStats = $postFirstCaptureStats
		postFirstCaptureAsset = $postFirstCaptureAsset
		postCaptureStats = $postCaptureStats
		postCaptureAsset = $postCaptureAsset
		warmupCapture = [ordered]@{
			path = $warmupPath
			sha256 = (Get-FileHash -LiteralPath $warmupPath -Algorithm SHA256).Hash.ToLowerInvariant()
			bytes = [UInt64](Get-Item -LiteralPath $warmupPath).Length
			envelope = $warmup
		}
		captures = @(
			[ordered]@{
				path = $firstPath
				sha256 = $firstHash
				bytes = [UInt64](Get-Item -LiteralPath $firstPath).Length
				envelope = $first
			},
			[ordered]@{
				path = $secondPath
				sha256 = $secondHash
				bytes = [UInt64](Get-Item -LiteralPath $secondPath).Length
				envelope = $second
			}
		)
		unchangedByteIdentity = $firstHash -eq $secondHash
		expectedFrameIdentity = if($ExpectAnimatedChange) {
			'animated-change'
		} else {
			'static-repeat'
		}
	}
	[IO.File]::WriteAllText(
		(Join-Path $outputRoot 'live-visual-summary.json'),
		($summary | ConvertTo-Json -Depth 64),
		$utf8NoBom)
	if($ExpectAnimatedChange -and $summary.unchangedByteIdentity)
	{
		throw 'Repeated animated live Vulkan captures were byte-identical.'
	}
	if(-not $ExpectAnimatedChange -and -not $summary.unchangedByteIdentity)
	{
		throw 'Repeated static live Vulkan captures were not byte-identical.'
	}
}
finally
{
	if($null -ne $editor)
	{
		$editor.Refresh()
		if($validationRouteOverrideActive -and -not $editor.HasExited)
		{
			try
			{
				Invoke-McpTool `
					'renderer.virtualGeometry.validationRoute.reset' | Out-Null
				$validationRouteOverrideActive = $false
			}
			catch
			{
				# Process cleanup below is the final isolation boundary when a
				# renderer failure prevents the transient override reset.
			}
		}
		if($playEntered -and -not $editor.HasExited)
		{
			try
			{
				Invoke-McpTool 'play.exit' ([ordered]@{
					expectedState = 'playing'
					correlationId = 'feature-045-live-finally-exit'
				}) | Out-Null
				$playEntered = $false
			}
			catch
			{
				# Process cleanup below remains authoritative when renderer loss
				# or listener shutdown prevents a final restoration response.
			}
		}
		if(-not $editor.HasExited)
		{
			[void]$editor.CloseMainWindow()
			if(-not $editor.WaitForExit(5000))
			{
				try
				{
					$editor.Kill()
				}
				catch [System.ComponentModel.Win32Exception]
				{
					# The editor can exit between the timeout and Kill(). Refresh
					# before treating that benign race as a cleanup failure.
					$editor.Refresh()
					if(-not $editor.HasExited)
					{
						throw
					}
				}
				if(-not $editor.HasExited)
				{
					$editor.WaitForExit()
				}
			}
		}
		if($null -ne $stdoutTask)
		{
			[IO.File]::WriteAllText(
				$stdoutPath,
				$stdoutTask.GetAwaiter().GetResult(),
				$utf8NoBom)
		}
		if($null -ne $stderrTask)
		{
			[IO.File]::WriteAllText(
				$stderrPath,
				$stderrTask.GetAwaiter().GetResult(),
				$utf8NoBom)
		}
	}
	if($CleanupValidationScene -and $null -ne $createdValidationScenePath)
	{
		# Cleanup is intentionally limited to the exact disposable scene and its
		# sidecar after the editor exits. Never recurse or remove an authored path.
		$projectBoundary = $projectPath.TrimEnd(
			[IO.Path]::DirectorySeparatorChar,
			[IO.Path]::AltDirectorySeparatorChar) +
			[IO.Path]::DirectorySeparatorChar
		if(-not $createdValidationScenePath.StartsWith(
			$projectBoundary, [StringComparison]::OrdinalIgnoreCase))
		{
			throw 'Refusing to clean a validation scene outside the project root.'
		}
		foreach($cleanupPath in @(
			$createdValidationScenePath,
			"$createdValidationScenePath.meta"))
		{
			if(Test-Path -LiteralPath $cleanupPath -PathType Leaf)
			{
				Remove-Item -LiteralPath $cleanupPath -Force
			}
		}
	}
}
