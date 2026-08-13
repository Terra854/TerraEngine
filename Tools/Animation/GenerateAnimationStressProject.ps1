# GenerateAnimationStressProject.ps1 - Materializes the reviewed animation stress descriptors.
#
# Use -ValidateOnly during intermediate phases. Generation creates a fresh project
# below out/ but never launches the editor, a runtime, or a timed workload.

[CmdletBinding()]
param(
	[ValidateNotNullOrEmpty()]
	[string]$ManifestPath = "Tests/Fixtures/Animation/stress/workloads.json",

	[ValidateNotNullOrEmpty()]
	[string]$OutputRoot = "out/generated/animation-stress",

	[string[]]$WorkloadId = @(),

	[switch]$ValidateOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../.."))
$script:OutRoot = [System.IO.Path]::GetFullPath((Join-Path $script:RepoRoot "out"))
$script:BaselineRoot = [System.IO.Path]::GetFullPath((Join-Path $script:RepoRoot "out/validation/animation/baseline"))
$script:Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$script:ExpectedManifestSha256 = "6773caf89e87b2fc5f1277ca5bde92aa68f4f20581a223af1910cb62a95c8aff"
$script:ExpectedRuntimeContractSha256 = "e9e231c9c9b9e6f22868cec47146436e9a8aac90802e9d1d26b6cfeee88f98c2"
$script:ExpectedPerformanceWorkloadsSha256 = "a50c359f5dda932c0852eaa9a83c2e57cbbca3c60f038e975f7b67e0b17774f6"

function Resolve-RepoPath {
	param([Parameter(Mandatory = $true)][string]$Path)
	if ([System.IO.Path]::IsPathRooted($Path)) {
		return [System.IO.Path]::GetFullPath($Path)
	}
	return [System.IO.Path]::GetFullPath((Join-Path $script:RepoRoot $Path))
}

function Test-ChildPath {
	param(
		[Parameter(Mandatory = $true)][string]$Parent,
		[Parameter(Mandatory = $true)][string]$Child
	)
	$parentPrefix = [System.IO.Path]::GetFullPath($Parent).TrimEnd("\", "/") + [System.IO.Path]::DirectorySeparatorChar
	return [System.IO.Path]::GetFullPath($Child).StartsWith($parentPrefix, [System.StringComparison]::OrdinalIgnoreCase)
}

function Get-Sha256 {
	param([Parameter(Mandatory = $true)][string]$Path)
	if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
		throw "Required source-controlled file is missing: '$Path'."
	}
	return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function ConvertTo-CanonicalValue {
	param([AllowNull()]$Value)
	if ($null -eq $Value) { return $null }
	if ($Value -is [string] -or $Value -is [char] -or $Value -is [bool] -or $Value -is [ValueType]) {
		return $Value
	}
	if ($Value -is [System.Collections.IDictionary]) {
		$ordered = [ordered]@{}
		foreach ($key in @($Value.Keys | ForEach-Object { [string]$_ } | Sort-Object)) {
			$ordered[$key] = ConvertTo-CanonicalValue $Value[$key]
		}
		return $ordered
	}
	if ($Value -is [System.Collections.IEnumerable]) {
		$items = @()
		foreach ($item in $Value) { $items += ,(ConvertTo-CanonicalValue $item) }
		return ,$items
	}
	$object = [ordered]@{}
	foreach ($property in @($Value.PSObject.Properties.Name | Sort-Object)) {
		$object[$property] = ConvertTo-CanonicalValue $Value.PSObject.Properties[$property].Value
	}
	return $object
}

function Get-CanonicalSha256 {
	param([Parameter(Mandatory = $true)]$Value)
	$text = ConvertTo-CanonicalValue $Value | ConvertTo-Json -Compress -Depth 100
	$algorithm = [System.Security.Cryptography.SHA256]::Create()
	try {
		return ([System.BitConverter]::ToString($algorithm.ComputeHash($script:Utf8NoBom.GetBytes($text)))).Replace("-", "").ToLowerInvariant()
	}
	finally {
		$algorithm.Dispose()
	}
}

function Read-Json {
	param([Parameter(Mandatory = $true)][string]$Path)
	try {
		return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
	}
	catch {
		throw "JSON could not be read from '$Path': $($_.Exception.Message)"
	}
}

function Write-Json {
	param(
		[Parameter(Mandatory = $true)][string]$Path,
		[Parameter(Mandatory = $true)]$Value
	)
	$parent = Split-Path -Parent $Path
	[System.IO.Directory]::CreateDirectory($parent) | Out-Null
	$text = ($Value | ConvertTo-Json -Depth 100).Replace("`r`n", "`n") + "`n"
	[System.IO.File]::WriteAllText($Path, $text, $script:Utf8NoBom)
}

function Assert-FrozenManifest {
	param(
		[Parameter(Mandatory = $true)]$Manifest,
		[Parameter(Mandatory = $true)][string]$ResolvedManifestPath
	)
	if ((Get-Sha256 $ResolvedManifestPath) -cne $script:ExpectedManifestSha256) {
		throw "The stress workload manifest bytes differ from the reviewed animation stress identity."
	}
	if ($Manifest.schemaVersion -ne 1 -or $Manifest.feature -ne "animation" -or
		$Manifest.task -ne "animation-stress-workloads" -or $Manifest.performanceAcceptanceMode -ne "release" -or
		-not $Manifest.performanceExecutionDeferred) {
		throw "The stress manifest identity or final-phase execution guard is invalid."
	}
	if ([string]$Manifest.authority.runtimeContract.sha256 -cne $script:ExpectedRuntimeContractSha256 -or
		[string]$Manifest.authority.performanceWorkloads.sha256 -cne $script:ExpectedPerformanceWorkloadsSha256) {
		throw "The stress manifest authority hashes are not the frozen contract values."
	}
	foreach ($authority in @($Manifest.authority.runtimeContract, $Manifest.authority.performanceWorkloads)) {
		$authorityPath = Resolve-RepoPath ([string]$authority.path)
		if ((Get-Sha256 $authorityPath) -cne [string]$authority.sha256) {
			throw "Authority bytes do not match '$($authority.path)'."
		}
	}

	$requiredIds = @("crowd-1000", "complex-hero", "mode-churn-10000", "randomized-scheduler", "device-loss-recovery", "memory-pressure")
	$actualIds = @($Manifest.workloads | ForEach-Object { [string]$_.id })
	if ($actualIds.Count -ne $requiredIds.Count) {
		throw "The stress manifest must contain exactly six reviewed workload definitions."
	}
	foreach ($requiredId in $requiredIds) {
		$matches = @($Manifest.workloads | Where-Object { $_.id -ceq $requiredId })
		if ($matches.Count -ne 1) { throw "Required workload '$requiredId' is missing or duplicated." }
		$actualHash = Get-CanonicalSha256 $matches[0].definition
		if ($actualHash -cne [string]$matches[0].definitionSha256) {
			throw "Workload '$requiredId' does not match its canonical definition hash."
		}
	}

	$crowd = @($Manifest.workloads | Where-Object { $_.id -ceq "crowd-1000" })[0].definition
	$hero = @($Manifest.workloads | Where-Object { $_.id -ceq "complex-hero" })[0].definition
	$churn = @($Manifest.workloads | Where-Object { $_.id -ceq "mode-churn-10000" })[0].definition
	if ($crowd.activeCharacters -ne 1000 -or $crowd.jointsPerCharacter -ne 100 -or
		$crowd.blendedClipsPerCharacter -ne 2 -or $crowd.morphTargetsPerCharacter -ne 4 -or
		$crowd.shadowCastingViews -ne 1 -or $crowd.measuredSecondsAfterWarmup -ne 60 -or
		$hero.sourceJoints -ne 256 -or $hero.activeMorphTargets -ne 2 -or $hero.ikChains -ne 2 -or
		$hero.gameCaptureViews -ne 2 -or -not $hero.motionMatching -or
		-not $hero.deformableVirtualGeometry -or -not $hero.partialRagdoll -or
		$churn.timelineChanges -ne 10000) {
		throw "A frozen crowd, hero, or mode-churn content value was weakened."
	}
	return $requiredIds
}

$resolvedManifest = Resolve-RepoPath $ManifestPath
$manifest = Read-Json $resolvedManifest
$requiredWorkloadIds = @(Assert-FrozenManifest $manifest $resolvedManifest)
$selectedIds = if ($WorkloadId.Count -eq 0) { $requiredWorkloadIds } else { @($WorkloadId) }
foreach ($selectedId in $selectedIds) {
	if ($requiredWorkloadIds -cnotcontains $selectedId) {
		throw "Unknown workload '$selectedId'. Allowed values: $($requiredWorkloadIds -join ', ')."
	}
}

if ($ValidateOnly) {
	[ordered]@{
		schemaVersion = 1
		tool = "GenerateAnimationStressProject"
		mode = "readiness-only"
		passed = $true
		performanceExecuted = $false
		manifestSha256 = Get-Sha256 $resolvedManifest
		workloads = $selectedIds
	} | ConvertTo-Json -Compress -Depth 20 | Write-Output
	return
}

$resolvedOutput = Resolve-RepoPath $OutputRoot
if (-not (Test-ChildPath $script:OutRoot $resolvedOutput) -or
	(Test-ChildPath $script:BaselineRoot $resolvedOutput)) {
	throw "OutputRoot must be a fresh directory below out/ and outside the immutable baseline tree."
}
if (Test-Path -LiteralPath $resolvedOutput) {
	throw "OutputRoot already exists; generation never overwrites evidence: '$resolvedOutput'."
}

$selected = @($manifest.workloads | Where-Object { $selectedIds -ccontains $_.id })
$project = [ordered]@{
	name = "Animation Stress Project"
	version = "1.0.0"
	startupScene = "Assets/Scenes/AnimationStress.tescene"
	stressManifestSha256 = Get-Sha256 $resolvedManifest
}
$settings = [ordered]@{
	schemaVersion = 1
	fixedTickRateHz = $manifest.generation.fixedTickRateHz
	workerCount = $manifest.generation.workerCount
	randomAlgorithm = $manifest.generation.randomAlgorithm
	performanceExecutionDeferred = $true
	workloads = @($selected | ForEach-Object { [ordered]@{ id = $_.id; definitionSha256 = $_.definitionSha256 } })
}
$scene = [ordered]@{
	format = "TerraEngineAnimationStressScene"
	schemaVersion = 1
	generationOnly = $true
	workloads = @($selected | ForEach-Object { [ordered]@{ id = $_.id; definition = $_.definition } })
}

Write-Json (Join-Path $resolvedOutput "project.json") $project
Write-Json (Join-Path $resolvedOutput "ProjectSettings/AnimationStress.json") $settings
Write-Json (Join-Path $resolvedOutput "Assets/Scenes/AnimationStress.tescene") $scene
$result = [ordered]@{
	schemaVersion = 1
	tool = "GenerateAnimationStressProject"
	passed = $true
	performanceExecuted = $false
	generatedAtUtc = [DateTime]::UtcNow.ToString("o")
	manifest = [ordered]@{ path = $ManifestPath; sha256 = Get-Sha256 $resolvedManifest }
	outputRoot = $resolvedOutput.Replace("\", "/")
	workloads = $selectedIds
}
Write-Json (Join-Path $resolvedOutput "generation-result.json") $result
$result | ConvertTo-Json -Compress -Depth 20 | Write-Output
