# VerifyInstalledTerrainContract.ps1 - fail-closed installed terrain audit.
[CmdletBinding()]
param(
	[string]$InstallRoot = 'out/build/RelWithDebInfo/Build',
	[string]$OutputDirectory = '',
	[string]$SourceRoot = '',
	[switch]$DryRun
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$install = [IO.Path]::GetFullPath($InstallRoot)
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $OutputDirectory = Join-Path $install 'Tests/Tools/Terrain' }
$output = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $output | Out-Null
$missing = [System.Collections.Generic.List[string]]::new()
$violations = [System.Collections.Generic.List[string]]::new()
$requiredFoundation = @(
	'Editor/TerraEngine-Editor.exe', 'Editor/TerraEngineTerrainCompiler.exe',
	'Tests/Tools/Terrain/TerraEngineTerrainFixtureGenerator.exe', 'Tests/Tools/Terrain/TerraEngineTerrainBenchmark.exe',
	'Tests/Tools/Terrain/RunTerrainValidation.ps1', 'Tests/Tools/Terrain/RunTerrainSoak.ps1',
	'Tests/Tools/Terrain/VerifyTerrainArchitecture.ps1', 'Tests/Tools/Terrain/VerifyInstalledTerrainContract.ps1',
	'Tests/Tools/Terrain/hardware-matrix.json', 'Tests/Tools/Terrain/quality-profiles.json',
	'Tests/Tools/Terrain/workloads.json', 'Tests/Fixtures/Terrain/manifest.json',
	'Tests/Fixtures/Terrain/SaveRecovery/manifest.json', 'Schemas/Terrain/terrain-edit-v1.schema.json'
)
$requiredShaderSources = @(
	'Engine/Shaders/Terrain/TerrainMaterial.hlsli',
	'Engine/Shaders/Terrain/TerrainVisibility.task.hlsl', 'Engine/Shaders/Terrain/TerrainVisibility.mesh.hlsl',
	'Engine/Shaders/Terrain/TerrainVisibility.frag.hlsl',
	'Engine/Shaders/Terrain/TerrainSurface.comp.hlsl', 'Editor/Shaders/Terrain/TerrainBrushPreview.vert.hlsl',
	'Editor/Shaders/Terrain/TerrainBrushPreview.frag.hlsl'
)
$requiredShaderPackages = @(
	'Engine/Shaders/Terrain/TerrainVisibility.task.tspf',
	'Engine/Shaders/Terrain/TerrainVisibility.mesh.tspf',
	'Engine/Shaders/Terrain/TerrainVisibility.frag.tspf',
	'Engine/Shaders/Terrain/TerrainSurface.comp.tspf',
	'Editor/Shaders/Terrain/TerrainVisibility.task.tspf',
	'Editor/Shaders/Terrain/TerrainVisibility.mesh.tspf',
	'Editor/Shaders/Terrain/TerrainVisibility.frag.tspf',
	'Editor/Shaders/Terrain/TerrainSurface.comp.tspf',
	'Editor/Shaders/Terrain/TerrainBrushPreview.vert.tspf',
	'Editor/Shaders/Terrain/TerrainBrushPreview.frag.tspf'
)
$requiredPageFormatArtifacts = @(
	'include/TerraEngine/Renderer/Terrain/TerrainPageFormat.h'
)
$required = @($requiredFoundation + $requiredShaderSources + $requiredShaderPackages + $requiredPageFormatArtifacts)
foreach ($relative in $required) {
	$path = Join-Path $install $relative
	if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
		$missing.Add($relative)
		continue
	}
	$item = Get-Item -LiteralPath $path
	if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
		$violations.Add("Installed artifact is a reparse-point/source fallback instead of a self-contained file: $relative")
	}
	if ($item.Length -eq 0) { $violations.Add("Installed artifact is empty: $relative") }
}

# Incremental installs must not retain the retired vertex-only terrain route.
$forbiddenArtifacts = @(
	'Engine/Shaders/Terrain/TerrainVisibility.vert.hlsl',
	'Engine/Shaders/Terrain/TerrainVisibility.vert.tspf',
	'Editor/Shaders/Terrain/TerrainVisibility.vert.hlsl',
	'Editor/Shaders/Terrain/TerrainVisibility.vert.tspf'
)
$presentForbidden = @()
foreach ($relative in $forbiddenArtifacts) {
	if (Test-Path -LiteralPath (Join-Path $install $relative) -PathType Leaf) {
		$presentForbidden += $relative
		$violations.Add("Retired terrain shader artifact is still installed: $relative. Reinstall after pruning the old terrain shader tree.")
	}
}

# Validate semantic contract markers, not just filenames. This catches stale
# placeholder files copied from an earlier installation.
$pageFormatRelative = $requiredPageFormatArtifacts[0]
$pageFormatPath = Join-Path $install $pageFormatRelative
if (Test-Path -LiteralPath $pageFormatPath -PathType Leaf) {
	$pageFormat = Get-Content -Raw -LiteralPath $pageFormatPath
	foreach ($marker in @('kMagic', 'kMaximumDecodedPageBytes', 'kStitchVariantCount', 'ValidateTerrainPageHeader')) {
		if ($pageFormat -notmatch [regex]::Escape($marker)) {
			$violations.Add("Installed terrain page ABI is missing required marker '$marker': $pageFormatRelative")
		}
	}
}

