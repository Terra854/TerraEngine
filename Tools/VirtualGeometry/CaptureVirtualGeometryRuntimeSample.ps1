# Production virtual-geometry installed Vulkan runtime adapter.
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
#   TERRA_VG_RUNTIME_MESH_ID
#   TERRA_VG_RUNTIME_MATERIAL_ID
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
	throw "The production virtual-geometry runtime adapter supports Vulkan only, not '$backend'."
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
$isAuthoredSceneWorkload = $workload -in @('existing-4000-animated', 'dense-animated-vg', 'mixed-animation')
$isRawStaticWorkload = $workload -in @('repeated-static', 'ten-renderable')
if($workload -eq 'heterogeneous-static')
{
	throw 'heterogeneous-static requires 4000 distinct material identities; no disposable material-corpus adapter is available.'
}
$animatedProjectProfile = $null
$animatedWorkloadProfile = $null
$rawStaticWorkloadProfile = $null
$rawStaticScenarioProfile = $null
$projectAnimatorCount = [UInt64]0
$expectedAnimatorCount = [UInt64]0
$capturedAnimatorCount = [UInt64]0
$minimumUniqueDeformationDispatches = [UInt64]0
$runtimePrefabInstanceCount = [UInt64]1
$runtimePrefabSpacing = 0.0
$animatedConfigurationReason = ''
if($isAnimatedWorkload -or $isAuthoredSceneWorkload -or $isRawStaticWorkload)
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
	if($isAnimatedWorkload -or $isAuthoredSceneWorkload)
	{
		# Authored-scene workloads need their own scene path and thresholds. Only the
		# disposable animated-crowd fixture reuses the generic crowd profile.
		$profileMember = if($workload -eq 'animated-hero') { 'hero' } elseif($workload -eq 'animated-crowd') { 'crowd' } else { $workload }
		$animatedWorkloadProfile = Get-OptionalProperty $animatedProjectProfile $profileMember
		if($null -eq $animatedWorkloadProfile -and -not $isAuthoredSceneWorkload)
		{
			throw "Animated workload profile omitted '$profileMember'."
		}
		if($isAuthoredSceneWorkload) { $expectedAnimatorCount = [UInt64](Get-OptionalProperty $animatedWorkloadProfile 'minimumAnimatorInstances') }
		else { $expectedAnimatorCount = Get-RequiredProfilePositiveInteger `
			$animatedWorkloadProfile 'minimumAnimatorInstances'
		}
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
	if($isRawStaticWorkload)
	{
		# Keep the raw direct fixture versioned beside the animated-project
		# profiles so script defaults, workload metadata, and review notes cannot
		# quietly drift to different mesh or material identities.
		$rawStaticWorkloadProfile = Get-OptionalProperty $animatedProjectProfile 'raw-static'
		if($null -eq $rawStaticWorkloadProfile)
		{
			throw "Animated workload profile omitted 'raw-static'."
		}
		foreach($requiredRawStaticField in @('asset', 'meshAssetId', 'materialAssetId'))
		{
			[void](Get-RequiredProfileString $rawStaticWorkloadProfile $requiredRawStaticField)
		}
		$rawStaticMaterialIds = @(
			Get-OptionalProperty $rawStaticWorkloadProfile 'materialAssetIds' |
				ForEach-Object { [string]$_ })
		if($rawStaticMaterialIds.Count -ne 3)
		{
			throw "Animated workload profile 'raw-static' requires exactly three materialAssetIds."
		}
		if([double](Get-OptionalProperty $rawStaticWorkloadProfile 'rawInstanceScale') -le 0.0)
		{
			throw "Animated workload profile 'raw-static' requires a positive rawInstanceScale."
		}
		$rawStaticScenarioProfile = $rawStaticWorkloadProfile
		if($workload -eq 'repeated-static')
		{
			$rawStaticScenarioProfile = Get-OptionalProperty `
				$animatedProjectProfile 'repeated-static'
			if($null -eq $rawStaticScenarioProfile)
			{
				throw "Animated workload profile omitted 'repeated-static'."
			}
			$rawStaticGridSpacing = [double](Get-OptionalProperty `
				$rawStaticScenarioProfile 'gridSpacing')
			if($rawStaticGridSpacing -le 0.0)
			{
				throw "Animated workload profile 'repeated-static' requires a positive gridSpacing."
			}
		}
		foreach($cameraProfileField in @('cameraPosition', 'cameraTarget'))
		{
			$cameraProfileValue = @(Get-OptionalProperty `
				$rawStaticScenarioProfile $cameraProfileField)
			if($cameraProfileValue.Count -ne 3)
			{
				throw "Raw-static profile requires exactly three $cameraProfileField values."
			}
		}
	}
}
if($isAuthoredSceneWorkload)
{
	# Authored-scene aliases measure the scene's existing population exactly.
	# A zero transient count prevents the live harness from adding a validation
	# prefab on top of the workload named by the manifest.
	$runtimePrefabInstanceCount = [UInt64]0
}
if($isRawStaticWorkload)
{
	# The raw path creates transient MeshRenderComponents only; no Animator
	# component or authored Scene.tescene write is involved. Its identities select
	# Viking Room, a 3,828-triangle OBJ MeshImporter fixture with no generated
	# virtual-geometry payload. The live capture must still prove the direct route
	# and zero streaming; this profile never treats metadata as runtime evidence.
	$runtimePrefabInstanceCount = if($workload -eq 'repeated-static') { [UInt64]4000 } else { [UInt64]10 }
}

