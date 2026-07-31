[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)]
	[Alias('Input')]
	[string[]]$CapturePath,
	[Parameter(Mandatory = $true)]
	[string]$Output
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-NullableProperty($Object, [string]$Name)
{
	if($null -eq $Object)
	{
		return $null
	}
	$property = $Object.PSObject.Properties[$Name]
	return $(if($null -eq $property) { $null } else { $property.Value })
}

function Get-FirstValue([object[]]$Values)
{
	foreach($value in $Values)
	{
		if($null -ne $value -and
			-not [string]::IsNullOrWhiteSpace([string]$value))
		{
			return [string]$value
		}
	}
	return ''
}

function Get-NormalizedCapture([string]$Path)
{
	if(-not (Test-Path -LiteralPath $Path -PathType Leaf))
	{
		return [ordered]@{
			source = [IO.Path]::GetFullPath($Path)
			classification = 'insufficient'
			reason = 'reference-capture-missing'
			provenance = $null
			rawSamples = @()
			rejectedSamples = @()
			unavailableSamples = @()
			validSampleCount = 0
		}
	}

	try
	{
		$capture = Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
	}
	catch
	{
		return [ordered]@{
			source = [IO.Path]::GetFullPath($Path)
			classification = 'insufficient'
			reason = 'reference-capture-unreadable-or-malformed-json'
			provenance = $null
			rawSamples = @()
			rejectedSamples = @(@{ reason = 'unreadable-or-malformed-json' })
			unavailableSamples = @()
			validSampleCount = 0
		}
	}
	$identity = Get-NullableProperty $capture 'identity'
	$environment = Get-NullableProperty $capture 'environment'
	$workload = Get-NullableProperty $capture 'workload'
	$pathProof = Get-NullableProperty $capture 'vulkanPathProof'
	$provenance = [ordered]@{
		contentHash = Get-FirstValue @(
			(Get-NullableProperty $capture 'contentHash'),
			(Get-NullableProperty $identity 'assetHash'))
		cameraPathHash = Get-FirstValue @(
			(Get-NullableProperty $capture 'cameraPathHash'),
			(Get-NullableProperty $workload 'cameraPathHash'))
		qualityProfileHash = Get-FirstValue @(
			(Get-NullableProperty $capture 'qualityProfileHash'),
			(Get-NullableProperty $environment 'quality'))
		hardwareIdentity = Get-FirstValue @(
			(Get-NullableProperty $capture 'hardwareIdentity'),
			(Get-NullableProperty $environment 'adapter'))
		graphicsPath = Get-FirstValue @(
			(Get-NullableProperty $capture 'graphicsPath'),
			(Get-NullableProperty $environment 'backend'))
		runtimeRevision = Get-FirstValue @(
			(Get-NullableProperty $capture 'runtimeRevision'),
			(Get-NullableProperty $identity 'engineHash'))
		commandHash = Get-FirstValue @(
			(Get-NullableProperty $capture 'commandHash'),
			(Get-NullableProperty $identity 'buildRecipeHash'))
		referenceRelease = Get-FirstValue @(
			(Get-NullableProperty $capture 'referenceRelease'),
			(Get-NullableProperty $identity 'release'))
		referenceBuild = Get-FirstValue @(
			(Get-NullableProperty $capture 'referenceBuild'),
			(Get-NullableProperty $identity 'build'))
		projectIdentity = Get-FirstValue @(
			(Get-NullableProperty $capture 'projectIdentity'),
			(Get-NullableProperty $identity 'projectHash'))
		executableIdentity = Get-FirstValue @(
			(Get-NullableProperty $capture 'executableIdentity'),
			(Get-NullableProperty $identity 'executableHash'))
		vulkanCapabilityState = Get-FirstValue @(
			(Get-NullableProperty $capture 'vulkanCapabilityState'),
			(Get-NullableProperty $pathProof 'state'))
		workloadIdentity = Get-FirstValue @(
			(Get-NullableProperty $capture 'workloadIdentity'),
			(Get-NullableProperty $workload 'name'))
	}
	$required = @(
		'contentHash',
		'cameraPathHash',
		'qualityProfileHash',
		'hardwareIdentity',
		'graphicsPath',
		'runtimeRevision',
		'commandHash',
		'referenceRelease',
		'referenceBuild',
		'projectIdentity',
		'executableIdentity',
		'vulkanCapabilityState',
		'workloadIdentity')
	$missing = @($required | Where-Object {
		[string]::IsNullOrWhiteSpace([string]$provenance[$_])
	})
	$raw = @((Get-NullableProperty $capture 'rawSamples'))
	$rejected = @($raw | Where-Object {
		(Get-NullableProperty $_ 'availability') -ne 'measured' -or
		-not [string]::IsNullOrWhiteSpace(
			[string](Get-NullableProperty $_ 'rejectionReason'))
	})
	$unavailable = @($raw | Where-Object {
		(Get-NullableProperty $_ 'availability') -in
			@('unavailable', 'pending', 'invalid')
	})
	$valid = @($raw | Where-Object {
		(Get-NullableProperty $_ 'availability') -eq 'measured' -and
		[string]::IsNullOrWhiteSpace(
			[string](Get-NullableProperty $_ 'rejectionReason'))
	})

	$classification = 'imported'
	$reason = 'reference-capture-normalized'
	if($missing.Count -ne 0)
	{
		$classification = 'insufficient'
		$reason = 'missing-provenance:' + ($missing -join ',')
	}
	elseif($provenance.graphicsPath.ToLowerInvariant() -ne 'vulkan')
	{
		$classification = 'incomparable'
		$reason = 'reference-graphics-path-is-not-vulkan'
	}
	elseif($provenance.vulkanCapabilityState.ToLowerInvariant() -in
		@('unsupported', 'experimental', 'semantically-different'))
	{
		$classification = 'incomparable'
		$reason = 'reference-vulkan-capability-is-not-production-equivalent'
	}
	elseif($provenance.vulkanCapabilityState.ToLowerInvariant() -ne
		'production-supported')
	{
		$classification = 'insufficient'
		$reason = 'reference-vulkan-capability-proof-missing'
	}
	elseif($valid.Count -lt 6)
	{
		$classification = 'insufficient'
		$reason = 'fewer-than-six-valid-reference-samples'
	}

	return [ordered]@{
		source = (Resolve-Path -LiteralPath $Path).Path
		classification = $classification
		reason = $reason
		provenance = $provenance
		rawSamples = $raw
		rejectedSamples = $rejected
		unavailableSamples = $unavailable
		validSampleCount = $valid.Count
	}
}

