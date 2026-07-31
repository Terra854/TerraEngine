# Feature 045 installed Vulkan runtime adapter.
#
# The native benchmark launches this script through
# VirtualGeometryRuntimeAdapter.cmd. Each invocation starts the installed
# editor, captures two real Vulkan frames, reads delayed timestamp evidence,
# inspects residency state, and writes one bounded benchmark capture. A sample
# fails closed when the device route, timestamps, image identity, counters, or
# validation log cannot support a measured claim.
#
# Optional environment overrides let a validation machine select a different
# installed fixture without changing the benchmark command:
#   TERRA_VG_RUNTIME_EDITOR
#   TERRA_VG_RUNTIME_PROJECT
#   TERRA_VG_RUNTIME_ASSET
#   TERRA_VG_RUNTIME_PREFAB
#   TERRA_VG_RUNTIME_PREFAB_CHILD
#   TERRA_VG_RUNTIME_PREFAB_SCALE
#   TERRA_VG_RUNTIME_MCP_PORT_BASE
#   TERRA_VG_RUNTIME_STREAMING_SETTLE_MS

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$utf8NoBom = [Text.UTF8Encoding]::new($false)

function Parse-AdapterArguments([string[]]$Values)
{
	$result = [ordered]@{}
	for($index = 0; $index -lt $Values.Count; ++$index)
	{
		$name = [string]$Values[$index]
		if(-not $name.StartsWith('--', [StringComparison]::Ordinal))
		{
			throw "Unexpected runtime-adapter argument: $name"
		}
		if($index + 1 -ge $Values.Count)
		{
			throw "Missing value after runtime-adapter argument: $name"
		}
		$result[$name.Substring(2)] = [string]$Values[++$index]
	}
	return $result
}

function Require-AdapterArgument($Options, [string]$Name)
{
	if(-not $Options.Contains($Name) -or
		[string]::IsNullOrWhiteSpace([string]$Options[$Name]))
	{
		throw "Required runtime-adapter argument is missing: --$Name"
	}
	return [string]$Options[$Name]
}

