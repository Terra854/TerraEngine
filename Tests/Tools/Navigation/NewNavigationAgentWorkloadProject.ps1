#requires -Version 7.0
<#
.SYNOPSIS
  Generates a deterministic, source-free navigation workload project.

Requires PowerShell 7.0 or newer (the generator uses UInt64/UL literals and
PowerShell 7 JSON/file semantics).

The generated scene is real authored input: a 129x129 flat triangle mesh is
written in the checked .tenavgeom wire format, boxes use ordinary physics
components, and agent settings are serialized as NavigationAgentComponent
values. Runtime routes, receipts, and guidance are intentionally absent.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$OutputDirectory,
	[Parameter(Mandatory = $true)][string]$ManifestPath,
	[ValidateSet('direct', 'agent')][string]$Cohort = 'agent',
	# Clearance parity is intentionally a separate tiny scene. It must not add a
	# full-width wall to the 1,000-owner qualification workload and change its
	# admission/latency evidence.
	[ValidateSet('workload', 'clearance-parity')][string]$Scenario = 'workload',
	[ValidateRange(1, 1000)][int]$Population = 1000,
	[string]$InstallRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$seed = [UInt64]59001
$out = [IO.Path]::GetFullPath($OutputDirectory)
$manifest = [IO.Path]::GetFullPath($ManifestPath)
if(-not (Test-Path -LiteralPath $manifest -PathType Leaf)) { throw "Manifest not found: $manifest" }
$manifestDocument = Get-Content -LiteralPath $manifest -Raw | ConvertFrom-Json
if($manifestDocument.fixtureId -ne 'navigation-agent-workload-v2' -or
	$manifestDocument.generator.seed -ne 59001 -or $manifestDocument.generator.endpointPairCount -ne 1000 -or
	$manifestDocument.world.terrain.boundsMetres.min[0] -ne -64 -or $manifestDocument.world.terrain.boundsMetres.max[0] -ne 64 -or
	$manifestDocument.world.terrain.walkableMarginMetres -ne 2 -or $manifestDocument.generator.endpointPairFormula.gridResolution -ne 64 -or
	$manifestDocument.world.staticBoxes.count -ne 64 -or $manifestDocument.cohorts.destinationChangesPerSecond -ne 100 -or
	$manifestDocument.workers.qualificationCount -ne 8) { throw 'AgentWorkload manifest constants do not match the deterministic workload contract.' }
New-Item -ItemType Directory -Force -Path $out | Out-Null
foreach($relative in @('Assets/Navigation', 'Assets/Terrain', 'Assets/Scenes', 'Assets/Scripts', 'BuildProfiles', 'ProjectSettings', 'Library')) {
	New-Item -ItemType Directory -Force -Path (Join-Path $out $relative) | Out-Null
}

function Write-U32([IO.BinaryWriter]$writer, [UInt32]$value) { $writer.Write($value) }
function Write-U64([IO.BinaryWriter]$writer, [UInt64]$value) { $writer.Write($value) }
function Write-F32([IO.BinaryWriter]$writer, [single]$value) { $writer.Write($value) }
function Split-Mix([UInt64]$value) {
	$mask = [Numerics.BigInteger]::Parse('18446744073709551615')
	$z = ([Numerics.BigInteger]$value + [Numerics.BigInteger]::Parse('11400714819323198485')) -band $mask
	$z = (($z -bxor ($z -shr 30)) * [Numerics.BigInteger]::Parse('13787848793156543929')) -band $mask
	$z = (($z -bxor ($z -shr 27)) * [Numerics.BigInteger]::Parse('10723151780598845931')) -band $mask
	$z = ($z -bxor ($z -shr 31)) -band $mask
	return [UInt64]$z
}
function New-Id([UInt32]$index) {
	# Stable 32-hex UUID text; the high prefix identifies this fixture family.
	return ('590100000000000000000000{0:x8}' -f $index)
}
function Get-PerimeterSlots {
	$slots = [Collections.Generic.List[object]]::new()
	for($x = 0; $x -lt 64; ++$x) { $slots.Add([pscustomobject]@{ x = $x; z = 0 }) }
	for($z = 1; $z -lt 64; ++$z) { $slots.Add([pscustomobject]@{ x = 63; z = $z }) }
	for($x = 62; $x -ge 0; --$x) { $slots.Add([pscustomobject]@{ x = $x; z = 63 }) }
	for($z = 62; $z -gt 0; --$z) { $slots.Add([pscustomobject]@{ x = 0; z = $z }) }
	return $slots
}
function Get-Position([object]$slot) {
	$slot = @($slot)[0]
	$x = [double]$slot.x; $z = [double]$slot.z
	# Keep each arithmetic operand scalar. PowerShell otherwise unwraps an
	# object property collection into an Object[] during JSON generation.
	# Preserve the 64-by-64, 252-slot topology while mapping both extrema into
	# the declared two-metre walkable margin: [-62, 62], not [-62, 64].
	$marginSafeStep = 124.0 / 63.0
	$px = [double](-62.0 + ($marginSafeStep * $x))
	$pz = [double](-62.0 + ($marginSafeStep * $z))
	return [double[]]@($px, 0.0, $pz)
}