$outputPath = Resolve-InputPath $outputArgument (Get-Location).Path
$artifactRoot = "$outputPath.artifacts"
[IO.Directory]::CreateDirectory($artifactRoot) | Out-Null

$editorDefault = Join-Path $installedRoot 'Editor/TerraEngine-Editor.exe'
$projectDefault = Join-Path $installedRoot 'Tests/Fixtures/Animation/ImportValidation'
if($isAnimatedWorkload -or $isAuthoredSceneWorkload -or $isRawStaticWorkload)
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
$assetDefault = if($isRawStaticWorkload)
{
	Get-RequiredProfileString $rawStaticWorkloadProfile 'asset'
}
else
{
	'Assets/Models/ImportedHero.gltf'
}
$prefabDefault = 'Assets/Models/ImportedHero_Generated/ImportedHero.teprefab'
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
$meshAssetId = (Get-EnvironmentOverride 'TERRA_VG_RUNTIME_MESH_ID' '').Trim().ToLowerInvariant()
$materialAssetId = (Get-EnvironmentOverride 'TERRA_VG_RUNTIME_MATERIAL_ID' '').Trim().ToLowerInvariant()
$materialAssetIds = @((Get-EnvironmentOverride 'TERRA_VG_RUNTIME_MATERIAL_IDS' '') -split ',' | ForEach-Object { $_.Trim().ToLowerInvariant() } | Where-Object { $_ })
$meshAssetId = if($isRawStaticWorkload -and [string]::IsNullOrWhiteSpace($meshAssetId)) { Get-RequiredProfileString $rawStaticWorkloadProfile 'meshAssetId' } else { $meshAssetId }
$materialAssetId = if($isRawStaticWorkload -and [string]::IsNullOrWhiteSpace($materialAssetId)) { Get-RequiredProfileString $rawStaticWorkloadProfile 'materialAssetId' } else { $materialAssetId }
if($isRawStaticWorkload -and $workload -eq 'ten-renderable' -and $materialAssetIds.Count -eq 0)
{
	# Assign only in the three-material case. Returning an empty array through a
	# PowerShell `if` expression emits no pipeline object and becomes `$null`,
	# which would make the repeated-static path fail before launching the editor.
	$materialAssetIds = @($rawStaticMaterialIds | ForEach-Object {
		$_.Trim().ToLowerInvariant()
	})
}
if($materialAssetIds.Count -gt 0) { $materialAssetId = '' }
$useRawMesh = -not [string]::IsNullOrWhiteSpace($meshAssetId)
if($useRawMesh -and $meshAssetId -notmatch '^[0-9a-f]{32}$')
{
	throw 'TERRA_VG_RUNTIME_MESH_ID must be a 32-character hexadecimal UUID.'
}
if(-not [string]::IsNullOrWhiteSpace($materialAssetId) -and
	$materialAssetId -notmatch '^[0-9a-f]{32}$')
{
	throw 'TERRA_VG_RUNTIME_MATERIAL_ID must be empty or a 32-character hexadecimal UUID.'
}
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
if($isAnimatedWorkload -or $isAuthoredSceneWorkload)
{
	$startupSceneRelative = if($isAuthoredSceneWorkload) {
		Get-RequiredProfileString $animatedWorkloadProfile 'scene'
	} else {
		Get-RequiredProfileString $animatedProjectProfile 'startupScene'
	}
	$startupScene = Join-Path $projectPath $startupSceneRelative
	if(-not (Test-Path -LiteralPath $startupScene -PathType Leaf))
	{
		throw "Animated-project startup scene is missing: $startupScene"
	}
	# Authored-scene captures deliberately do not instantiate an asset or prefab.
	# Checking the raw-static defaults here would reject a valid authored scene
	# before the live harness has a chance to select and measure it.
	$requiredRuntimeAssets = if($isAnimatedWorkload) { @($asset, $prefab) } else { @() }
	foreach($requiredAsset in $requiredRuntimeAssets)
	{
		if(-not (Test-Path -LiteralPath (Join-Path $projectPath $requiredAsset) -PathType Leaf))
		{
			throw "Animated workload asset is missing: $requiredAsset"
		}
	}
	$scene = Get-Content -Raw -LiteralPath $startupScene | ConvertFrom-Json
	$projectAnimatorCount = Get-SceneComponentCount (Get-OptionalProperty $scene 'objects') 'AnimatorComponent'
	$minimumProjectAnimatorCount = if($isAuthoredSceneWorkload) {
		$expectedAnimatorCount
	} else {
		Get-RequiredProfilePositiveInteger $animatedProjectProfile 'minimumProjectAnimatorCount'
	}
	if($projectAnimatorCount -lt $minimumProjectAnimatorCount)
	{
		throw "Animated-project scene exposes $projectAnimatorCount animators; profile requires at least $minimumProjectAnimatorCount."
	}
	# Add a real transient population in the open authored scene. Each prefab is
	# assigned its own stable animation-instance identity by the engine, so a
	# crowd capture measures distinct deformation work without modifying or
	# saving the user's project scene.
	if($isAnimatedWorkload)
	{
		$runtimePrefabInstanceCount = $expectedAnimatorCount
		$capturedAnimatorCount = $runtimePrefabInstanceCount
	}
	else
	{
		$capturedAnimatorCount = $projectAnimatorCount
	}
	if($capturedAnimatorCount -lt $expectedAnimatorCount)
	{
		$animatedConfigurationReason = "Animated workload requires $expectedAnimatorCount animator instances, but the prepared live scene exposes only $capturedAnimatorCount."
	}
}

