# RunTerrainSoak.ps1 - Fail-closed installed terrain soak contract.
#
# Dry-run validates the installed bundle and writes unavailable evidence. A
# non-dry run invokes the installed benchmark; this script never fabricates
# timing, residency, or pass values when a required artifact is missing.

[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$InstallRoot,
	[Parameter(Mandatory = $true)][string]$OutputDirectory,
	[ValidateRange(1, 1440)][int]$DurationMinutes = 30,
	[ValidateRange(0, 86400)][int]$DurationSecondsOverride = 0,
	[string]$Fixture = "terrain-256km",
	[string]$Route = "terrain-1000km-edit-route",
	[string]$TepakArchive = "",
	[string]$ContentMountRoot = "",
	[switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$install = [System.IO.Path]::GetFullPath($InstallRoot)
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
[System.IO.Directory]::CreateDirectory($output) | Out-Null
$manifest = Join-Path $install "Tests/Fixtures/Terrain/manifest.json"
$workloads = Join-Path $install "Tests/Tools/Terrain/workloads.json"
$benchmark = Join-Path $install "Tests/Tools/Terrain/TerraEngineTerrainBenchmark.exe"
$fixtureGenerator = Join-Path $install "Tests/Tools/Terrain/TerraEngineTerrainFixtureGenerator.exe"
$result = Join-Path $output "terrain-soak-result.json"
$missing = [System.Collections.Generic.List[string]]::new()
foreach ($required in @($manifest, $workloads)) {
	if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { $missing.Add($required) }
}
$requiredSeconds = $DurationMinutes * 60
$runtimeSeconds = if ($DurationSecondsOverride -gt 0) { $DurationSecondsOverride } else { $requiredSeconds }
$shortSmoke = $DurationSecondsOverride -gt 0 -or $DurationMinutes -lt 30
if (-not [string]::IsNullOrWhiteSpace($TepakArchive) -and -not (Test-Path -LiteralPath $TepakArchive -PathType Leaf)) { $missing.Add($TepakArchive) }
if (-not [string]::IsNullOrWhiteSpace($ContentMountRoot) -and -not (Test-Path -LiteralPath $ContentMountRoot -PathType Container)) { $missing.Add($ContentMountRoot) }
if (-not $DryRun) {
	foreach ($executable in @($benchmark, $fixtureGenerator)) {
		if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) { $missing.Add($executable) }
	}
}

$evidence = [ordered]@{
	schemaVersion = [ordered]@{ major = 1; minor = 0 }
	status = if ($missing.Count -gt 0) { "unavailable" } elseif ($DryRun) { "dry-run" } else { "unavailable" }
	installRoot = $install
	fixture = $Fixture
	route = $Route
	durationMinutes = $DurationMinutes
	requiredDurationSeconds = $requiredSeconds
	runtimeDurationSeconds = $runtimeSeconds
	durationSecondsOverride = $DurationSecondsOverride
	shortSmoke = $shortSmoke
	requiredArtifacts = @($manifest, $workloads, $fixtureGenerator, $benchmark)
	missingArtifacts = @($missing)
	metrics = "unavailable-until-measured-route-execution"
	contentMountAudit = [ordered]@{ archive = $TepakArchive; mountRoot = $ContentMountRoot; status = if ($TepakArchive -or $ContentMountRoot) { "supplied-and-checked" } else { "not-supplied" } }
	iterations = 0
	elapsedSeconds = 0.0
}

if (-not $DryRun -and $missing.Count -eq 0) {
	$benchmarkResult = Join-Path $output "terrain-benchmark-result.json"
	$generatedFixture = Join-Path $output "terrain-256km.json"
	$stopwatch = $null
	try {
		# The sparse 256 km fixture is generated immediately before the route run,
		# proving the installed generator and benchmark consume the same artifact.
		& $fixtureGenerator --output $generatedFixture
		if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $generatedFixture -PathType Leaf)) {
			throw "Terrain fixture generator did not publish the 256 km fixture."
		}
		$evidence.status = 'running'
		$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
		$iteration = 0
		do {
			$iteration++
			$evidence.iterations = $iteration
			$evidence.benchmarkResult = $benchmarkResult
			# Require fresh evidence after each invocation before another iteration;
			# an old successful file must not survive a missing child result.
			if (Test-Path -LiteralPath $benchmarkResult) { Remove-Item -LiteralPath $benchmarkResult -Force }
			& $benchmark --fixture $generatedFixture --route $Route --duration-minutes 0 --collision true --output $benchmarkResult
			if ($LASTEXITCODE -ne 0) { throw "Terrain benchmark exited with code $LASTEXITCODE." }
			$evidence.benchmarkSha256 = (Get-FileHash -LiteralPath $benchmarkResult -Algorithm SHA256).Hash.ToLowerInvariant()
			$benchmarkJson = Get-Content -Raw -LiteralPath $benchmarkResult | ConvertFrom-Json
			$evidence.benchmarkResult = $benchmarkResult
			$evidence.iterations = $iteration
			$evidence.elapsedSeconds = $stopwatch.Elapsed.TotalSeconds
			if ([string]$benchmarkJson.status -eq 'unavailable') {
				# The CPU-only producer honestly lacks a live route. Stop now rather
				# than waiting 30 minutes to rediscover that missing prerequisite.
				$evidence.status = 'unavailable'
				$evidence.unavailableReason = 'Installed terrain benchmark did not execute the required live route.'
				$qualification = $benchmarkJson.PSObject.Properties['qualification']
				$evidence.benchmarkQualification = if ($null -ne $qualification) { $qualification.Value } else { $null }
				break
			}
			if ([string]$benchmarkJson.status -notin @('available', 'measured') -or $benchmarkJson.route -ne $Route -or $null -eq $benchmarkJson.distanceKilometres -or $benchmarkJson.distanceKilometres -ne 1000) {
				throw "Terrain benchmark did not publish available evidence for the requested 1000 km route."
			}
		} while ($stopwatch.Elapsed.TotalSeconds -lt $runtimeSeconds)
		$stopwatch.Stop()
		if ($evidence.status -eq 'unavailable') {
			# Raw unsupported evidence remains intact; vector timings do not become
			# terrain/render measurements. The common epilogue writes and rejects it.
			$evidence.metrics = 'unavailable-until-measured-route-execution'
		} else {
			$evidence.fixtureArtifact = $generatedFixture
			$evidence.benchmarkResult = $benchmarkResult
			$evidence.iterations = $iteration
			$evidence.elapsedSeconds = $stopwatch.Elapsed.TotalSeconds
			$evidence.artifactHashes = [ordered]@{
				manifest = (Get-FileHash -LiteralPath $manifest -Algorithm SHA256).Hash.ToLowerInvariant()
				workloads = (Get-FileHash -LiteralPath $workloads -Algorithm SHA256).Hash.ToLowerInvariant()
				fixture = (Get-FileHash -LiteralPath $generatedFixture -Algorithm SHA256).Hash.ToLowerInvariant()
				benchmark = (Get-FileHash -LiteralPath $benchmarkResult -Algorithm SHA256).Hash.ToLowerInvariant()
			}
			if (-not [string]::IsNullOrWhiteSpace($TepakArchive)) { $evidence.artifactHashes.tepak = (Get-FileHash -LiteralPath $TepakArchive -Algorithm SHA256).Hash.ToLowerInvariant() }
			if ($evidence.elapsedSeconds -lt $runtimeSeconds) { throw "Measured route duration did not reach the requested runtime." }
			$evidence.status = if ($shortSmoke) { "measured-smoke" } else { "measured" }
			$evidence.metrics = $benchmarkJson.rawMetrics
		}
	}
	catch {
		if ($null -ne $stopwatch) { $stopwatch.Stop(); $evidence.elapsedSeconds = $stopwatch.Elapsed.TotalSeconds }
		$evidence.status = "failed"
		$evidence.failure = $_.Exception.Message
	}
}

$evidence | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $result -Encoding utf8
Write-Output (ConvertTo-Json $evidence -Depth 10)
if ($evidence.status -eq "unavailable" -or $evidence.status -eq "failed") { exit 1 }