# Produce the exact perimeter pairing rule documented by AgentWorkload/manifest.json.
$slots = Get-PerimeterSlots
$endpointPairs = [Collections.Generic.List[object]]::new()
for($i = 0; $i -lt $Population; ++$i) {
	$startSlot = [int]((Split-Mix ($seed + [UInt64](2 * $i))) % [UInt64]$slots.Count)
	$delta = 1 + [int]((Split-Mix ($seed + [UInt64](2 * $i + 1))) % [UInt64]($slots.Count - 1))
	$destinationSlot = ($startSlot + $delta) % $slots.Count
	$endpointPairs.Add([ordered]@{ start = (Get-Position ($slots[$startSlot])); destination = (Get-Position ($slots[$destinationSlot])) })
}
# Fail before generating project assets if a future formula places an endpoint
# outside the manifest's declared walkable margin. This validates the exact
# copied endpoint values consumed by both generated scenes and host validation.
$minimumWalkable = [double]$manifestDocument.world.terrain.boundsMetres.min[0] + [double]$manifestDocument.world.terrain.walkableMarginMetres
$maximumWalkable = [double]$manifestDocument.world.terrain.boundsMetres.max[0] - [double]$manifestDocument.world.terrain.walkableMarginMetres
foreach($pair in $endpointPairs) {
	foreach($endpoint in @($pair.start, $pair.destination)) {
		foreach($coordinate in @([double]$endpoint[0], [double]$endpoint[2])) {
			if(-not [double]::IsFinite($coordinate) -or $coordinate -lt ($minimumWalkable - 1e-9) -or $coordinate -gt ($maximumWalkable + 1e-9)) {
				throw "Generated endpoint falls outside the declared walkable margin [$minimumWalkable, $maximumWalkable]."
			}
		}
	}
}