function Resolve-InputPath([string]$Path, [string]$BasePath)
{
	if([IO.Path]::IsPathRooted($Path))
	{
		return [IO.Path]::GetFullPath($Path)
	}
	return [IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

function Get-EnvironmentOverride([string]$Name, [string]$Fallback)
{
	$value = [Environment]::GetEnvironmentVariable($Name, 'Process')
	if([string]::IsNullOrWhiteSpace($value))
	{
		return $Fallback
	}
	return $value
}

function Get-OptionalProperty($Value, [string]$Name)
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

function Get-RequiredProfileString($Value, [string]$Name)
{
	$result = [string](Get-OptionalProperty $Value $Name)
	if([string]::IsNullOrWhiteSpace($result))
	{
		throw "Animated-project profile omitted required string '$Name'."
	}
	return $result
}

function Get-RequiredProfilePositiveInteger($Value, [string]$Name)
{
	$result = [UInt64](Get-OptionalProperty $Value $Name)
	if($result -eq 0)
	{
		throw "Animated-project profile requires positive '$Name'."
	}
	return $result
}

function Get-SceneComponentCount($Objects, [string]$ComponentName)
{
	# The project scene is an authored hierarchy. Count every nested object so
	# the runner cannot call a four-animator scene a crowd merely by its name.
	$count = [UInt64]0
	foreach($object in @($Objects))
	{
		if($null -eq $object)
		{
			continue
		}
		$components = Get-OptionalProperty $object 'components'
		if($null -ne $components -and $null -ne $components.PSObject.Properties[$ComponentName])
		{
			++$count
		}
		$count += Get-SceneComponentCount (Get-OptionalProperty $object 'children') $ComponentName
	}
	return $count
}

function Get-Sha256Text([string]$Value)
{
	$bytes = [Text.Encoding]::UTF8.GetBytes($Value)
	$sha = [Security.Cryptography.SHA256]::Create()
	try
	{
		return ([BitConverter]::ToString($sha.ComputeHash($bytes))).
			Replace('-', '').ToLowerInvariant()
	}
	finally
	{
		$sha.Dispose()
	}
}

function Get-FileSha256OrUnavailable([string]$Path)
{
	if(-not (Test-Path -LiteralPath $Path -PathType Leaf))
	{
		return 'unavailable'
	}
	return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.
		ToLowerInvariant()
}

function Get-DirectoryInventoryHash([string]$Path)
{
	if(-not (Test-Path -LiteralPath $Path -PathType Container))
	{
		return 'unavailable'
	}
	$inventory = @(
		Get-ChildItem -LiteralPath $Path -File -Recurse |
			Sort-Object FullName |
			ForEach-Object {
				$relative = $_.FullName.Substring($Path.TrimEnd(
					[IO.Path]::DirectorySeparatorChar).Length).TrimStart(
						[IO.Path]::DirectorySeparatorChar)
				"$relative|$($_.Length)|$((Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash)"
			}
	)
	return Get-Sha256Text ($inventory -join "`n")
}

function Read-ActiveVulkanIdentity([string]$StdoutPath)
{
	$text = Get-Content -Raw -LiteralPath $StdoutPath
	$adapterMatch = [Regex]::Match(
		$text,
		'Selected GPU:\s*([^\r\n\x1b]+)',
		[Text.RegularExpressions.RegexOptions]::IgnoreCase)
	$driverMatch = [Regex]::Match(
		$text,
		'Driver Version:\s*([^\r\n\x1b]+)',
		[Text.RegularExpressions.RegexOptions]::IgnoreCase)
	if(-not $adapterMatch.Success -or -not $driverMatch.Success)
	{
		throw 'Installed editor log did not identify the active Vulkan GPU and driver.'
	}
	return [ordered]@{
		adapter = $adapterMatch.Groups[1].Value.Trim()
		driver = $driverMatch.Groups[1].Value.Trim()
	}
}

function Get-NonNegativeCounter($Value, [string]$Name)
{
	$counter = Get-OptionalProperty $Value $Name
	if($null -eq $counter)
	{
		throw "Runtime evidence omitted required counter '$Name'."
	}
	$number = [Int64]$counter
	if($number -lt 0)
	{
		throw "Runtime evidence published negative counter '$Name'."
	}
	return $number
}

$options = Parse-AdapterArguments $args
$outputArgument = Require-AdapterArgument $options 'output'
$workload = Require-AdapterArgument $options 'workload'
$backend = (Require-AdapterArgument $options 'backend').ToLowerInvariant()
$sampleIndex = [UInt64](Require-AdapterArgument $options 'sample-index')
if($backend -ne 'vulkan')
{
	throw "Feature 045 runtime adapter supports Vulkan only, not '$backend'."
}

$scriptRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$twoLevelsUp = [IO.Path]::GetFullPath((Join-Path $scriptRoot '../..'))
$sourceTree = Test-Path -LiteralPath (
	Join-Path $twoLevelsUp 'CMakeLists.txt') -PathType Leaf
$repositoryRoot = if($sourceTree)
{
	$twoLevelsUp
}
else
{
		# In the install tree this is the Build directory containing Editor,
		# Tests, Tools, and Docs.
	$twoLevelsUp
}
$installedRoot = if($sourceTree)
{
	[IO.Path]::GetFullPath((Join-Path $repositoryRoot (
		'out/build/RelWithDebInfo/Build')))
}
else
{
	$repositoryRoot
}
$workingRoot = if($sourceTree)
{
	$repositoryRoot
}
else
{
	$installedRoot
}

$isAnimatedWorkload = $workload -in @('animated-hero', 'animated-crowd')
$animatedProjectProfile = $null
$animatedWorkloadProfile = $null
$projectAnimatorCount = [UInt64]0
$expectedAnimatorCount = [UInt64]0
$minimumUniqueDeformationDispatches = [UInt64]0
$runtimePrefabInstanceCount = [UInt64]1
$runtimePrefabSpacing = 0.0
$animatedConfigurationReason = ''
if($isAnimatedWorkload)
{
	$profilePath = Resolve-InputPath (
		Get-EnvironmentOverride 'TERRA_VG_RUNTIME_ANIMATED_PROJECT_PROFILE' (
			Join-Path $scriptRoot 'animated-project-runtime.json')) $workingRoot
	if(-not (Test-Path -LiteralPath $profilePath -PathType Leaf))
	{
		throw "Animated workload profile is missing: $profilePath"
	}
	$animatedProjectProfile = Get-Content -Raw -LiteralPath $profilePath | ConvertFrom-Json
	if([UInt64](Get-OptionalProperty $animatedProjectProfile 'schemaVersion') -ne 1)
	{
		throw 'Animated workload profile must use schemaVersion 1.'
	}
	$profileMember = if($workload -eq 'animated-hero') { 'hero' } else { 'crowd' }
	$animatedWorkloadProfile = Get-OptionalProperty $animatedProjectProfile $profileMember
	if($null -eq $animatedWorkloadProfile)
	{
		throw "Animated workload profile omitted '$profileMember'."
	}
	$expectedAnimatorCount = Get-RequiredProfilePositiveInteger `
		$animatedWorkloadProfile 'minimumAnimatorInstances'
	$minimumUniqueDeformationDispatches = Get-RequiredProfilePositiveInteger `
		$animatedWorkloadProfile 'minimumUniqueDeformationDispatches'
	$runtimePrefabSpacing = [double](Get-OptionalProperty `
		$animatedWorkloadProfile 'prefabSpacing')
	$runtimeCameraPosition = @(
		Get-OptionalProperty $animatedWorkloadProfile 'cameraPosition')
	if($runtimeCameraPosition.Count -ne 3)
	{
		throw "Animated workload '$profileMember' requires exactly three cameraPosition values."
	}
	foreach($cameraCoordinate in $runtimeCameraPosition)
	{
		$cameraValue = [double]$cameraCoordinate
		# Windows PowerShell uses .NET Framework, which predates
		# Double.IsFinite. The two legacy predicates express the same check.
		if([double]::IsNaN($cameraValue) -or
			[double]::IsInfinity($cameraValue))
		{
			throw "Animated workload '$profileMember' contains a non-finite cameraPosition value."
		}
	}
}

$outputPath = Resolve-InputPath $outputArgument (Get-Location).Path
$artifactRoot = "$outputPath.artifacts"
[IO.Directory]::CreateDirectory($artifactRoot) | Out-Null

$editorDefault = Join-Path $installedRoot 'Editor/TerraEngine-Editor.exe'
$projectDefault = Join-Path $installedRoot 'Tests/Fixtures/Animation/US1Import'
if($isAnimatedWorkload)
{
	# Source-tree runs discover the user-provided project beside the repository.
	# Installed trees cannot assume that layout, so callers retain the explicit
	# TERRA_VG_RUNTIME_PROJECT override documented at the top of this script.
	$projectDefault = Join-Path (Split-Path -Parent $repositoryRoot) (
		Get-RequiredProfileString $animatedProjectProfile `
			'projectDirectoryRelativeToRepositoryParent')
}
$editorPath = Resolve-InputPath (
	Get-EnvironmentOverride 'TERRA_VG_RUNTIME_EDITOR' $editorDefault) $workingRoot
$projectPath = Resolve-InputPath (
	Get-EnvironmentOverride 'TERRA_VG_RUNTIME_PROJECT' $projectDefault) $workingRoot
$assetDefault = 'Assets/Models/US1Hero.gltf'
$prefabDefault = 'Assets/Models/US1Hero_Generated/US1Hero.teprefab'
$prefabChildDefault = ''
$prefabScaleDefault = '1'
if($isAnimatedWorkload)
{
	$assetDefault = Get-RequiredProfileString $animatedWorkloadProfile 'asset'
	$prefabDefault = Get-RequiredProfileString $animatedWorkloadProfile 'prefab'
	$prefabChildDefault = Get-RequiredProfileString $animatedWorkloadProfile 'prefabChildObject'
	$prefabScaleDefault = [string](Get-OptionalProperty $animatedWorkloadProfile 'prefabScale')
}
$asset = Get-EnvironmentOverride 'TERRA_VG_RUNTIME_ASSET' $assetDefault
$prefab = Get-EnvironmentOverride 'TERRA_VG_RUNTIME_PREFAB' $prefabDefault
$prefabChild = Get-EnvironmentOverride 'TERRA_VG_RUNTIME_PREFAB_CHILD' $prefabChildDefault
$prefabScale = [double](Get-EnvironmentOverride 'TERRA_VG_RUNTIME_PREFAB_SCALE' $prefabScaleDefault)
$portBase = [int](Get-EnvironmentOverride 'TERRA_VG_RUNTIME_MCP_PORT_BASE' '43175')
$streamingSettleMilliseconds = [int](Get-EnvironmentOverride `
	'TERRA_VG_RUNTIME_STREAMING_SETTLE_MS' '250')
$mcpPort = $portBase + [int](($sampleIndex - 1) % 1000)

foreach($requiredFile in @(
	$editorPath,
	(Join-Path $scriptRoot 'CaptureVirtualGeometryLiveVisual.ps1'),
	(Join-Path $scriptRoot 'runtime-adapter-preferences.json')))
{
	if(-not (Test-Path -LiteralPath $requiredFile -PathType Leaf))
	{
		throw "Runtime adapter input is missing: $requiredFile"
	}
}
if(-not (Test-Path -LiteralPath $projectPath -PathType Container))
{
	throw "Runtime adapter project is missing: $projectPath"
}
if($isAnimatedWorkload)
{
	$startupScene = Join-Path $projectPath (
		Get-RequiredProfileString $animatedProjectProfile 'startupScene')
	if(-not (Test-Path -LiteralPath $startupScene -PathType Leaf))
	{
		throw "Animated-project startup scene is missing: $startupScene"
	}
	foreach($requiredAsset in @($asset, $prefab))
	{
		if(-not (Test-Path -LiteralPath (Join-Path $projectPath $requiredAsset) -PathType Leaf))
		{
			throw "Animated workload asset is missing: $requiredAsset"
		}
	}
	$scene = Get-Content -Raw -LiteralPath $startupScene | ConvertFrom-Json
	$projectAnimatorCount = Get-SceneComponentCount (Get-OptionalProperty $scene 'objects') 'AnimatorComponent'
	$minimumProjectAnimatorCount = Get-RequiredProfilePositiveInteger `
		$animatedProjectProfile 'minimumProjectAnimatorCount'
	if($projectAnimatorCount -lt $minimumProjectAnimatorCount)
	{
		throw "Animated-project scene exposes $projectAnimatorCount animators; profile requires at least $minimumProjectAnimatorCount."
	}
	# Add a real transient population in the open authored scene. Each prefab is
	# assigned its own stable animation-instance identity by the engine, so a
	# crowd capture measures distinct deformation work without modifying or
	# saving the user's project scene.
	$runtimePrefabInstanceCount = $expectedAnimatorCount
	$capturedAnimatorCount = $runtimePrefabInstanceCount
	if($capturedAnimatorCount -lt $expectedAnimatorCount)
	{
		$animatedConfigurationReason = "Animated workload requires $expectedAnimatorCount animator instances, but the prepared live scene exposes only $capturedAnimatorCount."
	}
}

$liveArguments = @{
	InstalledEditor = $editorPath
	Project = $projectPath
	Asset = $asset
	Preferences = (Join-Path $scriptRoot 'runtime-adapter-preferences.json')
	OutputDirectory = $artifactRoot
	Prefab = $prefab
	PrefabName = "Feature 045 runtime sample $sampleIndex"
	PrefabChildObject = $prefabChild
	PrefabScale = $prefabScale
	PrefabInstanceCount = [int]$runtimePrefabInstanceCount
	PrefabSpacing = $runtimePrefabSpacing
	StreamingSettleMilliseconds = $streamingSettleMilliseconds
	McpPort = $mcpPort
	StartupTimeoutSeconds = 90
}
if($isAnimatedWorkload)
{
	# scene.open is deliberately approval-gated in the editor. Automated
	# benchmarks instead use an exact disposable scene and delete it after the
	# editor exits, while the authored startup scene remains the audited source
	# of the project's animator inventory above.
	$liveArguments['CreateValidationScene'] = $true
	$liveArguments['CleanupValidationScene'] = $true
	$liveArguments['ValidationScenePath'] =
		"Assets/Scenes/.terra-validation/Feature045-$workload-$sampleIndex-$PID.tescene"
	$liveArguments['ValidationCameraPosition'] = @(
		[double]$runtimeCameraPosition[0],
		[double]$runtimeCameraPosition[1],
		[double]$runtimeCameraPosition[2])
	# Animation authority and per-instance deformation advance only in the
	# runtime scene. Require visibly different frames so a static editor pose
	# cannot be promoted into measured animated hero or crowd evidence.
	$liveArguments['EnterPlayMode'] = $true
	$liveArguments['ExpectAnimatedChange'] = $true
	# Headless editor captures render through the active Scene viewport before
	# the dedicated custom camera target. Query that live selection instead of
	# the idle Game viewport, whose truthful counters remain zero.
	$liveArguments['DiagnosticsViewType'] = 'Scene'
}
& (Join-Path $scriptRoot 'CaptureVirtualGeometryLiveVisual.ps1') @liveArguments

$summaryPath = Join-Path $artifactRoot 'live-visual-summary.json'
if(-not (Test-Path -LiteralPath $summaryPath -PathType Leaf))
{
	throw 'Installed live capture did not publish its summary.'
}
$summary = Get-Content -Raw -LiteralPath $summaryPath | ConvertFrom-Json
$vulkanIdentity = Read-ActiveVulkanIdentity (
	Join-Path $artifactRoot 'installed-editor.stdout.log')

$capabilityEnvelope = Get-OptionalProperty $summary 'postCaptureCapabilityEvidence'
$capabilityData = Get-OptionalProperty $capabilityEnvelope 'data'
$measurements = @(
	(Get-OptionalProperty $capabilityData 'measurements') |
		Where-Object {
			[string](Get-OptionalProperty $_ 'availability') -eq 'measured' -and
			-not [string]::IsNullOrWhiteSpace(
				[string](Get-OptionalProperty $_ 'executionEvidenceId')) -and
			[double](Get-OptionalProperty $_ 'durationNanoseconds') -gt 0.0
		})
$validatedStages = @(
	(Get-OptionalProperty $capabilityData 'capabilities') |
		Where-Object {
			[bool](Get-OptionalProperty $_ 'executed') -and
			[bool](Get-OptionalProperty $_ 'validated')
		} |
		ForEach-Object { [string](Get-OptionalProperty $_ 'stage') })
$requiredStages = @('traversal', 'compaction', 'indirect-generation')
$missingStages = @($requiredStages | Where-Object {
	$validatedStages -notcontains $_
})

$statsEnvelope = Get-OptionalProperty $summary 'postCaptureStats'
$statsData = Get-OptionalProperty $statsEnvelope 'data'
$streaming = Get-OptionalProperty $statsData 'streaming'
if($null -eq $streaming)
{
	throw 'Runtime evidence omitted the typed streaming diagnostics summary.'
}
$streamingCounters = [ordered]@{
	submittedRequests = Get-NonNegativeCounter $streaming 'submittedRequests'
	completedReads = Get-NonNegativeCounter $streaming 'completedReads'
	acceptedPublicationPages = Get-NonNegativeCounter $streaming 'acceptedPublicationPages'
	rejectedPublications = Get-NonNegativeCounter $streaming 'rejectedPublications'
	pendingDecodePages = Get-NonNegativeCounter $streaming 'pendingDecodePages'
	pendingUploadPages = Get-NonNegativeCounter $streaming 'pendingUploadPages'
	physicalResidencyRows = Get-NonNegativeCounter $streaming 'physicalResidencyRows'
	visibleResidentPages = Get-NonNegativeCounter $streaming 'visibleResidentPages'
}

$stderrText = Get-Content -Raw -LiteralPath (
	Join-Path $artifactRoot 'installed-editor.stderr.log')
$stdoutText = Get-Content -Raw -LiteralPath (
	Join-Path $artifactRoot 'installed-editor.stdout.log')
$validationPattern = '(?i)(VUID-|VK_ERROR|validation error|device lost)'
$validationErrors = @(
	[Regex]::Matches($stderrText + "`n" + $stdoutText, $validationPattern) |
		ForEach-Object { $_.Value } |
		Select-Object -Unique)
$captureRows = @((Get-OptionalProperty $summary 'captures'))
$captureFilesValid = $captureRows.Count -eq 2 -and @(
	$captureRows | Where-Object {
		-not (Test-Path -LiteralPath (
			[string](Get-OptionalProperty $_ 'path')) -PathType Leaf)
	}).Count -eq 0
$streamingActivityValid = $true
if($workload -eq 'page-residency-pressure')
{
	# The pressure workload must prove the real request/read/publication path.
	# Zero is a valid bounded value for other workloads, but it cannot establish
	# that a streaming soak exercised streaming.
	$streamingActivityValid =
		$streamingCounters.submittedRequests -gt 0 -and
		$streamingCounters.completedReads -gt 0 -and
		$streamingCounters.acceptedPublicationPages -gt 0 -and
		$streamingCounters.physicalResidencyRows -gt 0 -and
		$streamingCounters.visibleResidentPages -gt 0 -and
		$streamingCounters.pendingDecodePages -eq 0 -and
		$streamingCounters.pendingUploadPages -eq 0
}
$animationActivityValid = $true
$observedUniqueDeformationDispatches = [UInt64]0
$observedAnimationOwnerMappings = [UInt64]0
$observedAnimationWorkPackets = [UInt64]0
$observedAnimatedRenderables = [UInt64]0
$ownerExecutionEvidenceValid = $true
$deformationDiagnosticsValid = $true
if($workload -in @('animated-hero', 'animated-crowd'))
{
	$postCaptureAssetEnvelope = Get-OptionalProperty $summary 'postCaptureAsset'
	$postCaptureAssetData = Get-OptionalProperty $postCaptureAssetEnvelope 'data'
	$runtimeDiagnostics = Get-OptionalProperty (
		$postCaptureAssetData) 'runtimeDiagnostics'
	$runtimeViews = @(
		Get-OptionalProperty $runtimeDiagnostics 'views')
	$animatedRows = @($runtimeViews | ForEach-Object {
		Get-OptionalProperty $_ 'animatedVirtualGeometry'
	})
	# Project numeric values before summing. Measure-Object -Property treats a
	# script block as a literal property name and silently reported zero.
	$observedUniqueDeformationDispatches = [UInt64](($animatedRows |
		ForEach-Object {
			[UInt64](Get-OptionalProperty $_ 'uniqueDispatches')
		} |
		Measure-Object -Sum).Sum)
	$animationDiagnosticsEnvelope = Get-OptionalProperty $summary 'animationDiagnostics'
	$animationDiagnosticsData = Get-OptionalProperty $animationDiagnosticsEnvelope 'data'
	$gpuPresentation = Get-OptionalProperty $animationDiagnosticsData 'gpuPresentation'
	$renderableBindings = Get-OptionalProperty $animationDiagnosticsData 'renderableBindings'
	$observedAnimationOwnerMappings =
		[UInt64](Get-OptionalProperty $gpuPresentation 'ownerMappingCount')
	$observedAnimationWorkPackets =
		[UInt64](Get-OptionalProperty $gpuPresentation 'workPacketCount')
	$observedAnimatedRenderables =
		[UInt64](Get-OptionalProperty $renderableBindings 'totalAnimatedRenderables')
	# Requested clone counts are not execution evidence. Bind acceptance to the
	# immutable animation presentation's owner/work counts and the renderer's
	# independently published deformation dispatch count.
	$ownerExecutionEvidenceValid =
		$observedAnimationOwnerMappings -ge $expectedAnimatorCount -and
		$observedAnimationWorkPackets -ge $expectedAnimatorCount -and
		$observedAnimatedRenderables -ge $expectedAnimatorCount
	# Deformation is renderer-neutral compute work and may not own a distinct
	# Vulkan timestamp query. Validate it from the immutable per-view plan:
	# accepted shader contract, dynamic bounds, no fallback, and actual work.
	$invalidDeformationRows = @($animatedRows | Where-Object {
		-not [bool](Get-OptionalProperty $_ 'shaderContractValid') -or
		[UInt64](Get-OptionalProperty $_ 'fallbackCount') -ne 0 -or
		-not ([string](Get-OptionalProperty $_ 'boundsSource')).StartsWith(
			'Planned', [StringComparison]::Ordinal)
	})
	$deformationDiagnosticsValid =
		$animatedRows.Count -gt 0 -and
		$invalidDeformationRows.Count -eq 0 -and
		$observedUniqueDeformationDispatches -ge
			$minimumUniqueDeformationDispatches
	if($deformationDiagnosticsValid -and
		$validatedStages -notcontains 'deformation')
	{
		$validatedStages += 'deformation'
	}
	$animationActivityValid = [string]::IsNullOrWhiteSpace($animatedConfigurationReason) -and
		$ownerExecutionEvidenceValid -and
		$validatedStages -contains 'deformation' -and
		$deformationDiagnosticsValid
}
$frameIdentityValid = if($isAnimatedWorkload)
{
	-not [bool](Get-OptionalProperty $summary 'unchangedByteIdentity')
}
else
{
	[bool](Get-OptionalProperty $summary 'unchangedByteIdentity')
}
$boundedCounters = $frameIdentityValid -and
	[string](Get-OptionalProperty $summary 'route') -eq
		'vulkan-device-authored-indirect' -and
	$measurements.Count -gt 0 -and
	$missingStages.Count -eq 0 -and
	$validationErrors.Count -eq 0 -and
	$captureFilesValid -and
	$streamingActivityValid -and
	$animationActivityValid -and
	[bool](Get-OptionalProperty $statsEnvelope 'ok')

$captureTime = [DateTime]::UtcNow.ToString('o')
$executionSeed = "$workload|$sampleIndex|$captureTime|$($vulkanIdentity.adapter)|$($vulkanIdentity.driver)"
$captureId = "vg-runtime-" + (Get-Sha256Text $executionSeed).Substring(0, 24)
$rawSamples = @(
	$measurements | ForEach-Object {
		[ordered]@{
			availability = 'measured'
			executionId = $captureId
			executionEvidenceId = [string](
				Get-OptionalProperty $_ 'executionEvidenceId')
			durationNanoseconds = [double](
				Get-OptionalProperty $_ 'durationNanoseconds')
			rawTicks = Get-OptionalProperty $_ 'rawTicks'
			queryIdentity = [string](Get-OptionalProperty $_ 'queryIdentity')
			stage = [string](Get-OptionalProperty $_ 'stage')
			frameId = [UInt64](Get-OptionalProperty $_ 'frameId')
			viewId = [UInt64](Get-OptionalProperty $_ 'viewId')
			rendererGeneration = [UInt64](
				Get-OptionalProperty $_ 'rendererGeneration')
			sampleOrdinal = $sampleIndex
		}
	})
$durations = @($rawSamples | ForEach-Object {
	[double]$_.durationNanoseconds
})
$median = if($durations.Count -gt 0)
{
	$sorted = @($durations | Sort-Object)
	$sorted[[Math]::Floor(($sorted.Count - 1) / 2)]
}
else
{
	$null
}

$prefabFile = Join-Path $projectPath $prefab
$shaderDirectory = Join-Path $installedRoot 'Editor/Shaders/VirtualGeometry'
$capture = [ordered]@{
	identity = [ordered]@{
		schemaVersion = '1'
		workloadVersion = '1'
		captureId = $captureId
		runId = $captureId
		executionId = $captureId
		captureTime = $captureTime
		engineHash = Get-FileSha256OrUnavailable $editorPath
		sourceHash = Get-FileSha256OrUnavailable $editorPath
		# Installed validation has no source diff file. Hash the explicit
		# installed-tree identity plus executable hash so downstream validators
		# receive a real immutable digest instead of a textual placeholder.
		diffHash = Get-Sha256Text (
			"installed-tree|" + (Get-FileSha256OrUnavailable $editorPath))
		executableHash = Get-FileSha256OrUnavailable $editorPath
		shaderHash = Get-DirectoryInventoryHash $shaderDirectory
		assetHash = Get-FileSha256OrUnavailable $prefabFile
		buildRecipeHash = Get-Sha256Text (
			"$workload|$asset|$prefab|$prefabScale")
	}
	environment = [ordered]@{
		backend = 'vulkan'
		adapter = $vulkanIdentity.adapter
		driver = $vulkanIdentity.driver
		route = [string](Get-OptionalProperty $summary 'route')
		resolution = '480x270'
		quality = 'feature-045-installed-validation'
	}
	workload = [ordered]@{
		name = $workload
		sceneHash = Get-Sha256Text (
			"$projectPath|$prefab|$asset")
		workloadHash = Get-Sha256Text $workload
	}
	warmupAndSampling = [ordered]@{
		warmupSamples = 1
		requestedSamples = $rawSamples.Count
		randomizedOrder = $false
	}
	capabilities = @((Get-OptionalProperty $capabilityData 'capabilities'))
	rawSamples = $rawSamples
	statistics = [ordered]@{
		medianNanoseconds = $median
		sampleCount = $rawSamples.Count
		rejectedCount = if($boundedCounters) { 0 } else { 1 }
		boundedCounters = $boundedCounters
		rejectionReasons = @(
			if($missingStages.Count -gt 0)
			{
				"Missing validated stages: $($missingStages -join ', ')"
			}
			if($validationErrors.Count -gt 0)
			{
				'Vulkan validation or device-loss diagnostics were present.'
			}
			if(-not $captureFilesValid)
			{
				'Live PNG capture artifacts were incomplete.'
			}
			if(-not $streamingActivityValid)
			{
				'Streaming pressure produced no complete request/read/publication activity.'
			}
			if(-not $animationActivityValid)
			{
				if(-not [string]::IsNullOrWhiteSpace($animatedConfigurationReason))
				{
					$animatedConfigurationReason
				}
				elseif(-not $ownerExecutionEvidenceValid)
				{
					"Animated workload requires $expectedAnimatorCount published owner mappings, work packets, and renderables; observed $observedAnimationOwnerMappings/$observedAnimationWorkPackets/$observedAnimatedRenderables."
				}
				else
				{
					"Animated workload requires $minimumUniqueDeformationDispatches validated unique deformation dispatches."
				}
			}
		)
	}
	geometry = [ordered]@{
		route = [string](Get-OptionalProperty $summary 'route')
		validatedStages = $validatedStages
	}
	residencyAndStreaming = [ordered]@{
		boundedCounters = $boundedCounters
		counters = $streamingCounters
		lastRejectionReason = [string](
			Get-OptionalProperty $streaming 'lastRejectionReason')
	}
	visibilityAndShading = [ordered]@{
		unchangedByteIdentity = [bool](
			Get-OptionalProperty $summary 'unchangedByteIdentity')
		expectedFrameIdentity = if($isAnimatedWorkload) {
			'animated-change'
		} else {
			'static-repeat'
		}
		captures = $captureRows
	}
	deformation = [ordered]@{
		projectAnimatorCount = $projectAnimatorCount
		runtimePrefabInstanceCount = $runtimePrefabInstanceCount
		capturedAnimatorCount = $runtimePrefabInstanceCount
		requiredAnimatorInstances = $expectedAnimatorCount
		minimumUniqueDeformationDispatches = $minimumUniqueDeformationDispatches
		observedUniqueDeformationDispatches =
			$observedUniqueDeformationDispatches
		observedAnimationOwnerMappings = $observedAnimationOwnerMappings
		observedAnimationWorkPackets = $observedAnimationWorkPackets
		observedAnimatedRenderables = $observedAnimatedRenderables
		diagnosticsValidated = $deformationDiagnosticsValid
		validationSource = 'immutable-renderer-deformation-plan'
		configurationReason = $animatedConfigurationReason
	}
	auxiliaryViews = [ordered]@{}
	fallbacksAndFailures = [ordered]@{
		validationErrors = $validationErrors
	}
	baselineAndComparability = [ordered]@{
		status = if($boundedCounters) { 'measured' } else { 'rejected' }
		source = 'installed-vulkan-live-capture'
	}
	artifacts = [ordered]@{
		liveSummary = $summaryPath
		artifactDirectory = $artifactRoot
	}
	integrity = [ordered]@{
		hash = Get-Sha256Text (
			(($captureRows | ForEach-Object {
				[string](Get-OptionalProperty $_ 'sha256')
			}) -join '|') + '|' + (($rawSamples | ForEach-Object {
				[string]$_.executionEvidenceId
			}) -join '|'))
		diagnostics = [ordered]@{
			validationErrorCount = $validationErrors.Count
		}
	}
}

$outputParent = Split-Path -Parent $outputPath
if(-not [string]::IsNullOrWhiteSpace($outputParent))
{
	[IO.Directory]::CreateDirectory($outputParent) | Out-Null
}
[IO.File]::WriteAllText(
	$outputPath,
	($capture | ConvertTo-Json -Depth 64),
	$utf8NoBom)
if(-not $boundedCounters)
{
	throw "Runtime sample $sampleIndex failed the bounded Vulkan evidence gate."
}
Write-Output "published measured Vulkan runtime sample $sampleIndex to $outputPath"
