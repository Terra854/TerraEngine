[CmdletBinding()]
param(
	[string]$Matrix = 'Tests/Tools/VirtualGeometry/hardware-matrix.json',
	[string]$Runner = 'out/build/RelWithDebInfo/Build/Tests/Tools/VirtualGeometry/TerraEngineVirtualGeometryBenchmark.exe',
	[string]$Output = 'out/evidence/virtual-geometry-production/backend-parity.json',
	[string]$Workloads = 'Tests/Tools/VirtualGeometry/workloads.json',
	[string]$Quality = 'Tests/Tools/VirtualGeometry/quality-profiles.json',
	[string]$RuntimeAdapter = '',
	[ValidateRange(1, 86400)]
	[int]$DurationSeconds = 5,
	[ValidateRange(1, 60000)]
	[int]$SampleIntervalMilliseconds = 1000,
	[switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$utf8NoBom = [Text.UTF8Encoding]::new($false)

function Get-ObservedVendorFamily([string]$Adapter)
{
	# Adapter names are runtime evidence rather than setup-time assumptions.
	# Normalize only the required adapter families, retaining an
	# empty value for every unrecognized adapter so it can never become a pass.
	if($Adapter -match '(?i)\b(NVIDIA|GeForce|Quadro)\b')
	{
		return 'NVIDIA'
	}
	if($Adapter -match '(?i)\b(AMD|Radeon)\b')
	{
		return 'AMD'
	}
	if($Adapter -match '(?i)\b(Intel|Arc)\b')
	{
		return 'Intel'
	}
	return ''
}

foreach($manifestPath in @($Matrix, $Workloads, $Quality))
{
	if(-not (Test-Path -LiteralPath $manifestPath -PathType Leaf))
	{
		throw "Required parity manifest is missing: $manifestPath"
	}
}
$matrixDocument = Get-Content -Raw -LiteralPath $Matrix | ConvertFrom-Json
$profiles = @($matrixDocument.hardwareProfiles)
if($profiles.Count -eq 0)
{
	throw 'The hardware matrix has no profiles.'
}
if(@($profiles | Where-Object {
	[string]$_.backend -notin @('Vulkan', 'not-captured')
}).Count -ne 0)
{
	throw 'Production virtual-geometry backend parity is Vulkan-only.'
}
if($DryRun)
{
	Write-Output "validated $($profiles.Count) Vulkan hardware profiles and runtime-adapter forwarding (dry run; unavailable hardware remains unexecuted)"
	exit 0
}
if(-not (Test-Path -LiteralPath $Runner -PathType Leaf))
{
	throw "Parity runner unavailable: $Runner"
}

$outputParent = Split-Path -Parent $Output
if(-not [string]::IsNullOrWhiteSpace($outputParent))
{
	[IO.Directory]::CreateDirectory($outputParent) | Out-Null
}
$rows = [Collections.Generic.List[object]]::new()
foreach($profile in $profiles)
{
	$profileId = [string]$profile.id
	$declaredAvailability = [string]$profile.availability
	# The setup manifest cannot know which adapter will execute a later installed
	# validation run. A supplied runtime adapter is therefore allowed to probe the
	# current host, while named future lanes still require an explicit "available"
	# declaration from the machine assigned to that vendor.
	$isCurrentHostProbe = $profileId -eq 'host-current' -and
		-not [string]::IsNullOrWhiteSpace($RuntimeAdapter)
	$shouldExecute = $declaredAvailability -eq 'available' -or
		$isCurrentHostProbe
	if(-not $shouldExecute)
	{
		$rows.Add([ordered]@{
			id = $profileId
			vendor = [string]$profile.vendor
			backend = [string]$profile.backend
			scope = if($profileId -eq 'host-current') {
				'current-host'
			} else {
				'cross-vendor'
			}
			observedVendor = ''
			status = 'unavailable'
			declaredAvailability = $declaredAvailability
			reason = if($profileId -eq 'host-current') {
				'No runtime adapter was supplied to probe the current host.'
			} else {
				'The manifest does not identify this vendor lane as present on the runner.'
			}
		})
		continue
	}
	if([string]::IsNullOrWhiteSpace($RuntimeAdapter) -or
		-not (Test-Path -LiteralPath $RuntimeAdapter -PathType Leaf))
	{
		$rows.Add([ordered]@{
			id = $profileId
			vendor = [string]$profile.vendor
			backend = [string]$profile.backend
			scope = if($profileId -eq 'host-current') {
				'current-host'
			} else {
				'cross-vendor'
			}
			observedVendor = ''
			status = 'unavailable'
			declaredAvailability = $declaredAvailability
			reason = 'A runnable runtime adapter is required to measure this locally available lane.'
		})
		continue
	}
	$resultPath = "$Output.$profileId.json"
	& $Runner `
		'--workloads' $Workloads `
		'--matrix' $Matrix `
		'--quality' $Quality `
		'--workload' 'baseline-processor-expanded' `
		'--runtime-adapter' $RuntimeAdapter `
		'--duration-seconds' $DurationSeconds `
		'--sample-interval-milliseconds' $SampleIntervalMilliseconds `
		'--output' $resultPath
	$runnerExitCode = $LASTEXITCODE
	if(-not (Test-Path -LiteralPath $resultPath -PathType Leaf))
	{
		$rows.Add([ordered]@{
			id = $profileId
			vendor = [string]$profile.vendor
			backend = [string]$profile.backend
			scope = if($profileId -eq 'host-current') {
				'current-host'
			} else {
				'cross-vendor'
			}
			observedVendor = ''
			status = 'incomplete'
			runnerExitCode = $runnerExitCode
			reason = 'The benchmark runner did not publish a capture.'
		})
		continue
	}
	$capture = Get-Content -Raw -LiteralPath $resultPath | ConvertFrom-Json
	$executionId = [string]$capture.identity.executionId
	$observedBackend = [string]$capture.environment.backend
	$observedAdapter = [string]$capture.environment.adapter
	$observedVendor = Get-ObservedVendorFamily $observedAdapter
	$measured = @($capture.rawSamples | Where-Object {
		$_.availability -eq 'measured' -and
		[string]$_.executionId -eq $executionId -and
		[double]$_.durationNanoseconds -gt 0
	})
	$vendor = [string]$profile.vendor
	$vendorMatches = $profileId -eq 'host-current' -or
		$vendor -eq 'unknown-at-setup' -or
		$observedAdapter.IndexOf($vendor, [StringComparison]::OrdinalIgnoreCase) -ge 0
	$accepted = $runnerExitCode -eq 0 -and
		-not [string]::IsNullOrWhiteSpace($executionId) -and
		$observedBackend -ieq 'Vulkan' -and
		$vendorMatches -and
		$measured.Count -ge 5
	$rows.Add([ordered]@{
		id = $profileId
		vendor = $vendor
		backend = $observedBackend
		scope = if($profileId -eq 'host-current') {
			'current-host'
		} else {
			'cross-vendor'
		}
		status = if($accepted) { 'measured' } else { 'incomplete' }
		runnerExitCode = $runnerExitCode
		executionId = $executionId
		observedAdapter = $observedAdapter
		observedVendor = $observedVendor
		measuredSamples = $measured.Count
		result = [ordered]@{
			path = [IO.Path]::GetFullPath($resultPath)
			bytes = (Get-Item -LiteralPath $resultPath).Length
			sha256 = (Get-FileHash -LiteralPath $resultPath -Algorithm SHA256).
				Hash.ToLowerInvariant()
		}
		reason = if($accepted) { '' } else {
			'Measured parity requires a matching Vulkan vendor, a real execution identity, and at least five matching measured samples.'
		}
	})
}

$currentHostRows = @($rows | Where-Object { $_.scope -eq 'current-host' })
$currentHostStatus = if($currentHostRows.Count -gt 0 -and
	@($currentHostRows | Where-Object { $_.status -ne 'measured' }).Count -eq 0) {
	'measured'
} else {
	'incomplete'
}
$requiredFamilies = @($matrixDocument.requiredFamilies | ForEach-Object {
	[string]$_
})
$measuredFamilies = @($requiredFamilies | Where-Object {
	$requiredFamily = $_
	@($rows | Where-Object {
		$_.status -eq 'measured' -and
		(
			$_.vendor -ieq $requiredFamily -or
			$_.observedVendor -ieq $requiredFamily
		)
	}).Count -gt 0
})
$crossVendorStatus = if($requiredFamilies.Count -gt 0 -and
	$measuredFamilies.Count -eq $requiredFamilies.Count) {
	'measured'
} else {
	'incomplete'
}
# A matrix without required families may accept its current Vulkan host. This
# matrix explicitly lists AMD, NVIDIA, and Intel, so its overall release status
# remains incomplete until those external lanes publish measurements.
$status = if($requiredFamilies.Count -gt 0) {
	$crossVendorStatus
} else {
	$currentHostStatus
}
$result = [ordered]@{
	schemaVersion = 3
	status = $status
	identity = [ordered]@{
		executionId = [Guid]::NewGuid().ToString('D')
		backend = 'Vulkan'
	}
	matrix = [ordered]@{
		path = [IO.Path]::GetFullPath($Matrix)
		sha256 = (Get-FileHash -LiteralPath $Matrix -Algorithm SHA256).
			Hash.ToLowerInvariant()
	}
	currentHostVulkan = [ordered]@{
		status = $currentHostStatus
		profileCount = $currentHostRows.Count
	}
	crossVendorCoverage = [ordered]@{
		status = $crossVendorStatus
		requiredFamilies = $requiredFamilies
		measuredFamilies = $measuredFamilies
		missingFamilies = @($requiredFamilies | Where-Object {
			$_ -notin $measuredFamilies
		})
	}
	profiles = $rows
	note = 'The current-host Vulkan result is reported separately. Unavailable required vendor lanes remain unavailable, never count as passes, and keep the overall status incomplete when the matrix explicitly requires them.'
}
[IO.File]::WriteAllText(
	[IO.Path]::GetFullPath($Output),
	($result | ConvertTo-Json -Depth 16),
	$utf8NoBom)
if($status -ne 'measured')
{
	exit 2
}