# Emit checked .tenavgeom: flat 129x129 vertices and two triangles per cell.
$geometryPath = Join-Path $out 'Assets/Navigation/terrain.tenavgeom'
$payload = [IO.MemoryStream]::new()
$payloadWriter = [IO.BinaryWriter]::new($payload)
$grid = 129
for($z = 0; $z -lt $grid; ++$z) { for($x = 0; $x -lt $grid; ++$x) {
	$payloadWriter.Write([single](-64.0 + $x)); $payloadWriter.Write([single]0.0); $payloadWriter.Write([single](-64.0 + $z))
} }
for($z = 0; $z -lt ($grid - 1); ++$z) { for($x = 0; $x -lt ($grid - 1); ++$x) {
	$a = [UInt32]($z * $grid + $x); $b = $a + 1U; $c = $a + [UInt32]$grid; $d = $c + 1U
	$payloadWriter.Write($a); $payloadWriter.Write($d); $payloadWriter.Write($b)
	$payloadWriter.Write($a); $payloadWriter.Write($c); $payloadWriter.Write($d)
} }
$payloadWriter.Flush(); $payloadBytes = $payload.ToArray(); $payloadWriter.Dispose(); $payload.Dispose()
function Fnv1a([byte[]]$bytes) {
	$hash = [UInt64]14695981039346656037
	foreach($byte in $bytes) { $hash = [UInt64]((([Numerics.BigInteger]$hash -bxor $byte) * 1099511628211) -band [Numerics.BigInteger]::Parse('18446744073709551615')) }
	return $hash
}
$fingerprint = Fnv1a $payloadBytes
$header = [IO.MemoryStream]::new(); $headerWriter = [IO.BinaryWriter]::new($header)
$headerWriter.Write([Text.Encoding]::ASCII.GetBytes('TENAVGEO'))
Write-U32 $headerWriter 1U; Write-U32 $headerWriter 1U
Write-U64 $headerWriter ([UInt64]($grid * $grid)); Write-U64 $headerWriter ([UInt64](2 * ($grid - 1) * ($grid - 1))); Write-U64 $headerWriter 0UL
Write-U64 $headerWriter 112UL; Write-U64 $headerWriter ([UInt64](112 + 12 * $grid * $grid)); Write-U64 $headerWriter ([UInt64](112 + 12 * $grid * $grid + 12 * 2 * ($grid - 1) * ($grid - 1)))
foreach($value in @(-64.0, 0.0, -64.0, 64.0, 0.0, 64.0)) { Write-F32 $headerWriter ([single]$value) }
Write-U64 $headerWriter $fingerprint; Write-U64 $headerWriter 0UL; Write-U64 $headerWriter ([UInt64]$payloadBytes.Length)
$headerWriter.Flush(); $headerBytes = $header.ToArray(); $headerWriter.Dispose(); $header.Dispose()
$geometryBytes = [Collections.Generic.List[byte]]::new(); $geometryBytes.AddRange($headerBytes); $geometryBytes.AddRange($payloadBytes)
$checksum = Fnv1a ([byte[]]$geometryBytes.ToArray())
for($offset = 0; $offset -lt 8; ++$offset) { $geometryBytes[96 + $offset] = [byte](($checksum -shr (8 * $offset)) -band 0xff) }
[IO.File]::WriteAllBytes($geometryPath, $geometryBytes.ToArray())

# The navigation mesh is accompanied by the authored terrain parent asset. The
# editor's TerrainCreationService materializes its private TERRAROOT1 bundle
# when this descriptor is imported; keeping the descriptor in the generated
# project makes terrain ownership explicit instead of treating the mesh as
# synthetic runtime geometry.
$seedRoot = if($InstallRoot) { Join-Path $InstallRoot 'Tests/Fixtures/Navigation/AgentWorkload/TerrainSeed' } else { Join-Path $PSScriptRoot '../../Fixtures/Navigation/AgentWorkload/TerrainSeed' }
$seedTerrain = Join-Path $seedRoot 'Navigation059.teterrain'
$seedMeta = $seedTerrain + '.meta'
$seedData = $seedTerrain + '.data'
$seedCurrent = Join-Path $seedData 'CURRENT'
$seedRootDir = Join-Path $seedData 'roots'
foreach($required in @($seedTerrain, $seedMeta, $seedCurrent, $seedRootDir)) {
	if(-not (Test-Path -LiteralPath $required)) { throw "TerrainSeed is incomplete; missing $required" }
}
$terrainDescriptor = Get-Content -LiteralPath $seedTerrain -Raw | ConvertFrom-Json
if($terrainDescriptor.format -ne 'TerraEngineTerrain' -or $terrainDescriptor.rootFormat -ne 'TERRAROOT1' -or
	$terrainDescriptor.width -ne 129 -or $terrainDescriptor.height -ne 129 -or $terrainDescriptor.sampleSpacingMetres -ne 1) {
	throw 'TerrainSeed descriptor is not the required 129x129, one-metre TERRAROOT1 asset.'
}
$terrainAssetId = [string]$terrainDescriptor.assetId
$currentBytes = [IO.File]::ReadAllText($seedCurrent)
# These are hash-pinned runtime inputs, not platform-native text documents.
# Refuse a checkout-converted bundle before producing an unusable project.
if($currentBytes.Contains("`r")) {
	throw 'TerrainSeed CURRENT has noncanonical CRLF bytes; restore the LF fixture protected by .gitattributes.'
}
$rootPointer = $currentBytes.Split("`n")[0]
if($rootPointer -notmatch '^[0-9a-f]{32}$' -or -not (Test-Path -LiteralPath (Join-Path $seedRootDir ($rootPointer + '.root')))) {
	throw 'TerrainSeed CURRENT does not resolve to its canonical root manifest.'
}
$rootManifestPath = Join-Path $seedRootDir ($rootPointer + '.root')
if([IO.File]::ReadAllText($rootManifestPath).Contains("`r")) {
	throw 'TerrainSeed root manifest has noncanonical CRLF bytes; restore the hash-pinned LF fixture.'
}
Copy-Item -LiteralPath $seedTerrain -Destination (Join-Path $out 'Assets/Terrain/NavigationTerrain.teterrain') -Force
Copy-Item -LiteralPath $seedMeta -Destination (Join-Path $out 'Assets/Terrain/NavigationTerrain.teterrain.meta') -Force
Copy-Item -LiteralPath $seedData -Destination (Join-Path $out 'Assets/Terrain/NavigationTerrain.teterrain.data') -Recurse -Force
$copiedMetaPath = Join-Path $out 'Assets/Terrain/NavigationTerrain.teterrain.meta'
$copiedMeta = Get-Content -LiteralPath $copiedMetaPath -Raw | ConvertFrom-Json
$copiedMeta.sourcePath = 'Terrain/NavigationTerrain.teterrain'
$copiedMeta | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $copiedMetaPath -Encoding utf8

