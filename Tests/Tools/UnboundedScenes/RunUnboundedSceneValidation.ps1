[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string]$InstallRoot,
	[Parameter(Mandatory=$true)][string]$OutputDirectory,
	[int]$Runs = 5,
	[string]$ValidationExecutable,
	[switch]$RunCoordinateMatrix,
	[switch]$RunMultiView,
	[switch]$RunMultiPhysics,
	[switch]$RunCrossingStress,
	[switch]$RunPersistenceCycles,
	[switch]$RunLegacyRejection
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if($Runs -lt 5){ throw 'At least five complete runs are required.' }
if(-not (Test-Path -LiteralPath $InstallRoot -PathType Container)){ throw "Install root missing: $InstallRoot" }
$exe = if($ValidationExecutable){ $ValidationExecutable } else { Join-Path $InstallRoot 'Tests/Tools/UnboundedScenes/TerraEngine-UnboundedSceneValidation.exe' }
$workloads = Join-Path $InstallRoot 'Tests/Tools/UnboundedScenes/workloads.json'
$fixtureManifest = Join-Path $InstallRoot 'Tests/Fixtures/UnboundedScenes/fixture-manifest.json'
if(-not (Test-Path -LiteralPath $exe -PathType Leaf)){ throw "Installed validator missing: $exe" }
if(-not (Test-Path -LiteralPath $workloads -PathType Leaf)){ throw "Installed workload manifest missing: $workloads" }
# The installed inventory is part of the acceptance identity. Parse it before
# any scenario runs so a missing or corrupt fixture bundle cannot report success.
if(-not (Test-Path -LiteralPath $fixtureManifest -PathType Leaf)){ throw "Installed fixture manifest missing: $fixtureManifest" }
$fixtureInventory = Get-Content -Raw -LiteralPath $fixtureManifest | ConvertFrom-Json
if($fixtureInventory.feature -ne '052-unbounded-scenes' -or @($fixtureInventory.fixtures).Count -lt 8){ throw 'Installed fixture manifest is incomplete or belongs to another feature.' }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$names = @('coordinate','streaming','multiview','multiphysics','crossing','persistence','legacy','diagnostics')
$selected = @(); if($RunCoordinateMatrix){$selected += 'coordinate'}; if($RunMultiView){$selected += 'multiview'}; if($RunMultiPhysics){$selected += 'multiphysics'}; if($RunCrossingStress){$selected += 'crossing'}; if($RunPersistenceCycles){$selected += 'persistence'}; if($RunLegacyRejection){$selected += 'legacy'}
if($selected.Count -eq 0){ $selected = $names }
$result = [ordered]@{ schemaVersion=2; status='incomplete'; requestedRuns=$Runs; completeRuns=0; artifactFinalized=$false; unavailableIsNotZero=$true; rawArtifacts=@(); scenarios=[ordered]@{} }
foreach($scenario in $selected){
	$raw = Join-Path $OutputDirectory ("{0}-run.json" -f $scenario)
	& $exe --scenario $scenario --output $raw --workloads $workloads --runs $Runs
	if($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $raw -PathType Leaf)){ throw "Validator failed for scenario: $scenario" }
	$evidence = Get-Content -Raw -LiteralPath $raw | ConvertFrom-Json
	if($evidence.status -ne 'measured' -or -not $evidence.artifactFinalized){ throw "Incomplete validator evidence: $scenario" }
	$entry = $evidence.scenarios.$scenario
	if($null -eq $entry -or $entry.status -ne 'passed' -or @($entry.rawSamples).Count -lt $Runs){ throw "Scenario did not produce $Runs complete samples: $scenario" }
	$result.rawArtifacts += $raw
	$result.scenarios[$scenario] = $entry
}
$result.completeRuns = $Runs; $result.status = 'measured'; $result.artifactFinalized = $true
$result | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $OutputDirectory 'unbounded-scene-validation.json') -Encoding utf8
