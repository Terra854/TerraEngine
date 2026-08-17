# CaptureTerrainEvidence.ps1 -- Converts five or more raw benchmark runs into lanes.
param([Parameter(Mandatory=$true)][string[]]$BenchmarkResult,[string]$CaptureDirectory='out/evidence/terrain/candidate/captures')
Set-StrictMode -Version Latest; $ErrorActionPreference='Stop'
if($BenchmarkResult.Count -lt 5) { throw 'Terrain candidate evidence requires at least five comparable benchmark captures.' }
$rawCaptures=@(); $identities=@()
foreach($path in $BenchmarkResult) {
	if(!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Raw terrain benchmark is missing: $path" }
	$raw=Get-Content -Raw -LiteralPath $path | ConvertFrom-Json
	if($raw.status -ne 'available' -or $raw.route -ne 'terrain-1000km-edit-route' -or $raw.distanceKilometres -ne 1000) { throw "Raw terrain benchmark is not comparable: $path" }
	$rawCaptures += $raw
	$identities += [ordered]@{path=[IO.Path]::GetFullPath($path);sha256=(Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()}
}
New-Item -ItemType Directory -Force -Path $CaptureDirectory | Out-Null
function Join-Samples([string]$name) { @($rawCaptures | ForEach-Object { @($_.rawMetrics.$name) }) }
function Percentiles($values) {
	$ordered=@($values | Sort-Object); if($ordered.Count -eq 0) { return $null }
	$p50=$ordered[[Math]::Ceiling($ordered.Count*0.50)-1]
	$p95=$ordered[[Math]::Ceiling($ordered.Count*0.95)-1]
	[ordered]@{sampleCount=$ordered.Count;p50Nanoseconds=[uint64]$p50;p95Nanoseconds=[uint64]$p95}
}
function Write-Lane([string]$id,$metrics) {
	$available=$null -ne $metrics
	[ordered]@{status=if($available){'available'}else{'unavailable'};passing=$false;reason=if($available){'Five comparable raw captures recorded; thresholds remain a separate production gate.'}else{'Benchmark cannot measure this installed runtime lane.'};rawMetrics=$metrics;artifactSha256=if($available){$identities[0].sha256}else{$null};sourceCaptures=if($available){$identities}else{@()}} | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $CaptureDirectory "$id.json") -Encoding utf8
}
Write-Lane 'authoring' ([ordered]@{preview=Percentiles (Join-Samples 'previewNanoseconds')})
Write-Lane 'render-culling' ([ordered]@{cook=Percentiles (Join-Samples 'cookNanoseconds');upload=Percentiles (Join-Samples 'uploadNanoseconds');publication=Percentiles (Join-Samples 'publicationNanoseconds')})
Write-Lane 'collision' ([ordered]@{construction=Percentiles (Join-Samples 'collisionNanoseconds')})
Write-Lane 'streaming-decode-io' $null
Write-Lane 'save-recovery' $null
Write-Output "Terrain lane captures written to $CaptureDirectory"
