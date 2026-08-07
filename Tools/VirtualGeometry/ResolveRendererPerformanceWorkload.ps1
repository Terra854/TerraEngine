[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)]
	[string]$Workload,
	[string]$Manifest = 'Tests/Fixtures/RendererPerformanceBaseline/workloads.json',
	[switch]$AsJson
)

# Resolve the immutable workload contract without opening or saving a project.
# This command is intentionally fail-closed: a profile marked blocked cannot be
# passed to a capture runner and silently mistaken for measured evidence.
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$manifestPath = if([IO.Path]::IsPathRooted($Manifest)) { $Manifest } else { Join-Path $repo $Manifest }
if(-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "Workload manifest missing: $manifestPath" }
$document = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
$entry = @($document.workloads | Where-Object { $_.name -eq $Workload }) | Select-Object -First 1
if($null -eq $entry) { throw "Unknown renderer performance workload: $Workload" }
$capture = $entry.PSObject.Properties['capture']
if($null -eq $capture) { throw "Workload '$Workload' has no capture profile." }
$profile = $capture.Value
$result = [ordered]@{
	name = [string]$entry.name
	renderables = [int]$entry.renderables
	materials = [int]$entry.materials
	capture = $profile
}
if($AsJson) { $result | ConvertTo-Json -Depth 16; exit 0 }
if($profile.PSObject.Properties['status'] -and [string]$profile.status -eq 'blocked') {
	Write-Error "Workload '$Workload' is blocked: $([string]$profile.blocker)"
	exit 2
}
$prefab = if($profile.PSObject.Properties['prefab']) { [string]$profile.prefab } else { '' }
$mesh = if($profile.PSObject.Properties['meshAssetId']) { [string]$profile.meshAssetId } else { '' }
Write-Output "workload=$($result.name) mode=$([string]$profile.mode) scene=$([string]$profile.scene) prefab=$prefab meshAssetId=$mesh instances=$([int]$profile.prefabInstanceCount) persistAuthoredScene=$([bool]$profile.persistAuthoredScene)"