$prefabName = "Virtual Geometry runtime sample $sampleIndex"
$rawPressureObjectName = ''
$rawPressureGeometryRequired = $useRawMesh -and
	$workload -eq 'page-residency-pressure'
if($rawPressureGeometryRequired)
{
	# A pressure sample needs a large projected raw mesh. The generated name is
	# also the exact object selector that the live harness scales and reports.
	$prefabScale = 10.0
	$rawPressureObjectName = "Virtual Geometry page residency pressure $sampleIndex"
	$prefabName = $rawPressureObjectName
}

$liveArguments = @{
	InstalledEditor = $editorPath
	Project = $projectPath
	Asset = $asset
	Preferences = (Join-Path $scriptRoot 'runtime-adapter-preferences.json')
	OutputDirectory = $artifactRoot
	Prefab = $prefab
	PrefabName = $prefabName
	PrefabChildObject = $prefabChild
	PrefabScale = $prefabScale
	# Viking Room has reviewable one-unit bounds, so use the unscaled source
	# geometry rather than altering its transient representation for the camera.
	RawMeshInstanceScale = if($isRawStaticWorkload) {
		[double](Get-OptionalProperty $rawStaticWorkloadProfile 'rawInstanceScale')
	} else { 1.0 }
	RawMeshGridSpacing = if($isRawStaticWorkload -and $workload -eq 'repeated-static') {
		[double](Get-OptionalProperty $rawStaticScenarioProfile 'gridSpacing')
	} else { 1.0 }
	PrefabInstanceCount = [int]$runtimePrefabInstanceCount
	PrefabSpacing = $runtimePrefabSpacing
	MeshAssetId = $meshAssetId
	MaterialAssetId = $materialAssetId
	MaterialAssetIds = $materialAssetIds
	StreamingSettleMilliseconds = $streamingSettleMilliseconds
	McpPort = $mcpPort
	StartupTimeoutSeconds = 90
}
if($isAuthoredSceneWorkload)
{
	$liveArguments['Asset'] = ''
	$liveArguments['Prefab'] = ''
	$sceneProfile = Get-OptionalProperty $animatedProjectProfile $workload
	$liveArguments['ScenePath'] = [string](Get-RequiredProfileString $sceneProfile 'scene')
	$liveArguments['ValidationCameraPosition'] = @([double]$runtimeCameraPosition[0], [double]$runtimeCameraPosition[1], [double]$runtimeCameraPosition[2])
	$liveArguments['DiagnosticsViewType'] = 'Scene'
	$liveArguments['ExpectAnimatedChange'] = $true
}
if($useRawMesh)
{
	# Raw mesh samples always use a disposable scene. This keeps the benchmark
	# isolated from authored hierarchies while still exercising the installed
	# asset database, renderer, streaming workers, and Vulkan backend.
	# The mesh/material IDs are applied directly to the transient components.
	# Do not inspect a separate virtual-geometry asset: direct workloads have no
	# such asset and their typed route/counter evidence is the authority instead.
	$liveArguments['Asset'] = ''
	$liveArguments['CreateValidationScene'] = $true
	$liveArguments['CleanupValidationScene'] = $true
	$liveArguments['ValidationScenePath'] =
		"Assets/Scenes/.terra-validation/virtual-geometry-$workload-$sampleIndex-$PID.tescene"
	if($rawPressureGeometryRequired)
	{
		# Keep the camera close to the ten-times raw mesh so projected error
		# creates sustained non-root page demand. The close crop is intentional:
		# this workload validates streaming pressure, while the normal live
		# visual capture owns full-object framing.
		$liveArguments['ValidationCameraPosition'] = @(0.0, 0.3, 3.0)
		$liveArguments['ValidationCameraTarget'] = @(0.0, 0.3, 0.0)
	}
	else
	{
		$liveArguments['ValidationCameraPosition'] = @(
			Get-OptionalProperty $rawStaticScenarioProfile 'cameraPosition')
		$liveArguments['ValidationCameraTarget'] = @(
			Get-OptionalProperty $rawStaticScenarioProfile 'cameraTarget')
	}
}

