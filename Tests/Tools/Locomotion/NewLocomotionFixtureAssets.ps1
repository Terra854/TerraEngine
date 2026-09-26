#requires -Version 7.0
<#
.SYNOPSIS
  Materializes the frozen locomotion fixture through production animation codecs.
.EXAMPLE
  ./NewLocomotionFixtureAssets.ps1 -OutputDirectory out/locomotion-assets `
    -ManifestPath Tests/Fixtures/Locomotion/workloads.json `
    -MaterializerPath out/build/RelWithDebInfo/Build/Tests/Tools/Locomotion/TerraEngine-LocomotionFixtureMaterializer.exe

The destination must be new or empty. Run against the final workloads.json to
record its exact SHA-256; this tool emits assets and provenance, not acceptance.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$OutputDirectory,
	[Parameter(Mandatory = $true)][string]$ManifestPath,
	[Parameter(Mandatory = $true)][string]$MaterializerPath,
	[string]$InstallRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$output = [IO.Path]::GetFullPath($OutputDirectory)
$manifestFile = [IO.Path]::GetFullPath($ManifestPath)
$tool = [IO.Path]::GetFullPath($MaterializerPath)
$install = if([string]::IsNullOrWhiteSpace($InstallRoot)) {
	[IO.Path]::GetFullPath((Join-Path (Split-Path -Parent $tool) '../../..'))
} else { [IO.Path]::GetFullPath($InstallRoot) }
if(-not (Test-Path -LiteralPath $manifestFile -PathType Leaf)) { throw "Manifest not found: $manifestFile" }
if(-not (Test-Path -LiteralPath $tool -PathType Leaf)) { throw "Production materializer not found: $tool" }
if(Test-Path -LiteralPath $output) {
	if(-not (Test-Path -LiteralPath $output -PathType Container) -or @(Get-ChildItem -LiteralPath $output -Force).Count -ne 0) {
		throw "Output must be a new or empty directory: $output"
	}
} else { New-Item -ItemType Directory -Path $output | Out-Null }
$source = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
if($source.fixtureId -ne 'locomotion-060-v1' -or $source.generator.seed -ne 60060 -or
	$source.lanes.stress.population -ne 1000 -or $source.lanes.stress.motionModes.Velocity -ne 700 -or
	$source.lanes.stress.motionModes.RootMotion -ne 300) {
	throw 'Workloads manifest differs from the frozen locomotion-060-v1 fixture.'
}
$assetDirectory = Join-Path $output 'Assets/Animation'
New-Item -ItemType Directory -Path $assetDirectory -Force | Out-Null
# A validation tool may be installed below Tests/Tools rather than beside the
# runtime DLL. Scope the loader path to this child invocation, then restore it.
$oldPath = $env:PATH
$oldLdPath = $env:LD_LIBRARY_PATH
try {
	$loaderDirectories = @($install, (Join-Path $install 'Engine'), (Join-Path $install 'Editor'))
	$env:PATH = (($loaderDirectories + @($oldPath)) -join [IO.Path]::PathSeparator)
	$env:LD_LIBRARY_PATH = (($loaderDirectories + @($oldLdPath)) -join [IO.Path]::PathSeparator)
	& $tool $assetDirectory
	if($LASTEXITCODE -ne 0) { throw "Production materializer failed with exit code $LASTEXITCODE" }
} finally {
	$env:PATH = $oldPath
	$env:LD_LIBRARY_PATH = $oldLdPath
}

