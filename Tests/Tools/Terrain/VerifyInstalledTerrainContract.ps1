# VerifyInstalledTerrainContract.ps1 - fail-closed installed terrain audit.
[CmdletBinding()]
param(
	[string]$InstallRoot = 'out/build/RelWithDebInfo/Build',
	[string]$OutputDirectory = '',
	[switch]$DryRun
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$install = [IO.Path]::GetFullPath($InstallRoot)
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory = Join-Path $install 'Tests/Tools/Terrain' }
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $output | Out-Null
$missing = [System.Collections.Generic.List[string]]::new()
$required = @(
	'Editor/TerraEngine-Editor.exe', 'Editor/TerraEngineTerrainCompiler.exe',
	'Tests/Tools/Terrain/TerraEngineTerrainFixtureGenerator.exe', 'Tests/Tools/Terrain/TerraEngineTerrainBenchmark.exe',
	'Tests/Tools/Terrain/RunTerrainValidation.ps1', 'Tests/Tools/Terrain/RunTerrainSoak.ps1',
	'Tests/Tools/Terrain/hardware-matrix.json', 'Tests/Tools/Terrain/quality-profiles.json',
	'Tests/Tools/Terrain/workloads.json', 'Tests/Fixtures/Terrain/manifest.json',
	'Tests/Fixtures/Terrain/SaveRecovery/manifest.json', 'Schemas/Terrain/terrain-edit-v1.schema.json',
	'Engine/Shaders/Terrain/TerrainVisibility.task.hlsl', 'Engine/Shaders/Terrain/TerrainVisibility.mesh.hlsl',
	'Engine/Shaders/Terrain/TerrainVisibility.frag.hlsl',
	'Engine/Shaders/Terrain/TerrainSurface.comp.hlsl', 'Editor/Shaders/Terrain/TerrainBrushPreview.vert.hlsl',
	'Editor/Shaders/Terrain/TerrainBrushPreview.frag.hlsl'
)
foreach ($relative in $required) {
	$path = Join-Path $install $relative
	if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { $missing.Add($relative) }
}
$tepak = @(Get-ChildItem -LiteralPath $install -Recurse -File -Filter '*.tepak' -ErrorAction SilentlyContinue)
$contentMountCandidates = @((Join-Path $install 'Engine/ContentMount'), (Join-Path $install 'Content'))
$contentMountPresent = @($contentMountCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Container }).Count -gt 0
$result = [ordered]@{
	schemaVersion = [ordered]@{ major = 1; minor = 0 }
	status = if ($missing.Count -eq 0) { 'dry-run' } else { 'unavailable' }
	installRoot = $install
	requiredArtifacts = $required
	missingArtifacts = @($missing)
	shaderAudit = [ordered]@{ authoredSource = 'checked'; compiledPackages = 'checked-by-install-rule'; status = if ($missing.Count -eq 0) { 'available' } else { 'unavailable' } }
	contentMountAudit = [ordered]@{ discovered = $contentMountPresent; status = if ($contentMountPresent) { 'available' } else { 'unavailable' } }
	tepakAudit = [ordered]@{ discoveredCount = $tepak.Count; status = if ($tepak.Count -gt 0) { 'available' } else { 'unavailable' } }
	artifactHashes = @{}
	note = 'No performance or backend capability is claimed by this artifact audit.'
}
if ($missing.Count -eq 0) {
	foreach ($relative in $required) { $result.artifactHashes[$relative] = (Get-FileHash (Join-Path $install $relative) -Algorithm SHA256).Hash.ToLowerInvariant() }
}
$resultPath = Join-Path $output 'terrain-installed-contract.json'
$result | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $resultPath -Encoding utf8
$result | ConvertTo-Json -Depth 12
if ($missing.Count -gt 0) { exit 1 }