# Workload identity owns the transient representation override used by this
# sample. This keeps automatic/direct/virtual measurements reproducible without
# changing the project or persisting an editor preference.
$representationRoute = switch($workload)
{
	'repeated-static' { 'direct' }
	'ten-renderable' { 'direct' }
	'low-poly-unique-direct' { 'direct' }
	'low-poly-repeated-direct' { 'direct' }
	'low-poly-forced-virtual-geometry' { 'virtual-hardware' }
	'low-poly-automatic-routing' { 'automatic' }
	default { 'virtual-hardware' }
}
$liveArguments['RepresentationRoute'] = $representationRoute
if($representationRoute -in @('virtual-hardware', 'virtual-compute'))
{
	# The installed Vulkan lane must exercise a real raster backend route;
	# processor-expanded or ungraded execution cannot become timing evidence.
	$liveArguments['ValidationRoute'] =
		if($representationRoute -eq 'virtual-compute') { 'compute' } else { 'hardware' }
}
# Profiler frame records are published at runtime frame boundaries. Enter Play
# Mode for every benchmark sample so static geometry captures produce real
# profiler frames instead of rendering only through the idle editor viewport.
$liveArguments['EnterPlayMode'] = $true
if($isAnimatedWorkload)
{
	# scene.open is deliberately approval-gated in the editor. Automated
	# benchmarks instead use an exact disposable scene and delete it after the
	# editor exits, while the authored startup scene remains the audited source
	# of the project's animator inventory above.
	$liveArguments['CreateValidationScene'] = $true
	$liveArguments['CleanupValidationScene'] = $true
	$liveArguments['ValidationScenePath'] =
		"Assets/Scenes/.terra-validation/virtual-geometry-$workload-$sampleIndex-$PID.tescene"
	$liveArguments['ValidationCameraPosition'] = @(
		[double]$runtimeCameraPosition[0],
		[double]$runtimeCameraPosition[1],
		[double]$runtimeCameraPosition[2])
	# Animation authority and per-instance deformation advance only in the
	# runtime scene. Require visibly different frames so a static editor pose
	# cannot be promoted into measured animated hero or crowd evidence.
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

# The live harness owns the exact editor transactions. Keep those records in
# the adapter artifact so consumers can audit which route, pages, and profiler
# operation produced the native benchmark sample without reopening the editor.
$routeEvidence = [ordered]@{
	rendererRoute = [string](Get-OptionalProperty $summary 'route')
	validation = Get-OptionalProperty $summary 'validationRoute'
	representation = Get-OptionalProperty $summary 'representationRoute'
}
$pageEvidence = [ordered]@{
	diagnosticsViewType = [string](Get-OptionalProperty $summary 'diagnosticsViewType')
	postWarmupStats = Get-OptionalProperty $summary 'postWarmupCaptureStats'
	postFirstCaptureStats = Get-OptionalProperty $summary 'postFirstCaptureStats'
	postCaptureStats = Get-OptionalProperty $summary 'postCaptureStats'
	postCaptureAsset = Get-OptionalProperty $summary 'postCaptureAsset'
}
$profilerEvidence = Get-OptionalProperty $summary 'profilerEvidence'
$instantiatedGeometry = Get-OptionalProperty $summary 'instantiatedGeometry'
$rawGeometryEvidenceValid = $true
if($useRawMesh)
{
	$publishedObjectNames = @(
		Get-OptionalProperty $instantiatedGeometry 'objectNames')
	$publishedMeshId = [string](Get-OptionalProperty $instantiatedGeometry 'meshAssetId')
	$publishedMaterialId = [string](Get-OptionalProperty $instantiatedGeometry 'materialAssetId')
	# PowerShell represents an empty JSON array as an empty custom object in
	# some hosts. Normalize through strings so one-material captures retain an
	# intentionally empty array rather than acquiring a phantom identity.
	$publishedMaterialIds = @(
		(Get-OptionalProperty $instantiatedGeometry 'materialAssetIds') |
		ForEach-Object { [string]$_ } |
		Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
	$expectedMaterialIdentitySet = @($materialAssetIds | Sort-Object -Unique)
	$publishedMaterialIdentitySet = @($publishedMaterialIds | Sort-Object -Unique)
	$materialEvidenceMatches = if($expectedMaterialIdentitySet.Count -gt 0)
	{
		($publishedMaterialIdentitySet -join ',') -eq
			($expectedMaterialIdentitySet -join ',')
	}
	else
	{
		$publishedMaterialId -eq $materialAssetId
	}
	# Multi-instance raw workloads intentionally suffix every generated name.
	# Validate the exact count plus deterministic first/last names rather than
	# looking for an unsuffixed prefix that is present only in one-instance runs.
	$expectedFirstObjectName = if($runtimePrefabInstanceCount -gt 1) {
		"$prefabName 1"
	} else {
		$prefabName
	}
	$expectedLastObjectName = if($runtimePrefabInstanceCount -gt 1) {
		"$prefabName $runtimePrefabInstanceCount"
	} else {
		$prefabName
	}
	$objectNameEvidenceMatches =
		$publishedObjectNames.Count -eq $runtimePrefabInstanceCount -and
		$publishedObjectNames[0] -eq $expectedFirstObjectName -and
		$publishedObjectNames[-1] -eq $expectedLastObjectName -and
		@($publishedObjectNames | Sort-Object -Unique).Count -eq
			$runtimePrefabInstanceCount
	$rawGeometryEvidenceValid =
		[string](Get-OptionalProperty $instantiatedGeometry 'source') -eq 'raw-mesh' -and
		$publishedMeshId -eq $meshAssetId -and
		$materialEvidenceMatches -and
		$objectNameEvidenceMatches
	if($rawPressureGeometryRequired)
	{
		# This confirms the scale operation addressed the dynamically generated
		# pressure object rather than an authored prefab child with a coincidental name.
		$rawGeometryEvidenceValid = $rawGeometryEvidenceValid -and
			$publishedObjectNames -contains $rawPressureObjectName -and
			[double](Get-OptionalProperty $instantiatedGeometry 'scale') -eq 10.0 -and
			[string](Get-OptionalProperty $instantiatedGeometry 'scaleTargetObject') -eq
				$rawPressureObjectName
	}
}

$capabilityEnvelope = Get-OptionalProperty $summary 'postCaptureCapabilityEvidence'
$capabilityData = Get-OptionalProperty $capabilityEnvelope 'data'
# The live harness takes repeated next-frame presents and reads the capability
# receipt after each one. Preserve a stage row for each unique backend
# execution/stage pair; a final snapshot may repeat the last pair but cannot
# manufacture a sample or suppress a different stage from that execution.
$capabilityEvidenceEnvelopes = @(
	(Get-OptionalProperty $summary 'postPresentCapabilityEvidence')) + @(
	$capabilityEnvelope)
$measurements = [Collections.Generic.List[object]]::new()
$measuredExecutionStageKeys = [Collections.Generic.HashSet[string]]::new(
	[StringComparer]::Ordinal)
$validatedStages = [Collections.Generic.HashSet[string]]::new(
	[StringComparer]::Ordinal)
foreach($evidenceEnvelope in $capabilityEvidenceEnvelopes)
{
	$evidenceData = Get-OptionalProperty $evidenceEnvelope 'data'
	foreach($measurement in @(Get-OptionalProperty $evidenceData 'measurements'))
	{
		$executionEvidenceId = [string](
			Get-OptionalProperty $measurement 'executionEvidenceId')
		$measurementStage = [string](Get-OptionalProperty $measurement 'stage')
		$measurementEvidenceKey = "$executionEvidenceId|$measurementStage"
		if([string](Get-OptionalProperty $measurement 'availability') -eq 'measured' -and
			-not [string]::IsNullOrWhiteSpace($executionEvidenceId) -and
			[double](Get-OptionalProperty $measurement 'durationNanoseconds') -gt 0.0 -and
			$measuredExecutionStageKeys.Add($measurementEvidenceKey))
		{
			$measurements.Add($measurement)
		}
	}
	foreach($capability in @(Get-OptionalProperty $evidenceData 'capabilities'))
	{
		if([bool](Get-OptionalProperty $capability 'executed') -and
			[bool](Get-OptionalProperty $capability 'validated'))
		{
			$null = $validatedStages.Add([string](Get-OptionalProperty $capability 'stage'))
		}
	}
}
$measurements = @($measurements)
$validatedStages = @($validatedStages)
$postPresentCaptures = @(
	Get-OptionalProperty $summary 'postPresentCaptures')
$postPresentFrameIds = [Collections.Generic.HashSet[UInt64]]::new()
$postPresentFramesValid = $postPresentCaptures.Count -ge 6
foreach($postPresentCapture in $postPresentCaptures)
{
	$frameId = [UInt64](Get-OptionalProperty $postPresentCapture 'frameId')
	if($frameId -eq 0 -or -not $postPresentFrameIds.Add($frameId))
	{
		$postPresentFramesValid = $false
	}
}
$directRouteWorkload = $workload -in @(
	'low-poly-unique-direct',
	'low-poly-repeated-direct',
	'low-poly-automatic-routing',
	'repeated-static',
	'ten-renderable')
$directRouteMeasured =
	[string](Get-OptionalProperty $summary 'route') -eq 'direct-indexed'
$requiredStages = if($directRouteWorkload)
{
	# Direct-resident geometry intentionally owns no VG traversal, decode, or
	# residency work. Its fence-delayed main-pass timestamp is the only required
	# GPU stage for low-poly calibration.
	@('hardware-raster')
}
else
{
	@('traversal', 'compaction', 'indirect-generation')
}
$missingStages = @($requiredStages | Where-Object {
	$validatedStages -notcontains $_
})

$statsEnvelope = Get-OptionalProperty $summary 'postCaptureStats'
$statsData = Get-OptionalProperty $statsEnvelope 'data'
$routeEvidence.geometryRoute = [string](Get-OptionalProperty $statsData 'geometryRoute')
$routeEvidence.source = [string](Get-OptionalProperty $statsData 'geometryRouteSource')
$routeEvidence.directBatchCount = [UInt64](Get-OptionalProperty $statsData 'geometryRouteDirectBatchCount')
$routeEvidence.sharedInstanceCount = [UInt64](Get-OptionalProperty $statsData 'geometryRouteSharedInstanceCount')
$routeEvidence.suppressedPageRequests = [UInt64](Get-OptionalProperty $statsData 'geometryRouteSuppressedPageRequests')
$routeEvidence.suppressedDecodeBlocks = [UInt64](Get-OptionalProperty $statsData 'geometryRouteSuppressedDecodeBlocks')
$routeEvidence.suppressedResidencyRecords = [UInt64](Get-OptionalProperty $statsData 'geometryRouteSuppressedResidencyRecords')
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
# The live harness publishes sampled PNG metrics for both graded captures.
# Require visible content from each frame so a successful file write, static
# byte identity, or nonzero GPU timestamp can never accept an all-black image.
$minimumSampledNonBlackPixels = [UInt64]8
$minimumMeanLuminance = 2.0
$finalCaptureVisualContentValid = $captureFilesValid
foreach($captureRow in $captureRows)
{
	$metrics = Get-OptionalProperty $captureRow 'metrics'
	$metricSampleCount = [UInt64](Get-OptionalProperty $metrics 'samples')
	$metricNonBlackRatio = [double](Get-OptionalProperty $metrics 'nonBlackRatio')
	$metricNonBlackPixels = [UInt64][Math]::Floor(
		$metricSampleCount * $metricNonBlackRatio)
	$metricMeanLuminance = [double](Get-OptionalProperty $metrics 'meanLuminance')
	if($null -eq $metrics -or
		[bool](Get-OptionalProperty $metrics 'blackFrame') -or
		$metricSampleCount -eq 0 -or
		$metricNonBlackPixels -lt $minimumSampledNonBlackPixels -or
		$metricMeanLuminance -le $minimumMeanLuminance)
	{
		$finalCaptureVisualContentValid = $false
	}
}
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
# Preserve the final typed counters beside the capture snapshots. A consumer
# can therefore inspect page demand and completion without deriving it from
# the renderer route or from the PNG artifacts.
$pageEvidence.counters = $streamingCounters
$pageEvidence.activityValid = $streamingActivityValid
$animationActivityValid = $true
$observedUniqueDeformationDispatches = [UInt64]0
$perViewUniqueDeformationDispatches = [UInt64]0
$completedBackendRecordedDispatches = [UInt64]0
$deformationExecutionSource = 'not-applicable'
$completedBackendGenerationComplete = $false
$observedAnimationOwnerMappings = [UInt64]0
$observedAnimationWorkPackets = [UInt64]0
$observedAnimatedRenderables = [UInt64]0
$observedActiveAnimators = [UInt64]0
$observedEvaluatedAnimators = [UInt64]0
$observedCpuFallbackAnimators = [UInt64]0
$observedDegradedAnimators = [UInt64]0
$observedAnimationFallbacks = [UInt64]0
$observedRejectedAnimationBatches = [UInt64]0
$requiredGpuOwnedAnimators = [UInt64]0
$ownerExecutionEvidenceValid = $true
$animationAccountingValid = $true
$deformationDiagnosticsValid = $true
if($isAnimatedWorkload -or $isAuthoredSceneWorkload)
{
	$postCaptureAssetEnvelope = Get-OptionalProperty $summary 'postCaptureAsset'
	$postCaptureAssetData = Get-OptionalProperty $postCaptureAssetEnvelope 'data'
	$runtimeDiagnostics = Get-OptionalProperty (
		$postCaptureAssetData) 'runtimeDiagnostics'
	$runtimeViews = @(
		Get-OptionalProperty $runtimeDiagnostics 'views')
	# A renderer may omit per-view deformation counters while still publishing a
	# completed backend receipt. Exclude those null view records so they are not
	# misclassified as invalid deformation evidence before the receipt fallback.
	$animatedRows = @($runtimeViews | ForEach-Object {
		Get-OptionalProperty $_ 'animatedVirtualGeometry'
	} | Where-Object { $null -ne $_ })
	# Project numeric values before summing. Measure-Object -Property treats a
	# script block as a literal property name and silently reported zero.
	$perViewUniqueDeformationDispatches = [UInt64](($animatedRows |
		ForEach-Object {
			[UInt64](Get-OptionalProperty $_ 'uniqueDispatches')
		} |
		Measure-Object -Sum).Sum)
	$animationDiagnosticsEnvelope = Get-OptionalProperty $summary 'animationDiagnostics'
	$animationDiagnosticsData = Get-OptionalProperty $animationDiagnosticsEnvelope 'data'
	$gpuPresentation = Get-OptionalProperty $animationDiagnosticsData 'gpuPresentation'
	$renderableBindings = Get-OptionalProperty $animationDiagnosticsData 'renderableBindings'
	$animationSummary = Get-OptionalProperty $animationDiagnosticsData 'summary'
	$animationCounters = Get-OptionalProperty $animationSummary 'counters'
	$gpuBackendExecution = Get-OptionalProperty $animationDiagnosticsData 'gpuBackendExecution'
	$latestCompletedBackendExecution = Get-OptionalProperty $gpuBackendExecution 'latestCompleted'
	$observedAnimationOwnerMappings =
		[UInt64](Get-OptionalProperty $gpuPresentation 'ownerMappingCount')
	$observedAnimationWorkPackets =
		[UInt64](Get-OptionalProperty $gpuPresentation 'workPacketCount')
	$observedAnimatedRenderables =
		[UInt64](Get-OptionalProperty $renderableBindings 'totalAnimatedRenderables')
	$observedActiveAnimators = [UInt64](Get-OptionalProperty $animationCounters 'activeAnimators')
	$observedEvaluatedAnimators = [UInt64](Get-OptionalProperty $animationCounters 'evaluatedAnimators')
	$observedCpuFallbackAnimators = [UInt64](Get-OptionalProperty $animationCounters 'cpuFallbackAnimators')
	$observedDegradedAnimators = [UInt64](Get-OptionalProperty $animationCounters 'degradedAnimators')
	$observedAnimationFallbacks = [UInt64](Get-OptionalProperty $animationCounters 'fallbackCount')
	$observedRejectedAnimationBatches = [UInt64](Get-OptionalProperty $animationCounters 'rejectedBatches')
	$completedBackendRecordedDispatches = [UInt64](Get-OptionalProperty `
		$latestCompletedBackendExecution 'recordedDispatchCount')
	# A completed receipt must identify a real renderer generation and submission.
	# Planned/submitted work is deliberately insufficient because it may never run.
	$completedBackendGenerationComplete =
		[bool](Get-OptionalProperty $gpuBackendExecution 'completed') -and
		[UInt64](Get-OptionalProperty $latestCompletedBackendExecution 'frameId') -gt 0 -and
		[UInt64](Get-OptionalProperty $latestCompletedBackendExecution 'planGeneration') -gt 0 -and
		[UInt64](Get-OptionalProperty $latestCompletedBackendExecution 'rendererGeneration') -gt 0 -and
		[UInt64](Get-OptionalProperty $latestCompletedBackendExecution 'submissionSerial') -gt 0
	# CPU fallback is explicit runtime accounting, not an excuse to lower the
	# workload threshold. Require enough GPU-owned entries after subtracting only
	# the reported CPU fallback population from both active and evaluated totals.
	$cpuFallbackWithinTotals = $observedCpuFallbackAnimators -le $observedActiveAnimators -and
		$observedCpuFallbackAnimators -le $observedEvaluatedAnimators
	if($cpuFallbackWithinTotals)
	{
		$requiredGpuOwnedAnimators = [Math]::Max(
			$observedActiveAnimators - $observedCpuFallbackAnimators,
			$observedEvaluatedAnimators - $observedCpuFallbackAnimators)
	}
	# Requested clone counts are not execution evidence. Bind acceptance to the
	# immutable animation presentation, require complete total coverage, and keep
	# degradation, unreported fallback, and rejected publication paths at zero.
	$ownerExecutionEvidenceValid =
		$observedAnimationOwnerMappings -ge $requiredGpuOwnedAnimators -and
		$observedAnimationWorkPackets -ge $requiredGpuOwnedAnimators -and
		$observedAnimatedRenderables -ge $expectedAnimatorCount
	$animationAccountingValid = $cpuFallbackWithinTotals -and
		$observedActiveAnimators -ge $expectedAnimatorCount -and
		$observedEvaluatedAnimators -ge $expectedAnimatorCount -and
		$observedCpuFallbackAnimators + $observedAnimationOwnerMappings -ge $expectedAnimatorCount -and
		$observedCpuFallbackAnimators + $observedAnimationWorkPackets -ge $expectedAnimatorCount -and
		$observedDegradedAnimators -eq 0 -and
		$observedAnimationFallbacks -eq 0 -and
		$observedRejectedAnimationBatches -eq 0
	# Deformation is renderer-neutral compute work and may not own a distinct
	# Vulkan timestamp query. Validate it from the immutable per-view plan:
	# accepted shader contract, dynamic bounds, no fallback, and actual work.
	$invalidDeformationRows = @($animatedRows | Where-Object {
		-not [bool](Get-OptionalProperty $_ 'shaderContractValid') -or
		[UInt64](Get-OptionalProperty $_ 'fallbackCount') -ne 0 -or
		-not ([string](Get-OptionalProperty $_ 'boundsSource')).StartsWith(
			'Planned', [StringComparison]::Ordinal)
	})
	if($perViewUniqueDeformationDispatches -gt 0)
	{
		$observedUniqueDeformationDispatches = $perViewUniqueDeformationDispatches
		$deformationExecutionSource = 'renderer-per-view-uniqueDispatches'
		$deformationDiagnosticsValid = $invalidDeformationRows.Count -eq 0 -and
			$perViewUniqueDeformationDispatches -ge $minimumUniqueDeformationDispatches
	}
	else
	{
		$observedUniqueDeformationDispatches = $completedBackendRecordedDispatches
		$deformationExecutionSource = 'completed-backend-recordedDispatchCount'
		$deformationDiagnosticsValid = $invalidDeformationRows.Count -eq 0 -and
			$completedBackendGenerationComplete -and
			$completedBackendRecordedDispatches -ge $minimumUniqueDeformationDispatches
	}
	if($deformationDiagnosticsValid -and
		$validatedStages -notcontains 'deformation')
	{
		$validatedStages += 'deformation'
	}
	$animationActivityValid = [string]::IsNullOrWhiteSpace($animatedConfigurationReason) -and
		$ownerExecutionEvidenceValid -and
		$animationAccountingValid -and
		$validatedStages -contains 'deformation' -and
		$deformationDiagnosticsValid
}
$frameIdentityValid = if($isAnimatedWorkload -or $isAuthoredSceneWorkload)
{
	-not [bool](Get-OptionalProperty $summary 'unchangedByteIdentity')
}
else
{
	[bool](Get-OptionalProperty $summary 'unchangedByteIdentity')
}
$routeExecutionValid = if($directRouteWorkload)
{
	$directRouteMeasured -and
	$streamingCounters.submittedRequests -eq 0 -and
	$streamingCounters.completedReads -eq 0 -and
	$streamingCounters.acceptedPublicationPages -eq 0 -and
	$streamingCounters.physicalResidencyRows -eq 0 -and
	$streamingCounters.visibleResidentPages -eq 0
}
else
{
	[string](Get-OptionalProperty $summary 'route') -eq
		'vulkan-device-authored-indirect'
}
$boundedCounters = $frameIdentityValid -and
	$routeExecutionValid -and
	$measurements.Count -gt 0 -and
	$postPresentFramesValid -and
	$missingStages.Count -eq 0 -and
	$validationErrors.Count -eq 0 -and
	$captureFilesValid -and
	$finalCaptureVisualContentValid -and
	$streamingActivityValid -and
	$streamingCounters.rejectedPublications -eq 0 -and
	$animationActivityValid -and
	$rawGeometryEvidenceValid -and
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

$identityAssetFile = if($isAuthoredSceneWorkload)
{
	# Authored-scene workloads deliberately have no disposable prefab. Bind the
	# benchmark identity to the exact .tescene bytes already validated and opened
	# by the live harness, rather than emitting an invented unavailable token.
	$startupScene
}
elseif($useRawMesh)
{
	Join-Path $projectPath $asset
}
else
{
	Join-Path $projectPath $prefab
}
$identityAssetHash = Get-FileSha256OrUnavailable $identityAssetFile
if($isAuthoredSceneWorkload -and $identityAssetHash -notmatch '^[0-9a-f]{64}$')
{
	throw "Authored-scene identity must be the SHA-256 of the validated scene file: $identityAssetFile"
}
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
		assetHash = $identityAssetHash
		buildRecipeHash = Get-Sha256Text (
			"$workload|$asset|$prefab|$prefabScale|$meshAssetId|$materialAssetId")
	}
	environment = [ordered]@{
		backend = 'vulkan'
		adapter = $vulkanIdentity.adapter
		driver = $vulkanIdentity.driver
		route = $routeEvidence.rendererRoute
		resolution = '480x270'
		quality = 'production-installed-validation'
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
			if(-not $routeExecutionValid)
			{
				if($directRouteWorkload)
				{
					'Low-poly capture did not prove the direct-indexed route with all VG-only streaming work suppressed.'
				}
				else
				{
					'Capture did not prove the device-authored Vulkan virtual-geometry route.'
				}
			}
			if($missingStages.Count -gt 0)
			{
				"Missing validated stages: $($missingStages -join ', ')"
			}
			if(-not $postPresentFramesValid)
			{
				'Post-present sampling did not publish at least six distinct nonzero frame IDs.'
			}
			if($validationErrors.Count -gt 0)
			{
				'Vulkan validation or device-loss diagnostics were present.'
			}
			if(-not $captureFilesValid)
			{
				'Live PNG capture artifacts were incomplete.'
			}
			if(-not $finalCaptureVisualContentValid)
			{
				'Live PNG captures were black or lacked meaningful sampled luminance.'
			}
			if(-not $streamingActivityValid)
			{
				'Streaming pressure produced no complete request/read/publication activity.'
			}
			if(-not $rawGeometryEvidenceValid)
			{
				'Raw-mesh object, scale, mesh identity, or material identity evidence was incomplete.'
			}
			if(-not $animationActivityValid)
			{
				if(-not [string]::IsNullOrWhiteSpace($animatedConfigurationReason))
				{
					$animatedConfigurationReason
				}
				elseif(-not $ownerExecutionEvidenceValid)
				{
					"Animated workload requires $requiredGpuOwnedAnimators GPU-owned mappings/work packets after explicit CPU fallback accounting; observed $observedAnimationOwnerMappings/$observedAnimationWorkPackets with $observedAnimatedRenderables renderables."
				}
				elseif(-not $animationAccountingValid)
				{
					"Animated workload accounting rejected active/evaluated/CPU-fallback coverage or observed degradation, fallback, or rejected batches."
				}
				else
				{
					"Animated workload requires $minimumUniqueDeformationDispatches validated deformation executions from $deformationExecutionSource."
				}
			}
		)
	}
	geometry = [ordered]@{
		route = $routeEvidence.rendererRoute
		validatedStages = $validatedStages
		rawMesh = $instantiatedGeometry
		rawGeometryEvidenceValid = $rawGeometryEvidenceValid
	}
	routeEvidence = $routeEvidence
	pageEvidence = $pageEvidence
	profilerEvidence = $profilerEvidence
	residencyAndStreaming = [ordered]@{
		boundedCounters = $boundedCounters
		counters = $streamingCounters
		lastRejectionReason = [string](
			Get-OptionalProperty $streaming 'lastRejectionReason')
	}
	visibilityAndShading = [ordered]@{
		visualGatePassed = $captureFilesValid -and
			$finalCaptureVisualContentValid -and
			$frameIdentityValid -and
			$validationErrors.Count -eq 0
		contentMetricsPassed = $finalCaptureVisualContentValid
		minimumSampledNonBlackPixels = $minimumSampledNonBlackPixels
		minimumMeanLuminance = $minimumMeanLuminance
		unchangedByteIdentity = [bool](
			Get-OptionalProperty $summary 'unchangedByteIdentity')
		expectedFrameIdentity = if($isAnimatedWorkload -or $isAuthoredSceneWorkload) {
			'animated-change'
		} else {
			'static-repeat'
		}
		captures = $captureRows
	}
	deformation = [ordered]@{
		projectAnimatorCount = $projectAnimatorCount
		runtimePrefabInstanceCount = $runtimePrefabInstanceCount
		capturedAnimatorCount = $capturedAnimatorCount
		requiredAnimatorInstances = $expectedAnimatorCount
		minimumUniqueDeformationDispatches = $minimumUniqueDeformationDispatches
		observedUniqueDeformationDispatches =
			$observedUniqueDeformationDispatches
		perViewUniqueDeformationDispatches =
			$perViewUniqueDeformationDispatches
		completedBackendRecordedDispatches =
			$completedBackendRecordedDispatches
		deformationExecutionSource = $deformationExecutionSource
		completedBackendGenerationComplete = $completedBackendGenerationComplete
		observedAnimationOwnerMappings = $observedAnimationOwnerMappings
		observedAnimationWorkPackets = $observedAnimationWorkPackets
		observedAnimatedRenderables = $observedAnimatedRenderables
		observedActiveAnimators = $observedActiveAnimators
		observedEvaluatedAnimators = $observedEvaluatedAnimators
		observedCpuFallbackAnimators = $observedCpuFallbackAnimators
		requiredGpuOwnedAnimators = $requiredGpuOwnedAnimators
		observedDegradedAnimators = $observedDegradedAnimators
		observedAnimationFallbacks = $observedAnimationFallbacks
		observedRejectedAnimationBatches = $observedRejectedAnimationBatches
		accountingValidated = $animationAccountingValid
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
