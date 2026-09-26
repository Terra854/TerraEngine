#requires -Version 7.0
<#
.SYNOPSIS
  Creates a disposable TerraEngine project for locomotion-060 qualification.
.EXAMPLE
  ./NewLocomotionWorkloadProject.ps1 -OutputDirectory out/locomotion-w8 `
    -ManifestPath Tests/Fixtures/Locomotion/workloads.json `
    -FixtureAssetRoot Tests/Fixtures/Locomotion/Generated -WorkerCount 8 `
    -Population 1000 -InstallRoot out/build/RelWithDebInfo/Build

The project contains real production-cooked animation assets, a TERRAROOT1
terrain source repeated across disjoint lanes, and authored scene components.
The separate installed validation host must publish every source and report
actual movement; this generator alone never declares a case passed.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$OutputDirectory,
	[Parameter(Mandatory = $true)][string]$ManifestPath,
	[Parameter(Mandatory = $true)][string]$FixtureAssetRoot,
	[Parameter(Mandatory = $true)][ValidateSet(1, 4, 8)][int]$WorkerCount,
	[Parameter(Mandatory = $true)][int]$Population,
	[Parameter(Mandatory = $true)][string]$InstallRoot,
	[ValidateSet('Startup', 'Smoke', 'Semantic')][string]$StartupScene = 'Startup'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$output = [IO.Path]::GetFullPath($OutputDirectory)
$manifestFile = [IO.Path]::GetFullPath($ManifestPath)
$assetRoot = [IO.Path]::GetFullPath($FixtureAssetRoot)
$install = [IO.Path]::GetFullPath($InstallRoot)
if($Population -ne 1000) { throw 'The frozen qualification cohort requires exactly 1000 characters.' }
foreach($file in @($manifestFile, (Join-Path $assetRoot 'fixture-assets.json'))) {
	if(-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Required fixture manifest missing: $file" }
}
if(Test-Path -LiteralPath $output) {
	if(-not (Test-Path -LiteralPath $output -PathType Container) -or @(Get-ChildItem -LiteralPath $output -Force).Count -ne 0) {
		throw "Output must be a new or empty directory: $output"
	}
}
$manifest = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
if($manifest.fixtureId -ne 'locomotion-060-v1' -or $manifest.generator.seed -ne 60060 -or
	$manifest.lanes.stress.population -ne 1000 -or $manifest.lanes.stress.motionModes.Velocity -ne 700 -or
	$manifest.lanes.stress.motionModes.RootMotion -ne 300 -or
	@($manifest.schedules.workerCounts) -notcontains $WorkerCount) {
	throw 'Workloads manifest does not match locomotion-060-v1 or the requested worker variant.'
}
$assetManifest = Get-Content -LiteralPath (Join-Path $assetRoot 'fixture-assets.json') -Raw | ConvertFrom-Json
if($assetManifest.schemaVersion -ne 1 -or $assetManifest.fixtureId -ne 'locomotion-060-assets-v1' -or
	@($assetManifest.assets).Count -ne 6) { throw 'Cooked fixture manifest is incomplete.' }
$manifestHash = (Get-FileHash -LiteralPath $manifestFile -Algorithm SHA256).Hash.ToLowerInvariant()
if($assetManifest.sourceManifestSha256 -ne $manifestHash) {
	throw 'Cooked fixture provenance does not match the exact frozen workloads manifest.'
}
foreach($asset in $assetManifest.assets) {
	if($asset.path -notmatch '^Assets/Animation/[a-zA-Z0-9.-]+$' -or $asset.assetId -notmatch '^[0-9a-f]{32}$' -or
		$asset.sha256 -notmatch '^[0-9a-f]{64}$') { throw "Invalid cooked asset record for role $($asset.role)." }
	$file = Join-Path $assetRoot $asset.path
	if(-not (Test-Path -LiteralPath $file -PathType Leaf) -or
		(Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -ne $asset.sha256) {
		throw "Cooked asset hash mismatch: $file"
	}
}

# Reuse the real published terrain descriptor/bundle and canonical scene/project
# envelope from Navigation059. Installed runs never fall back to repository files.
$navigationTool = Join-Path $install 'Tests/Tools/Navigation/NewNavigationAgentWorkloadProject.ps1'
$navigationManifest = Join-Path $install 'Tests/Fixtures/Navigation/AgentWorkload/manifest.json'
foreach($file in @($navigationTool, $navigationManifest)) {
	if(-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Installed navigation prerequisite missing: $file" }
}
& $navigationTool -OutputDirectory $output -ManifestPath $navigationManifest -Cohort agent -Scenario workload -Population 1000 -InstallRoot $install
$sceneFile = Join-Path $output 'Assets/Scenes/Startup.tescene'
$baseScene = Get-Content -LiteralPath $sceneFile -Raw | ConvertFrom-Json -AsHashtable
$terrain = @($baseScene.objects | Where-Object { $_.name -eq 'NAV_TERRAIN' })
$owners = @($baseScene.objects | Where-Object { $_.name -like 'NAV_OWNER_*' } | Sort-Object name)
$presentation = @($baseScene.objects | Where-Object { $_.name -eq 'Sky' -or $_.name -eq 'Main Camera' })
$obstacles = @($baseScene.objects | Where-Object { $_.name -like 'NAV_BOX_*' -or $_.name -eq 'NAV_BLOCKER' })
if($terrain.Count -ne 1 -or $owners.Count -ne 1000 -or $presentation.Count -ne 2 -or
	@($obstacles | Where-Object { $_.name -eq 'NAV_BLOCKER' }).Count -ne 1) {
	throw 'Navigation foundation project omitted terrain, presentation, or the 1000 stable owner records.'
}
New-Item -ItemType Directory -Path (Join-Path $output 'Assets/Animation') -Force | Out-Null
foreach($asset in $assetManifest.assets) {
	$destination = Join-Path $output $asset.path
	Copy-Item -LiteralPath (Join-Path $assetRoot $asset.path) -Destination $destination
}

function Clone-Record($source) {
	return ConvertFrom-Json -AsHashtable -InputObject ($source | ConvertTo-Json -Depth 50)
}
function Stable-Id([UInt32]$index) { return ('600600000000000000000000{0:x8}' -f $index) }
function Set-RootPosition($record, [double[]]$position) {
	# World/parent-relative authority must agree for a root object. Every
	# coordinate remains inside Transform's bounded ±8192-metre local space.
	$record.components.Transform.WorldPose.position.offsetMetres = $position
	$record.components.Transform.ParentRelativePose.translationMetres = $position
}
function New-Controller {
	return [ordered]@{
		active = $true; schemaVersion = 1; shapeKind = 'Capsule'; radius = 0.35; height = 1.8
		localCenter = @(0.0, 0.9, 0.0); upDirection = @(0.0, 1.0, 0.0)
		stepHeight = 0.4; supportVolumeRadius = 0.35; supportVolumeHeight = 0.2
		maximumSlopeAngleDegrees = 50.0; contactPadding = 0.02; mass = 75.0; maximumStrength = 1000.0
		collisionLayerId = 0; collisionMask = 4294967295; groundAdhesion = 0.0; allowStairWalking = $true
		useGravity = $true; gravityScale = 1.0; jumpSpeed = 5.0; rotationPolicy = 'AlignToUp'; enabled = $true
	}
}
function New-Locomotion([bool]$rootMode) {
	return [ordered]@{
		active = $true; controllerBinding = 1; navigationBinding = 1; animatorBinding = 1
		animatorObjectId = '00000000000000000000000000000000'
		maximumSpeed = 5.0; acceleration = 20.0; braking = 25.0; turnRate = 6.283185307179586
		facing = 1; initialMotionMode = $(if($rootMode) { 1 } else { 0 })
		speedParameter = 'Speed'; localDirectionXParameter = 'DirectionX'; localDirectionZParameter = 'DirectionZ'
		groundedParameter = 'Grounded'; movingParameter = 'Moving'
	}
}
function New-Animator([bool]$rootMode) {
	return [ordered]@{
		active = $true; schemaVersion = 1; controllerAssetId = '54455252412d4c4f434f3036308ca55a'
		defaultClipAssetId = '00000000000000000000000000000000'; loopDefaultClip = $true
		playAutomatically = $true; defaultClipStartFrame = 0
		# ExtractOnly prevents authored object motion; the leased Locomotion root
		# consumer alone may route an evaluation interval into Physics.
		rootMotionPolicy = $(if($rootMode) { 1 } else { 0 })
		ragdollProfileOverrideAssetId = '54455252412d4c4f434f303630a0a55a'
	}
}
function Configure-Actor($actor, [int]$index, [bool]$rootMode) {
	$actor.uuid = Stable-Id ([UInt32](1000 + $index))
	$actor.name = ('LOCO_OWNER_{0:d4}' -f $index)
	Set-RootPosition $actor ([double[]]@((-60.0 + 8.0 * $index), 0.0, -40.0))
	$actor.components.NavigationAgentComponent = [ordered]@{
		active = $true; radiusMetres = 0.4; heightMetres = 2.0; stepHeightMetres = 0.35
		maxSlopeDegrees = 45.0; clearanceMarginMetres = 0.1; arrivalToleranceMetres = 0.25
	}
	$actor.components.CharacterControllerComponent = New-Controller
	$actor.components.CharacterLocomotionComponent = New-Locomotion $rootMode
	$actor.components.AnimatorComponent = New-Animator $rootMode
}

# Sixty-three instances of the real 128-metre terrain tile give 8064 metres
# of continuous support. Eight-metre parallel lane spacing accommodates the
# frozen looping root-yaw arc without a neighboring capsule collision.
$tiles = [Collections.Generic.List[object]]::new()
for($tileIndex = 0; $tileIndex -lt 63; ++$tileIndex) {
	$copy = Clone-Record $terrain[0]
	$copy.uuid = Stable-Id ([UInt32](100 + $tileIndex))
	$copy.name = ('LOCO_TERRAIN_{0:d2}' -f $tileIndex)
	Set-RootPosition $copy ([double[]]@((-64.0 + 128.0 * $tileIndex), 0.0, -64.0))
	$tiles.Add($copy)
}
$actors = [Collections.Generic.List[object]]::new()
for($index = 0; $index -lt 1000; ++$index) {
	$actor = Clone-Record $owners[$index]
	Configure-Actor $actor $index ($index -ge 700)
	$actors.Add($actor)
}
$geometryBlocker = Clone-Record ($obstacles | Where-Object { $_.name -eq 'NAV_BLOCKER' } | Select-Object -First 1)
$geometryBlocker.uuid = Stable-Id 50U
$geometryBlocker.name = 'LOCO_GEOMETRY_MUTATION_BLOCKER'
$geometryBlocker.components.ColliderSetComponent.colliders[0].recordId = Stable-Id 350U
$geometryBlocker.components.ColliderSetComponent.colliders[0].shape.halfExtents = @(0.25, 0.5, 0.25)
Set-RootPosition $geometryBlocker ([double[]]@(-56.0, 0.5, 0.0))
$baseScene.name = 'Locomotion 060 Stress'
$baseScene.workload = [ordered]@{
		fixtureId = 'locomotion-060-v1'; seed = 60060; workerCount = $WorkerCount
		population = 1000; velocityCount = 700; rootMotionCount = 300
		terrainTileCount = 63; laneSpacingMetres = 8.0
		coverageRequirement = 'publish-all-terrain-collision-and-navigation-sources-before-admission'
	}
$baseScene.objects = @($presentation) + @($tiles.ToArray()) + @($geometryBlocker) + @($actors.ToArray())

# The small smoke scene is deliberately not a measured 1001st actor. It has
# no managed script owner, so installed ABI/receipt probes can Acquire safely.
$smoke = Clone-Record $owners[0]
Configure-Actor $smoke 0 $false
$smoke.uuid = Stable-Id 1U
$smoke.name = 'LOCO_VALIDATION_SMOKE'
Set-RootPosition $smoke ([double[]]@(4.0, 0.0, 8.0))
# The physical terrain provider owns sample-space coordinates. Keep this
# single smoke terrain untransformed so navigation and physical support agree.
# This does not qualify the separate, still-unverified stress terrain layout.
$smokeTerrain = Clone-Record $tiles[0]
Set-RootPosition $smokeTerrain ([double[]]@(0.0, 0.0, 0.0))
$smokeScene = [ordered]@{
	format = 'TerraSceneUnboundedV1'; name = 'Locomotion 060 Smoke'
	workload = [ordered]@{ fixtureId = 'locomotion-060-v1'; role = 'validation-smoke'; requiresPublishedTerrain = $true }
	objects = @($presentation) + @($smokeTerrain) + @($smoke)
}

# A small obstacle scene gives the semantic host real collision inputs; the
# host still owns destination changes, terrain edits, and per-case evidence.
$semanticActor = Clone-Record $owners[0]
Configure-Actor $semanticActor 0 $false
$semanticActor.uuid = Stable-Id 2U
$semanticActor.name = 'LOCO_SEMANTIC_ACTOR'
Set-RootPosition $semanticActor ([double[]]@(-22.0, 0.0, -20.0))
$wall = Clone-Record $obstacles[0]
$wall.uuid = Stable-Id 3U
$wall.name = 'LOCO_SEMANTIC_WALL'
$wall.components.ColliderSetComponent.colliders[0].recordId = Stable-Id 303U
Set-RootPosition $wall ([double[]]@(0.0, 1.0, -20.0))
$wall.components.ColliderSetComponent.colliders[0].shape.halfExtents = @(0.5, 1.0, 8.0)
$narrowLeft = Clone-Record $obstacles[0]
$narrowLeft.uuid = Stable-Id 4U
$narrowLeft.name = 'LOCO_NARROW_LEFT'
$narrowLeft.components.ColliderSetComponent.colliders[0].recordId = Stable-Id 304U
Set-RootPosition $narrowLeft ([double[]]@(18.0, 1.0, -1.45))
$narrowLeft.components.ColliderSetComponent.colliders[0].shape.halfExtents = @(8.0, 1.0, 1.0)
$narrowRight = Clone-Record $obstacles[0]
$narrowRight.uuid = Stable-Id 5U
$narrowRight.name = 'LOCO_NARROW_RIGHT'
$narrowRight.components.ColliderSetComponent.colliders[0].recordId = Stable-Id 305U
Set-RootPosition $narrowRight ([double[]]@(18.0, 1.0, 1.45))
$narrowRight.components.ColliderSetComponent.colliders[0].shape.halfExtents = @(8.0, 1.0, 1.0)
function New-SemanticBox([string]$name, [UInt32]$id, [double[]]$position,
	[double[]]$halfExtents, [double[]]$localRotation) {
	$box = Clone-Record $obstacles[0]
	$box.uuid = Stable-Id $id
	$box.name = $name
	$box.components.ColliderSetComponent.colliders[0].recordId = Stable-Id ([UInt32]($id + 300))
	$box.components.ColliderSetComponent.colliders[0].shape.halfExtents = $halfExtents
	$box.components.ColliderSetComponent.colliders[0].localRotation = $localRotation
	Set-RootPosition $box $position
	return $box
}
$identityRotation = [double[]]@(1.0, 0.0, 0.0, 0.0)
$shallowStep = New-SemanticBox 'LOCO_SHALLOW_STEP' 10U ([double[]]@(-48.0, 0.15, 10.0)) ([double[]]@(2.0, 0.15, 2.0)) $identityRotation
$excessiveStep = New-SemanticBox 'LOCO_EXCESSIVE_STEP' 11U ([double[]]@(-36.0, 0.25, 10.0)) ([double[]]@(2.0, 0.25, 2.0)) $identityRotation
function Ramp-Rotation([double]$degrees) {
	$radians = $degrees * [Math]::PI / 180.0
	return [double[]]@([Math]::Cos($radians / 2.0), -[Math]::Sin($radians / 2.0), 0.0, 0.0)
}
$shallowRamp = New-SemanticBox 'LOCO_SHALLOW_SLOPE' 12U ([double[]]@(-12.0, (3.0 * [Math]::Sin(35.0 * [Math]::PI / 180.0) + 0.1), 35.0)) ([double[]]@(2.0, 0.1, 3.0)) (Ramp-Rotation 35.0)
$excessiveRamp = New-SemanticBox 'LOCO_EXCESSIVE_SLOPE' 13U ([double[]]@(12.0, (3.0 * [Math]::Sin(60.0 * [Math]::PI / 180.0) + 0.1), 35.0)) ([double[]]@(2.0, 0.1, 3.0)) (Ramp-Rotation 60.0)
$holeSecondTile = Clone-Record $terrain[0]
$holeSecondTile.uuid = Stable-Id 14U
$holeSecondTile.name = 'LOCO_TERRAIN_AFTER_GAP'
# Tile0 ends at X=64; the next real terrain footprint begins at X=66.
# This two-metre absence of support is not a painted obstacle or fake mask.
Set-RootPosition $holeSecondTile ([double[]]@(66.0, 0.0, -64.0))
$movingTarget = Clone-Record $owners[0]
$movingTarget.uuid = Stable-Id 20U
$movingTarget.name = 'LOCO_MOVING_TARGET'
$movingTarget.components = [ordered]@{ Transform = $movingTarget.components.Transform }
Set-RootPosition $movingTarget ([double[]]@(20.0, 0.0, -20.0))
$dynamicBlocker = Clone-Record ($obstacles | Where-Object { $_.name -eq 'NAV_BLOCKER' } | Select-Object -First 1)
$dynamicBlocker.uuid = Stable-Id 21U
$dynamicBlocker.name = 'LOCO_DYNAMIC_BLOCKER'
$dynamicBlocker.components.ColliderSetComponent.colliders[0].recordId = Stable-Id 321U
$dynamicBlocker.components.ColliderSetComponent.colliders[0].shape.halfExtents = @(1.0, 0.5, 1.0)
Set-RootPosition $dynamicBlocker ([double[]]@(0.0, 0.5, -15.0))
$semanticScene = [ordered]@{
	format = 'TerraSceneUnboundedV1'; name = 'Locomotion 060 Semantic'
	workload = [ordered]@{ fixtureId = 'locomotion-060-v1'; role = 'semantic'; requiresPublishedTerrain = $true }
	objects = @($presentation) + @($tiles[0], $holeSecondTile) + @($semanticActor, $movingTarget, $dynamicBlocker,
		$wall, $narrowLeft, $narrowRight, $shallowStep, $excessiveStep, $shallowRamp, $excessiveRamp)
}

function Write-Json([string]$path, $document) {
	$json = ($document | ConvertTo-Json -Depth 100).Replace("`r`n", "`n").TrimEnd("`n") + "`n"
	[IO.File]::WriteAllText($path, $json, [Text.UTF8Encoding]::new($false))
}
Write-Json $sceneFile $baseScene
Write-Json (Join-Path $output 'Assets/Scenes/Smoke.tescene') $smokeScene
Write-Json (Join-Path $output 'Assets/Scenes/Semantic.tescene') $semanticScene
# The editor restores Library/LastScene.txt before consulting game.json or the
# export profile. Keep all three ordinary project selectors on the same scene.
[IO.File]::WriteAllText((Join-Path $output 'Library/LastScene.txt'),
	('Scenes/' + $StartupScene + ".tescene`n"), [Text.UTF8Encoding]::new($false))

