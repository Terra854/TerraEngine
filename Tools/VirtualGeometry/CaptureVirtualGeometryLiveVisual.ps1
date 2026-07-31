[CmdletBinding()]
param(
	[string]$InstalledEditor = 'out/build/RelWithDebInfo/Build/Editor/TerraEngine-Editor.exe',
	[string]$Project = 'Tests/Fixtures/Animation/ImportValidation',
	[string]$Asset = '',
	[string]$Preferences = 'out/validation/virtual-geometry/live/editor-preferences.json',
	[string]$OutputDirectory = 'out/validation/virtual-geometry/live',
	[string]$Prefab = 'Assets/Models/ImportHero_Generated/ImportHero.teprefab',
	[string]$PrefabName = 'Virtual Geometry Validation Hero',
	[string]$PrefabChildObject = '',
	[string]$MeshAssetId = '',
	[string]$MaterialAssetId = '',
	[double]$PrefabScale = 1.0,
	[int]$PrefabInstanceCount = 1,
	[int]$PrefabRequestedAnimationMode = -1,
	[double]$PrefabSpacing = 0.0,
	[string]$ScenePath = '',
	[switch]$CreateValidationScene,
	[switch]$CleanupValidationScene,
	[string]$ValidationScenePath = 'Assets/Scenes/VirtualGeometryLive.tescene',
	[double[]]$ValidationCameraPosition = @(0.0, 0.5, 1.0),
	[double[]]$ValidationCameraTarget = @(0.0, 0.5, 0.0),
	[int]$StreamingSettleMilliseconds = 250,
	[int]$McpPort = 43155,
	[int]$StartupTimeoutSeconds = 90,
	[int]$CaptureTimeoutMilliseconds = 60000,
	[string]$ValidationRoute = '',
	[ValidateSet('', 'automatic', 'direct', 'virtual-hardware', 'virtual-compute')]
	[string]$RepresentationRoute = '',
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
foreach($cameraVector in @($ValidationCameraPosition, $ValidationCameraTarget))
{
	if($cameraVector.Count -ne 3)
	{
		throw 'Validation camera position and target must each contain three coordinates.'
	}
}
$meshAssetIdentity = $MeshAssetId.Trim().ToLowerInvariant()
$materialAssetIdentity = $MaterialAssetId.Trim().ToLowerInvariant()
$useRawMesh = -not [string]::IsNullOrWhiteSpace($meshAssetIdentity)
if($useRawMesh -and $meshAssetIdentity -notmatch '^[0-9a-f]{32}$')
{
	throw 'MeshAssetId must be a 32-character hexadecimal UUID.'
}
if(-not [string]::IsNullOrWhiteSpace($materialAssetIdentity) -and
	$materialAssetIdentity -notmatch '^[0-9a-f]{32}$')
{
	throw 'MaterialAssetId must be empty or a 32-character hexadecimal UUID.'
}
if($useRawMesh -and -not $CreateValidationScene)
{
	throw 'Raw mesh validation requires CreateValidationScene so the authored project remains untouched.'
}
$validationRouteName = $ValidationRoute.Trim().ToLowerInvariant()
if($validationRouteName -notin @('', 'hardware', 'compute', 'indexed', 'hybrid'))
{
	throw "ValidationRoute must be hardware, compute, indexed, hybrid, or empty; received '$ValidationRoute'."
}
$representationRouteName = $RepresentationRoute.Trim().ToLowerInvariant()
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

function Save-EditorWindowCapture(
	[Diagnostics.Process]$Process,
	[string]$OutputPath)
{
	# MCP camera captures prove renderer output, while this operating-system
	# capture proves the installed editor itself was visible and responsive.
	# Capture only the exact process window instead of the full developer desktop.
	Add-Type -AssemblyName System.Drawing
	if(-not ('TerraEngineLiveWindowCaptureNative' -as [type]))
	{
		Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class TerraEngineLiveWindowCaptureNative
{
	[StructLayout(LayoutKind.Sequential)]
	public struct Rect
	{
		public int Left;
		public int Top;
		public int Right;
		public int Bottom;
	}

	[DllImport("user32.dll")]
	public static extern bool GetWindowRect(IntPtr window, out Rect rectangle);

	[DllImport("user32.dll")]
	public static extern bool ShowWindow(IntPtr window, int command);

	[DllImport("user32.dll")]
	public static extern bool SetForegroundWindow(IntPtr window);

	[DllImport("user32.dll")]
	public static extern bool PrintWindow(
		IntPtr window,
		IntPtr deviceContext,
		uint flags);
}
'@
	}
	$Process.Refresh()
	$window = $Process.MainWindowHandle
	if($window -eq [IntPtr]::Zero)
	{
		throw 'Installed editor did not publish a capturable main window.'
	}
	[void][TerraEngineLiveWindowCaptureNative]::ShowWindow($window, 9)
	[void][TerraEngineLiveWindowCaptureNative]::SetForegroundWindow($window)
	Start-Sleep -Milliseconds 250
	$rectangle = [TerraEngineLiveWindowCaptureNative+Rect]::new()
	if(-not [TerraEngineLiveWindowCaptureNative]::GetWindowRect(
		$window, [ref]$rectangle))
	{
		throw 'Unable to read the installed editor window bounds.'
	}
	$width = $rectangle.Right - $rectangle.Left
	$height = $rectangle.Bottom - $rectangle.Top
	if($width -lt 64 -or $height -lt 64)
	{
		throw "Installed editor window bounds are invalid: ${width}x${height}."
	}
	$bitmap = [Drawing.Bitmap]::new($width, $height)
	$graphics = [Drawing.Graphics]::FromImage($bitmap)
	try
	{
		# PW_RENDERFULLCONTENT asks Desktop Window Manager for the composed
		# window, including Vulkan/ImGui client pixels. Fall back to an exact
		# on-screen rectangle only on platforms that reject that request.
		$deviceContext = $graphics.GetHdc()
		try
		{
			$printed = [TerraEngineLiveWindowCaptureNative]::PrintWindow(
				$window,
				$deviceContext,
				2)
		}
		finally
		{
			$graphics.ReleaseHdc($deviceContext)
		}
		if(-not $printed)
		{
			$graphics.CopyFromScreen(
				$rectangle.Left,
				$rectangle.Top,
				0,
				0,
				[Drawing.Size]::new($width, $height))
		}
		$bitmap.Save($OutputPath, [Drawing.Imaging.ImageFormat]::Png)
	}
	finally
	{
		$graphics.Dispose()
		$bitmap.Dispose()
	}
	return [ordered]@{
		status = 'measured'
		kind = 'editor-window-screenshot'
		path = $OutputPath
		width = $width
		height = $height
		sha256 = (Get-FileHash -LiteralPath $OutputPath -Algorithm SHA256).
			Hash.ToLowerInvariant()
	}
}

function Save-PickingReviewImage(
	[string]$SourcePath,
	[string]$OutputPath,
	[int[]]$Pixel,
	[string]$Status)
{
	# The review image never substitutes for the authoritative Vulkan result.
	# It marks the exact queried pixel on a copy of the real renderer capture so
	# a human can correlate the separately recorded measured/unavailable state.
	Add-Type -AssemblyName System.Drawing
	$source = [Drawing.Image]::FromFile($SourcePath)
	$bitmap = [Drawing.Bitmap]::new($source)
	$graphics = [Drawing.Graphics]::FromImage($bitmap)
	$pen = [Drawing.Pen]::new([Drawing.Color]::FromArgb(255, 255, 32, 32), 3.0)
	$brush = [Drawing.SolidBrush]::new(
		[Drawing.Color]::FromArgb(220, 16, 16, 16))
	$textBrush = [Drawing.SolidBrush]::new([Drawing.Color]::White)
	$font = [Drawing.Font]::new('Segoe UI', 12.0, [Drawing.FontStyle]::Bold)
	try
	{
		$x = [Math]::Max(0, [Math]::Min($bitmap.Width - 1, $Pixel[0]))
		$y = [Math]::Max(0, [Math]::Min($bitmap.Height - 1, $Pixel[1]))
		$graphics.DrawEllipse($pen, $x - 10, $y - 10, 20, 20)
		$graphics.DrawLine($pen, $x - 16, $y, $x + 16, $y)
		$graphics.DrawLine($pen, $x, $y - 16, $x, $y + 16)
		$label = "Authoritative pick: $Status at ($($Pixel[0]), $($Pixel[1]))"
		$size = $graphics.MeasureString($label, $font)
		$graphics.FillRectangle($brush, 4, 4, $size.Width + 12, $size.Height + 8)
		$graphics.DrawString($label, $font, $textBrush, 10, 8)
		$bitmap.Save($OutputPath, [Drawing.Imaging.ImageFormat]::Png)
	}
	finally
	{
		$font.Dispose()
		$textBrush.Dispose()
		$brush.Dispose()
		$pen.Dispose()
		$graphics.Dispose()
		$bitmap.Dispose()
		$source.Dispose()
	}
}

function New-UnavailableEvidence([string]$Kind, [string]$Reason)
{
	# Optional evidence must never be represented as a successful measurement.
	# Consumers can distinguish a missing capability from an empty payload.
	return [ordered]@{
		status = 'unavailable'
		kind = $Kind
		reason = $Reason
		availability = 'unavailable'
	}
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
$representationRouteSet = $null
$representationRouteReset = $null
$representationRouteOverrideActive = $false
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
	$validationLight = $null
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
				($useRawMesh -or $tools -contains 'prefab.instantiate') -and
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
			name = 'Virtual Geometry Validation Camera'
		})
		Invoke-McpTool 'component.add' ([ordered]@{
			object = 'Virtual Geometry Validation Camera'
			component = 'Transform'
		}) | Out-Null
		Invoke-McpTool 'component.add' ([ordered]@{
			object = 'Virtual Geometry Validation Camera'
			component = 'CameraComponent'
		}) | Out-Null
		Invoke-McpTool 'component.set_property' ([ordered]@{
			object = 'Virtual Geometry Validation Camera'
			component = 'Transform'
			property = 'Position'
			value = @($ValidationCameraPosition)
		}) | Out-Null
		# The disposable scene has no authored illumination. Add a real point
		# light through the transactional editor API so visual grading evaluates
		# shaded geometry and shadow submission instead of a black silhouette.
		$validationLight = Invoke-McpTool 'object.create' ([ordered]@{
			name = 'Virtual Geometry Validation Light'
		})
		Invoke-McpTool 'component.add' ([ordered]@{
			object = 'Virtual Geometry Validation Light'
			component = 'Transform'
		}) | Out-Null
		Invoke-McpTool 'component.add' ([ordered]@{
			object = 'Virtual Geometry Validation Light'
			component = 'LightComponent'
		}) | Out-Null
		Invoke-McpTool 'component.set_property' ([ordered]@{
			object = 'Virtual Geometry Validation Light'
			component = 'Transform'
			property = 'Position'
			value = @(2.0, 3.0, 3.0)
		}) | Out-Null
		Invoke-McpTool 'component.set_property' ([ordered]@{
			object = 'Virtual Geometry Validation Light'
			component = 'LightComponent'
			property = 'intensity'
			value = 2500.0
		}) | Out-Null
		Invoke-McpTool 'component.set_property' ([ordered]@{
			object = 'Virtual Geometry Validation Light'
			component = 'LightComponent'
			property = 'range'
			value = 20.0
		}) | Out-Null
		Invoke-McpTool 'component.set_property' ([ordered]@{
			object = 'Virtual Geometry Validation Light'
			component = 'LightComponent'
			property = 'castsShadows'
			value = $true
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
	$usedBulkPrefabPublication = -not $useRawMesh -and
		($PrefabInstanceCount -gt 1 -or $PrefabRequestedAnimationMode -ge 0) -and
		$tools -contains 'prefab.instantiate_many'
	if($useRawMesh)
	{
		for($instanceIndex = 0; $instanceIndex -lt $PrefabInstanceCount; ++$instanceIndex)
		{
			$instanceName = $instanceNames[$instanceIndex]
			# Build raw-mesh validation objects exclusively through protected,
			# reflected editor mutations. The source project and asset metadata
			# remain immutable while the installed renderer receives the exact
			# cooked mesh and material identities selected by the caller.
			$instance = Invoke-McpTool 'object.create' ([ordered]@{
				name = $instanceName
			})
			Invoke-McpTool 'component.add' ([ordered]@{
				object = $instanceName
				component = 'Transform'
			}) | Out-Null
			Invoke-McpTool 'component.add' ([ordered]@{
				object = $instanceName
				component = 'MeshRenderComponent'
			}) | Out-Null
			Invoke-McpTool 'component.set_property' ([ordered]@{
				object = $instanceName
				component = 'MeshRenderComponent'
				property = 'meshId'
				value = $meshAssetIdentity
			}) | Out-Null
			if(-not [string]::IsNullOrWhiteSpace($materialAssetIdentity))
			{
				Invoke-McpTool 'component.set_property' ([ordered]@{
					object = $instanceName
					component = 'MeshRenderComponent'
					property = 'materialId'
					value = $materialAssetIdentity
				}) | Out-Null
			}
			$instances += $instance
		}
	}
	elseif($usedBulkPrefabPublication)
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
		if($PrefabRequestedAnimationMode -ge 0 -and
			-not $useRawMesh -and
			-not $usedBulkPrefabPublication)
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
	if($PrefabScale -ne 1.0)
	{
		if($PrefabInstanceCount -ne 1)
		{
			throw 'Validation-object scaling requires exactly one validation instance.'
		}
		$scaleTargetObject = if($useRawMesh)
		{
			$instanceNames[0]
		}
		else
		{
			$PrefabChildObject
		}
		if([string]::IsNullOrWhiteSpace($scaleTargetObject))
		{
			throw 'Prefab scaling requires PrefabChildObject when no raw mesh is supplied.'
		}
		# Raw meshes have no prefab child. Target their generated validation name;
		# prefab captures continue to scale their explicit child object. Both paths
		# mutate only the disposable scene and increase projected page demand.
		Invoke-McpTool 'component.set_property' ([ordered]@{
			object = $scaleTargetObject
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
				name = 'Virtual Geometry Validation Route Bootstrap'
				position = @($ValidationCameraPosition)
				target = @($ValidationCameraTarget)
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
	if(-not [string]::IsNullOrWhiteSpace($representationRouteName))
	{
		$representationRouteSet = Invoke-McpTool 'route.lock' ([ordered]@{
			route = $representationRouteName
			enabled = $true
		})
		if(-not $representationRouteSet.ok)
		{
			throw "Representation route lock was unavailable: $($representationRouteSet.message)"
		}
		$representationRouteOverrideActive = $true
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
			correlationId = 'virtual-geometry-live-enter'
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
		name = 'Virtual Geometry Static Baseline'
		position = @($ValidationCameraPosition)
		target = @($ValidationCameraTarget)
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
	$cameraReviewPath = Join-Path $outputRoot 'camera-capture.png'
	[IO.File]::Copy($firstPath, $cameraReviewPath, $true)
	$shadowReviewPath = Join-Path $outputRoot 'shadow-review.png'
	$shadowReviewArguments = [ordered]@{}
	foreach($entry in $cameraArguments.GetEnumerator())
	{
		$shadowReviewArguments[$entry.Key] = $entry.Value
	}
	$shadowReviewArguments.name = 'Virtual Geometry Shadow Review'
	$shadowReview = Invoke-McpPngCapture `
		'camera.capture_custom' `
		$shadowReviewArguments `
		$shadowReviewPath
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
	# Collect optional evidence only when the installed editor advertises the
	# operation. A rejected request is recorded as unavailable, never inferred
	# from the camera image or from CPU-side projection.
	$firstHash = (Get-FileHash -LiteralPath $firstPath -Algorithm SHA256).Hash.ToLowerInvariant()
	$secondHash = (Get-FileHash -LiteralPath $secondPath -Algorithm SHA256).Hash.ToLowerInvariant()
	$pickingEvidence = New-UnavailableEvidence 'authoritative-picking' 'renderer.virtualGeometry.authoritativePick is not advertised.'
	$shadowEvidence = New-UnavailableEvidence 'shadow-diagnostics' 'Shadow-view diagnostics are not advertised by this editor.'
	$editorWindowEvidence = New-UnavailableEvidence 'editor-window-screenshot' 'No editor-window screenshot operation is advertised.'
	$profilerEvidence = New-UnavailableEvidence 'profiler-open-responsiveness' 'Profiler capture operations are not advertised.'
	if($tools -contains 'renderer.virtualGeometry.authoritativePick')
	{
		try
		{
			# Try the centre and a bounded grid because the editor view can have a
			# different aspect ratio from the dedicated capture target. Poll each
			# request for several frames so fence-delayed identity publication is
			# not mistaken for an unsupported pick.
			$pickPixels = @(
				@(240, 135),
				@(180, 135), @(300, 135),
				@(240, 90), @(240, 180),
				@(120, 90), @(360, 90),
				@(120, 180), @(360, 180)
			)
			$pickRequest = $null
			$pick = $null
			$pickState = $null
			$selectedPickPixel = @(240, 135)
			foreach($candidatePixel in $pickPixels)
			{
				$pickRequest = Invoke-McpTool 'renderer.virtualGeometry.authoritativePick' ([ordered]@{
					viewType = $DiagnosticsViewType
					pixel = @($candidatePixel)
					request = $true
				})
				for($poll = 0; $poll -lt 5; ++$poll)
				{
					Start-Sleep -Milliseconds 100
					$pick = Invoke-McpTool 'renderer.virtualGeometry.authoritativePick' ([ordered]@{
						viewType = $DiagnosticsViewType
						pixel = @($candidatePixel)
						request = $false
					})
					$pickState = Get-PropertyValue (
						Get-PropertyValue $pick 'data') 'authoritative'
					if([string](Get-PropertyValue $pickState 'status') -eq 'Ready')
					{
						break
					}
				}
				$selectedPickPixel = @($candidatePixel)
				if([string](Get-PropertyValue $pickState 'status') -eq 'Ready')
				{
					break
				}
			}
			if([string](Get-PropertyValue $pickState 'status') -ne 'Ready')
			{
				$pickingEvidence = New-UnavailableEvidence 'authoritative-picking' "Pick state was not Ready ($([string](Get-PropertyValue $pickState 'status')))."
				$pickingEvidence.state = $pickState
			}
			else
			{
				$pickingEvidence = [ordered]@{
				status = 'measured'
				kind = 'authoritative-picking'
				viewType = $DiagnosticsViewType
				pixel = @($selectedPickPixel)
				request = $pickRequest
				state = $pick
				associatedImage = [ordered]@{ path = $firstPath; sha256 = $firstHash }
				}
			}
			$pickingReviewPath = Join-Path $outputRoot 'picking-review.png'
			Save-PickingReviewImage `
				-SourcePath $firstPath `
				-OutputPath $pickingReviewPath `
				-Pixel $selectedPickPixel `
				-Status ([string](Get-PropertyValue $pickState 'status'))
			$pickingEvidence.reviewImage = [ordered]@{
				path = $pickingReviewPath
				sha256 = (Get-FileHash -LiteralPath $pickingReviewPath -Algorithm SHA256).
					Hash.ToLowerInvariant()
			}
		}
		catch
		{
			$pickingEvidence = New-UnavailableEvidence 'authoritative-picking' $_.Exception.Message
		}
	}
	if($tools -contains 'renderer.virtualGeometry.stats')
	{
		try
		{
			$shadowStats = Invoke-McpTool 'renderer.virtualGeometry.stats' ([ordered]@{ viewType = 'Shadow' })
			$shadowDiagnosticsPath = Join-Path $outputRoot 'shadow-diagnostics.json'
			[IO.File]::WriteAllText($shadowDiagnosticsPath, ($shadowStats | ConvertTo-Json -Depth 64), $utf8NoBom)
			$shadowEvidence = [ordered]@{
				status = 'measured'
				kind = 'shadow-diagnostics'
				viewType = 'Shadow'
				diagnostics = $shadowStats
				path = $shadowDiagnosticsPath
				image = [ordered]@{
					status = 'measured'
					kind = 'visible-shadow-camera'
					path = $shadowReviewPath
					sha256 = (Get-FileHash -LiteralPath $shadowReviewPath -Algorithm SHA256).
						Hash.ToLowerInvariant()
					envelope = $shadowReview
				}
			}
		}
		catch
		{
			# Shadow-buffer diagnostics are optional, but the separately captured
			# lit camera remains valid human-review evidence. Never promote it to
			# a shadow-buffer measurement when the view type is unavailable.
			$shadowEvidence = New-UnavailableEvidence 'shadow-diagnostics' $_.Exception.Message
			$shadowEvidence.image = [ordered]@{
				status = 'measured'
				kind = 'visible-shadow-camera'
				path = $shadowReviewPath
				sha256 = (Get-FileHash -LiteralPath $shadowReviewPath -Algorithm SHA256).
					Hash.ToLowerInvariant()
				envelope = $shadowReview
			}
		}
	}
	if($tools -contains 'profiler.capture.start' -and
		$tools -contains 'profiler.capture.stop')
	{
		$profilerWatch = [Diagnostics.Stopwatch]::StartNew()
		$profilerStarted = $false
		try
		{
			$profilerBefore = Invoke-McpTool 'profiler.capture.list'
			$profilerBeforeIds = @(
				$profilerBefore.data.artifacts |
					ForEach-Object { [string]$_.artifactId }
			)
			# Sixteen per-frame samples are large enough to prove the buffered
			# capture path without keeping the profiler armed for a long workload.
			$profilerStart = Invoke-McpTool 'profiler.capture.start' ([ordered]@{
				captureLevel = 'sampled'
				maxFrames = 16
				sampleIntervalFrames = 1
			})
			$profilerStarted = $true
			Start-Sleep -Milliseconds ([Math]::Max(1000, $StreamingSettleMilliseconds))
			$profilerStop = Invoke-McpTool 'profiler.capture.stop'
			$profilerStopJobId = [string]$profilerStop.data.jobId
			if([string]::IsNullOrWhiteSpace($profilerStopJobId))
			{
				throw 'Profiler capture stop did not return a durable job identity.'
			}
			$profilerJob = $null
			$profilerDeadline = [DateTime]::UtcNow.AddSeconds(60)
			do
			{
				$profilerJob = Invoke-McpTool 'jobs.get' ([ordered]@{
					jobId = $profilerStopJobId
				})
				$profilerJobState = [string]$profilerJob.data.job.state
				if($profilerJobState -notin @('succeeded', 'failed', 'canceled'))
				{
					Start-Sleep -Milliseconds 100
				}
			}
			while($profilerJobState -notin @('succeeded', 'failed', 'canceled') -and
				[DateTime]::UtcNow -lt $profilerDeadline)
			if($profilerJobState -ne 'succeeded')
			{
				throw "Profiler persistence ended in state '$profilerJobState'."
			}
			$profilerAfter = Invoke-McpTool 'profiler.capture.list'
			$newProfilerArtifacts = @(
				$profilerAfter.data.artifacts |
					Where-Object {
						$profilerBeforeIds -notcontains [string]$_.artifactId
					}
			)
			if($newProfilerArtifacts.Count -ne 1)
			{
				throw "Profiler persistence published $($newProfilerArtifacts.Count) new artifacts instead of one."
			}
			$profilerArtifact = $newProfilerArtifacts[0]
			$profilerSummary = Invoke-McpTool 'profiler.capture.summary' ([ordered]@{
				artifactId = [string]$profilerArtifact.artifactId
			})
			$profilerFrameCount = [UInt64]$profilerSummary.data.summary.frameCount
			if($profilerFrameCount -eq 0)
			{
				throw 'Profiler capture persisted without any completed frame samples.'
			}
			$profilerLoad = Invoke-McpTool 'profiler.capture.load' ([ordered]@{
				artifactId = [string]$profilerArtifact.artifactId
			})
			$profilerLoadJob = $null
			$profilerLoadJobId = [string]$profilerLoad.data.jobId
			if(-not [string]::IsNullOrWhiteSpace($profilerLoadJobId))
			{
				$loadDeadline = [DateTime]::UtcNow.AddSeconds(60)
				do
				{
					$profilerLoadJob = Invoke-McpTool 'jobs.get' ([ordered]@{
						jobId = $profilerLoadJobId
					})
					$loadState = [string]$profilerLoadJob.data.job.state
					if($loadState -notin @('succeeded', 'failed', 'canceled'))
					{
						Start-Sleep -Milliseconds 100
					}
				}
				while($loadState -notin @('succeeded', 'failed', 'canceled') -and
					[DateTime]::UtcNow -lt $loadDeadline)
				if($loadState -ne 'succeeded')
				{
					throw "Profiler recovery load ended in state '$loadState'."
				}
			}
			Start-Sleep -Milliseconds 250
			$profilerWatch.Stop()
			$profilerEvidence = [ordered]@{
				status = 'measured'
				kind = 'profiler-open-responsiveness'
				elapsedMilliseconds = $profilerWatch.Elapsed.TotalMilliseconds
				start = $profilerStart
				stop = $profilerStop
				persistenceJob = $profilerJob
				artifact = $profilerArtifact
				summary = $profilerSummary
				frameCount = $profilerFrameCount
				load = $profilerLoad
				loadJob = $profilerLoadJob
			}
		}
		catch
		{
			if($profilerStarted)
			{
				# Best-effort cleanup; failed persistence remains unavailable.
				try { Invoke-McpTool 'profiler.capture.stop' | Out-Null } catch { }
			}
			$profilerEvidence = New-UnavailableEvidence 'profiler-open-responsiveness' $_.Exception.Message
		}
	}
	$editorWindowPath = Join-Path $outputRoot 'editor-window.png'
	try
	{
		$editorWindowEvidence = Save-EditorWindowCapture `
			-Process $editor `
			-OutputPath $editorWindowPath
	}
	catch
	{
		$editorWindowEvidence = New-UnavailableEvidence `
			'editor-window-screenshot' `
			$_.Exception.Message
	}
	if($playEntered)
	{
		$playExit = Invoke-McpTool 'play.exit' ([ordered]@{
			expectedState = 'playing'
			correlationId = 'virtual-geometry-live-exit'
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
	if($representationRouteOverrideActive)
	{
		$representationRouteReset = Invoke-McpTool 'route.lock' ([ordered]@{ route = 'automatic'; enabled = $false })
		$representationRouteOverrideActive = $false
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
	$measuredGeometryRoute = [string](Get-PropertyValue (
		Get-PropertyValue $postCaptureStats 'data') 'geometryRoute')
	$firstHash = (Get-FileHash -LiteralPath $firstPath -Algorithm SHA256).Hash.ToLowerInvariant()
	$secondHash = (Get-FileHash -LiteralPath $secondPath -Algorithm SHA256).Hash.ToLowerInvariant()
	$summary = [ordered]@{
		schemaVersion = 1
		product = 'virtual-geometry-production'
		route = if($measuredGeometryRoute -eq 'direct_meshlet') {
			'direct-indexed'
		} elseif($deviceAuthoredRoute) {
			'vulkan-device-authored-indirect'
		} else {
			'processor-expanded-fallback'
		}
		geometryRoute = $measuredGeometryRoute
		capturedAt = [DateTime]::UtcNow.ToString('o')
		editor = $editorPath
		project = $projectPath
		mcpEndpoint = $endpoint
		sceneObjectCount = @($scene.data.objects).Count
		validationScene = $validationScene
		validationCamera = $validationCamera
		validationCameraConfiguration = [ordered]@{
			position = @($ValidationCameraPosition)
			target = @($ValidationCameraTarget)
			verticalFovDegrees = 60.0
		}
		validationLight = $validationLight
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
		representationRoute = [ordered]@{
			requested = if([string]::IsNullOrWhiteSpace($representationRouteName)) { $null } else { $representationRouteName }
			set = $representationRouteSet
			reset = $representationRouteReset
		}
		diagnosticsViewType = $DiagnosticsViewType
		instantiatedPrefab = $instances[0]
		instantiatedPrefabs = $instances
		instantiatedGeometry = [ordered]@{
			source = if($useRawMesh) { 'raw-mesh' } else { 'prefab' }
			meshAssetId = if($useRawMesh) { $meshAssetIdentity } else { $null }
			materialAssetId = if($useRawMesh) { $materialAssetIdentity } else { $null }
			objects = $instances
			objectNames = $instanceNames
			scale = $PrefabScale
			scaleTargetObject = if($PrefabScale -ne 1.0) {
				if($useRawMesh) { $instanceNames[0] } else { $PrefabChildObject }
			} else {
				$null
			}
		}
		animationDiagnostics = $animationDiagnostics
		capabilityEvidence = $capabilities
		postCaptureCapabilityEvidence = $postCaptureCapabilities
		postWarmupCaptureStats = $postWarmupCaptureStats
		postWarmupCaptureAsset = $postWarmupCaptureAsset
		postFirstCaptureStats = $postFirstCaptureStats
		postFirstCaptureAsset = $postFirstCaptureAsset
		postCaptureStats = $postCaptureStats
		postCaptureAsset = $postCaptureAsset
		pickingEvidence = $pickingEvidence
		shadowEvidence = $shadowEvidence
		editorWindowEvidence = $editorWindowEvidence
		profilerEvidence = $profilerEvidence
		cameraReview = [ordered]@{
			path = $cameraReviewPath
			sha256 = (Get-FileHash -LiteralPath $cameraReviewPath -Algorithm SHA256).
				Hash.ToLowerInvariant()
		}
		shadowReview = [ordered]@{
			path = $shadowReviewPath
			sha256 = (Get-FileHash -LiteralPath $shadowReviewPath -Algorithm SHA256).
				Hash.ToLowerInvariant()
			envelope = $shadowReview
		}
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
					correlationId = 'virtual-geometry-live-finally-exit'
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
