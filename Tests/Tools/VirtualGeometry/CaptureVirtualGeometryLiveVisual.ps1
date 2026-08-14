[CmdletBinding()]
param(
	[string]$InstalledEditor = 'out/build/RelWithDebInfo/Build/Editor/TerraEngine-Editor.exe',
	[string]$Project = 'Tests/Fixtures/Animation/ImportValidation',
	[string]$Asset = '',
	[string]$Preferences = 'out/validation/virtual-geometry/live/editor-preferences.json',
	[string]$OutputDirectory = 'out/validation/virtual-geometry/live',
	[string]$Prefab = 'Assets/Models/ImportHero_Generated/ImportHero.teprefab',
	[string[]]$PrefabVariants = @(),
	[string]$PrefabName = 'Virtual Geometry Validation Hero',
	[string]$PrefabChildObject = '',
	[string]$MeshAssetId = '',
	[string]$MaterialAssetId = '',
	[string[]]$MaterialAssetIds = @(),
	[double]$PrefabScale = 1.0,
	[double]$RawMeshInstanceScale = 1.0,
	[double]$RawMeshGridSpacing = 1.0,
	[int]$PrefabInstanceCount = 1,
	[int]$PrefabRequestedAnimationMode = -1,
	[string[]]$PrefabDefaultClipAssetIds = @(),
	# Each entry is a comma-separated clip UUID list for the prefab variant at the
	# same index. This keeps mixed-skeleton crowds heterogeneous without applying a
	# clip from one skeleton to an incompatible variant.
	[string[]]$PrefabVariantDefaultClipAssetIds = @(),
	[ValidateRange(0, 1048576)]
	[int]$PrefabDefaultClipStartFrameStep = 0,
	[double]$PrefabSpacing = 0.0,
	[string]$ScenePath = '',
	[switch]$CreateValidationScene,
	[switch]$CleanupValidationScene,
	[string]$ValidationScenePath = 'Assets/Scenes/VirtualGeometryLive.tescene',
	[double[]]$ValidationCameraPosition = @(0.0, 0.5, 1.0),
	[double[]]$ValidationCameraTarget = @(0.0, 0.5, 0.0),
	[ValidateRange(1.0, 179.0)]
	[double]$ValidationCameraVerticalFovDegrees = 60.0,
	[int]$StreamingSettleMilliseconds = 250,
	[int]$SoakDurationSeconds = 0,
	[int]$SoakSampleIntervalSeconds = 10,
	[int]$McpPort = 43155,
	[int]$StartupTimeoutSeconds = 90,
	[int]$CaptureTimeoutMilliseconds = 60000,
	[ValidateRange(6, 64)]
	[int]$PostPresentSampleCount = 8,
	[ValidateRange(0, 10000000)]
	[int]$ProfilerMaxFrames = 16,
	[ValidateRange(1, 3600)]
	[int]$ProfilerCaptureDurationSeconds = 1,
	# Specialized overhead runners can measure the profiler's idle window without
	# creating and selecting a capture between paired conditions.
	[switch]$SkipProfilerCapture,
	# Matched recording comparisons collect the same bounded 100 ms closed-window
	# Engine::Update stream as the disabled control, while the Journal is active.
	[switch]$ProfilerMatchedOverheadMeasurement,
	[ValidateRange(0, 3600)]
	[int]$WarmupSeconds = 0,
	[switch]$ProfilerWindowVisibleDuringCapture,
	# When set, keep the window closed while recording starts and open it at the
	# requested elapsed second. -1 preserves the historical start-visible behavior.
	[ValidateRange(-1, 3600)]
	[int]$ProfilerWindowOpenAtSeconds = -1,
	[ValidateRange(0, 3600)]
	[int]$ProfilerIdleMeasurementDurationSeconds = 0,
	[ValidateRange(0, 3600)]
	[int]$ProfilerHealthCheckpointIntervalSeconds = 0,
	[ValidateRange(10, 1000)]
	[int]$ProfilerIdleMeasurementIntervalMilliseconds = 100,
	[switch]$ProfilerIdleMeasurementWindowVisible,
	# A paired run measures both closed and open-idle states inside one Play Mode
	# session. OpenFirst reverses the order for AB/BA bias control.
	[switch]$ProfilerIdlePairedMeasurement,
	[switch]$ProfilerIdleOpenFirst,
	[ValidateRange(1, 20)]
	[int]$ProfilerIdlePairCount = 1,
	[string]$ValidationRoute = '',
	[ValidateSet('', 'automatic', 'direct', 'virtual-hardware', 'virtual-compute')]
	[string]$RepresentationRoute = '',
	[ValidateSet('Game', 'Scene')]
	[string]$DiagnosticsViewType = 'Game',
	[ValidateSet('', 'off', 'lod', 'cluster', 'meshlet_id', 'bounds', 'culling', 'residency', 'hiz', 'error')]
	[string]$SceneViewVisualization = '',
	[switch]$EnterPlayMode,
	[switch]$UseExistingEditor,
	[switch]$KeepEditorOpen,
	[switch]$ExpectAnimatedChange
	,[switch]$SparseTransformMutation
	,[ValidateRange(0.01, 100.0)]
	[double]$SparseTransformPercentPerFrame = 1.0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../..'))
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
	# Live metrics are emitted as ordered dictionaries before they are serialized.
	# Read their keys explicitly so the same helper works for both those native
	# PowerShell records and the PSCustomObjects returned by MCP JSON responses.
	if($Value -is [Collections.IDictionary] -and $Value.Contains($Name))
	{
		return $Value[$Name]
	}
	$property = $Value.PSObject.Properties[$Name]
	if($null -eq $property)
	{
		return $null
	}
	return $property.Value
}