$manifestRelative = 'Tests/Fixtures/Terrain/manifest.json'
$manifestPath = Join-Path $install $manifestRelative
if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
	try {
		$manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
		$rootFixture = @($manifest.fixtures | Where-Object { $_.id -eq 'terrain-terraroot1-contract' })
		$lodFixture = @($manifest.fixtures | Where-Object { $_.id -eq 'terrain-multi-area-lod' })
		if ($rootFixture.Count -ne 1 -or $rootFixture[0].acceptedMagic -ne 'TERRAROOT1') {
			$violations.Add("Installed terrain manifest must contain exactly one canonical TERRAROOT1 contract fixture.")
		}
		if ($lodFixture.Count -ne 1 -or @($lodFixture[0].lodCellExtents).Count -ne 4) {
			$violations.Add("Installed terrain manifest must contain the multi-area 256/128/64/32 page-format fixture.")
		}
	} catch {
		$violations.Add("Installed terrain manifest is not valid JSON: $($_.Exception.Message)")
	}
}

# Text artifacts may use install-relative paths and $PSScriptRoot, but cannot
# retain an absolute checkout/build path. When invoked from the source script,
# infer the repository root so its exact path is also rejected deterministically.
if ([string]::IsNullOrWhiteSpace($SourceRoot)) {
	$sourceCandidate = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../'))
	if (Test-Path -LiteralPath (Join-Path $sourceCandidate 'CMakeLists.txt') -PathType Leaf) {
		$SourceRoot = $sourceCandidate
	}
}
$sourceFallbackArtifacts = [System.Collections.Generic.List[string]]::new()
$absoluteCheckoutPattern = '(?im)(?:[A-Za-z]:[\\/]|/(?:home|Users|workspace|workspaces|mnt)/)[^''"\r\n]*(?:Engine|Editor|Tests)[\\/]'
foreach ($relative in $required) {
	$extension = [IO.Path]::GetExtension($relative).ToLowerInvariant()
	if ($extension -notin @('.ps1', '.json', '.md', '.hlsl', '.hlsli', '.h')) { continue }
	# The two audit scripts intentionally contain the forbidden-token patterns
	# they enforce. Scanning their own rule text would report a false runtime
	# source fallback even though neither script is a content-loading path.
	if ($relative -in @(
		'Tests/Tools/Terrain/VerifyTerrainArchitecture.ps1',
		'Tests/Tools/Terrain/VerifyInstalledTerrainContract.ps1')) { continue }
	$path = Join-Path $install $relative
	if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
	$content = Get-Content -Raw -LiteralPath $path
	$hasExactSourceRoot = -not [string]::IsNullOrWhiteSpace($SourceRoot) -and $content -match [regex]::Escape([IO.Path]::GetFullPath($SourceRoot))
	if ($hasExactSourceRoot -or $content -match 'CMAKE_SOURCE_DIR|PROJECT_SOURCE_DIR' -or $content -match $absoluteCheckoutPattern) {
		$sourceFallbackArtifacts.Add($relative)
		$violations.Add("Installed text artifact retains a source/build-tree runtime path: $relative")
	}
}

$tepak = @(Get-ChildItem -LiteralPath $install -Recurse -File -Filter '*.tepak' -ErrorAction SilentlyContinue)
$contentMountCandidates = @((Join-Path $install 'Engine/ContentMount'), (Join-Path $install 'Content'))
$contentMountPresent = @($contentMountCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Container }).Count -gt 0
$hasFailures = $missing.Count -gt 0 -or $violations.Count -gt 0
$missingShaderArtifacts = @($requiredShaderSources + $requiredShaderPackages | Where-Object { $missing -contains $_ })
$shaderContractViolations = @($violations | Where-Object { $_ -match 'shader|Engine/Shaders/Terrain|Editor/Shaders/Terrain' })
$missingPageFormatArtifacts = @($requiredPageFormatArtifacts | Where-Object { $missing -contains $_ })
$pageFormatContractViolations = @($violations | Where-Object { $_ -match 'page ABI|TerrainPageFormat' })
$result = [ordered]@{
	schemaVersion = [ordered]@{ major = 1; minor = 0 }
	status = if ($hasFailures) { 'unavailable' } elseif ($DryRun) { 'dry-run' } else { 'available' }
	installRoot = $install
	requiredArtifacts = $required
	missingArtifacts = @($missing)
	contractViolations = @($violations)
	shaderAudit = [ordered]@{
		authoredSources = $requiredShaderSources
		compiledPackages = $requiredShaderPackages
		retiredArtifactsPresent = $presentForbidden
		status = if ($missingShaderArtifacts.Count -eq 0 -and $shaderContractViolations.Count -eq 0) { 'available' } else { 'unavailable' }
	}
	pageFormatAudit = [ordered]@{
		artifacts = $requiredPageFormatArtifacts
		status = if ($missingPageFormatArtifacts.Count -eq 0 -and $pageFormatContractViolations.Count -eq 0) { 'available' } else { 'unavailable' }
	}
	sourceFallbackAudit = [ordered]@{
		artifacts = @($sourceFallbackArtifacts)
		status = if ($sourceFallbackArtifacts.Count -eq 0) { 'clear' } else { 'failed' }
	}
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
if ($hasFailures) { exit 1 }