$captures = @($CapturePath | ForEach-Object {
	Get-NormalizedCapture $_
})
$classification = 'imported'
if($captures.classification -contains 'incomparable')
{
	$classification = 'incomparable'
}
elseif($captures.classification -contains 'insufficient')
{
	$classification = 'insufficient'
}
$provenanceFields = @(
	'contentHash', 'cameraPathHash', 'qualityProfileHash', 'hardwareIdentity',
	'graphicsPath', 'runtimeRevision', 'commandHash', 'referenceRelease',
	'referenceBuild', 'projectIdentity', 'executableIdentity',
	'vulkanCapabilityState', 'workloadIdentity')
$provenance = $null
$provenanceMismatches = @()
if($captures.Count -gt 0)
{
	$provenance = $captures[0].provenance
	foreach($capture in $captures | Select-Object -Skip 1)
	{
		foreach($field in $provenanceFields)
		{
			if([string]$capture.provenance[$field] -ne [string]$provenance[$field])
			{
				$provenanceMismatches += $field
			}
		}
	}
	$provenanceMismatches = @($provenanceMismatches | Sort-Object -Unique)
}
if($provenanceMismatches.Count -ne 0)
{
	$classification = 'incomparable'
}
$rawSamples = @($captures | ForEach-Object { $_.rawSamples })
$rejectedSamples = @($captures | ForEach-Object { $_.rejectedSamples })
$unavailableSamples = @($captures | ForEach-Object { $_.unavailableSamples })
$record = [ordered]@{
	schemaVersion = 2
	status = 'normalized-reference-set'
	classification = $classification
	provenance = $provenance
	provenanceMismatches = $provenanceMismatches
	rawSamples = $rawSamples
	validSampleCount = @($rawSamples | Where-Object {
		(Get-NullableProperty $_ 'availability') -eq 'measured' -and
		[string]::IsNullOrWhiteSpace([string](Get-NullableProperty $_ 'rejectionReason'))
	}).Count
	captures = $captures
	rejectedSamples = $rejectedSamples
	unavailableSamples = $unavailableSamples
	minimumValidSamplesPerWorkload = 6
	unavailableIsNotZero = $true
}
$outputDirectory = Split-Path -Parent $Output
if(-not [string]::IsNullOrWhiteSpace($outputDirectory))
{
	New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}
$record | ConvertTo-Json -Depth 64 |
	Set-Content -LiteralPath $Output -Encoding utf8
if($classification -ne 'imported')
{
	exit 2
}