# A compilable script reference exercises the installed managed facade during
# export. The validation host remains sole command producer for measured actors.
$script = @'
// LocomotionWorkload.cs - managed API linkage marker for generated projects.
using TerraEngine;
using TerraEngine.Locomotion;
public static class LocomotionWorkload
{
	public static bool Ready(CharacterLocomotion motion)
	{
		return motion.CopyObservation(out CharacterLocomotionObservation observation) == LocomotionStatus.Success
			&& observation.Handle.IsValid;
	}
}
'@
[IO.File]::WriteAllText((Join-Path $output 'Assets/Scripts/LocomotionWorkload.cs'), $script.Replace("`r`n", "`n"), [Text.UTF8Encoding]::new($false))
Write-Json (Join-Path $output 'game.json') ([ordered]@{
	name = 'Locomotion 060'; startupScene = ('Scenes/' + $StartupScene + '.tescene')
	scripts = @('Assets/Scripts/LocomotionWorkload.cs')
})
Write-Json (Join-Path $output 'BuildProfiles/locomotion.json') ([ordered]@{
	name = ('Locomotion 060 W' + $WorkerCount); id = ('locomotion-060-w' + $WorkerCount)
	platform = 'windows'; outputDirectory = ('Builds/Locomotion-W' + $WorkerCount)
	startupScene = ('Scenes/' + $StartupScene + '.tescene'); includedScenes = @('Scenes/Startup.tescene', 'Scenes/Smoke.tescene', 'Scenes/Semantic.tescene')
	enableManagedCompilation = $true; requiredWorkerCount = $WorkerCount
	navigationGeometry = 'Assets/Navigation/terrain.tenavgeom'
	terrainAsset = 'Assets/Terrain/NavigationTerrain.teterrain'
})
New-Item -ItemType Directory -Path (Join-Path $output 'Assets/Locomotion') -Force | Out-Null
$assetHashes = [Collections.Generic.List[object]]::new()
# Include the installed managed reference copied by the base project generator.
# Its exact bytes are an input to export, not a deterministic product of this
# script, and recording the hash makes a changing install candidate visible.
foreach($relativeRoot in @('Assets/Animation', 'Assets/Navigation', 'Assets/Terrain', 'Build/Assembly-CSharp/References')) {
	$root = Join-Path $output $relativeRoot
	foreach($file in @(Get-ChildItem -LiteralPath $root -Recurse -File | Sort-Object FullName)) {
		$relative = [IO.Path]::GetRelativePath($output, $file.FullName).Replace('\', '/')
		$assetHashes.Add([ordered]@{ path = $relative; sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant() })
	}
}
$metadata = [ordered]@{
	schemaVersion = 1; fixtureId = 'locomotion-060-v1'; sourceManifestSha256 = $manifestHash
	seed = 60060; workerCount = $WorkerCount; population = 1000
	startupScene = ('Scenes/' + $StartupScene + '.tescene')
	motionModes = [ordered]@{ Velocity = 700; RootMotion = 300 }
	rootAdmissionPrerequisite = 'publish-controller-and-explicit-StartRootMotion-trigger-before-RootMotion-Acquire'
	roles = [ordered]@{ validationSmoke = [ordered]@{ scene = 'Scenes/Smoke.tescene'; objectId = (Stable-Id 1U) }; semantic = [ordered]@{ scene = 'Scenes/Semantic.tescene'; objectId = (Stable-Id 2U) } }
	semanticGeometry = [ordered]@{
		movingTarget = [ordered]@{ objectId = (Stable-Id 20U); initialMetres = @(20.0, 0.0, -20.0); movementRecipe = 'transactional-target-transform-revision' }
		dynamicBlocker = [ordered]@{ objectId = (Stable-Id 21U); initialCenterMetres = @(0.0, 0.5, -15.0); halfExtentsMetres = @(1.0, 0.5, 1.0); movementRecipe = 'transactional-kinematic-transform-revision' }
		wall = [ordered]@{ objectId = (Stable-Id 3U); startMetres = @(-22.0, 0.0, -20.0) }
		narrow = [ordered]@{ leftObjectId = (Stable-Id 4U); rightObjectId = (Stable-Id 5U); gapMetres = 0.9; startMetres = @(18.0, 0.0, -6.0) }
		shallowStep = [ordered]@{ objectId = (Stable-Id 10U); heightMetres = 0.30; startMetres = @(-48.0, 0.0, 5.0) }
		excessiveStep = [ordered]@{ objectId = (Stable-Id 11U); heightMetres = 0.50; startMetres = @(-36.0, 0.0, 5.0) }
		shallowSlope = [ordered]@{ objectId = (Stable-Id 12U); degrees = 35.0; startMetres = @(-12.0, 0.0, 30.5) }
		excessiveSlope = [ordered]@{ objectId = (Stable-Id 13U); degrees = 60.0; startMetres = @(12.0, 0.0, 30.5) }
		terrainHole = [ordered]@{ nearTileObjectId = (Stable-Id 100U); farTileObjectId = (Stable-Id 14U); missingSupportX = @(64.0, 66.0); startMetres = @(62.0, 0.0, 0.0) }
	}
	caseIds = @($manifest.lanes.semantic.caseIds)
	controller = [ordered]@{ shape = 'Capsule'; radiusMetres = 0.35; totalHeightMetres = 1.8; localCenterMetres = @(0.0, 0.9, 0.0); stepHeightMetres = 0.4; maximumSlopeDegrees = 50.0; contactPaddingMetres = 0.02 }
	navigationProfile = [ordered]@{ radiusMetres = 0.4; heightMetres = 2.0; stepHeightMetres = 0.35; maximumSlopeDegrees = 45.0; clearanceMarginMetres = 0.1 }
	terrain = [ordered]@{ tileCount = 63; tileWidthMetres = 128.0; laneSpacingMetres = 8.0; source = 'Assets/Terrain/NavigationTerrain.teterrain'; sourceObjectIds = @($tiles | ForEach-Object { $_.uuid }); coverageRequirement = 'all-tiles-current-physical-and-navigation-publication' }
	geometryMutation = [ordered]@{ objectId = (Stable-Id 50U); colliderRecordId = (Stable-Id 350U); kind = 'kinematic-box'; initialCenterMetres = @(-56.0, 0.5, 0.0); halfExtentsMetres = @(0.25, 0.5, 0.25); updateFrequencyHz = 1; centerFormula = 'x=-56+0.5*sin(2*pi*t/8), y=0.5, z=0'; protectedMutationRequired = $true }
	actors = @($actors | ForEach-Object { [ordered]@{ objectId = $_.uuid; motionMode = $_.components.CharacterLocomotionComponent.initialMotionMode; startMetres = $_.components.Transform.WorldPose.position.offsetMetres } })
	assets = $assetHashes.ToArray()
	qualificationState = 'unrun-requires-installed-host-evidence'
}
Write-Json (Join-Path $output 'Assets/Locomotion/workload-metadata.json') $metadata
Write-Host "Generated locomotion-060 W$WorkerCount project with 1000 characters and 63 terrain tiles at $output. No qualification was run."
