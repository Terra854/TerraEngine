# VerifyTerrainArchitecture.ps1 - static terrain architecture guardrails.
#
# This check is intentionally conservative: it proves required seams are
# present, but never claims runtime scheduling or publication correctness.
[CmdletBinding()]
param([string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../../')).Path)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$failures = [System.Collections.Generic.List[string]]::new()
function Read-Source([string]$relative) {
	$path = Join-Path $SourceRoot $relative
	if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { $failures.Add("Missing source: $relative"); return '' }
	return Get-Content -Raw -LiteralPath $path
}
function Require-Pattern([string]$relative, [string]$pattern, [string]$message) {
	$content = Read-Source $relative
	if ($content -and $content -notmatch $pattern) { $failures.Add($message) }
}

Require-Pattern 'Engine/Components/TerrainComponent.h' 'Component<TerrainComponent>' 'TerrainComponent must use Component<TerrainComponent>.'
Require-Pattern 'Engine/Components/TerrainComponent.h' 'kComponentName' 'TerrainComponent must expose kComponentName.'
Require-Pattern 'Engine/Components/TerrainComponent.h' 'static void reflect' 'TerrainComponent must provide reflection registration.'
foreach ($system in @(
	'Engine/Terrain/TerrainStreamingSystem',
	'Engine/Terrain/TerrainDerivedSystem',
	'Engine/Physics/Terrain/TerrainCollisionSystem',
	'Editor/TerraEngine-Editor/Terrain/TerrainAuthoringSystem')) {
	Require-Pattern "$system.h" 'final\s*:\s*public\s+SystemsBase' "$system must inherit SystemsBase."
	Require-Pattern "$system.h" 'SchedulerDeclaration' "$system must declare scheduler resources."
	Require-Pattern "$system.cpp" 'DeclareFrameWorkloads' "$system must declare worker/protected workloads."
}
foreach ($source in @('Engine/Terrain/TerrainStreamingSystem.cpp','Engine/Terrain/TerrainDerivedSystem.cpp','Engine/Physics/Terrain/TerrainCollisionSystem.cpp','Editor/TerraEngine-Editor/Terrain/TerrainProceduralJobs.cpp')) {
	Require-Pattern $source 'cancel|Cancel|atomic' "$source must expose cancellation handling."
}
foreach ($root in @('Engine/Terrain','Engine/Physics/Terrain','Editor/TerraEngine-Editor/Terrain')) {
	$matches = Get-ChildItem -LiteralPath (Join-Path $SourceRoot $root) -Recurse -Include '*.cpp','*.h' -File | Select-String -Pattern 'ObjectManager|ComponentStorage'
	if ($matches) { $failures.Add("Worker-facing terrain code references live object/component storage under $root.") }
}
$result = [ordered]@{ schemaVersion = 1; status = if ($failures.Count -eq 0) { 'passed-static-guardrails' } else { 'failed' }; failures = @($failures); runtimeProof = 'not-provided-by-static-check' }
$result | ConvertTo-Json -Depth 8
if ($failures.Count -gt 0) { exit 1 }
