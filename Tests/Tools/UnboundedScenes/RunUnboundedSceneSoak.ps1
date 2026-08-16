[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string]$InstallRoot,
	[Parameter(Mandatory=$true)][string]$OutputDirectory,
	[int]$DurationMinutes = 0,
	[int]$DurationHours = 0,
	[string]$ValidationExecutable,
	[switch]$DryRun
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if($DurationMinutes -gt 0 -and $DurationHours -gt 0){ throw 'Choose minutes or hours, not both.' }
$durationSeconds=if($DurationHours -gt 0){$DurationHours*3600}elseif($DurationMinutes -gt 0){$DurationMinutes*60}else{0}
if($durationSeconds -notin @(300,21600)){ throw 'Soak duration must be exactly five minutes or six hours.' }
if(-not (Test-Path -LiteralPath $InstallRoot -PathType Container)){ throw "Install root missing: $InstallRoot" }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$path=Join-Path $OutputDirectory 'unbounded-scene-soak.json'
$profile=if($durationSeconds -eq 21600){'unbounded-scenes-soak-6h-v1'}else{'unbounded-scenes-soak-smoke-v1'}
if($DryRun){
	[ordered]@{schemaVersion=2;profileId=$profile;durationSeconds=$durationSeconds;status='dry-run-validated';boundedHighWaterChecks=$true;deterministicCrossingHash='contract-only';artifactFinalized=$true;unavailableIsNotZero=$true}|ConvertTo-Json|Set-Content -LiteralPath $path -Encoding utf8
	Write-Output "validated duration=$durationSeconds seconds (dry run)"; exit 0
}
$exe=if($ValidationExecutable){$ValidationExecutable}else{Join-Path $InstallRoot 'Tests/Tools/UnboundedScenes/TerraEngine-UnboundedSceneValidation.exe'}
$workloads=Join-Path $InstallRoot 'Tests/Tools/UnboundedScenes/workloads.json'
$fixtureManifest=Join-Path $InstallRoot 'Tests/Fixtures/UnboundedScenes/fixture-manifest.json'
if(-not (Test-Path -LiteralPath $exe -PathType Leaf)){ throw "Installed validator missing: $exe" }
if(-not (Test-Path -LiteralPath $workloads -PathType Leaf)){ throw "Installed workload manifest missing: $workloads" }
# Validate the installed fixture inventory before starting the wall clock. A
# corrupt or incomplete bundle must fail immediately instead of wasting a soak.
if(-not (Test-Path -LiteralPath $fixtureManifest -PathType Leaf)){ throw "Installed fixture manifest missing: $fixtureManifest" }
$fixtureInventory=Get-Content -Raw -LiteralPath $fixtureManifest|ConvertFrom-Json
if($fixtureInventory.feature -ne '052-unbounded-scenes' -or @($fixtureInventory.fixtures).Count -lt 8){ throw 'Installed fixture manifest is incomplete or belongs to another feature.' }
$raw=Join-Path $OutputDirectory 'unbounded-scene-soak-raw.json'; $clock=[Diagnostics.Stopwatch]::StartNew()
& $exe --scenario all --output $raw --workloads $workloads --runs 5 --duration-seconds $durationSeconds
if($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $raw -PathType Leaf)){ throw 'Soak validator failed.' }
$evidence=Get-Content -Raw -LiteralPath $raw|ConvertFrom-Json
$clock.Stop()
if($clock.Elapsed.TotalSeconds -lt $durationSeconds){ throw 'Soak finalized before the requested full duration elapsed.' }
if($evidence.status -ne 'measured' -or -not $evidence.artifactFinalized){ throw 'Soak evidence was not finalized.' }
foreach($scenario in $evidence.scenarios.PSObject.Properties){ if($scenario.Value.status -ne 'passed' -or $scenario.Value.deterministicHash -eq $null){ throw "Soak scenario failed or drifted: $($scenario.Name)" } }
[ordered]@{schemaVersion=2;profileId=$profile;durationSeconds=$durationSeconds;completedDurationSeconds=$clock.Elapsed.TotalSeconds;status='measured';boundedHighWaterChecks=$true;deterministicCrossingHash=$evidence.scenarios.crossing.deterministicHash;rawArtifact=$raw;artifactFinalized=$true;unavailableIsNotZero=$true}|ConvertTo-Json|Set-Content -LiteralPath $path -Encoding utf8
