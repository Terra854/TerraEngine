[CmdletBinding()]
param(
	[string]$Runner = 'out/build/RelWithDebInfo/Build/Tools/TerraEngineVirtualGeometryBenchmark.exe',
	[string]$RuntimeAdapter = '',
	[string]$Output = 'out/evidence/virtual-geometry-production/animated-benchmark.json',
	[string]$Workloads = 'Tools/VirtualGeometry/workloads.json',
	[string]$Matrix = 'Tools/VirtualGeometry/hardware-matrix.json',
	[string]$Quality = 'Tools/VirtualGeometry/quality-profiles.json',
	[string]$HeroWorkloadId = 'animated-hero',
	[string]$CrowdWorkloadId = 'animated-crowd',
	[string]$FullReferenceEvidence = '',
	[string]$MillionDecisionEvidence = '',
	[int]$DurationSeconds = 30,
	[int]$SampleIntervalMilliseconds = 1000,
	[switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

foreach($manifest in @($Workloads, $Matrix, $Quality))
{
	if(-not (Test-Path -LiteralPath $manifest -PathType Leaf))
	{
		throw "Manifest missing: $manifest"
	}
	Get-Content -Raw -LiteralPath $manifest | ConvertFrom-Json | Out-Null
}
if($DurationSeconds -lt 1 -or $SampleIntervalMilliseconds -lt 1)
{
	throw 'DurationSeconds and SampleIntervalMilliseconds must be positive.'
}
if([string]::IsNullOrWhiteSpace($HeroWorkloadId) -or
	[string]::IsNullOrWhiteSpace($CrowdWorkloadId))
{
	throw 'HeroWorkloadId and CrowdWorkloadId are required runtime-adapter selectors.'
}

function Get-AnimatedGeometryPrerequisite([string]$Name, [string]$Path)
{
	# Animated-geometry validation combines independent correctness evidence with live timing. Do not
	# silently promote a hero/crowd capture into proof of full-reference parity
	# or the million-decision bounds corpus when their artifact was not supplied.
	if([string]::IsNullOrWhiteSpace($Path))
	{
		return [ordered]@{
			name = $Name
			status = 'unavailable'
			path = ''
			reason = "A $Name evidence JSON path is required for a complete animated-geometry result."
		}
	}
	if(-not (Test-Path -LiteralPath $Path -PathType Leaf))
	{
		return [ordered]@{
			name = $Name
			status = 'unavailable'
			path = $Path
			reason = "The supplied $Name evidence file does not exist."
		}
	}
	try
	{
		$evidence = Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
		$status = [string]$evidence.status
		if($status -notin @('accepted', 'complete', 'measured', 'passed', 'validated'))
		{
			return [ordered]@{
				name = $Name
				status = 'incomplete'
				path = $Path
				reason = "The $Name evidence did not publish an accepted status."
			}
		}
		return [ordered]@{
			name = $Name
			status = 'accepted'
			path = $Path
			reason = ''
		}
	}
	catch
	{
		return [ordered]@{
			name = $Name
			status = 'unavailable'
			path = $Path
			reason = "The $Name evidence is not readable JSON: $($_.Exception.Message)"
		}
	}
}

$animatedGeometryPrerequisites = @(
	Get-AnimatedGeometryPrerequisite 'full-reference' $FullReferenceEvidence
	Get-AnimatedGeometryPrerequisite 'million-decision' $MillionDecisionEvidence
)
if($DryRun)
{
	Write-Output "validated animated hero/crowd runtime plan duration=$DurationSeconds seconds with $(@($animatedGeometryPrerequisites | Where-Object { $_.status -eq 'accepted' }).Count)/2 animated-geometry correctness prerequisites (dry run; no animation measurement)"
	exit 0
}
if(-not (Test-Path -LiteralPath $Runner -PathType Leaf))
{
	throw "Animated benchmark runner unavailable: $Runner"
}
if([string]::IsNullOrWhiteSpace($RuntimeAdapter) -or
	-not (Test-Path -LiteralPath $RuntimeAdapter -PathType Leaf))
{
	throw 'A runnable -RuntimeAdapter is required. It must publish bounded measured Vulkan captures for both hero and crowd selectors.'
}

$outputParent = Split-Path -Parent $Output
if(-not [string]::IsNullOrWhiteSpace($outputParent))
{
	New-Item -ItemType Directory -Force -Path $outputParent | Out-Null
}
$rows = @()
foreach($workload in @(
	[ordered]@{ id = 'hero'; selector = $HeroWorkloadId },
	[ordered]@{ id = 'crowd'; selector = $CrowdWorkloadId }))
{
	$resultPath = "$Output.$($workload.id).json"
	# The native benchmark owns the repeated timing loop. Passing the adapter and
	# duration explicitly prevents a hero/crowd wrapper from inventing samples.
	& $Runner `
		'--workloads' $Workloads `
		'--matrix' $Matrix `
		'--quality' $Quality `
		'--workload' $workload.selector `
		'--runtime-adapter' $RuntimeAdapter `
		'--duration-seconds' $DurationSeconds `
		'--sample-interval-milliseconds' $SampleIntervalMilliseconds `
		'--output' $resultPath
	$runnerExitCode = $LASTEXITCODE
	if(-not (Test-Path -LiteralPath $resultPath -PathType Leaf))
	{
		throw "Animated $($workload.id) workload did not publish raw evidence."
	}
	$raw = Get-Content -Raw -LiteralPath $resultPath | ConvertFrom-Json
	$measured = @($raw.rawSamples | Where-Object {
		$_.availability -eq 'measured' -and
		-not [string]::IsNullOrWhiteSpace([string]$_.executionId) -and
		$_.durationNanoseconds -gt 0
	})
	$bounded = $null -ne $raw.statistics -and [bool]$raw.statistics.boundedCounters
	# Runtime adapters are allowed to publish a structured unavailable result.
	# Read optional fields through PSObject.Properties so StrictMode does not
	# replace the adapter's original failure reason with a missing-property error.
	$runtimeSampling = $null
	$runtimeSamplingProperty = $raw.PSObject.Properties['runtimeSampling']
	if($null -ne $runtimeSamplingProperty)
	{
		$runtimeSampling = $runtimeSamplingProperty.Value
	}
	$actualDurationSeconds = 0.0
	$durationProperty = if($null -ne $runtimeSampling) { $runtimeSampling.PSObject.Properties['actualDurationSeconds'] } else { $null }
	if($null -ne $durationProperty -and $null -ne $durationProperty.Value)
	{
		$actualDurationSeconds = [double]$durationProperty.Value
	}
	$durationSatisfied = $actualDurationSeconds -ge $DurationSeconds
	$failureReason = ''
	$reasonProperty = if($null -ne $runtimeSampling) { $runtimeSampling.PSObject.Properties['reason'] } else { $null }
	if($null -ne $reasonProperty -and $null -ne $reasonProperty.Value)
	{
		$failureReason = [string]$reasonProperty.Value
	}
	if([string]::IsNullOrWhiteSpace($failureReason) -and $null -ne $raw.integrity)
	{
		$diagnosticsProperty = $raw.integrity.PSObject.Properties['diagnostics']
		if($null -ne $diagnosticsProperty -and $null -ne $diagnosticsProperty.Value)
		{
			$adapterProperty = $diagnosticsProperty.Value.PSObject.Properties['runtimeAdapter']
			if($null -ne $adapterProperty -and $null -ne $adapterProperty.Value)
			{
				$failureReason = [string]$adapterProperty.Value
			}
		}
	}
	$rows += [ordered]@{
		id = $workload.id
		selector = $workload.selector
		status = if($runnerExitCode -eq 0 -and $measured.Count -gt 0 -and
			$bounded -and $durationSatisfied){'measured-result-published'}else{'incomplete'}
		runnerExitCode = $runnerExitCode
		measuredSamples = $measured.Count
		boundedCounters = $bounded
		durationSatisfied = $durationSatisfied
		actualDurationSeconds = $actualDurationSeconds
		failureReason = $failureReason
		result = $resultPath
	}
}
$missingPrerequisites = @($animatedGeometryPrerequisites | Where-Object {
	$_.status -ne 'accepted'
})
$status = if(@($rows | Where-Object {
	$_.status -ne 'measured-result-published'
}).Count -eq 0 -and $missingPrerequisites.Count -eq 0){'measured-result-published'}else{'incomplete'}
[ordered]@{
	schemaVersion = 1
	status = $status
	runtimeAdapter = $RuntimeAdapter
	durationSeconds = $DurationSeconds
	animatedGeometryPrerequisites = $animatedGeometryPrerequisites
	workloads = $rows
	note = 'Only accepted full-reference and million-decision evidence plus repeated bounded Vulkan runtime captures with execution IDs satisfy the complete animated-geometry workflow.'
} | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $Output -Encoding utf8
if($status -ne 'measured-result-published')
{
	exit 2
}
