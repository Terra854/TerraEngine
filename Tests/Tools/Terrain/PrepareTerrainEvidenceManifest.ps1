# PrepareTerrainEvidenceManifest.ps1 -- fail-closed terrain production evidence.
#
# This preparation step never runs a benchmark and never manufactures a timing.
# Root capture automation may supply one JSON capture per required workflow; any
# missing, malformed, or unhashed capture is retained as unavailable/non-passing.

param(
	[string]$Output = 'out/evidence/terrain/candidate/manifest.json',
	[string]$HardwareMatrix = 'Tests/Tools/Terrain/hardware-matrix.json',
	[string]$QualityProfiles = 'Tests/Tools/Terrain/quality-profiles.json',
	[string]$Workloads = 'Tests/Tools/Terrain/workloads.json',
	[string]$CaptureDirectory = 'out/evidence/terrain/candidate/captures'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
function Resolve-RepoFile([string]$path) { Join-Path $repo $path }
function Hash-File([string]$path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant() }

$matrixPath = Resolve-RepoFile $HardwareMatrix
$qualityPath = Resolve-RepoFile $QualityProfiles
$workloadsPath = Resolve-RepoFile $Workloads
foreach($path in @($matrixPath, $qualityPath, $workloadsPath)) { if(!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required terrain evidence input is missing: $path" } }
$matrix = Get-Content -Raw -LiteralPath $matrixPath | ConvertFrom-Json
$quality = Get-Content -Raw -LiteralPath $qualityPath | ConvertFrom-Json

# These are the five non-interchangeable production evidence lanes required by
# the terrain acceptance contract.
$lanes = @(
	@{ id = 'authoring'; workload = 'terrain-benchmark-interactive' },
	@{ id = 'render-culling'; workload = 'terrain-seam-matrix' },
	@{ id = 'streaming-decode-io'; workload = 'terrain-1000km-edit-route' },
	@{ id = 'collision'; workload = 'terrain-collision-matrix' },
	@{ id = 'save-recovery'; workload = 'terrain-automation-parity' }
)
$rows = @()
foreach($lane in $lanes) {
	$capture = Join-Path (Resolve-RepoFile $CaptureDirectory) ($lane.id + '.json')
	$row = [ordered]@{ id = $lane.id; workload = $lane.workload; status = 'unavailable'; passing = $false; reason = 'No raw capture supplied.'; rawMetrics = $null; artifactSha256 = $null; sourceCaptures = @() }
	if(Test-Path -LiteralPath $capture -PathType Leaf) {
		try {
			$raw = Get-Content -Raw -LiteralPath $capture | ConvertFrom-Json
			if($raw.status -eq 'available' -and $null -ne $raw.rawMetrics -and $raw.artifactSha256 -match '^[a-fA-F0-9]{64}$') {
				$row.status = 'available'; $row.passing = [bool]$raw.passing; $row.reason = [string]$raw.reason; $row.rawMetrics = $raw.rawMetrics; $row.artifactSha256 = ([string]$raw.artifactSha256).ToLowerInvariant(); $row.sourceCaptures = @($raw.sourceCaptures)
			} else { $row.reason = 'Capture lacks available status, raw metrics, or SHA256 artifact identity.' }
		} catch { $row.reason = 'Capture JSON is invalid.' }
	}
	$rows += [pscustomobject]$row
}

$manifest = [ordered]@{
	schemaVersion = 1
	status = if(($rows | Where-Object { $_.status -ne 'available' }).Count -eq 0) { 'complete-candidate' } else { 'incomplete-unavailable' }
	passing = (($rows | Where-Object { $_.status -ne 'available' -or -not $_.passing }).Count -eq 0)
	comparison = [ordered]@{ hardwareMatrixSha256 = Hash-File $matrixPath; qualityProfilesSha256 = Hash-File $qualityPath; workloadsSha256 = Hash-File $workloadsPath; workstation = $matrix.referenceWorkstation.id; profiles = @($quality.qualityProfiles | ForEach-Object { $_.id }) }
	captures = $rows
}
$outputPath = Resolve-RepoFile $Output
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $outputPath) | Out-Null
[IO.File]::WriteAllText($outputPath, ($manifest | ConvertTo-Json -Depth 16), [Text.UTF8Encoding]::new($false))
Write-Output "Terrain evidence manifest: $outputPath ($($manifest.status), passing=$($manifest.passing))"
