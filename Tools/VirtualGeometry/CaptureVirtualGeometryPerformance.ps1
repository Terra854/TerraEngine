[CmdletBinding()]
param(
	[string]$BenchmarkExecutable = 'out/build/RelWithDebInfo/Build/Tools/TerraEngineVirtualGeometryBenchmark.exe',
	[string]$Output = ('out/evidence/virtual-geometry-production/performance/' + [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssZ') + '.json'),
	[string]$Workloads = 'Tools/VirtualGeometry/workloads.json',
	[string]$Matrix = 'Tools/VirtualGeometry/hardware-matrix.json',
	[string]$Quality = 'Tools/VirtualGeometry/quality-profiles.json',
	[string]$WorkloadId = 'baseline-processor-expanded',
	[string]$RuntimeAdapter = 'out/build/RelWithDebInfo/Build/Tools/VirtualGeometry/VirtualGeometryRuntimeAdapter.cmd',
	[ValidateRange(1, 86400)]
	[int]$DurationSeconds = 6,
	[ValidateRange(1, 600000)]
	[int]$SampleIntervalMilliseconds = 1000,
	[string[]]$VisualSummary,
	[switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
function Resolve-Repo([string]$Path) { if([IO.Path]::IsPathRooted($Path)){return [IO.Path]::GetFullPath($Path)}; return [IO.Path]::GetFullPath((Join-Path $repo $Path)) }
$exe = Resolve-Repo $BenchmarkExecutable
$outputPath = Resolve-Repo $Output
$runtimeAdapterPath = Resolve-Repo $RuntimeAdapter
foreach($manifest in @($Workloads,$Matrix,$Quality)) { $path = Resolve-Repo $manifest; if(-not (Test-Path -LiteralPath $path -PathType Leaf)){ throw "Manifest missing: $path" }; Get-Content -Raw $path | ConvertFrom-Json | Out-Null }
if($VisualSummary -and $VisualSummary.Count -gt 0) {
	# powershell.exe -File can pass a comma-separated value as one string. Expand
	# that spelling so command-line and interactive array invocation behave alike.
	$summaryInputs = @(
		$VisualSummary |
			ForEach-Object { @([string]$_ -split ',') } |
			ForEach-Object { $_.Trim() } |
			Where-Object { -not [string]::IsNullOrWhiteSpace($_) }
	)
	if($DryRun) {
		foreach($summaryPath in $summaryInputs) {
			$path = Resolve-Repo $summaryPath
			if(-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Visual summary missing: $path" }
		}
		Write-Output "validated $($summaryInputs.Count) installed-editor visual summary inputs (dry run; no timing invented)"
		exit 0
	}
	$rows = [Collections.Generic.List[object]]::new()
	$sourceSummaries = [Collections.Generic.List[object]]::new()
	$seenEvidence = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	foreach($summaryPath in $summaryInputs) {
		$path = Resolve-Repo $summaryPath
		if(-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Visual summary missing: $path" }
		$summary = Get-Content -Raw $path | ConvertFrom-Json
		$measurements = @()
		$postCapture = $summary.PSObject.Properties['postCaptureCapabilityEvidence']
		if($null -ne $postCapture -and $null -ne $postCapture.Value.data) {
			$measurements = @($postCapture.Value.data.measurements)
		}
		elseif($null -ne $summary.PSObject.Properties['rows']) {
			# Retain the small row contract for standalone adapters. Rows must
			# explicitly assert that they were captured after live rendering.
			$measurements = @($summary.rows | Where-Object { $_.postCaptureMeasured -eq $true })
		}
		foreach($row in $measurements) {
			$executionId = [string]$row.executionEvidenceId
			$duration = $row.durationNanoseconds
			$evidenceKey = "$executionId|$duration|$([string]$summary.capturedAt)|$([string]$row.stage)"
			$validMeasurement = $row.availability -eq 'measured' -and -not [string]::IsNullOrWhiteSpace($executionId) -and $duration -is [ValueType] -and [double]$duration -gt 0
			if($validMeasurement -and $seenEvidence.Add($evidenceKey)) {
				$rows.Add([ordered]@{
					availability = 'measured'
					durationNanoseconds = [double]$duration
					stage = [string]$row.stage
					executionEvidenceId = $executionId
					source = 'installed-editor-post-capture-measurement'
				})
			}
		}
		$sourceSummaries.Add([ordered]@{
			path = $path
			sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
			capturedAt = [string]$summary.capturedAt
		})
	}
	if($rows.Count -lt 5) { throw "At least five post-capture measured visual rows are required; found $($rows.Count)" }
	$captureExecutionId = 'installed-editor-set-' + [guid]::NewGuid().ToString('N')
	foreach($row in $rows) {
		$row['executionId'] = $captureExecutionId
	}
	$sorted = @($rows | ForEach-Object { [double]$_.durationNanoseconds } | Sort-Object)
	$median = $sorted[[Math]::Floor(($sorted.Count - 1) / 2)]
	$p95 = $sorted[[Math]::Min($sorted.Count - 1, [Math]::Ceiling($sorted.Count * 0.95) - 1)]
	$mean = ($sorted | Measure-Object -Average).Average
	$variance = (($sorted | ForEach-Object { ($_ - $mean) * ($_ - $mean) } | Measure-Object -Sum).Sum / $sorted.Count)
	$parent = Split-Path -Parent $outputPath; New-Item -ItemType Directory -Force -Path $parent | Out-Null
	[ordered]@{
		identity = [ordered]@{
			schemaVersion = '1'
			workloadVersion = '1'
			captureId = [guid]::NewGuid().ToString('N')
			executionId = $captureExecutionId
			captureTime = [DateTime]::UtcNow.ToString('o')
		}
		environment = [ordered]@{
			backend = 'vulkan'
			route = 'installed-editor-live'
		}
		workload = [ordered]@{ name = $WorkloadId }
		warmupAndSampling = [ordered]@{
			warmupSamples = 0
			requestedSamples = $rows.Count
			randomizedOrder = $false
		}
		capabilities = @()
		rawSamples = $rows
		statistics = [ordered]@{
			medianNanoseconds = $median
			p95Nanoseconds = $p95
			p99Nanoseconds = $sorted[[Math]::Min($sorted.Count - 1, [Math]::Ceiling($sorted.Count * 0.99) - 1)]
			sampleCount = $rows.Count
			rejectedCount = 0
			varianceNanosecondsSquared = $variance
			worstNanoseconds = $sorted[-1]
			boundedCounters = $false
		}
		failureAndOverflowCounters = [ordered]@{}
		baselineAndComparability = [ordered]@{
			status = 'measured'
			source = 'installed-editor-post-capture-measurements'
		}
		integrity = [ordered]@{
			sourceSummaries = $sourceSummaries
		}
	} | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $outputPath -Encoding utf8
	Write-Output "capture written from installed-editor visual summaries: $outputPath"
	exit 0
}
if(-not $DryRun -and -not (Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Benchmark executable missing: $exe" }
if(-not (Test-Path -LiteralPath $runtimeAdapterPath -PathType Leaf)) { throw "Runtime adapter missing: $runtimeAdapterPath" }
if($DryRun) { Write-Output "validated manifests, executable, and runtime adapter (dry run; no capture launched): $exe"; exit 0 }
$parent = Split-Path -Parent $outputPath; New-Item -ItemType Directory -Force -Path $parent | Out-Null
$args = @(
	'--workloads', (Resolve-Repo $Workloads),
	'--matrix', (Resolve-Repo $Matrix),
	'--quality', (Resolve-Repo $Quality),
	'--workload', $WorkloadId,
	'--runtime-adapter', $runtimeAdapterPath,
	'--duration-seconds', $DurationSeconds,
	'--sample-interval-milliseconds', $SampleIntervalMilliseconds,
	'--output', $outputPath)
& $exe @args
if($LASTEXITCODE -ne 0) { throw "Benchmark executable failed with exit code $LASTEXITCODE" }
$capture = Get-Content -Raw $outputPath | ConvertFrom-Json
$measured = @($capture.rawSamples | Where-Object { $_.availability -eq 'measured' -and $_.executionId -and $_.durationNanoseconds -gt 0 })
if($measured.Count -lt 6) { throw "Capture is incomplete: at least six measured samples with execution IDs are required (found $($measured.Count))" }
Write-Output "capture written to $outputPath; only runtime-published measurements are accepted"