function Transform([double[]]$position) {
	# Serializer::DeserializeCanonicalTransform requires both world authority and
	# parent-relative authority. Every generated object is a scene root, so its
	# parent-relative translation is the same local metre offset and space id zero.
	return [ordered]@{
		WorldPose = [ordered]@{
			position = [ordered]@{ cell = [ordered]@{ x = '0'; y = '0'; z = '0' }; offsetMetres = $position }
			rotation = @(1.0, 0.0, 0.0, 0.0)
			scale = @(1.0, 1.0, 1.0)
		}
		ParentRelativePose = [ordered]@{
			translationMetres = $position
			rotation = @(1.0, 0.0, 0.0, 0.0)
			scale = @(1.0, 1.0, 1.0)
			transformSpaceId = [UInt64]0
		}
	}
}

function Assert-CanonicalTransform([object]$transform, [string]$objectName) {
	# Keep this generator guard structurally identical to the canonical serializer
	# contract. It fails before export if a new fixture object drops a required pose
	# field, rather than silently loading without its Transform component.
	if($null -eq $transform -or -not $transform.Contains('WorldPose') -or -not $transform.Contains('ParentRelativePose')) {
		throw "Generated object '$objectName' is missing canonical Transform authority."
	}
	$world = $transform.WorldPose
	$relative = $transform.ParentRelativePose
	if($null -eq $world -or $null -eq $relative -or $null -eq $world.position -or $null -eq $world.position.cell) {
		throw "Generated object '$objectName' has an incomplete canonical Transform."
	}
	foreach($key in @('x', 'y', 'z')) {
		if(-not $world.position.cell.Contains($key) -or [string]::IsNullOrWhiteSpace([string]$world.position.cell[$key])) {
			throw "Generated object '$objectName' has an invalid world cell."
		}
	}
	$worldOffset = @($world.position.offsetMetres)
	$relativeTranslation = @($relative.translationMetres)
	foreach($vector in @($worldOffset, $relativeTranslation, @($world.scale), @($relative.scale))) {
		if($vector.Count -ne 3 -or @($vector | Where-Object { -not [double]::IsFinite([double]$_) }).Count -ne 0) {
			throw "Generated object '$objectName' has an invalid canonical Transform vector."
		}
	}
	foreach($rotation in @(@($world.rotation), @($relative.rotation))) {
		if($rotation.Count -ne 4 -or @($rotation | Where-Object { -not [double]::IsFinite([double]$_) }).Count -ne 0) {
			throw "Generated object '$objectName' has an invalid canonical Transform rotation."
		}
	}
	for($index = 0; $index -lt 3; ++$index) {
		if([double]$worldOffset[$index] -ne [double]$relativeTranslation[$index]) {
			throw "Generated object '$objectName' has mismatched root world and parent-relative translations."
		}
	}
	if(-not ($relative.transformSpaceId -is [UInt64])) {
		throw "Generated object '$objectName' has a non-unsigned transform space id."
	}
}
function Box-Collider([UInt32]$index) {
	return [ordered]@{ active = $true; colliders = @([ordered]@{ recordId = (New-Id (100000 + $index)); recordVersion = 1; localCenter = @(0.0, 0.0, 0.0); localRotation = @(1.0, 0.0, 0.0, 0.0); shape = [ordered]@{ type = 'Box'; halfExtents = @(1.5, 1.0, 1.5) }; materialId = '00000000000000000000000000000000'; useDefaultMaterial = $true; isTrigger = $false; useLayerOverride = $false; collisionLayerId = 0; eventPolicy = 'All'; queryFlags = 15; enabled = $true }) }
}
function Blocker-Collider {
	return [ordered]@{ active = $true; colliders = @([ordered]@{ recordId = (New-Id 102000); recordVersion = 1; localCenter = @(0.0, 0.0, 0.0); localRotation = @(1.0, 0.0, 0.0, 0.0); shape = [ordered]@{ type = 'Box'; halfExtents = @(1.0, 1.0, 1.0) }; materialId = '00000000000000000000000000000000'; useDefaultMaterial = $true; isTrigger = $false; useLayerOverride = $false; collisionLayerId = 0; eventPolicy = 'All'; queryFlags = 15; enabled = $true }) }
}
function Clearance-Collider([UInt32]$index, [double[]]$halfExtents) {
	# One scene ColliderSet record becomes one immutable navigation source. Keep
	# each jamb and the roof separate so their conservative source AABBs preserve
	# the physical opening between them.
	return [ordered]@{ active = $true; colliders = @([ordered]@{ recordId = (New-Id (103000 + $index)); recordVersion = 1; localCenter = @(0.0, 0.0, 0.0); localRotation = @(1.0, 0.0, 0.0, 0.0); shape = [ordered]@{ type = 'Box'; halfExtents = $halfExtents }; materialId = '00000000000000000000000000000000'; useDefaultMaterial = $true; isTrigger = $false; useLayerOverride = $false; collisionLayerId = 0; eventPolicy = 'All'; queryFlags = 15; enabled = $true }) }
}

