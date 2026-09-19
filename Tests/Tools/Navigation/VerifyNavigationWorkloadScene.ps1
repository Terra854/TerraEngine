#requires -Version 7.0
<##
.SYNOPSIS
    Verifies the authored presentation and navigation invariants of a generated
    AgentWorkload startup scene.

.DESCRIPTION
    This is a source-independent regression check for
    NewNavigationAgentWorkloadProject.ps1.  It intentionally validates the
    serialized scene rather than a renderer publication counter: the Sky object
    must own both the active sky and its directional light, while the camera,
    terrain material, and navigation participants remain intact.

.EXAMPLE
    pwsh -File Tests/Tools/Navigation/VerifyNavigationWorkloadScene.ps1 `
        -ProjectRoot out/NavigationWorkload-agent
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$ProjectRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-Scene([bool]$Condition, [string]$Message)
{
	if(-not $Condition) { throw "Navigation workload scene invariant failed: $Message" }
}

function Assert-Near([double]$Actual, [double]$Expected, [string]$Label)
{
	Assert-Scene ([Math]::Abs($Actual - $Expected) -le 0.00001) "$Label expected $Expected, got $Actual"
}

function Assert-Vector([object]$Actual, [double[]]$Expected, [string]$Label)
{
	Assert-Scene ($null -ne $Actual -and @($Actual).Count -eq $Expected.Count) "$Label has the wrong component count"
	for($index = 0; $index -lt $Expected.Count; ++$index)
	{
		Assert-Near ([double]$Actual[$index]) $Expected[$index] "$Label[$index]"
	}
}

function Get-Component([object]$Object, [string]$Name)
{
	if($null -eq $Object) { return $null }
	$componentsProperty = $Object.PSObject.Properties['components']
	if($null -eq $componentsProperty -or $null -eq $componentsProperty.Value) { return $null }
	$property = $componentsProperty.Value.PSObject.Properties[$Name]
	if($null -eq $property) { return $null }
	return $property.Value
}

$root = [IO.Path]::GetFullPath($ProjectRoot)
$scenePath = Join-Path $root 'Assets/Scenes/Startup.tescene'
$metadataPath = Join-Path $root 'Assets/Navigation/workload-metadata.json'
Assert-Scene (Test-Path -LiteralPath $scenePath -PathType Leaf) "missing Assets/Scenes/Startup.tescene"
Assert-Scene (Test-Path -LiteralPath $metadataPath -PathType Leaf) "missing Assets/Navigation/workload-metadata.json"

$scene = Get-Content -LiteralPath $scenePath -Raw | ConvertFrom-Json
$metadata = Get-Content -LiteralPath $metadataPath -Raw | ConvertFrom-Json
Assert-Scene ([string]$metadata.cohort -in @('direct', 'agent')) "metadata cohort is invalid: $($metadata.cohort)"
Assert-Scene ([int]$metadata.population -ge 0) "metadata population is invalid: $($metadata.population)"
$objects = @($scene.objects)
Assert-Scene ($objects.Count -gt 0) 'startup scene contains no objects'

$skyObjects = @($objects | Where-Object { $_.name -eq 'Sky' })
Assert-Scene ($skyObjects.Count -eq 1) "expected exactly one Sky object, found $($skyObjects.Count)"
$sky = $skyObjects[0]
$skyComponent = Get-Component $sky 'SkyComponent'
$skyLight = Get-Component $sky 'LightComponent'
Assert-Scene ($null -ne $skyComponent -and $skyComponent.active -eq $true) 'Sky must own an active SkyComponent'
Assert-Scene ($null -ne $skyLight -and $skyLight.active -eq $true) 'Sky must own an active LightComponent'
Assert-Scene ([int]$skyLight.type -eq 0) 'Sky light must be directional (type 0)'

$lightOwners = @($objects | Where-Object { $null -ne (Get-Component $_ 'LightComponent') })
Assert-Scene ($lightOwners.Count -eq 1 -and $lightOwners[0].name -eq 'Sky') 'the generated scene must not split its directional light onto another object'

$cameraObjects = @($objects | Where-Object { $_.name -eq 'Main Camera' })
Assert-Scene ($cameraObjects.Count -eq 1) "expected exactly one Main Camera, found $($cameraObjects.Count)"
$camera = $cameraObjects[0]
$cameraComponent = Get-Component $camera 'CameraComponent'
Assert-Scene ($null -ne $cameraComponent -and $cameraComponent.active -eq $true) 'Main Camera must be active'
Assert-Vector $camera.components.Transform.ParentRelativePose.translationMetres @(0.0, 1.4, 4.0) 'Main Camera position'

$terrainObjects = @($objects | Where-Object { $_.name -eq 'NAV_TERRAIN' })
Assert-Scene ($terrainObjects.Count -eq 1) "expected exactly one NAV_TERRAIN object, found $($terrainObjects.Count)"
$terrain = $terrainObjects[0]
$terrainComponent = Get-Component $terrain 'TerrainComponent'
Assert-Scene ($null -ne $terrainComponent -and $terrainComponent.active -eq $true -and $terrainComponent.visible -eq $true) 'NAV_TERRAIN must remain active and visible'
Assert-Vector $terrainComponent.baseColor @(0.18, 0.55, 0.16) 'NAV_TERRAIN authored baseColor'
Assert-Vector $terrain.components.Transform.ParentRelativePose.translationMetres @(-64.0, 0.0, -64.0) 'NAV_TERRAIN position'

$ownerObjects = @($objects | Where-Object { [string]$_.name -like 'NAV_OWNER_*' })
Assert-Scene ($ownerObjects.Count -eq [int]$metadata.population) "NAV_OWNER count $($ownerObjects.Count) does not match metadata population $($metadata.population)"
$agentOwnerObjects = @($ownerObjects | Where-Object { $null -ne (Get-Component $_ 'NavigationAgentComponent') })
if([string]$metadata.cohort -eq 'agent')
{
	Assert-Scene ($agentOwnerObjects.Count -eq $ownerObjects.Count) 'every authored NAV_OWNER must have an agent in the agent cohort'
}
else
{
	Assert-Scene ($agentOwnerObjects.Count -eq 0) 'direct cohort must not retain authored NavigationAgentComponent owners'
}

Write-Output ([ordered]@{
	status = 'passed'
	scene = $scenePath
	skyOwnsDirectionalLight = $true
	cameraPosition = @($camera.components.Transform.ParentRelativePose.translationMetres)
	terrainBaseColor = @($terrainComponent.baseColor)
	ownerCount = $ownerObjects.Count
	agentCount = $agentOwnerObjects.Count
} | ConvertTo-Json -Depth 6 -Compress)
