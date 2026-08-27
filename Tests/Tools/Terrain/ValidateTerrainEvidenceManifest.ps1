# ValidateTerrainEvidenceManifest.ps1 -- structural, fail-closed evidence check.

param([Parameter(Mandatory = $true)][string]$Manifest)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if(!(Test-Path -LiteralPath $Manifest -PathType Leaf)) { throw "Terrain evidence manifest is missing: $Manifest" }
$document = Get-Content -Raw -LiteralPath $Manifest | ConvertFrom-Json
if($document.schemaVersion -ne 1 -or @($document.captures).Count -ne 5) { throw 'Terrain evidence requires schema v1 and exactly five capture lanes.' }
$expected = @('authoring', 'render-culling', 'streaming-decode-io', 'collision', 'save-recovery')
foreach($id in $expected) {
	$row = @($document.captures | Where-Object { $_.id -eq $id })
	if($row.Count -ne 1 -or $row[0].status -notin @('available', 'unavailable')) { throw "Terrain evidence lane is invalid: $id" }
	if($row[0].status -eq 'unavailable' -and $row[0].passing) { throw "Unavailable terrain evidence can never pass: $id" }
	if($row[0].status -eq 'available' -and (!$row[0].artifactSha256 -or $null -eq $row[0].rawMetrics)) { throw "Available terrain evidence requires raw metrics and an artifact hash: $id" }
	if($row[0].status -eq 'available' -and $id -eq 'authoring') {
		$rebuildCounts = @($row[0].rawMetrics.geometryRebuildCounts)
		$nonZeroRebuilds = @($rebuildCounts | Where-Object { [uint64]$_ -ne 0 })
		$missingMappingLatency = $null -eq $row[0].rawMetrics.mappingEdit
		$missingRebuildCounts = $rebuildCounts.Count -eq 0
		$hasGeometryRebuild = $nonZeroRebuilds.Count -ne 0
		if($missingMappingLatency -or $missingRebuildCounts -or $hasGeometryRebuild) {
			throw 'Available authoring evidence must measure mapping latency and prove zero geometry rebuilds.'
		}
	}
	if($row[0].status -eq 'available' -and $id -eq 'render-culling') {
		$metrics=$row[0].rawMetrics
		$missingPreparation = $null -eq $metrics.cpuPreparation
		$missingAllocation = $null -eq $metrics.allocationBytes
		$missingAreas = $null -eq $metrics.visibleAreaCount
		$missingNodes = $null -eq $metrics.selectedNodeCount
		$missingPages = $null -eq $metrics.residentPageCount
		$missingReduction = $null -eq $metrics.geometryReductionPercent
		$belowReductionTarget = [double]$metrics.minimumGeometryReductionPercent -lt 75.0
		if($missingPreparation -or $missingAllocation -or $missingAreas -or $missingNodes -or $missingPages -or $missingReduction -or $belowReductionTarget) {
			throw 'Available render evidence must include page/LOD counts, timings, allocations, and at least 75% distant geometry reduction.'
		}
	}
}
if($document.status -eq 'complete-candidate' -and -not $document.passing) { throw 'A complete terrain candidate must be passing only when all lanes pass.' }
Write-Output 'Terrain evidence manifest is structurally valid and fail-closed.'