function Assert-SkyLightingContract([object[]]$sceneObjects) {
	# The renderer intentionally accepts a directional sun only from the object
	# that owns the active SkyComponent. Keep the generated fixture aligned with
	# that authored ownership rule so an export cannot silently force its sky black.
	$skyObjects = @($sceneObjects | Where-Object {
		$null -ne $_.components -and $_.components.Contains('SkyComponent')
	})
	if($skyObjects.Count -ne 1) {
		throw "Generated workload scene must contain exactly one SkyComponent owner; found $($skyObjects.Count)."
	}

	$skyComponents = $skyObjects[0].components
	if(-not $skyComponents.Contains('LightComponent')) {
		throw "Generated Sky object '$($skyObjects[0].name)' must own its directional LightComponent."
	}

	$skyLight = $skyComponents.LightComponent
	if($null -eq $skyLight -or -not [bool]$skyLight.active -or [int]$skyLight.type -ne 0) {
		throw "Generated Sky object '$($skyObjects[0].name)' must own one active directional LightComponent."
	}

	if(@($sceneObjects | Where-Object { $_.name -eq 'Navigation Light' }).Count -ne 0) {
		throw 'Generated workload scene must not leave a detached Navigation Light outside the Sky owner.'
	}
}

$objects = [Collections.Generic.List[object]]::new()
$objects.Add([ordered]@{ uuid = (New-Id 10); name = 'Sky'; components = [ordered]@{ Transform = (Transform @(0.0, 0.0, 0.0)); SkyComponent = [ordered]@{ active = $true; exposure = 0.7; groundTint = @(0.06, 0.07, 0.09); horizonTint = @(0.45, 0.58, 0.72); rotationOffsetDegrees = 0.0; sourceAsset = ''; zenithTint = @(0.08, 0.16, 0.36) }; LightComponent = [ordered]@{ active = $true; appearance = 0; authoredPriority = 0.0; castsShadows = $true; color = @(1.0, 1.0, 1.0); height = 1.0; indirectMultiplier = 1.0; innerConeAngle = 0.349066; intensity = 2.0; outerConeAngle = 0.523599; range = 100.0; shadowBias = 0.005; shadowStrength = 1.0; stabilityPreference = 0.0; temperature = 6500.0; type = 0; unit = 1; width = 1.0 } } })
$objects.Add([ordered]@{ uuid = (New-Id 11); name = 'Main Camera'; components = [ordered]@{ Transform = (Transform @(0.0, 1.4, 4.0)); CameraComponent = [ordered]@{ active = $true; aspectRatio = 1.777777; farPlane = 100.0; nearPlane = 0.1; up = @(0.0, 1.0, 0.0); verticalFovDegrees = 60.0 } } })
$objects.Add([ordered]@{ uuid = (New-Id 1); name = 'NAV_TERRAIN'; isStatic = $true; components = [ordered]@{ Transform = (Transform @(-64.0, 0.0, -64.0)); TerrainComponent = [ordered]@{ active = $true; schemaVersion = 2; terrainAssetId = $terrainAssetId; visible = $true; castsShadows = $true; baseColor = @(0.18, 0.55, 0.16); metallic = 0.0; roughness = 0.92 }; RigidBodyComponent = [ordered]@{ motionType = 'Static'; useGravity = $false } } })
if($Scenario -eq 'workload') {
	for($i = 0; $i -lt 64; ++$i) {
		$row = [int][math]::Floor($i / 8); $column = [int]($i % 8)
		$px = [double](-28.0 + (8.0 * $column)); $pz = [double](-28.0 + (8.0 * $row))
		$position = [double[]]@($px, 0.0, $pz)
		$objects.Add([ordered]@{ uuid = (New-Id (1000 + $i)); name = ('NAV_BOX_{0:d2}' -f $i); isStatic = $true; components = [ordered]@{ Transform = (Transform $position); RigidBodyComponent = [ordered]@{ motionType = 'Static'; useGravity = $false }; ColliderSetComponent = (Box-Collider $i) } })
	}
	$objects.Add([ordered]@{ uuid = (New-Id 2000); name = 'NAV_BLOCKER'; isStatic = $true; components = [ordered]@{ Transform = (Transform @(0.0, 0.0, 0.0)); RigidBodyComponent = [ordered]@{ motionType = 'Kinematic'; useGravity = $false }; ColliderSetComponent = (Blocker-Collider) } })
	for($i = 0; $i -lt $Population; ++$i) {
		$position = $endpointPairs[$i].start
		$components = [ordered]@{ Transform = (Transform $position) }
		if($Cohort -eq 'agent') { $components.NavigationAgentComponent = [ordered]@{ active = $true; radiusMetres = 0.5; heightMetres = 2.0; stepHeightMetres = 0.4; maxSlopeDegrees = 45.0; clearanceMarginMetres = 0.1 } }
		$objects.Add([ordered]@{ uuid = (New-Id (3000 + $i)); name = ('NAV_OWNER_{0:d4}' -f $i); components = $components })
	}
}
else {
	# The four-metre opening is deliberately wider than Recast cell/erosion
	# quantization. The authored terrain support spans Z=[-64,64]; these jambs
	# extend two metres beyond that support, so a query cannot evade the roof at
	# a terrain edge. A 1.6m roof is therefore the only reason the 2.1m profile
	# fails while the 1.2m profile can cross from X=-24 to X=24.
	$objects.Add([ordered]@{ uuid = (New-Id 2100); name = 'NAV_CLEARANCE_LOWER_JAMB'; isStatic = $true; components = [ordered]@{ Transform = (Transform @(0.0, 0.875, -34.0)); RigidBodyComponent = [ordered]@{ motionType = 'Static'; useGravity = $false }; ColliderSetComponent = (Clearance-Collider 0 @([double]0.25, [double]0.875, [double]32.0)) } })
	$objects.Add([ordered]@{ uuid = (New-Id 2101); name = 'NAV_CLEARANCE_UPPER_JAMB'; isStatic = $true; components = [ordered]@{ Transform = (Transform @(0.0, 0.875, 34.0)); RigidBodyComponent = [ordered]@{ motionType = 'Static'; useGravity = $false }; ColliderSetComponent = (Clearance-Collider 1 @([double]0.25, [double]0.875, [double]32.0)) } })
	$objects.Add([ordered]@{ uuid = (New-Id 2102); name = 'NAV_CLEARANCE_ROOF'; isStatic = $true; components = [ordered]@{ Transform = (Transform @(0.0, 2.25, 0.0)); RigidBodyComponent = [ordered]@{ motionType = 'Static'; useGravity = $false }; ColliderSetComponent = (Clearance-Collider 2 @([double]0.25, [double]0.65, [double]2.0)) } })
}
$sceneName = if($Scenario -eq 'workload') { 'Navigation Agent Workload ' + $Cohort } else { 'Navigation Clearance Parity' }
$sceneWorkload = if($Scenario -eq 'workload') {
	[ordered]@{ scenario = $Scenario; seed = $seed; cohort = $Cohort; population = $Population; terrain = 'Assets/Navigation/terrain.tenavgeom'; endpointPairs = $endpointPairs }
} else {
	[ordered]@{ scenario = $Scenario; terrain = 'Assets/Navigation/terrain.tenavgeom'; route = [ordered]@{ start = @(-24.0, 0.0, 0.0); destination = @(24.0, 0.0, 0.0); roofBottomMetres = 1.6; openingWidthMetres = 4.0 } }
}
$scene = [ordered]@{ format = 'TerraSceneUnboundedV1'; name = $sceneName; workload = $sceneWorkload; objects = $objects }
# Validate every emitted record, including presentation-only objects. The scene
# loader removes a malformed Transform independently of its other components,
# which would otherwise make terrain/collider failures look like navigation bugs.
foreach($sceneObject in $scene.objects) {
	if($null -eq $sceneObject.components -or -not $sceneObject.components.Contains('Transform')) {
		throw "Generated object '$($sceneObject.name)' has no Transform component."
	}
	Assert-CanonicalTransform $sceneObject.components.Transform ([string]$sceneObject.name)
}
Assert-SkyLightingContract $scene.objects
$jsonOptions = @{ Depth = 20; Compress = $false }
$scene | ConvertTo-Json @jsonOptions | Set-Content -LiteralPath (Join-Path $out 'Assets/Scenes/Startup.tescene') -Encoding utf8
[ordered]@{ name = $sceneName; startupScene = 'Scenes/Startup.tescene'; scripts = @('Assets/Scripts/NavigationWorkloadMarker.cs') } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out 'game.json') -Encoding utf8
[IO.File]::WriteAllText((Join-Path $out 'Assets/Scripts/NavigationWorkloadMarker.cs'), "// Deterministic workload projects keep a managed assembly so export exercises the normal pipeline.`npublic static class NavigationWorkloadMarker { }`n")