function Get-TimedVisibilityWindow([object[]]$Samples, [string]$Label)
{
	$values = @($Samples | Where-Object { $_.valid } | ForEach-Object { [double]$_.engineCpuTimeMs } | Sort-Object)
	if($values.Count -lt 30)
	{
		return [ordered]@{
			status = 'unavailable'
			label = $Label
			sampleCount = $values.Count
			reason = 'Fewer than 30 valid Engine CPU timing samples were published.'
		}
	}
	$p95Index = [Math]::Min($values.Count - 1, [Math]::Max(0, [int][Math]::Ceiling($values.Count * 0.95) - 1))
	return [ordered]@{
		status = 'measured'; label = $Label; sampleCount = $values.Count
		metric = 'engine-cpu-time'; timingSource = 'Engine::Update editor-excluded wall duration'
		medianMs = $values[[int][Math]::Floor(($values.Count - 1) * 0.50)]
		p95Ms = $values[$p95Index]; maximumMs = $values[-1]
	}
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
if($CreateValidationScene -and $CleanupValidationScene)
{
	# Interrupted runs can leave their disposable scene behind. Before creating a
	# replacement, remove only an exact FramePublication-* scene under the
	# project's Assets/Scenes directory; authored scenes are never candidates.
	$scenesBoundary = [IO.Path]::GetFullPath(
		(Join-Path $projectPath 'Assets/Scenes')).TrimEnd('\', '/') +
		[IO.Path]::DirectorySeparatorChar
	$validationCandidate = [IO.Path]::GetFullPath((Join-Path $projectPath $ValidationScenePath))
	if(-not $validationCandidate.StartsWith($scenesBoundary, [StringComparison]::OrdinalIgnoreCase))
	{
		throw 'Refusing to clean a validation scene outside Assets/Scenes.'
	}
	$relativeCandidate = $validationCandidate.Substring(
		$projectPath.Length + 1).Replace('\', '/')
	if($relativeCandidate -notmatch '^Assets/Scenes/FramePublication-[^/]+\.tescene$')
	{
		throw 'ValidationScenePath must be an exact disposable Assets/Scenes/FramePublication-*.tescene path.'
	}
	foreach($stalePath in @($validationCandidate, "$validationCandidate.meta"))
	{
		if(Test-Path -LiteralPath $stalePath -PathType Leaf)
		{
			Remove-Item -LiteralPath $stalePath -Force
		}
	}
}
if([string]::IsNullOrWhiteSpace($ScenePath) -and $PrefabInstanceCount -lt 1)
{
	# Disposable raw/prefab captures must create at least one object. Authored
	# scene captures intentionally create none, because their renderables already
	# exist in the immutable scene selected through ScenePath.
	throw 'PrefabInstanceCount must be at least one.'
}
if($RawMeshInstanceScale -le 0.0)
{
	throw 'RawMeshInstanceScale must be positive.'
}
if($RawMeshGridSpacing -le 0.0)
{
	throw 'RawMeshGridSpacing must be positive.'
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
$materialAssetIdentities = @($MaterialAssetIds | ForEach-Object { ([string]$_).Trim().ToLowerInvariant() } | Where-Object { $_ })
$prefabDefaultClipAssetIdentities = @($PrefabDefaultClipAssetIds | ForEach-Object {
	([string]$_).Trim().ToLowerInvariant()
} | Where-Object { $_ })
$prefabVariantDefaultClipAssetIdentities = [Collections.Generic.List[object]]::new()
foreach($variantClipList in $PrefabVariantDefaultClipAssetIds)
{
	$variantClips = @(([string]$variantClipList).Split(',',
		[StringSplitOptions]::RemoveEmptyEntries) |
		ForEach-Object { $_.Trim().ToLowerInvariant() } | Where-Object { $_ })
	$prefabVariantDefaultClipAssetIdentities.Add($variantClips) | Out-Null
}
$prefabVariantIdentities = @($PrefabVariants | ForEach-Object {
	([string]$_).Trim()
} | Where-Object { $_ })
if($prefabVariantIdentities.Count -eq 0)
{
	# Authored-scene validation can intentionally add no objects. In that mode the
	# existing scene is the workload and the default prefab must not be inferred.
	$prefabVariantIdentities = @(if($PrefabInstanceCount -gt 0) { $Prefab })
}
if($prefabVariantIdentities.Count -gt $PrefabInstanceCount)
{
	throw 'PrefabVariants cannot contain more entries than PrefabInstanceCount.'
}
if($prefabVariantIdentities.Count -gt 1 -and $prefabDefaultClipAssetIdentities.Count -gt 0)
{
	throw 'PrefabDefaultClipAssetIds can only override a single prefab/skeleton workload.'
}
if($prefabVariantDefaultClipAssetIdentities.Count -gt $prefabVariantIdentities.Count)
{
	throw 'PrefabVariantDefaultClipAssetIds cannot contain more entries than PrefabVariants.'
}
foreach($variantClips in $prefabVariantDefaultClipAssetIdentities)
{
	foreach($clipAssetIdentity in @($variantClips))
	{
		if($clipAssetIdentity -notmatch '^[0-9a-f]{32}$')
		{
			throw 'PrefabVariantDefaultClipAssetIds entries must contain comma-separated 32-character hexadecimal UUIDs.'
		}
	}
}
foreach($clipAssetIdentity in $prefabDefaultClipAssetIdentities)
{
	if($clipAssetIdentity -notmatch '^[0-9a-f]{32}$')
	{
		throw 'PrefabDefaultClipAssetIds must contain 32-character hexadecimal UUIDs.'
	}
}
if($prefabDefaultClipAssetIdentities.Count -gt $PrefabInstanceCount)
{
	throw 'PrefabDefaultClipAssetIds cannot contain more entries than PrefabInstanceCount.'
}
if($materialAssetIdentities.Count -gt 0 -and -not [string]::IsNullOrWhiteSpace($materialAssetIdentity)) { throw 'Specify MaterialAssetId or MaterialAssetIds, not both.' }
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
foreach($materialIdentity in $materialAssetIdentities) { if($materialIdentity -notmatch '^[0-9a-f]{32}$') { throw 'Every MaterialAssetIds value must be a 32-character hexadecimal UUID.' } }
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

function Get-PngFrameMetrics([string]$Path)
{
	# The soak only needs a stable black-frame/luminance signal, so sample every
	# eighth pixel instead of making validation itself a meaningful CPU load.
	Add-Type -AssemblyName System.Drawing
	$bitmap = [Drawing.Bitmap]::FromFile($Path)
	try
	{
		$sampleCount = [UInt64]0
		$nonBlackCount = [UInt64]0
		$luminanceSum = 0.0
		for($y = 0; $y -lt $bitmap.Height; $y += 8)
		{
			for($x = 0; $x -lt $bitmap.Width; $x += 8)
			{
				$pixel = $bitmap.GetPixel($x, $y)
				$luminance = (0.2126 * $pixel.R) + (0.7152 * $pixel.G) + (0.0722 * $pixel.B)
				$luminanceSum += $luminance
				$sampleCount++
				if($luminance -gt 2.0)
				{
					$nonBlackCount++
				}
			}
		}
		return [ordered]@{
			width = $bitmap.Width
			height = $bitmap.Height
			samples = $sampleCount
			meanLuminance = if($sampleCount -eq 0) { 0.0 } else { $luminanceSum / $sampleCount }
			nonBlackRatio = if($sampleCount -eq 0) { 0.0 } else { [double]$nonBlackCount / $sampleCount }
			blackFrame = $nonBlackCount -eq 0
		}
	}
	finally
	{
		$bitmap.Dispose()
	}
}

# Keep standalone visual capture acceptance aligned with RuntimeSample. Eight
# bright sampled pixels prevent a one-pixel readback artifact from qualifying,
# while the luminance threshold rejects otherwise valid-but-empty black frames.
$minimumSampledNonBlackPixels = [UInt64]8
$minimumMeanLuminance = 2.0
function Test-MeaningfulPngMetrics($Metrics)
{
	if($null -eq $Metrics -or [bool](Get-PropertyValue $Metrics 'blackFrame'))
	{
		return $false
	}
	$sampleCount = [UInt64](Get-PropertyValue $Metrics 'samples')
	$nonBlackPixels = [UInt64][Math]::Floor(
		$sampleCount * [double](Get-PropertyValue $Metrics 'nonBlackRatio'))
	return $sampleCount -gt 0 -and
		$nonBlackPixels -ge $minimumSampledNonBlackPixels -and
		[double](Get-PropertyValue $Metrics 'meanLuminance') -gt
			$minimumMeanLuminance
}

function Get-CaptureForegroundPickCandidates([string]$Path)
{
	# The authoritative Vulkan readback and the PNG use the same dedicated
	# capture target. Choose real, substantial foreground pixels from that PNG
	# instead of assuming a fixed 480x270 coordinate intersects the workload.
	# These candidates only select readback locations; they never synthesize an
	# identity, so a missing or rejected GPU result remains unavailable evidence.
	Add-Type -AssemblyName System.Drawing
	$bitmap = [Drawing.Bitmap]::FromFile($Path)
	try
	{
		# Editor grid/orientation guides are bright enough to pass a simple
		# luminance test but do not write a conventional-visibility identity. Keep
		# candidates close to neutral so the red/green/blue guides cannot win the
		# centre-distance tie-break over an actual rendered model.
		$maximumPickChannelSpread = 40
		# A 21x21 area proof rejects the wide floor-grid segments present in the
		# mixed animation capture. The old 7x7 rule accepted the 74/74/74 grid at
		# (235,126), while a substantial model surface occupies this larger window.
		$pickNeighbourhoodRadius = 10
		$minimumPickInteriorCoverage = 0.90
		$maximumPickCandidates = 6
		$minimumCandidateSeparationPixels = 24
		$windowWidth = (2 * $pickNeighbourhoodRadius) + 1
		$windowPixelCount = $windowWidth * $windowWidth
		$minimumPickInteriorPixels = [int][Math]::Ceiling(
			$windowPixelCount * $minimumPickInteriorCoverage)
		$centreX = ($bitmap.Width - 1) / 2.0
		$centreY = ($bitmap.Height - 1) / 2.0
		# Summed-area rows make every 21x21 coverage query constant time. The
		# bounded ranking therefore remains cheap at capture resolution and avoids
		# repeatedly sampling the same bitmap pixels for every candidate.
		$foregroundIntegral = New-Object 'int[,]' ($bitmap.Height + 1), ($bitmap.Width + 1)
		for($y = 0; $y -lt $bitmap.Height; ++$y)
		{
			$rowForegroundPixels = 0
			for($x = 0; $x -lt $bitmap.Width; ++$x)
			{
				$pixel = $bitmap.GetPixel($x, $y)
				$luminance = (0.2126 * $pixel.R) + (0.7152 * $pixel.G) +
					(0.0722 * $pixel.B)
				$channelSpread = [Math]::Max($pixel.R, [Math]::Max($pixel.G, $pixel.B)) -
					[Math]::Min($pixel.R, [Math]::Min($pixel.G, $pixel.B))
				if($luminance -gt $minimumMeanLuminance -and
					$channelSpread -le $maximumPickChannelSpread)
				{
					++$rowForegroundPixels
				}
				$foregroundIntegral[($y + 1), ($x + 1)] =
					$foregroundIntegral[$y, ($x + 1)] + $rowForegroundPixels
			}
		}
		$candidatePool = [Collections.Generic.List[object]]::new()
		for($y = $pickNeighbourhoodRadius; $y -lt $bitmap.Height - $pickNeighbourhoodRadius; ++$y)
		{
			for($x = $pickNeighbourhoodRadius; $x -lt $bitmap.Width - $pickNeighbourhoodRadius; ++$x)
			{
				$pixel = $bitmap.GetPixel($x, $y)
				$luminance = (0.2126 * $pixel.R) + (0.7152 * $pixel.G) +
					(0.0722 * $pixel.B)
				$channelSpread = [Math]::Max($pixel.R, [Math]::Max($pixel.G, $pixel.B)) -
					[Math]::Min($pixel.R, [Math]::Min($pixel.G, $pixel.B))
				if($luminance -le $minimumMeanLuminance -or
					$channelSpread -gt $maximumPickChannelSpread)
				{
					continue
				}
				$left = $x - $pickNeighbourhoodRadius
				$top = $y - $pickNeighbourhoodRadius
				$rightExclusive = $x + $pickNeighbourhoodRadius + 1
				$bottomExclusive = $y + $pickNeighbourhoodRadius + 1
				$neighbourhoodCount = $foregroundIntegral[$bottomExclusive, $rightExclusive] -
					$foregroundIntegral[$top, $rightExclusive] -
					$foregroundIntegral[$bottomExclusive, $left] +
					$foregroundIntegral[$top, $left]
				if($neighbourhoodCount -lt $minimumPickInteriorPixels)
				{
					continue
				}
				$distanceSquared = (($x - $centreX) * ($x - $centreX)) +
					(($y - $centreY) * ($y - $centreY))
				$candidatePool.Add([ordered]@{
					pixel = @($x, $y)
					luminance = $luminance
					channelSpread = $channelSpread
					neighbourhoodForegroundPixels = $neighbourhoodCount
					neighbourhoodRadiusPixels = $pickNeighbourhoodRadius
					minimumInteriorPixels = $minimumPickInteriorPixels
					distanceSquared = $distanceSquared
				})
			}
		}
		# Rank full interiors first, then use distance and pixel coordinates as a
		# deterministic tie-break. Spatial separation prevents six retries from
		# probing the same model patch when an individual identity readback is empty.
		$rankedCandidates = @($candidatePool | Sort-Object `
			@{ Expression = { [int]$_.neighbourhoodForegroundPixels }; Descending = $true }, `
			@{ Expression = { [double]$_.distanceSquared }; Descending = $false }, `
			@{ Expression = { [int]$_.pixel[1] }; Descending = $false }, `
			@{ Expression = { [int]$_.pixel[0] }; Descending = $false })
		$selectedCandidates = [Collections.Generic.List[object]]::new()
		$minimumCandidateSeparationSquared =
			$minimumCandidateSeparationPixels * $minimumCandidateSeparationPixels
		foreach($candidate in $rankedCandidates)
		{
			$nearExistingCandidate = @($selectedCandidates | Where-Object {
				$offsetX = [int]$candidate.pixel[0] - [int]$_.pixel[0]
				$offsetY = [int]$candidate.pixel[1] - [int]$_.pixel[1]
				(($offsetX * $offsetX) + ($offsetY * $offsetY)) -lt
					$minimumCandidateSeparationSquared
			}).Count -ne 0
			if($nearExistingCandidate)
			{
				continue
			}
			$candidate.rank = $selectedCandidates.Count + 1
			$selectedCandidates.Add($candidate)
			if($selectedCandidates.Count -ge $maximumPickCandidates)
			{
				break
			}
		}
		return @($selectedCandidates)
	}
	finally
	{
		$bitmap.Dispose()
	}
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
$ownsEditor = $false
$stdoutTask = $null
$stderrTask = $null
$createdValidationScenePath = $null
$authoredScenePath = $null
$authoredSceneHashBefore = $null
$lastScenePath = Join-Path $projectPath 'Library/LastScene.txt'
$lastSceneExisted = Test-Path -LiteralPath $lastScenePath -PathType Leaf
$lastSceneContent = if($lastSceneExisted) { [IO.File]::ReadAllText($lastScenePath) } else { $null }
$sceneSelectedByStartup = $false
if(-not [string]::IsNullOrWhiteSpace($ScenePath))
{
	# LastScene.txt stores an Assets-relative spelling. Selecting through this
	# startup state avoids the approval-gated scene.open automation call.
	$startupSceneValue = $ScenePath.Replace('\', '/')
	if($startupSceneValue.StartsWith('Assets/', [StringComparison]::OrdinalIgnoreCase))
	{
		$startupSceneValue = $startupSceneValue.Substring(7)
	}
	$lastSceneDirectory = Split-Path -Parent $lastScenePath
	New-Item -ItemType Directory -Force -Path $lastSceneDirectory | Out-Null
	[IO.File]::WriteAllText($lastScenePath, $startupSceneValue, $utf8NoBom)
	$sceneSelectedByStartup = $true
}
if(-not [string]::IsNullOrWhiteSpace($ScenePath))
{
	$authoredScenePath = Resolve-RepoPath (Join-Path $projectPath $ScenePath)
	if(-not (Test-Path -LiteralPath $authoredScenePath -PathType Leaf)) { throw "Authored scene missing: $authoredScenePath" }
	$authoredSceneHashBefore = (Get-FileHash -LiteralPath $authoredScenePath -Algorithm SHA256).Hash.ToLowerInvariant()
}
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
$sparseTransformOperationActive = $false
$sparseTransformOperationId = ''
$sparseTransformEvidence = New-UnavailableEvidence 'sparse-transform-mutation' 'Sparse transform mutation was not requested.'
$warmupEvidence = New-UnavailableEvidence 'capture-warmup' 'No capture warmup was requested.'
try
{
	if($UseExistingEditor)
	{
		$runningEditors = @(Get-Process -Name 'TerraEngine-Editor' -ErrorAction SilentlyContinue)
		if($runningEditors.Count -ne 1)
		{
			throw 'UseExistingEditor requires exactly one running TerraEngine editor.'
		}
		$editor = $runningEditors[0]
	}
	else
	{
		$start = [Diagnostics.ProcessStartInfo]::new()
		$start.FileName = $editorPath
		$start.Arguments = "--project `"$projectPath`""
		$start.WorkingDirectory = Split-Path -Parent $editorPath
		$start.UseShellExecute = $false
		# A retained editor outlives this PowerShell invocation. Do not attach its
		# stdout/stderr to invocation-owned pipes: once the reader tasks go out of
		# scope, a busy editor can fill a pipe and stall its main thread (including
		# the MCP listener). Short owned runs retain their diagnostic log capture.
		$redirectEditorOutput = -not $KeepEditorOpen
		$start.RedirectStandardOutput = $redirectEditorOutput
		$start.RedirectStandardError = $redirectEditorOutput
		$start.EnvironmentVariables['TERRAENGINE_EDITOR_PREFS_SESSION_PATH'] =
			$preferencesPath
		$start.EnvironmentVariables['TERRAENGINE_SUPPRESS_PROJECT_RECENTS'] = '1'
		if(-not $EnterPlayMode -and [string]::IsNullOrWhiteSpace($ScenePath))
		{
			# A disposable static capture does not need project simulation readiness,
			# so avoid briefly rendering the developer's last scene before the
			# validation scene becomes active. An explicitly requested authored scene
			# must still open normally so its scene-dependent MCP tools can register.
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
		$ownsEditor = $true
		if($redirectEditorOutput)
		{
			$stdoutTask = $editor.StandardOutput.ReadToEndAsync()
			$stderrTask = $editor.StandardError.ReadToEndAsync()
		}
	}
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
					($sceneSelectedByStartup -or $CreateValidationScene -or
						$tools -contains 'prefab.instantiate') -and
					($sceneSelectedByStartup -or [string]::IsNullOrWhiteSpace($ScenePath) -or
						$tools -contains 'scene.open') -and
					([string]::IsNullOrWhiteSpace($Asset) -or
						$tools -contains 'asset.virtualGeometry.inspect') -and
					([string]::IsNullOrWhiteSpace($validationRouteName) -or
						($tools -contains 'renderer.virtualGeometry.validationRoute.set' -and
							$tools -contains 'renderer.virtualGeometry.validationRoute.reset')) -and
					(-not $CreateValidationScene -or
						$tools -contains 'scene.create') -and
					(-not $SparseTransformMutation -or
						($tools -contains 'renderer.validation.sparseTransform.start' -and
						 $tools -contains 'renderer.validation.sparseTransform.status' -and
						 $tools -contains 'renderer.validation.sparseTransform.stop')))
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
	if(-not [string]::IsNullOrWhiteSpace($ScenePath) -and -not $sceneSelectedByStartup)
	{
		# Opening the supplied authored scene gives installed validation the same
		# animator and renderer state as the real project without saving any of
		# the disposable instances added below.
		$validationScene = Invoke-McpTool 'scene.open' ([ordered]@{
			path = $ScenePath
		})
	}
	if($sceneSelectedByStartup)
	{
		# The editor advertises its MCP descriptors before the asynchronously
		# selected LastScene has crossed its protected publication boundary. Refresh
		# availability here so authored-scene captures do not mistake "not open yet"
		# for a missing Scene View visualizer or Play-mode capability.
		$startupSceneRequirements = @('scene.list_objects')
		if(-not [string]::IsNullOrWhiteSpace($SceneViewVisualization))
		{
			$startupSceneRequirements += 'scene_view.visualization.set'
		}
		if($EnterPlayMode)
		{
			$startupSceneRequirements += @('play.enter', 'play.exit')
		}
		$startupSceneDeadline = [DateTime]::UtcNow.AddSeconds(30)
		do
		{
			$startupSceneDescriptors = @((Invoke-McpRpc 'tools/list').result.tools)
			$tools = @(
				$startupSceneDescriptors |
					Where-Object { [bool]$_.availability.available } |
					ForEach-Object { [string]$_.name }
			)
			$startupSceneReady = @($startupSceneRequirements | Where-Object {
				$tools -notcontains $_
			}).Count -eq 0
			if(-not $startupSceneReady)
			{
				Start-Sleep -Milliseconds 100
			}
		}
		while(-not $startupSceneReady -and [DateTime]::UtcNow -lt $startupSceneDeadline)
		if(-not $startupSceneReady)
		{
			throw "Configured authored scene did not publish required MCP tools: $($startupSceneRequirements -join ', ')."
		}
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
		$validationLightPosition = @(2.0, 3.0, 3.0)
		$validationLightIntensity = 2500.0
		$validationLightRange = 20.0
		if($useRawMesh -and $PrefabInstanceCount -ge 1000)
		{
			# The dense raw-static grid is deliberately camera-facing and much
			# larger than the one-object control. Keep its disposable review light
			# near the capture camera with a range that covers every grid row.
			$validationLightPosition = @(
				[double]$ValidationCameraPosition[0],
				([double]$ValidationCameraTarget[1] + 10.0),
				([double]$ValidationCameraPosition[2] - 10.0))
			$validationLightIntensity = 40000.0
			$validationLightRange = 300.0
		}
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
			value = $validationLightPosition
		}) | Out-Null
		Invoke-McpTool 'component.set_property' ([ordered]@{
			object = 'Virtual Geometry Validation Light'
			component = 'LightComponent'
			property = 'intensity'
			value = $validationLightIntensity
		}) | Out-Null
		Invoke-McpTool 'component.set_property' ([ordered]@{
			object = 'Virtual Geometry Validation Light'
			component = 'LightComponent'
			property = 'range'
			value = $validationLightRange
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
		($PrefabInstanceCount -gt 1 -or $PrefabRequestedAnimationMode -ge 0 -or
			$prefabVariantIdentities.Count -gt 1 -or
			$prefabDefaultClipAssetIdentities.Count -gt 0 -or
			$PrefabDefaultClipStartFrameStep -gt 0) -and
		$tools -contains 'prefab.instantiate_many'
	$usedBulkRawPublication = $useRawMesh -and $PrefabInstanceCount -gt 1 -and
		$tools -contains 'transaction.execute'
	if($usedBulkRawPublication)
	{
		# Transactional object creation already supplies the required Transform.
		# Every raw instance also receives a deterministic position and, when
		# requested, a readable direct-mesh scale in this same protected publication.
		# Six operations per renderable remain well below the operation limit, while
		# 80 objects keep object/value JSON nodes below the separate 4,096-node
		# budget on every named workload.
		$objectsPerTransaction = 80
		$rawGridWidth = [int][Math]::Ceiling([Math]::Sqrt($PrefabInstanceCount))
		$rawGridRowCount = [int][Math]::Ceiling(
			$PrefabInstanceCount / [double]$rawGridWidth)
		$denseRawGrid = $PrefabInstanceCount -ge 1000
		# Dense raw-static rows use world Y rather than camera depth. The dedicated
		# camera faces the XY plane, so every independently authored transform stays
		# reviewable instead of projecting into one horizontal strip. Small scenes
		# retain the prior perspective-aware depth layout for material review.
		$rawGridDepthStart = if($denseRawGrid) { 0.0 } else { 5.0 }
		$rawGridDepthStep = if($denseRawGrid) { 0.0 } else { 3.0 }
		$rawCameraDistance = [Math]::Abs(
			[double]$ValidationCameraPosition[2] - [double]$ValidationCameraTarget[2])
		for($batchStart = 0; $batchStart -lt $PrefabInstanceCount;
			$batchStart += $objectsPerTransaction)
		{
			$batchEnd = [Math]::Min(
				$PrefabInstanceCount, $batchStart + $objectsPerTransaction)
			$operations = [Collections.Generic.List[object]]::new()
			for($instanceIndex = $batchStart; $instanceIndex -lt $batchEnd;
				++$instanceIndex)
			{
				$instanceName = $instanceNames[$instanceIndex]
				$localId = "raw-$instanceIndex"
				$target = [ordered]@{ localId = $localId }
				$instanceMaterialIdentity = if($materialAssetIdentities.Count -gt 0) {
					$materialAssetIdentities[$instanceIndex % $materialAssetIdentities.Count]
				} else { $materialAssetIdentity }
				$row = [Math]::Floor($instanceIndex / $rawGridWidth)
				$column = $instanceIndex % $rawGridWidth
				if($denseRawGrid)
				{
					$positionX = ($column - (($rawGridWidth - 1) / 2.0)) *
						$RawMeshGridSpacing
					$positionY = ($row - (($rawGridRowCount - 1) / 2.0)) *
						$RawMeshGridSpacing
					$positionZ = 0.0
				}
				else
				{
					$depthFromTarget = $rawGridDepthStart + ($row * $rawGridDepthStep)
					# A perspective-aware horizontal span keeps small material-review
					# rows inside the fixed capture camera.
					$horizontalSpan = ($rawCameraDistance + $depthFromTarget) * 0.60
					$columnFraction = if($rawGridWidth -eq 1) {
						0.0
					} else {
						(2.0 * $column / ($rawGridWidth - 1)) - 1.0
					}
					$positionX = $columnFraction * $horizontalSpan
					$positionY = 0.0
					$positionZ = -$depthFromTarget
				}
				$operations.Add([ordered]@{
					kind = 'object.create'; localId = $localId; name = $instanceName
				})
				$operations.Add([ordered]@{
					kind = 'object.set_metadata'; target = $target; static = $true
				})
				$operations.Add([ordered]@{
					kind = 'component.set_property'; target = $target
					component = 'Transform'; property = 'Position'
					value = @($positionX, $positionY, $positionZ)
				})
				if($RawMeshInstanceScale -ne 1.0)
				{
					$operations.Add([ordered]@{
						kind = 'component.set_property'; target = $target
						component = 'Transform'; property = 'Scale'
						value = @($RawMeshInstanceScale, $RawMeshInstanceScale, $RawMeshInstanceScale)
					})
				}
				$operations.Add([ordered]@{
					kind = 'component.add'; target = $target; component = 'MeshRenderComponent'
				})
				$operations.Add([ordered]@{
					kind = 'component.set_property'; target = $target
					component = 'MeshRenderComponent'; property = 'meshId'; value = $meshAssetIdentity
				})
				$operations.Add([ordered]@{
					kind = 'component.set_property'; target = $target
					component = 'MeshRenderComponent'; property = 'materialId'; value = $instanceMaterialIdentity
				})
			}
			$instances += Invoke-McpTool 'transaction.execute' ([ordered]@{
				label = "Publish raw renderer workload $batchStart-$($batchEnd - 1)"
				operations = $operations
			})
		}
	}
	elseif($useRawMesh)
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
			$instanceMaterialIdentity = if($materialAssetIdentities.Count -gt 0) { $materialAssetIdentities[$instanceIndex % $materialAssetIdentities.Count] } else { $materialAssetIdentity }
			if(-not [string]::IsNullOrWhiteSpace($instanceMaterialIdentity))
			{
				Invoke-McpTool 'component.set_property' ([ordered]@{
					object = $instanceName
					component = 'MeshRenderComponent'
					property = 'materialId'
					value = $instanceMaterialIdentity
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
		$globalColumns = [int][Math]::Ceiling([Math]::Sqrt($PrefabInstanceCount))
		$totalRows = [int][Math]::Ceiling($PrefabInstanceCount / [double]$globalColumns)
		$variantBaseCount = [int][Math]::Floor(
			$PrefabInstanceCount / [double]$prefabVariantIdentities.Count)
		$variantRemainder = $PrefabInstanceCount % $prefabVariantIdentities.Count
		$nextGlobalRow = 0
		for($variantIndex = 0; $variantIndex -lt $prefabVariantIdentities.Count; ++$variantIndex)
		{
			$variantCount = $variantBaseCount + $(if($variantIndex -lt $variantRemainder) { 1 } else { 0 })
			$variantRows = [int][Math]::Ceiling($variantCount / [double]$globalColumns)
			# Every asset occupies complete global rows. The groups therefore share one
			# centered crowd footprint without overlapping despite separate atomic
			# prefab publications for their incompatible serialized object graphs.
			$localHalfWidth = ($globalColumns - 1) * 0.5
			$totalHalfRows = ($totalRows - 1) * 0.5
			$variantOriginZ = ($nextGlobalRow + $localHalfWidth - $totalHalfRows) * $PrefabSpacing
			$bulkArguments = [ordered]@{
				prefab = $prefabVariantIdentities[$variantIndex]
				count = $variantCount
				namePrefix = if($prefabVariantIdentities.Count -eq 1) {
					$PrefabName
				} else {
					"$PrefabName Variant $($variantIndex + 1)"
				}
				spacing = $PrefabSpacing
				columns = $globalColumns
				origin = @(0.0, 0.0, $variantOriginZ)
			}
			if($PrefabRequestedAnimationMode -ge 0)
			{
				# The policy is serialized into every cloned AnimatorComponent before
				# publication, so no automatic evaluation frame can race a later mutation.
				$bulkArguments.requestedAnimationMode = $PrefabRequestedAnimationMode
			}
			# PowerShell 5 unwraps a single pipeline result into a scalar. Keep the
			# complete conditional inside an array subexpression so a one-clip prefab
			# still exposes Count and serializes defaultClipAssetIds as a JSON array.
			$variantClipIdentities = @(
				if($variantIndex -lt $prefabVariantDefaultClipAssetIdentities.Count)
				{
					$prefabVariantDefaultClipAssetIdentities[$variantIndex]
				}
				else
				{
					$prefabDefaultClipAssetIdentities
				}
			)
			if($variantClipIdentities.Count -gt 0)
			{
				# Clip identities are applied round-robin to private prefab clones. This
				# produces a heterogeneous workload without per-object live mutations.
				$bulkArguments.defaultClipAssetIds = $variantClipIdentities
			}
			if($PrefabDefaultClipStartFrameStep -gt 0)
			{
				# Phase offsets share the same protected publication as the clip identities.
				$bulkArguments.defaultClipStartFrameStep = $PrefabDefaultClipStartFrameStep
			}
			$instances += Invoke-McpTool 'prefab.instantiate_many' $bulkArguments
			$nextGlobalRow += $variantRows
		}
	}
	elseif([string]::IsNullOrWhiteSpace($ScenePath))
	{
		for($instanceIndex = 0; $instanceIndex -lt $PrefabInstanceCount; ++$instanceIndex)
		{
			$instances += Invoke-McpTool 'prefab.instantiate' ([ordered]@{
				prefab = $Prefab
				name = $instanceNames[$instanceIndex]
			})
		}
	}

	if([string]::IsNullOrWhiteSpace($ScenePath))
	{
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
		# Route availability is published by the first completed rendered frame.
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
				verticalFovDegrees = $ValidationCameraVerticalFovDegrees
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
	# Authored benchmark scenes can contain thousands of objects. Components are
	# not used by this capture (the result is only summarized as an object count),
	# and serializing every component can exceed the bounded MCP response budget
	# before the profiler even starts. Keep the richer component listing for
	# disposable validation scenes, while authored ScenePath captures request the
	# smallest read-only listing and still retain the pre/post scene hash guard.
	# The scene listing is paged by the editor at 256 objects. Walk every page
	# and retain only the exact count needed by the evidence summary; this keeps
	# large authored scenes bounded while detecting missing or repeated pages.
	$sceneObjectCount = 0
	$sceneOffset = 0
	$scenePageCount = 0
	$sceneIncludeComponents = [string]::IsNullOrWhiteSpace($ScenePath)
	do
	{
		$scenePage = Invoke-McpTool 'scene.list_objects' ([ordered]@{
			includeComponents = $sceneIncludeComponents
			offset = $sceneOffset
			limit = 256
		})
		$sceneData = Get-PropertyValue $scenePage 'data'
		$sceneObjects = Get-PropertyValue $sceneData 'objects'
		if($null -eq $sceneObjects -or $sceneObjects -is [string])
		{
			throw 'scene.list_objects returned no bounded objects page.'
		}
		$pageCount = @($sceneObjects).Count
		$pageReturned = Get-PropertyValue $sceneData 'returned'
		if($null -eq $pageReturned -or [int]$pageReturned -ne $pageCount -or $pageCount -gt 256)
		{
			throw 'scene.list_objects returned an invalid page count.'
		}
		$sceneObjectCount += $pageCount
		$scenePageCount++
		$sceneHasMore = [bool](Get-PropertyValue $sceneData 'hasMore')
		if($sceneHasMore)
		{
			$nextOffset = Get-PropertyValue $sceneData 'nextOffset'
			if($null -eq $nextOffset -or [int]$nextOffset -le $sceneOffset)
			{
				throw 'scene.list_objects returned a non-advancing nextOffset.'
			}
			$sceneOffset = [int]$nextOffset
		}
		if($scenePageCount -gt 100000)
		{
			throw 'scene.list_objects exceeded the bounded page limit.'
		}
	}
	while($sceneHasMore)
	$sceneViewVisualizationEvidence = $null
	if(-not [string]::IsNullOrWhiteSpace($SceneViewVisualization))
	{
		if($tools -notcontains 'scene_view.visualization.set')
		{
			throw 'The installed editor does not advertise Scene View visualization control.'
		}
		$sceneViewVisualizationEvidence = Invoke-McpTool `
			'scene_view.visualization.set' `
			([ordered]@{ mode = $SceneViewVisualization })
	}
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
		# Play transitions are published at a protected frame boundary. The request
		# can therefore lose an optimistic-state race while still reaching the exact
		# requested state. Accept only that idempotent success; every other rejection
		# remains a hard live-test failure.
		$playEnterReply = Invoke-McpRpc 'tools/call' ([ordered]@{
			name = 'play.enter'
			arguments = [ordered]@{
				expectedState = 'stopped'
				correlationId = 'virtual-geometry-live-enter'
			}
		})
		$playEnter = Get-McpEnvelope $playEnterReply 'play.enter'
		if(-not [bool](Get-PropertyValue $playEnter 'ok'))
		{
			$currentState = [string](Get-PropertyValue (Get-PropertyValue $playEnter 'data') 'currentState')
			$errorCategory = [string](Get-PropertyValue (Get-PropertyValue $playEnter 'error') 'category')
			if($currentState -ne 'playing' -or $errorCategory -ne 'conflict')
			{
				throw "MCP tool 'play.enter' failed before reaching the playing state."
			}
		}
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
	# Warmup begins only after the exact scene, population, routes, and Play state
	# are active, but it must finish before visual readback as well as profiler
	# recording. Large crowds can render a safe static fallback while their first
	# animation/GPU publication settles; grading that transition as the measured
	# frame would produce a false black-capture failure.
	if($WarmupSeconds -gt 0)
	{
		if($tools -notcontains 'profiler.window.visibility.set')
		{
			throw 'Warmup frame evidence requires profiler.window.visibility.set.'
		}
		$warmupBegin = Invoke-McpTool 'profiler.window.visibility.set' `
			([ordered]@{ visible = $false })
		$warmupStartFrame = [UInt64]$warmupBegin.data.editorFrameIndex
		$warmupWatch = [Diagnostics.Stopwatch]::StartNew()
		$warmupEnd = $warmupBegin
		do
		{
			Start-Sleep -Seconds 1
			$warmupEnd = Invoke-McpTool 'profiler.window.visibility.set' `
				([ordered]@{ visible = $false })
			$warmupCompletedFrames = [UInt64]$warmupEnd.data.editorFrameIndex -
				$warmupStartFrame
		}
		while($warmupWatch.Elapsed.TotalSeconds -lt $WarmupSeconds -or
			$warmupCompletedFrames -lt 120)
		$warmupWatch.Stop()
		$warmupEvidence = [ordered]@{
			status = 'measured'
			kind = 'capture-warmup'
			seconds = $warmupWatch.Elapsed.TotalSeconds
			completedFrames = $warmupCompletedFrames
			startFrameIndex = $warmupStartFrame
			endFrameIndex = [UInt64]$warmupEnd.data.editorFrameIndex
		}
	}
	$cameraArguments = [ordered]@{
		name = 'Virtual Geometry Validation Capture'
		# Keep capture-only evidence associated with the logical surface under
		# test without mutating either viewport's authored camera.
		viewType = $DiagnosticsViewType
		position = @($ValidationCameraPosition)
		target = @($ValidationCameraTarget)
		up = @(0.0, 1.0, 0.0)
		projection = 'perspective'
		verticalFovDegrees = $ValidationCameraVerticalFovDegrees
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
	# A looping animation can legitimately return to the same pose at the two
	# ordinary capture instants. Preserve that pair, then sample a few bounded,
	# deliberately non-uniform offsets before declaring the rendered animation
	# frozen. Static validation never enters this retry path and therefore keeps
	# its strict two-frame byte-identity contract.
	$animatedRetryCaptures = @()
	if($ExpectAnimatedChange)
	{
		$gradedHashes = @(
			(Get-FileHash -LiteralPath $firstPath -Algorithm SHA256).Hash.ToLowerInvariant(),
			(Get-FileHash -LiteralPath $secondPath -Algorithm SHA256).Hash.ToLowerInvariant())
		$retryDelaysMilliseconds = @(113, 197, 307)
		for($retryIndex = 0;
			$retryIndex -lt $retryDelaysMilliseconds.Count -and
			@($gradedHashes | Sort-Object -Unique).Count -lt 2;
			++$retryIndex)
		{
			Start-Sleep -Milliseconds $retryDelaysMilliseconds[$retryIndex]
			$retryPath = Join-Path $outputRoot (
				"live-vulkan-retry-{0:d2}.png" -f ($retryIndex + 1))
			$retryEnvelope = Invoke-McpPngCapture `
				'camera.capture_custom' `
				$cameraArguments `
				$retryPath
			$retryHash = (Get-FileHash -LiteralPath $retryPath -Algorithm SHA256).
				Hash.ToLowerInvariant()
			$animatedRetryCaptures += [ordered]@{
				path = $retryPath
				sha256 = $retryHash
				delayMilliseconds = $retryDelaysMilliseconds[$retryIndex]
				envelope = $retryEnvelope
			}
			$gradedHashes += $retryHash
		}
	}
	$postCaptureAnimationDiagnostics = $null
	if($tools -contains 'animation.runtime.diagnostics')
	{
		# Read the same immutable summary after both images. Comparing its
		# generation with the pre-warmup snapshot distinguishes a renderer/capture
		# freeze from an animation authority that never advanced.
		$postCaptureAnimationDiagnostics = Invoke-McpTool `
			'animation.runtime.diagnostics' `
			([ordered]@{
				includeDetails = $false
				maximumItems = 256
			})
	}
	$soakEvidence = [ordered]@{
		status = if($SoakDurationSeconds -gt 0) { 'measured' } else { 'not-requested' }
		durationSeconds = $SoakDurationSeconds
		sampleIntervalSeconds = $SoakSampleIntervalSeconds
		authoredCameraMutated = $false
		captures = @()
		errors = @()
	}
	if($SoakDurationSeconds -gt 0)
	{
		if($SoakSampleIntervalSeconds -lt 1)
		{
			throw 'SoakSampleIntervalSeconds must be at least one second.'
		}
		$soakDirectory = Join-Path $outputRoot 'sampled-presents'
		[IO.Directory]::CreateDirectory($soakDirectory) | Out-Null
		$soakWatch = [Diagnostics.Stopwatch]::StartNew()
		$sampleIndex = 0
		while($soakWatch.Elapsed.TotalSeconds -lt $SoakDurationSeconds)
		{
			# Rotate and gently dolly the custom capture camera around its target.
			# This never reads or writes the scene's authored camera transform.
			$angle = ($sampleIndex % 24) * ([Math]::PI / 12.0)
			$baseX = [double]$ValidationCameraPosition[0] - [double]$ValidationCameraTarget[0]
			$baseY = [double]$ValidationCameraPosition[1] - [double]$ValidationCameraTarget[1]
			$baseZ = [double]$ValidationCameraPosition[2] - [double]$ValidationCameraTarget[2]
			$dolly = 1.0 + (0.12 * [Math]::Sin($sampleIndex * ([Math]::PI / 6.0)))
			$rotatedX = (($baseX * [Math]::Cos($angle)) - ($baseZ * [Math]::Sin($angle))) * $dolly
			$rotatedZ = (($baseX * [Math]::Sin($angle)) + ($baseZ * [Math]::Cos($angle))) * $dolly
			$sampleArguments = [ordered]@{}
			foreach($entry in $cameraArguments.GetEnumerator())
			{
				$sampleArguments[$entry.Key] = $entry.Value
			}
			$sampleArguments.name = "Virtual Geometry Soak Present $sampleIndex"
			$sampleArguments.position = @(
				([double]$ValidationCameraTarget[0] + $rotatedX)
				([double]$ValidationCameraTarget[1] + $baseY)
				([double]$ValidationCameraTarget[2] + $rotatedZ))
			$samplePath = Join-Path $soakDirectory ("present-{0:d4}.png" -f $sampleIndex)
			try
			{
				$sampleEnvelope = Invoke-McpPngCapture 'camera.capture_custom' $sampleArguments $samplePath
				$sampleStats = Invoke-McpToolWithReadinessRetry `
					'renderer.virtualGeometry.stats' `
					([ordered]@{ viewType = $DiagnosticsViewType })
				$soakEvidence.captures += [ordered]@{
					index = $sampleIndex
					elapsedSeconds = $soakWatch.Elapsed.TotalSeconds
					position = $sampleArguments.position
					path = $samplePath
					sha256 = (Get-FileHash -LiteralPath $samplePath -Algorithm SHA256).Hash.ToLowerInvariant()
					metrics = Get-PngFrameMetrics $samplePath
					virtualGeometryStats = $sampleStats
					envelope = $sampleEnvelope
				}
			}
			catch
			{
				$soakEvidence.errors += [ordered]@{
					index = $sampleIndex
					elapsedSeconds = $soakWatch.Elapsed.TotalSeconds
					message = $_.Exception.Message
				}
			}
			$sampleIndex++
			$remainingSeconds = $SoakDurationSeconds - $soakWatch.Elapsed.TotalSeconds
			if($remainingSeconds -gt 0)
			{
				Start-Sleep -Milliseconds ([Math]::Min(
					$SoakSampleIntervalSeconds * 1000,
					[Math]::Ceiling($remainingSeconds * 1000)))
			}
		}
		$soakWatch.Stop()
		$soakEvidence.durationSecondsMeasured = $soakWatch.Elapsed.TotalSeconds
		$soakEvidence.blackFrameCount = @($soakEvidence.captures | Where-Object {
			[bool]$_.metrics.blackFrame }).Count
		$soakEvidence.uniqueFrameHashes = @($soakEvidence.captures.sha256 | Sort-Object -Unique).Count
	}
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
	# Timestamp receipts are fence-delayed. Submit several distinct post-present
	# frames and query capabilities after each one so a single editor session
	# contributes independent, backend-issued execution IDs. These are actual
	# captures, never duplicated values, and the adapter rejects the run if the
	# backend cannot publish at least the required measured rows.
	$postPresentDirectory = Join-Path $outputRoot 'post-present-samples'
	[IO.Directory]::CreateDirectory($postPresentDirectory) | Out-Null
	$postPresentCaptures = @()
	$postPresentCapabilityEvidence = @()
	for($postPresentIndex = 0; $postPresentIndex -lt $PostPresentSampleCount;
		++$postPresentIndex)
	{
		$postPresentArguments = [ordered]@{}
		foreach($entry in $cameraArguments.GetEnumerator())
		{
			$postPresentArguments[$entry.Key] = $entry.Value
		}
		$postPresentArguments.name = "Virtual Geometry Post Present $postPresentIndex"
		$postPresentPath = Join-Path $postPresentDirectory (
			"present-{0:d2}.png" -f $postPresentIndex)
		$postPresentCapture = Invoke-McpPngCapture `
			'camera.capture_custom' `
			$postPresentArguments `
			$postPresentPath
		$postPresentCapabilities = Invoke-McpTool `
			'renderer.virtualGeometry.capabilityEvidence' `
			([ordered]@{ view = $DiagnosticsViewType })
		$postPresentCaptures += [ordered]@{
			index = $postPresentIndex
			path = $postPresentPath
			sha256 = (Get-FileHash -LiteralPath $postPresentPath -Algorithm SHA256).
				Hash.ToLowerInvariant()
			frameId = Get-PropertyValue (Get-PropertyValue $postPresentCapture 'data') 'frameId'
			envelope = $postPresentCapture
		}
		$postPresentCapabilityEvidence += $postPresentCapabilities
	}
	$postPresentHashes = @($postPresentCaptures | ForEach-Object { $_.sha256 } | Sort-Object -Unique)
	$gpuPresentation = Get-PropertyValue (Get-PropertyValue $postCaptureAnimationDiagnostics 'data') 'gpuPresentation'
	$presentationPackets = @((Get-PropertyValue $gpuPresentation 'packets'))
	$unsafePresentationPackets = @($presentationPackets | Where-Object { [bool](Get-PropertyValue $_ 'safeFallback') })
	$healthyAnimatedPresentation = $null -ne $gpuPresentation -and [UInt64](Get-PropertyValue $gpuPresentation 'ownerMappingCount') -gt 0 -and [UInt64](Get-PropertyValue $gpuPresentation 'dispatchCommandCount') -gt 0 -and $presentationPackets.Count -gt 0 -and $unsafePresentationPackets.Count -eq 0
	# Query once more after the final present for the canonical final snapshot.
	# The sequence above is retained alongside it for runtime-row provenance.
	$postCaptureCapabilities = Invoke-McpTool `
		'renderer.virtualGeometry.capabilityEvidence' `
		([ordered]@{ view = $DiagnosticsViewType })
	$postCaptureStats = Invoke-McpToolWithReadinessRetry `
		'renderer.virtualGeometry.stats' `
		([ordered]@{ viewType = $DiagnosticsViewType })
	# View-dependent readback operations become available only after a complete
	# rendered frame. Refresh the descriptors here so startup availability cannot
	# incorrectly suppress authoritative picking or other delayed evidence.
	$postCaptureDescriptors = @((Invoke-McpRpc 'tools/list').result.tools)
	$tools = @(
		$postCaptureDescriptors |
			Where-Object { [bool]$_.availability.available } |
			ForEach-Object { [string]$_.name })
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
	$profilerWindowEvidence = [ordered]@{ status = 'not-requested'; openAtSeconds = $ProfilerWindowOpenAtSeconds }
	$profilerTimingWindowEvidence = $null
	$profilerIdleEvidence = New-UnavailableEvidence 'profiler-open-idle-overhead' 'Profiler idle measurement was not requested.'
	$matchedOverheadEvidence = New-UnavailableEvidence 'profiler-matched-recording-overhead' 'Matched recording overhead measurement was not requested.'
	$profilerHealthCheckpoints = [Collections.Generic.List[object]]::new()
	if($tools -contains 'renderer.virtualGeometry.authoritativePick')
	{
		try
		{
			$pickCandidates = @(Get-CaptureForegroundPickCandidates $firstPath)
			if($pickCandidates.Count -eq 0)
			{
				$pickingEvidence = New-UnavailableEvidence 'authoritative-picking' `
					'No substantial non-grid foreground pixel was available in the dedicated capture target.'
			}
			else
			{
				# A bounded, spatially separated candidate list tolerates an occluded
				# model while avoiding a retry storm. Every request still reads the
				# authoritative uint4 identity target, never the screenshot itself.
				$pickAttempts = [Collections.Generic.List[object]]::new()
				$pickRequest = $null
				$pick = $null
				$pickState = $null
				$selectedPickPixel = @($pickCandidates[0].pixel)
				$selectedPickCandidate = $null
				foreach($pickCandidate in $pickCandidates)
				{
					$selectedPickPixel = @($pickCandidate.pixel)
					$pickRequest = Invoke-McpTool 'renderer.virtualGeometry.authoritativePick' ([ordered]@{
						viewType = $DiagnosticsViewType
						capture = $true
						pixel = @($selectedPickPixel)
						request = $true
					})
					$pick = $null
					$pickState = $null
					for($poll = 0; $poll -lt 5; ++$poll)
					{
						Start-Sleep -Milliseconds 100
						$pick = Invoke-McpTool 'renderer.virtualGeometry.authoritativePick' ([ordered]@{
							viewType = $DiagnosticsViewType
							capture = $true
							pixel = @($selectedPickPixel)
							request = $false
						})
						$pickState = Get-PropertyValue (
							Get-PropertyValue $pick 'data') 'authoritative'
						if([string](Get-PropertyValue $pickState 'status') -eq 'Ready')
						{
							break
						}
					}
					$pickAttempts.Add([ordered]@{
						candidate = $pickCandidate
						request = $pickRequest
						state = $pick
						status = [string](Get-PropertyValue $pickState 'status')
					})
					if([string](Get-PropertyValue $pickState 'status') -eq 'Ready')
					{
						$selectedPickCandidate = $pickCandidate
						break
					}
				}
			if([string](Get-PropertyValue $pickState 'status') -ne 'Ready')
			{
				$pickingEvidence = New-UnavailableEvidence 'authoritative-picking' "No substantial capture candidate reached Ready ($([string](Get-PropertyValue $pickState 'status')))."
				$pickingEvidence.captureForegroundCandidates = $pickCandidates
				$pickingEvidence.attempts = @($pickAttempts)
				$pickingEvidence.state = $pickState
			}
			else
			{
				$pickingEvidence = [ordered]@{
				status = 'measured'
				kind = 'authoritative-picking'
				viewType = $DiagnosticsViewType
				pixel = @($selectedPickPixel)
				captureForegroundCandidate = $selectedPickCandidate
				captureForegroundCandidates = $pickCandidates
				attempts = @($pickAttempts)
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
	if($ProfilerIdleMeasurementDurationSeconds -gt 0)
	{
		if($tools -notcontains 'profiler.window.visibility.set')
		{
			$profilerIdleEvidence = New-UnavailableEvidence 'profiler-open-idle-overhead' 'Profiler window visibility automation is not advertised.'
		}
		else
		{
			$idlePairEvidence = [Collections.Generic.List[object]]::new()
			$idlePairTotal = if($ProfilerIdlePairedMeasurement) { $ProfilerIdlePairCount } else { 1 }
			for($idlePairIndex = 1; $idlePairIndex -le $idlePairTotal; ++$idlePairIndex)
			{
				$pairOpenFirst = [bool]$ProfilerIdleOpenFirst
				if(($idlePairIndex % 2) -eq 0) { $pairOpenFirst = -not $pairOpenFirst }
				$idleMeasurementStates = if($ProfilerIdlePairedMeasurement) {
					if($pairOpenFirst) { @($true, $false) } else { @($false, $true) }
				} else { @([bool]$ProfilerIdleMeasurementWindowVisible) }
				$idleConditionEvidence = [ordered]@{}
				$idleConditionSamples = @{
					closed = [Collections.Generic.List[object]]::new()
					openIdle = [Collections.Generic.List[object]]::new()
				}
				$idleConditionBlocks = @{ closed = [Collections.Generic.List[object]]::new(); openIdle = [Collections.Generic.List[object]]::new() }
				$idleBlockCount = if($ProfilerIdlePairedMeasurement) { [Math]::Max(1, [int][Math]::Ceiling($ProfilerIdleMeasurementDurationSeconds / 10.0)) } else { 1 }
				$idleBlockDurationSeconds = if($ProfilerIdlePairedMeasurement) { [double]$ProfilerIdleMeasurementDurationSeconds / $idleBlockCount } else { [double]$ProfilerIdleMeasurementDurationSeconds }
			for($idleBlock = 0; $idleBlock -lt $idleBlockCount; ++$idleBlock)
			{
			foreach($idleWindowVisible in $idleMeasurementStates)
			{
			try
			{
				# This phase intentionally runs while the Profiler is idle. Recording
				# would measure capture instrumentation as well as window presentation
				# and would therefore not answer the open-idle performance question.
				Invoke-McpTool 'profiler.window.visibility.set' ([ordered]@{
					visible = [bool]$idleWindowVisible
				}) | Out-Null
				Start-Sleep -Milliseconds 250
				$idleWatch = [Diagnostics.Stopwatch]::StartNew()
				$idleSamples = [Collections.Generic.List[object]]::new()
				while($idleWatch.Elapsed.TotalSeconds -lt $idleBlockDurationSeconds)
				{
					$timingSample = Invoke-McpTool 'profiler.window.visibility.set' ([ordered]@{
						visible = [bool]$idleWindowVisible
					})
					$engineCpuTimeMs = [double](Get-PropertyValue $timingSample.data 'engineCpuTimeMs')
					$engineCpuTimeValid = [bool](Get-PropertyValue $timingSample.data 'engineCpuTimeValid')
					$finiteEngineCpuTime = $engineCpuTimeValid -and
						-not [double]::IsNaN($engineCpuTimeMs) -and
						-not [double]::IsInfinity($engineCpuTimeMs) -and $engineCpuTimeMs -ge 0.0
					$frameTimeMs = [double](Get-PropertyValue $timingSample.data 'editorFrameTimeMs')
					$finiteFrameTime = -not [double]::IsNaN($frameTimeMs) -and -not [double]::IsInfinity($frameTimeMs)
					$profilerWindowDrawMs = [double](Get-PropertyValue $timingSample.data 'profilerWindowDrawMs')
					$profilerWindowDrawValid = [bool](Get-PropertyValue $timingSample.data 'profilerWindowDrawValid')
					$finiteProfilerWindowDraw = $profilerWindowDrawValid -and -not [double]::IsNaN($profilerWindowDrawMs) -and -not [double]::IsInfinity($profilerWindowDrawMs) -and $profilerWindowDrawMs -ge 0.0
					$imguiRenderMs = [double](Get-PropertyValue $timingSample.data 'imguiRenderMs')
					$imguiRenderValid = [bool](Get-PropertyValue $timingSample.data 'imguiRenderValid')
					$finiteImGuiRender = $imguiRenderValid -and -not [double]::IsNaN($imguiRenderMs) -and -not [double]::IsInfinity($imguiRenderMs) -and $imguiRenderMs -ge 0.0
					$presentUiMs = [double](Get-PropertyValue $timingSample.data 'presentUiMs')
					$presentUiValid = [bool](Get-PropertyValue $timingSample.data 'presentUiValid')
					$finitePresentUi = $presentUiValid -and -not [double]::IsNaN($presentUiMs) -and -not [double]::IsInfinity($presentUiMs) -and $presentUiMs -ge 0.0
					if($finiteEngineCpuTime)
					{
						$idleSamples.Add([ordered]@{
							elapsedMilliseconds = $idleWatch.Elapsed.TotalMilliseconds
							engineFrameIndex = [UInt64](Get-PropertyValue $timingSample.data 'engineFrameIndex')
							engineCpuTimeMs = $engineCpuTimeMs
							frameTimeMs = $frameTimeMs
							editorFrameTimeValid = $finiteFrameTime -and $frameTimeMs -gt 0.0
							editorFrameIndex = [UInt64](Get-PropertyValue $timingSample.data 'editorFrameIndex')
							profilerWindowDrawFrameIndex = [UInt64](Get-PropertyValue $timingSample.data 'profilerWindowDrawFrameIndex')
							profilerWindowDrawMs = $profilerWindowDrawMs
							profilerWindowDrawValid = $finiteProfilerWindowDraw
							imguiRenderFrameIndex = [UInt64](Get-PropertyValue $timingSample.data 'imguiRenderFrameIndex')
							imguiRenderMs = $imguiRenderMs
							imguiRenderValid = $finiteImGuiRender
							presentUiFrameIndex = [UInt64](Get-PropertyValue $timingSample.data 'presentUiFrameIndex')
							presentUiMs = $presentUiMs
							presentUiValid = $finitePresentUi
						})
					}
					Start-Sleep -Milliseconds $ProfilerIdleMeasurementIntervalMilliseconds
				}
				$idleWatch.Stop()
				if($idleSamples.Count -eq 0)
				{
					throw 'Profiler idle measurement produced no valid completed frame-delta samples.'
				}
				$sortedIdleFrameTimes = @($idleSamples | ForEach-Object {
					[double]$_.engineCpuTimeMs
				} | Sort-Object)
				$medianIndex = [int][Math]::Floor(($sortedIdleFrameTimes.Count - 1) * 0.50)
				$p95Index = [int][Math]::Min($sortedIdleFrameTimes.Count - 1,
					[Math]::Ceiling($sortedIdleFrameTimes.Count * 0.95) - 1)
				$p99Index = [int][Math]::Min($sortedIdleFrameTimes.Count - 1,
					[Math]::Ceiling($sortedIdleFrameTimes.Count * 0.99) - 1)
				$profilerWindowDrawSamples = @($idleSamples | Where-Object {
					[bool]$_.profilerWindowDrawValid
				} | ForEach-Object { [double]$_.profilerWindowDrawMs } | Sort-Object)
				$profilerWindowDrawTiming = if($profilerWindowDrawSamples.Count -eq 0)
				{
					[ordered]@{
						availability = 'unavailable'
						reason = 'ProfilerWindow::OnGui has not completed a measured visible frame.'
					}
				}
				else
				{
					$drawMedianIndex = [int][Math]::Floor(($profilerWindowDrawSamples.Count - 1) * 0.50)
					$drawP95Index = [int][Math]::Min($profilerWindowDrawSamples.Count - 1,
						[Math]::Ceiling($profilerWindowDrawSamples.Count * 0.95) - 1)
					[ordered]@{
						availability = 'measured'
						metric = 'ProfilerWindow::OnGui CPU duration'
						sampleCount = $profilerWindowDrawSamples.Count
						medianMs = $profilerWindowDrawSamples[$drawMedianIndex]
						p95Ms = $profilerWindowDrawSamples[$drawP95Index]
						maximumMs = $profilerWindowDrawSamples[-1]
					}
				}
				$getStageTimingSummary = {
					param([string]$DurationProperty, [string]$ValidProperty, [string]$Metric)
					$values = @($idleSamples | Where-Object {
						[bool]$_.($ValidProperty)
					} | ForEach-Object { [double]$_.($DurationProperty) } | Sort-Object)
					if($values.Count -eq 0)
					{
						return [ordered]@{ availability = 'unavailable'; reason = 'No completed stage measurement was published.' }
					}
					$stageMedianIndex = [int][Math]::Floor(($values.Count - 1) * 0.50)
					$stageP95Index = [int][Math]::Min($values.Count - 1,
						[Math]::Ceiling($values.Count * 0.95) - 1)
					return [ordered]@{
						availability = 'measured'
						metric = $Metric
						sampleCount = $values.Count
						medianMs = $values[$stageMedianIndex]
						p95Ms = $values[$stageP95Index]
						maximumMs = $values[-1]
					}
				}
				$imguiRenderTiming = & $getStageTimingSummary 'imguiRenderMs' 'imguiRenderValid' 'ImGui::Render CPU duration'
				$presentUiTiming = & $getStageTimingSummary 'presentUiMs' 'presentUiValid' 'VulkanContext::PresentEditor CPU duration'
				$hostFrameTiming = & $getStageTimingSummary 'frameTimeMs' 'editorFrameTimeValid' 'Editor host frame delta'
				$profilerIdleEvidence = [ordered]@{
					status = 'measured'
					kind = 'profiler-open-idle-overhead'
					windowVisible = [bool]$idleWindowVisible
					profilerState = 'idle'
					metric = 'engine-cpu-time'
					timingSource = 'Engine::Update editor-excluded wall duration'
					durationSeconds = $idleWatch.Elapsed.TotalSeconds
					sampleIntervalMilliseconds = $ProfilerIdleMeasurementIntervalMilliseconds
					sampleCount = $idleSamples.Count
					medianMs = $sortedIdleFrameTimes[$medianIndex]
					p95Ms = $sortedIdleFrameTimes[$p95Index]
					p99Ms = $sortedIdleFrameTimes[$p99Index]
					minimumMs = $sortedIdleFrameTimes[0]
					maximumMs = $sortedIdleFrameTimes[-1]
					profilerWindowDrawTiming = $profilerWindowDrawTiming
					hostFrameTiming = $hostFrameTiming
					imguiRenderTiming = $imguiRenderTiming
					presentUiTiming = $presentUiTiming
					samples = $idleSamples
				}
			}
			catch
			{
				$profilerIdleEvidence = New-UnavailableEvidence 'profiler-open-idle-overhead' $_.Exception.Message
			}
			finally
			{
				try
				{
					Invoke-McpTool 'profiler.window.visibility.set' ([ordered]@{ visible = $false }) | Out-Null
				}
				catch { }
			}
			$idleConditionName = if($idleWindowVisible) { 'openIdle' } else { 'closed' }
			$idleConditionEvidence[$idleConditionName] = $profilerIdleEvidence
			if($profilerIdleEvidence.status -eq 'measured') {
				$idleConditionSamples[$idleConditionName].AddRange($idleSamples.ToArray())
				$blockValues = @($idleSamples | ForEach-Object { [double]$_.engineCpuTimeMs } | Sort-Object)
				$blockP95 = [int][Math]::Min($blockValues.Count - 1, [Math]::Ceiling($blockValues.Count * 0.95) - 1)
				$idleConditionBlocks[$idleConditionName].Add([ordered]@{
					block = $idleBlock; sampleCount = $blockValues.Count
					medianMs = $blockValues[[int][Math]::Floor(($blockValues.Count - 1) * 0.50)]
					p95Ms = $blockValues[$blockP95]
				})
			}
			}
			}
			# Collapse the interleaved blocks into one condition record so the
			# paired gate compares samples collected throughout the same workload
			# phases, while transition frames remain excluded by each block settle.
			foreach($conditionName in @('closed','openIdle')) {
				$bucket = $idleConditionSamples[$conditionName]
				if($bucket.Count -gt 0) {
					$orderedTimes = @($bucket | ForEach-Object { [double]$_.engineCpuTimeMs } | Sort-Object)
					$medIndex = [int][Math]::Floor(($orderedTimes.Count - 1) * 0.50)
					$p95Index = [int][Math]::Min($orderedTimes.Count - 1, [Math]::Ceiling($orderedTimes.Count * 0.95) - 1)
					$merged = $idleConditionEvidence[$conditionName]
					$merged.sampleCount = $bucket.Count
					$merged.medianMs = $orderedTimes[$medIndex]
					$merged.p95Ms = $orderedTimes[$p95Index]
					$merged.minimumMs = $orderedTimes[0]
					$merged.maximumMs = $orderedTimes[-1]
					$merged.durationSeconds = [double]$ProfilerIdleMeasurementDurationSeconds
					$merged.samples = @($bucket)
					$merged.blocks = @($idleConditionBlocks[$conditionName])
				}
			}
			if($ProfilerIdlePairedMeasurement)
			{
				# A single-condition idle sample has no matched counterpart. Build
				# paired deltas only for the explicit AB/BA experiment.
				$pairedBlockDeltas = [Collections.Generic.List[object]]::new()
				$closedBlocks = @($idleConditionBlocks.closed); $openBlocks = @($idleConditionBlocks.openIdle)
				if($closedBlocks.Count -ne $openBlocks.Count -or $closedBlocks.Count -eq 0) { throw 'Interleaved profiler idle blocks are missing or unequal between conditions.' }
				for($blockIndex = 0; $blockIndex -lt $closedBlocks.Count; ++$blockIndex) {
					$closedBlock = $closedBlocks[$blockIndex]; $openBlock = $openBlocks[$blockIndex]
					$pairedBlockDeltas.Add([ordered]@{
						block = $blockIndex
						medianDeltaPercent = (($openBlock.medianMs - $closedBlock.medianMs) / $closedBlock.medianMs) * 100.0
						p95DeltaPercent = (($openBlock.p95Ms - $closedBlock.p95Ms) / $closedBlock.p95Ms) * 100.0
					})
				}
				$pairMedianDeltas = @($pairedBlockDeltas | ForEach-Object medianDeltaPercent | Sort-Object)
				$pairP95Deltas = @($pairedBlockDeltas | ForEach-Object p95DeltaPercent | Sort-Object)
				$idlePairEvidence.Add([ordered]@{
					pair = $idlePairIndex
					order = @($idleMeasurementStates | ForEach-Object { if($_) { 'open-idle' } else { 'closed' } })
					conditions = $idleConditionEvidence
					blockDeltas = @($pairedBlockDeltas)
					medianDeltaPercent = $pairMedianDeltas[[int][Math]::Floor(($pairMedianDeltas.Count - 1) * 0.50)]
					p95DeltaPercent = $pairP95Deltas[[int][Math]::Floor(($pairP95Deltas.Count - 1) * 0.50)]
				})
			}
			}
			if($ProfilerIdlePairedMeasurement)
			{
				$pairedConditionsMeasured = @($idlePairEvidence | Where-Object {
					[string]$_.conditions.closed.status -ne 'measured' -or
					[string]$_.conditions.openIdle.status -ne 'measured'
				}).Count -eq 0
				$profilerIdleEvidence = [ordered]@{
					status = if($pairedConditionsMeasured) { 'measured' } else { 'unavailable' }
					kind = 'profiler-open-idle-paired-overhead'
					pairCount = $idlePairEvidence.Count
					pairs = $idlePairEvidence
				}
			}
		}
	}
	if(-not $SkipProfilerCapture -and
		$tools -contains 'profiler.capture.start' -and
		$tools -contains 'profiler.capture.stop')
	{
		$profilerWatch = [Diagnostics.Stopwatch]::StartNew()
		$profilerStarted = $false
		$profilerStopRequested = $false
	$profilerHealthBefore = $null
	$profilerHealthAfter = $null
	$finishCaptureSubmissionMilliseconds = $null
	$finishMcpRoundTripMilliseconds = $null
	try
	{
		if($SparseTransformMutation)
		{
			$sparseStart = Invoke-McpTool 'renderer.validation.sparseTransform.start' ([ordered]@{
				expectedObjectCount = $PrefabInstanceCount
				percentPerFrame = $SparseTransformPercentPerFrame
				displacement = 0.01
			})
			$sparseTransformOperationId = [string]$sparseStart.data.operationId
			if([string]::IsNullOrWhiteSpace($sparseTransformOperationId))
			{
				throw 'Sparse transform start returned no operation identity.'
			}
			$sparseTransformOperationActive = $true
		}
		$openLater = $ProfilerWindowOpenAtSeconds -ge 0
		if($tools -contains 'profiler.window.visibility.set')
		{
			Invoke-McpTool 'profiler.window.visibility.set' ([ordered]@{
				visible = [bool]($ProfilerWindowVisibleDuringCapture -and -not $openLater)
			}) | Out-Null
			# Allow the requested layout to become the completed immutable UI state
			# before measurement begins.
			Start-Sleep -Milliseconds 250
		}
		# A zero bound deliberately selects unlimited history for coherence
			# and soak validation; short visual smokes retain the bounded default.
		$profilerStart = Invoke-McpTool 'profiler.capture.start' ([ordered]@{
				captureLevel = 'sampled'
				maxFrames = $ProfilerMaxFrames
				sampleIntervalFrames = 1
			})
		$profilerStarted = $true
		if($tools -contains 'profiler.capture.health') {
			$profilerHealthBefore = Invoke-McpTool 'profiler.capture.health'
		}
		$captureStart = [Diagnostics.Stopwatch]::StartNew()
		$nextHealthCheckpointSeconds = if($ProfilerHealthCheckpointIntervalSeconds -gt 0) { [double]$ProfilerHealthCheckpointIntervalSeconds } else { [double]::PositiveInfinity }
		if($ProfilerMatchedOverheadMeasurement)
		{
			if($openLater) { throw 'Matched recording overhead measurement cannot be combined with timed profiler-window opening.' }
			if($tools -notcontains 'profiler.window.visibility.set') { throw 'Matched recording overhead requires profiler.window.visibility.set.' }
			Invoke-McpTool 'profiler.window.visibility.set' ([ordered]@{ visible = $false }) | Out-Null
			Start-Sleep -Milliseconds 250
			$matchedWatch = [Diagnostics.Stopwatch]::StartNew()
			$matchedSamples = [Collections.Generic.List[object]]::new()
			while($matchedWatch.Elapsed.TotalSeconds -lt $ProfilerCaptureDurationSeconds)
			{
				$timing = Invoke-McpTool 'profiler.window.visibility.set' ([ordered]@{ visible = $false })
				$ms = [double](Get-PropertyValue $timing.data 'engineCpuTimeMs')
				$valid = [bool](Get-PropertyValue $timing.data 'engineCpuTimeValid') -and $ms -ge 0.0 -and -not [double]::IsNaN($ms) -and -not [double]::IsInfinity($ms)
				if($valid) { $matchedSamples.Add([ordered]@{ elapsedMilliseconds = $matchedWatch.Elapsed.TotalMilliseconds; engineFrameIndex = [UInt64](Get-PropertyValue $timing.data 'engineFrameIndex'); engineCpuTimeMs = $ms; valid = $true }) }
				Start-Sleep -Milliseconds 100
			}
			$matchedWatch.Stop()
			if($matchedSamples.Count -eq 0) { throw 'Matched recording overhead produced no valid Engine CPU samples.' }
			$matchedValues = @($matchedSamples | ForEach-Object { [double]$_.engineCpuTimeMs } | Sort-Object)
			$matchedP95Index = [int][Math]::Min($matchedValues.Count - 1, [Math]::Ceiling($matchedValues.Count * 0.95) - 1)
			$matchedOverheadEvidence = [ordered]@{
				status = 'measured'; kind = 'profiler-matched-recording-overhead'; profilerState = 'recording'; windowVisible = $false
				metric = 'engine-cpu-time'; timingSource = 'Engine::Update editor-excluded wall duration'; sampleIntervalMilliseconds = 100
				durationSeconds = $matchedWatch.Elapsed.TotalSeconds; sampleCount = $matchedValues.Count
				medianMs = $matchedValues[[int][Math]::Floor(($matchedValues.Count - 1) * 0.50)]; p95Ms = $matchedValues[$matchedP95Index]
				samples = @($matchedSamples)
			}
		}
		if($openLater)
		{
			if($tools -notcontains 'profiler.window.visibility.set') { throw 'Timed profiler-window opening requires profiler.window.visibility.set.' }
			$pairedWindows = [Collections.Generic.List[object]]::new()
			while($captureStart.Elapsed.TotalSeconds -lt $ProfilerWindowOpenAtSeconds)
			{
				Start-Sleep -Milliseconds 100
			}
			for($windowPair = 0; $windowPair -lt 5; ++$windowPair)
			{
				# Every condition gets its own one-second settle. This avoids counting
				# the completed frame from the visibility request itself and keeps each
				# pair comparable even when the workload is changing.
				Invoke-McpTool 'profiler.window.visibility.set' ([ordered]@{ visible = $false }) | Out-Null
				Start-Sleep -Milliseconds 1000
				$closedSamples = [Collections.Generic.List[object]]::new()
				$closedDeadline = [DateTime]::UtcNow.AddSeconds(10)
				while($closedSamples.Count -lt 30 -and [DateTime]::UtcNow -lt $closedDeadline)
				{
					$timing = Invoke-McpTool 'profiler.window.visibility.set' ([ordered]@{ visible = $false })
					$ms = [double](Get-PropertyValue $timing.data 'engineCpuTimeMs')
					$valid = [bool](Get-PropertyValue $timing.data 'engineCpuTimeValid') -and $ms -ge 0.0 -and -not [double]::IsNaN($ms) -and -not [double]::IsInfinity($ms)
					if($valid){$closedSamples.Add([ordered]@{engineCpuTimeMs=$ms;valid=$true})}
					Start-Sleep -Milliseconds 100
				}
				$openRequest = [Diagnostics.Stopwatch]::StartNew(); $openResult = Invoke-McpTool 'profiler.window.visibility.set' ([ordered]@{ visible = $true }); $openRequest.Stop(); Start-Sleep -Milliseconds 1000
				$openSamples = [Collections.Generic.List[object]]::new()
				$openDeadline = [DateTime]::UtcNow.AddSeconds(10)
				while($openSamples.Count -lt 30 -and [DateTime]::UtcNow -lt $openDeadline)
				{
					$timing = Invoke-McpTool 'profiler.window.visibility.set' ([ordered]@{ visible = $true })
					$ms = [double](Get-PropertyValue $timing.data 'engineCpuTimeMs'); $valid = [bool](Get-PropertyValue $timing.data 'engineCpuTimeValid') -and $ms -ge 0.0 -and -not [double]::IsNaN($ms) -and -not [double]::IsInfinity($ms)
					if($valid){$openSamples.Add([ordered]@{engineCpuTimeMs=$ms;valid=$true})}; Start-Sleep -Milliseconds 100
				}
				$closedWindow = Get-TimedVisibilityWindow $closedSamples.ToArray() "closed-pair-$($windowPair + 1)"; $openWindow = Get-TimedVisibilityWindow $openSamples.ToArray() "open-pair-$($windowPair + 1)"
				if($closedWindow.status -ne 'measured' -or $openWindow.status -ne 'measured'){throw 'Timed profiler-window pair did not produce 30 valid samples per condition.'}
				if([double]$closedWindow.medianMs -le 0.0 -or [double]$closedWindow.p95Ms -le 0.0 -or [double]$openWindow.medianMs -lt 0.0 -or [double]$openWindow.p95Ms -lt 0.0){throw 'Timed profiler-window pair published non-positive timing statistics.'}
				$pairedWindows.Add([ordered]@{pair=$windowPair + 1;closed=$closedWindow;open=$openWindow;latencyMilliseconds=$openRequest.Elapsed.TotalMilliseconds;response=$openResult.data;medianDeltaPercent=(($openWindow.medianMs-$closedWindow.medianMs)/$closedWindow.medianMs)*100.0;p95DeltaPercent=(($openWindow.p95Ms-$closedWindow.p95Ms)/$closedWindow.p95Ms)*100.0})
			}
			$preWindow = $pairedWindows[0].closed; $postWindow = $pairedWindows[0].open
			$profilerWindowEvidence.preOpen = $preWindow
			$profilerWindowEvidence.postOpen = $postWindow
			$profilerWindowEvidence.pairedWindows = @($pairedWindows)
			$latencies = @($pairedWindows | ForEach-Object { [double]$_.latencyMilliseconds })
			$profilerWindowEvidence.maxVisibilityLatencyMilliseconds = [double](($latencies | Measure-Object -Maximum).Maximum)
			$medians = @($pairedWindows | ForEach-Object { [double]$_.medianDeltaPercent } | Sort-Object)
			$p95s = @($pairedWindows | ForEach-Object { [double]$_.p95DeltaPercent } | Sort-Object)
			$profilerWindowEvidence.medianDeltaPercent = $medians[2]
			$profilerWindowEvidence.p95DeltaPercent = $p95s[2]
			$profilerWindowEvidence.visibilityRequestMilliseconds = $profilerWindowEvidence.maxVisibilityLatencyMilliseconds
			$profilerWindowEvidence.twoPercentGate = [double]$profilerWindowEvidence.medianDeltaPercent -le 2.0 -and [double]$profilerWindowEvidence.p95DeltaPercent -le 2.0 -and $profilerWindowEvidence.maxVisibilityLatencyMilliseconds -le 1000.0
			$profilerWindowEvidence.status = 'measured'
		}
		while($captureStart.Elapsed.TotalSeconds -lt [Math]::Max(
			$ProfilerCaptureDurationSeconds,
			$StreamingSettleMilliseconds / 1000.0))
		{
			if($captureStart.Elapsed.TotalSeconds -ge $nextHealthCheckpointSeconds -and $tools -contains 'profiler.capture.health') {
				$checkpoint = Invoke-McpTool 'profiler.capture.health'
				$profilerHealthCheckpoints.Add([ordered]@{ elapsedSeconds = $captureStart.Elapsed.TotalSeconds; health = $checkpoint })
				$nextHealthCheckpointSeconds += [double]$ProfilerHealthCheckpointIntervalSeconds
			}
			Start-Sleep -Milliseconds 100
		}
		if($tools -contains 'profiler.capture.health') {
			$profilerHealthAfter = Invoke-McpTool 'profiler.capture.health'
		}
		$profilerStopWatch = [Diagnostics.Stopwatch]::StartNew()
		$profilerStop = Invoke-McpTool 'profiler.capture.stop'
		$profilerStopWatch.Stop()
		$finishMcpRoundTripMilliseconds = $profilerStopWatch.Elapsed.TotalMilliseconds
		$finishCaptureSubmissionMilliseconds = Get-PropertyValue $profilerStop.data 'finishCaptureSubmissionMilliseconds'
		if($null -eq $finishCaptureSubmissionMilliseconds)
		{
			throw 'Profiler capture stop omitted authoritative finishCaptureSubmissionMilliseconds.'
		}
		$finishCaptureSubmissionMilliseconds = [double]$finishCaptureSubmissionMilliseconds
		if([double]::IsNaN($finishCaptureSubmissionMilliseconds) -or
			[double]::IsInfinity($finishCaptureSubmissionMilliseconds) -or
			$finishCaptureSubmissionMilliseconds -lt 0.0)
		{
			throw 'Profiler capture stop returned an invalid authoritative finish duration.'
		}
			if($sparseTransformOperationActive)
			{
				$sparseStop = Invoke-McpTool 'renderer.validation.sparseTransform.stop' ([ordered]@{
					operationId = $sparseTransformOperationId
				})
				$sparseTransformOperationActive = $false
				# Stop prevents new batches, while the last already-enqueued batch may
				# still need one protected scene-mutation flush. Poll its exact status
				# instead of weakening the evidence gate or racing that boundary.
				$sparseFinal = $sparseStop
				$sparseDeadline = [DateTime]::UtcNow.AddSeconds(30)
				while(-not [bool]$sparseFinal.data.failed -and
					[UInt64]$sparseFinal.data.pendingMutationCount -ne 0 -and
					[DateTime]::UtcNow -lt $sparseDeadline)
				{
					Start-Sleep -Milliseconds 100
					$sparseFinal = Invoke-McpTool 'renderer.validation.sparseTransform.status' ([ordered]@{
						operationId = $sparseTransformOperationId
					})
				}
				$sparseTransformEvidence = [ordered]@{
					status = 'measured'
					kind = 'sparse-transform-mutation'
					data = $sparseFinal.data
				}
				if([bool]$sparseFinal.data.failed -or [UInt64]$sparseFinal.data.pendingMutationCount -ne 0)
				{
					throw 'Sparse transform mutation did not finish with complete protected-commit evidence.'
				}
			}
			# Once stop is accepted, persistence owns this recording. Never issue a
			# second stop from error cleanup because that could save the finalized
			# document again under a second timestamp.
			$profilerStopRequested = $true
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
			# Refresh after finalization so committed count, resident permits, and
			# finalization timing describe the terminal session rather than the
			# last pre-stop frame.
			if($tools -contains 'profiler.capture.health') {
				$profilerHealthAfter = Invoke-McpTool 'profiler.capture.health'
			}
			# The durable stop job identifies the exact artifact it persisted. A
			# before/after list delta is unsafe because project-open discovery is
			# asynchronous and may publish older captures between the two reads.
			$profilerJobRecord = $profilerJob.data.job
			$profilerJobResult = Get-PropertyValue $profilerJobRecord 'result'
			$profilerArtifactId = [string](Get-PropertyValue $profilerJobResult 'artifactId')
			if([string]::IsNullOrWhiteSpace($profilerArtifactId))
			{
				$profilerJobArtifacts = @(Get-PropertyValue $profilerJobRecord 'artifacts')
				$profilerArtifactId = [string](
					$profilerJobArtifacts |
						Where-Object { [string]$_.kind -eq 'profiler_capture' } |
						Select-Object -First 1 -ExpandProperty artifactId)
			}
			if([string]::IsNullOrWhiteSpace($profilerArtifactId))
			{
				throw 'Profiler persistence succeeded without publishing its artifact identity.'
			}

			$profilerArtifact = $null
			$profilerArtifactDeadline = [DateTime]::UtcNow.AddSeconds(10)
			do
			{
				$profilerAfter = Invoke-McpTool 'profiler.capture.list'
				$matchingProfilerArtifacts = @(
					$profilerAfter.data.artifacts |
						Where-Object { [string]$_.artifactId -eq $profilerArtifactId })
				if($matchingProfilerArtifacts.Count -gt 1)
				{
					throw "Profiler artifact identity '$profilerArtifactId' was published more than once."
				}
				if($matchingProfilerArtifacts.Count -eq 1)
				{
					$profilerArtifact = $matchingProfilerArtifacts[0]
				}
				elseif([DateTime]::UtcNow -lt $profilerArtifactDeadline)
				{
					Start-Sleep -Milliseconds 100
				}
			}
			while($null -eq $profilerArtifact -and
				[DateTime]::UtcNow -lt $profilerArtifactDeadline)
			if($null -eq $profilerArtifact)
			{
				throw "Profiler artifact '$profilerArtifactId' did not become queryable after persistence."
			}
			# V1 summaries belong to the lazily opened index, not to catalog rows.
			# Load and validate that bounded index before requesting terminal metrics.
			$profilerLoad = Invoke-McpTool 'profiler.capture.load' ([ordered]@{
				artifactId = $profilerArtifactId
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
			$profilerSummary = Invoke-McpTool 'profiler.capture.summary' ([ordered]@{
				artifactId = $profilerArtifactId
			})
			$profilerFrameCount = [UInt64]$profilerSummary.data.summary.frameCount
			if($profilerFrameCount -eq 0)
			{
				throw 'Profiler capture persisted without any completed frame samples.'
			}
			# Terminal persists exact time-based first/final five-minute windows.
			# The compact index is still paged to verify count agreement, never to
			# approximate those windows from a fixed number of frames.
			$summaryWindowSource = $profilerSummary.data.summary
			$profilerTimingWindowEvidence = [pscustomobject]@{
				first = $summaryWindowSource.firstWindow
				final = $summaryWindowSource.finalWindow
			}
			if($tools -contains 'profiler.capture.index') {
				# Index loading is lazy: the first request may acknowledge with
				# state=queued. Poll the bounded page until total/frames are present,
				# rather than dereferencing the acknowledgement as a completed page.
				$indexPage = $null
				$indexDeadline = [DateTime]::UtcNow.AddSeconds(60)
				do {
					$indexPage = Invoke-McpTool 'profiler.capture.index' ([ordered]@{ offset = 0; limit = 512 })
					$indexData = Get-PropertyValue $indexPage 'data'
					$indexTotal = Get-PropertyValue $indexData 'total'
					$indexFrames = Get-PropertyValue $indexData 'frames'
					if($null -eq $indexTotal -or $null -eq $indexFrames) { Start-Sleep -Milliseconds 100 }
				} while(($null -eq $indexTotal -or $null -eq $indexFrames) -and [DateTime]::UtcNow -lt $indexDeadline)
				if($null -eq $indexTotal -or $null -eq $indexFrames) {
					throw 'Profiler V1 index remained queued or unavailable after the bounded 60-second wait.'
				}
				$totalIndexed = [int]$indexTotal
				if([UInt64]$totalIndexed -ne $profilerFrameCount) {
					throw 'Profiler V1 index count disagrees with its terminal summary.'
				}
				$profilerTimingWindowEvidence | Add-Member -NotePropertyName totalFrames -NotePropertyValue $totalIndexed
			}
			Start-Sleep -Milliseconds 250
			$profilerWatch.Stop()
			$profilerEvidence = [ordered]@{
				status = 'measured'
				kind = 'profiler-open-responsiveness'
				elapsedMilliseconds = $profilerWatch.Elapsed.TotalMilliseconds
				finishInteractiveBlockMilliseconds = $finishCaptureSubmissionMilliseconds
				finishMcpRoundTripMilliseconds = $finishMcpRoundTripMilliseconds
				start = $profilerStart
				stop = $profilerStop
				persistenceJob = $profilerJob
				artifact = $profilerArtifact
				summary = $profilerSummary
				frameCount = $profilerFrameCount
				load = $profilerLoad
				windowEvidence = $profilerTimingWindowEvidence
				loadJob = $profilerLoadJob
				window = $profilerWindowEvidence
				healthBefore = $profilerHealthBefore
				healthAfter = $profilerHealthAfter
			}
		}
		catch
		{
			if($sparseTransformOperationActive)
			{
				try
				{
					$sparseTransformEvidence = Invoke-McpTool 'renderer.validation.sparseTransform.stop' ([ordered]@{
						operationId = $sparseTransformOperationId
					})
				}
				catch { }
				$sparseTransformOperationActive = $false
			}
			if($profilerStarted -and -not $profilerStopRequested)
			{
				# Stop only a recording that never reached the persistence request.
				try { Invoke-McpTool 'profiler.capture.stop' | Out-Null } catch { }
			}
			$profilerEvidence = New-UnavailableEvidence 'profiler-open-responsiveness' $_.Exception.Message
		}
		finally
		{
			if($tools -contains 'profiler.window.visibility.set')
			{
				try
				{
					Invoke-McpTool 'profiler.window.visibility.set' ([ordered]@{ visible = $false }) | Out-Null
				}
				catch { }
			}
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
	$postCaptureStreaming = Get-PropertyValue (
		Get-PropertyValue $postCaptureStats 'data') 'streaming'
	if($null -eq $postCaptureStreaming)
	{
		throw 'Live Vulkan capture omitted the typed streaming diagnostics summary.'
	}
	$rejectedPublicationValue = Get-PropertyValue $postCaptureStreaming 'rejectedPublications'
	if($null -eq $rejectedPublicationValue)
	{
		throw 'Live Vulkan capture omitted the streaming rejectedPublications counter.'
	}
	$rejectedPublicationCount = [UInt64]$rejectedPublicationValue
	# Keep diagnostic provenance explicit: virtualGeometry.stats is a renderer
	# diagnostic readback, while the PNGs come from dedicated offscreen camera
	# targets.  They are sampled in the same post-present window but are not the
	# same output and must never be interpreted as one frame's counters.
	$diagnosticProvenance = [ordered]@{
		statsOperation = 'renderer.virtualGeometry.stats'
		statsViewType = $DiagnosticsViewType
		statsOutput = 'renderer-diagnostic-readback'
		captureOperation = 'camera.capture_custom'
		captureOutput = 'dedicated-offscreen-png-readback'
		postPresentFrameIds = @($postPresentCaptures | ForEach-Object { $_.frameId })
		statsCounterFrameId = Get-PropertyValue (Get-PropertyValue $postCaptureStats 'data') 'counterFrameId'
		statsDiagnosticsFrameId = Get-PropertyValue (Get-PropertyValue $postCaptureStats 'data') 'diagnosticsFrameId'
	}
	$firstHash = (Get-FileHash -LiteralPath $firstPath -Algorithm SHA256).Hash.ToLowerInvariant()
	$secondHash = (Get-FileHash -LiteralPath $secondPath -Algorithm SHA256).Hash.ToLowerInvariant()
	# Store coarse image-content metrics with every graded frame. A valid PNG and
	# matching static-frame hash alone cannot prove a rendered population: an
	# entirely black Vulkan readback satisfies both of those weaker checks.
	$warmupMetrics = Get-PngFrameMetrics $warmupPath
	$firstMetrics = Get-PngFrameMetrics $firstPath
	$secondMetrics = Get-PngFrameMetrics $secondPath
	$warmupVisualContentValid = Test-MeaningfulPngMetrics $warmupMetrics
	$firstVisualContentValid = Test-MeaningfulPngMetrics $firstMetrics
	$secondVisualContentValid = Test-MeaningfulPngMetrics $secondMetrics
	$captureEvidence = @(
		[ordered]@{
			path = $firstPath
			sha256 = $firstHash
			bytes = [UInt64](Get-Item -LiteralPath $firstPath).Length
			metrics = $firstMetrics
			envelope = $first
		},
		[ordered]@{
			path = $secondPath
			sha256 = $secondHash
			bytes = [UInt64](Get-Item -LiteralPath $secondPath).Length
			metrics = $secondMetrics
			envelope = $second
		}
	)
	foreach($retryCapture in $animatedRetryCaptures)
	{
		$retryMetrics = Get-PngFrameMetrics $retryCapture.path
		$captureEvidence += [ordered]@{
			path = $retryCapture.path
			sha256 = $retryCapture.sha256
			bytes = [UInt64](Get-Item -LiteralPath $retryCapture.path).Length
			metrics = $retryMetrics
			delayMilliseconds = $retryCapture.delayMilliseconds
			envelope = $retryCapture.envelope
		}
	}
	$allCaptureVisualContentValid = @($captureEvidence | Where-Object {
		-not (Test-MeaningfulPngMetrics $_.metrics)
	}).Count -eq 0
	$uniqueCaptureHashCount = @(
		$captureEvidence.sha256 | Sort-Object -Unique).Count
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
		sceneObjectCount = $sceneObjectCount
		validationScene = $validationScene
		startupSceneSelection = if($sceneSelectedByStartup) { [ordered]@{ path = $ScenePath; method = 'Library/LastScene.txt' } } else { $null }
		authoredScene = if($null -ne $authoredScenePath) { [ordered]@{ path = $authoredScenePath; sha256Before = $authoredSceneHashBefore } } else { $null }
		validationCamera = $validationCamera
		validationCameraConfiguration = [ordered]@{
			position = @($ValidationCameraPosition)
			target = @($ValidationCameraTarget)
			verticalFovDegrees = $ValidationCameraVerticalFovDegrees
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
		sceneViewVisualization = $sceneViewVisualizationEvidence
		instantiatedPrefab = if($instances.Count -gt 0) { $instances[0] } else { $null }
		instantiatedPrefabs = $instances
		instantiatedGeometry = [ordered]@{
			source = if($useRawMesh) { 'raw-mesh' } else { 'prefab' }
			meshAssetId = if($useRawMesh) { $meshAssetIdentity } else { $null }
			materialAssetId = if($useRawMesh) { $materialAssetIdentity } else { $null }
			# Preserve the complete authored identity set for workloads that cycle
			# deterministic materials. A scalar is insufficient to prove a ten-model
			# sample did not silently collapse to one material during publication.
			materialAssetIds = if($useRawMesh) { @($materialAssetIdentities) } else { @() }
			objects = $instances
			prefabVariants = @($prefabVariantIdentities)
			defaultClipAssetIds = @($prefabDefaultClipAssetIdentities)
			variantDefaultClipAssetIds = @($prefabVariantDefaultClipAssetIdentities |
				ForEach-Object { @($_) })
			defaultClipStartFrameStep = $PrefabDefaultClipStartFrameStep
			bulkTransactionalPublication = $usedBulkRawPublication
			objectNames = $instanceNames
			scale = if($useRawMesh -and $RawMeshInstanceScale -ne 1.0) {
				$RawMeshInstanceScale
			} else {
				$PrefabScale
			}
			scaleTargetObject = if($useRawMesh -and $RawMeshInstanceScale -ne 1.0 -and
			$PrefabInstanceCount -gt 1) {
				'all raw validation instances'
			} elseif($PrefabScale -ne 1.0) {
				if($useRawMesh) { $instanceNames[0] } else { $PrefabChildObject }
			} else {
				$null
			}
		}
		animationDiagnostics = $animationDiagnostics
		postCaptureAnimationDiagnostics = $postCaptureAnimationDiagnostics
		capabilityEvidence = $capabilities
		postCaptureCapabilityEvidence = $postCaptureCapabilities
		postPresentCaptures = $postPresentCaptures
		postPresentIdentity = [ordered]@{
			captureCount = $postPresentCaptures.Count
			uniqueCaptureHashCount = $postPresentHashes.Count
			changingCaptureHashes = $postPresentHashes.Count -gt 1
			healthyAnimatedPresentation = $healthyAnimatedPresentation
			ownerMappingCount = [UInt64](Get-PropertyValue $gpuPresentation 'ownerMappingCount')
			dispatchCommandCount = [UInt64](Get-PropertyValue $gpuPresentation 'dispatchCommandCount')
			safeFallbackPacketCount = $unsafePresentationPackets.Count
		}
		postPresentCapabilityEvidence = $postPresentCapabilityEvidence
		postWarmupCaptureStats = $postWarmupCaptureStats
		postWarmupCaptureAsset = $postWarmupCaptureAsset
		postFirstCaptureStats = $postFirstCaptureStats
		postFirstCaptureAsset = $postFirstCaptureAsset
		postCaptureStats = $postCaptureStats
		diagnosticProvenance = $diagnosticProvenance
		postCaptureAsset = $postCaptureAsset
		streamingPublicationEvidence = [ordered]@{
			rejectedPublications = $rejectedPublicationCount
			lastRejectionReason = [string](Get-PropertyValue $postCaptureStreaming 'lastRejectionReason')
		}
		visualContent = [ordered]@{
			minimumSampledNonBlackPixels = $minimumSampledNonBlackPixels
			minimumMeanLuminance = $minimumMeanLuminance
			warmupPassed = $warmupVisualContentValid
			firstPassed = $firstVisualContentValid
			secondPassed = $secondVisualContentValid
			allCapturesPassed = $allCaptureVisualContentValid
		}
		pickingEvidence = $pickingEvidence
		shadowEvidence = $shadowEvidence
		editorWindowEvidence = $editorWindowEvidence
		profilerEvidence = $profilerEvidence
		warmupEvidence = $warmupEvidence
		sparseTransformEvidence = $sparseTransformEvidence
		profilerIdleEvidence = $profilerIdleEvidence
		matchedOverheadEvidence = $matchedOverheadEvidence
		profilerHealthCheckpoints = @($profilerHealthCheckpoints)
		soakEvidence = $soakEvidence
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
			metrics = $warmupMetrics
			envelope = $warmup
		}
		captures = $captureEvidence
		uniqueCaptureHashCount = $uniqueCaptureHashCount
		unchangedByteIdentity = $uniqueCaptureHashCount -eq 1
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
	if($null -ne $authoredScenePath)
	{
		$authoredSceneHashAfter = (Get-FileHash -LiteralPath $authoredScenePath -Algorithm SHA256).Hash.ToLowerInvariant()
		$summary.authoredScene.sha256After = $authoredSceneHashAfter
		$summary.authoredScene.unchanged = $authoredSceneHashBefore -eq $authoredSceneHashAfter
		if(-not $summary.authoredScene.unchanged) { throw 'Authored Scene.tescene changed during live capture.' }
		[IO.File]::WriteAllText((Join-Path $outputRoot 'live-visual-summary.json'), ($summary | ConvertTo-Json -Depth 64), $utf8NoBom)
	}
	if($ExpectAnimatedChange -and $summary.unchangedByteIdentity)
	{
		throw 'Repeated animated live Vulkan captures were byte-identical.'
	}
	if($ExpectAnimatedChange -and (-not [bool]$summary.postPresentIdentity.changingCaptureHashes -or -not [bool]$summary.postPresentIdentity.healthyAnimatedPresentation))
	{
		throw 'Animated live capture lacked changing post-present hashes or healthy animation presentation evidence.'
	}
	if(-not $ExpectAnimatedChange -and -not $summary.unchangedByteIdentity)
	{
		throw 'Repeated static live Vulkan captures were not byte-identical.'
	}
	if(-not $warmupVisualContentValid -or -not $allCaptureVisualContentValid)
	{
		# The summary and every PNG remain on disk for diagnosis. Only the process
		# status changes, preventing standalone black captures from being
		# mistaken for visually valid renderer evidence.
		throw 'Live Vulkan captures were black or lacked meaningful sampled luminance.'
	}
	if($rejectedPublicationCount -ne [UInt64]0)
	{
		# A later accepted publication clears lastRejectionReason, so the monotonic
		# counter is the gate. This keeps a transient-looking failed batch visible
		# instead of allowing the final successful frame to mask it.
		throw "Live Vulkan capture observed $rejectedPublicationCount rejected virtual-geometry publication candidate(s)."
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
		if($sparseTransformOperationActive -and -not $editor.HasExited)
		{
			try
			{
				Invoke-McpTool 'renderer.validation.sparseTransform.stop' ([ordered]@{
					operationId = $sparseTransformOperationId
				}) | Out-Null
				$sparseTransformOperationActive = $false
			}
			catch { }
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
		if($ownsEditor -and -not $KeepEditorOpen -and -not $editor.HasExited)
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
		# Once the owned editor has exited, both redirected pipes are closed and
		# ReadToEndAsync must complete. Drain them here instead of sampling
		# IsCompleted: the task completion notification can lag process exit by a
		# scheduler turn, which previously omitted otherwise valid GPU provenance.
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
	# LastScene.txt is editor session state, not benchmark evidence. Restore the
	# exact prior bytes so authored startup/camera state cannot be persisted.
	if($lastSceneExisted)
	{
		[IO.File]::WriteAllText($lastScenePath, $lastSceneContent, $utf8NoBom)
	}
	elseif(Test-Path -LiteralPath $lastScenePath -PathType Leaf)
	{
		Remove-Item -LiteralPath $lastScenePath -Force
	}
}