# These identities are frozen by the source-independent fixture contract. The
# C++ tool writes the bytes; this script only verifies names and hashes them.
$expected = [ordered]@{
	skeleton = @{ file = 'skeleton.teskel'; id = '54455252412d4c4f434f3036303ca55a' }
	idleClip = @{ file = 'idle.teanim'; id = '54455252412d4c4f434f30363050a55a' }
	moveClip = @{ file = 'move.teanim'; id = '54455252412d4c4f434f30363064a55a' }
	rootClip = @{ file = 'root.teanim'; id = '54455252412d4c4f434f30363078a55a' }
	controller = @{ file = 'locomotion.teanimctrl'; id = '54455252412d4c4f434f3036308ca55a' }
	ragdollProfile = @{ file = 'locomotion.teragdoll'; id = '54455252412d4c4f434f303630a0a55a' }
}
$assets = [Collections.Generic.List[object]]::new()
foreach($role in $expected.Keys) {
	$entry = $expected[$role]
	$asset = Join-Path $assetDirectory $entry.file
	if(-not (Test-Path -LiteralPath $asset -PathType Leaf)) { throw "Missing cooked $role asset: $asset" }
	$assets.Add([ordered]@{
		role = $role
		path = 'Assets/Animation/' + $entry.file
		assetId = $entry.id
		sha256 = (Get-FileHash -LiteralPath $asset -Algorithm SHA256).Hash.ToLowerInvariant()
	})
}
if(@(Get-ChildItem -LiteralPath $assetDirectory -File).Count -ne $expected.Count) {
	throw 'The materializer emitted unexpected extra files; review the frozen asset list.'
}
$rootFixture = @($source.generator.contentProvenance.animationFixtures | Where-Object { $_.id -eq 'locomotion-animation-fixture-in-memory-assets' })
if($rootFixture.Count -ne 1 -or $null -eq $rootFixture[0].rootTimelineExpectation) {
	throw 'Frozen root timeline expectations are missing from workloads.json.'
}
# An expectation snapshot is not an observed run result. Keep the two separate
# so qualification cannot pass simply because deterministic files were made.
$expectedResults = [ordered]@{
	schemaVersion = 1
	fixtureId = 'locomotion-060-v1'
	status = 'expected-only-not-executed'
	caseExpectations = @($source.cases | ForEach-Object { [ordered]@{ id = $_.id; expected = $_.expected } })
	rootTimeline = $rootFixture[0].rootTimelineExpectation
	stress = [ordered]@{
		population = 1000; velocity = 700; rootMotion = 300
		minimumAcceptedChanges = $source.lanes.stress.minimumAcceptedChanges
		measurementSeconds = $source.lanes.stress.measurementSeconds
	}
	paired = [ordered]@{ maximumCandidateP95FrameTimeRatio = $source.lanes.paired.maximumCandidateP95FrameTimeRatio }
}
$expectedJson = ($expectedResults | ConvertTo-Json -Depth 30).Replace("`r`n", "`n").TrimEnd("`n") + "`n"
$expectedPath = Join-Path $output 'expected-results.json'
[IO.File]::WriteAllText($expectedPath, $expectedJson, [Text.UTF8Encoding]::new($false))
$sourceFile = Join-Path $PSScriptRoot 'LocomotionFixtureMaterializer.cpp'
$document = [ordered]@{
	schemaVersion = 1
	fixtureId = 'locomotion-060-assets-v1'
	sourceManifestSha256 = (Get-FileHash -LiteralPath $manifestFile -Algorithm SHA256).Hash.ToLowerInvariant()
	generatorSourceSha256 = (Get-FileHash -LiteralPath $sourceFile -Algorithm SHA256).Hash.ToLowerInvariant()
	expectedResultsPath = 'expected-results.json'
	expectedResultsSha256 = (Get-FileHash -LiteralPath $expectedPath -Algorithm SHA256).Hash.ToLowerInvariant()
	assets = $assets.ToArray()
}
$json = ($document | ConvertTo-Json -Depth 12).Replace("`r`n", "`n").TrimEnd("`n") + "`n"
[IO.File]::WriteAllText((Join-Path $output 'fixture-assets.json'), $json, [Text.UTF8Encoding]::new($false))
Write-Host "Materialized $($assets.Count) production-cooked locomotion assets at $output."