# Match the normal editor-created workspace: NativeAOT needs both the managed
# bridge and its analyzer. The analyzer emits InitManagedRuntimeV4/V3, so merely
# compiling an otherwise empty marker assembly is not sufficient for an export.
$referenceRoot = Join-Path $out 'Build/Assembly-CSharp/References'
New-Item -ItemType Directory -Force -Path $referenceRoot | Out-Null
$managedRoot = if([string]::IsNullOrWhiteSpace($InstallRoot)) {
	# This fallback keeps developer invocations usable, while installed acceptance
	# always selects the versioned runtime payload supplied through InstallRoot.
	[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../Editor/Managed'))
} else {
	Join-Path ([IO.Path]::GetFullPath($InstallRoot)) 'Editor/Managed'
}
$bridgeSource = Join-Path $managedRoot 'TerraEngine.CSharp.dll'
$analyzerCandidates = @(
	(Join-Path $managedRoot 'Analyzers/TerraEngine.Export.Generator.dll'),
	(Join-Path $managedRoot 'TerraEngine.Export.Generator/bin/Release/netstandard2.0/TerraEngine.Export.Generator.dll'),
	(Join-Path $managedRoot 'TerraEngine.Export.Generator/bin/Debug/netstandard2.0/TerraEngine.Export.Generator.dll')
)
$analyzerSource = @($analyzerCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1)[0]
if(-not (Test-Path -LiteralPath $bridgeSource -PathType Leaf)) {
	throw "Managed bridge required for NativeAOT export was not found: $bridgeSource"
}
if([string]::IsNullOrWhiteSpace($analyzerSource)) {
	throw "Managed export analyzer required for NativeAOT export was not found beneath: $managedRoot"
}
Copy-Item -LiteralPath $bridgeSource -Destination (Join-Path $referenceRoot 'TerraEngine.CSharp.dll') -Force
Copy-Item -LiteralPath $analyzerSource -Destination (Join-Path $referenceRoot 'TerraEngine.Export.Generator.dll') -Force

[IO.File]::WriteAllText((Join-Path $out 'Assembly-CSharp.csproj'), @"
<Project Sdk="Microsoft.NET.Sdk">
	<PropertyGroup>
		<TargetFramework>net10.0</TargetFramework>
		<ImplicitUsings>enable</ImplicitUsings>
		<Nullable>enable</Nullable>
		<EnableDynamicLoading>true</EnableDynamicLoading>
		<AllowUnsafeBlocks>true</AllowUnsafeBlocks>
		<AppendTargetFrameworkToOutputPath>false</AppendTargetFrameworkToOutputPath>
		<AppendRuntimeIdentifierToOutputPath>false</AppendRuntimeIdentifierToOutputPath>
		<OutputPath>Build/Assembly-CSharp/</OutputPath>
		<BaseOutputPath>Build/Assembly-CSharp/</BaseOutputPath>
		<GenerateAssemblyInfo>false</GenerateAssemblyInfo>
		<CopyLocalLockFileAssemblies>true</CopyLocalLockFileAssemblies>
	</PropertyGroup>

	<ItemGroup>
		<!-- Keep the reference and analyzer colocated with the generated project. -->
		<Reference Include="TerraEngine.CSharp">
			<HintPath>`$(MSBuildProjectDirectory)/Build/Assembly-CSharp/References/TerraEngine.CSharp.dll</HintPath>
			<Private>true</Private>
		</Reference>
		<Analyzer Include="`$(MSBuildProjectDirectory)/Build/Assembly-CSharp/References/TerraEngine.Export.Generator.dll" />
	</ItemGroup>

	<ItemGroup>
		<Compile Remove="Build\**\*.cs" />
		<Compile Remove="Library\**\*.cs" />
		<Compile Remove="Temp\**\*.cs" />
		<Compile Remove="ProjectSettings\**\*.cs" />
		<Compile Remove="UserSettings\**\*.cs" />
		<Compile Remove="Project\Internal\Export\**\*.cs" />
	</ItemGroup>
</Project>
"@)
[ordered]@{ projectDisplayName = $sceneName } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out 'ProjectSettings/ProjectSettings.terra') -Encoding utf8
[ordered]@{ debugEnabled = $false; preset = 4; validationEnabled = $true } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out 'ProjectSettings/RenderSettings.json') -Encoding utf8
Set-Content -LiteralPath (Join-Path $out 'Library/LastScene.txt') -Value 'Scenes/Startup.tescene' -Encoding utf8
[ordered]@{ name = ('Navigation ' + $Scenario + ' ' + $Cohort); id = ('navigation-' + $Scenario + '-' + $Cohort); platform = 'windows'; outputDirectory = ('Builds/Navigation-' + $Scenario + '-' + $Cohort); startupScene = 'Scenes/Startup.tescene'; enableManagedCompilation = $true; includedScenes = @('Scenes/Startup.tescene'); navigationGeometry = 'Assets/Navigation/terrain.tenavgeom'; terrainAsset = 'Assets/Terrain/NavigationTerrain.teterrain'; scenario = $Scenario; cohort = $Cohort; population = $Population } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $out 'BuildProfiles/navigation.json') -Encoding utf8
[ordered]@{ terrain = [ordered]@{ boundsMetres = @(-64.0, 0.0, -64.0, 64.0, 0.0, 64.0); gridResolution = 129; spacingMetres = 1.0; source = 'Assets/Navigation/terrain.tenavgeom'; authoredParent = 'Assets/Terrain/NavigationTerrain.teterrain'; assetId = $terrainAssetId }; scenario = $Scenario; navigationProfile = [ordered]@{ id = 'default-ground-agent'; radiusMetres = 0.5; heightMetres = 2.0; maximumStepMetres = 0.4; maximumSlopeDegrees = 45.0; clearanceMetres = 0.1 }; staticBoxes = $(if($Scenario -eq 'workload') { 64 } else { 3 }); movingBlocker = $(if($Scenario -eq 'workload') { 'NAV_BLOCKER' } else { '' }); cohort = $Cohort; population = $(if($Scenario -eq 'workload') { $Population } else { 0 }); seed = $seed; endpointPairs = $(if($Scenario -eq 'workload') { $endpointPairs } else { @() }) } | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $out 'Assets/Navigation/workload-metadata.json') -Encoding utf8
Write-Host "Generated $Scenario / $Cohort navigation project at $out."
