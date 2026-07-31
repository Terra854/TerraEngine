[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)][string]$Terra,
	[Parameter(Mandatory = $true)][string]$Reference,
	[Parameter(Mandatory = $true)][string]$Output,
	[string]$VisualEvidence = '',
	[ValidateRange(0.0, 1.0)][double]$SuperiorityMargin = 0.10,
	[ValidateRange(0.0, 1.0)][double]$ParityRegressionLimit = 0.10
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

function Get-Provenance($Capture)
{
	$identity = Get-NullableProperty $Capture 'identity'
	$environment = Get-NullableProperty $Capture 'environment'
	$workload = Get-NullableProperty $Capture 'workload'
	$provenance = Get-NullableProperty $Capture 'provenance'
	return [ordered]@{
		contentHash = Get-FirstValue @(
			(Get-NullableProperty $Capture 'contentHash'),
			(Get-NullableProperty $provenance 'contentHash'),
			(Get-NullableProperty $identity 'assetHash'))
		cameraPathHash = Get-FirstValue @(
			(Get-NullableProperty $Capture 'cameraPathHash'),
			(Get-NullableProperty $provenance 'cameraPathHash'),
			(Get-NullableProperty $workload 'cameraPathHash'))
		qualityProfileHash = Get-FirstValue @(
			(Get-NullableProperty $Capture 'qualityProfileHash'),
			(Get-NullableProperty $provenance 'qualityProfileHash'),
			(Get-NullableProperty $environment 'quality'))
		hardwareIdentity = Get-FirstValue @(
			(Get-NullableProperty $Capture 'hardwareIdentity'),
			(Get-NullableProperty $provenance 'hardwareIdentity'),
			(Get-NullableProperty $environment 'adapter'))
		graphicsPath = Get-FirstValue @(
			(Get-NullableProperty $Capture 'graphicsPath'),
			(Get-NullableProperty $provenance 'graphicsPath'),
			(Get-NullableProperty $environment 'backend'))
		runtimeRevision = Get-FirstValue @(
			(Get-NullableProperty $Capture 'runtimeRevision'),
			(Get-NullableProperty $provenance 'runtimeRevision'),
			(Get-NullableProperty $identity 'engineHash'))
		commandHash = Get-FirstValue @(
			(Get-NullableProperty $Capture 'commandHash'),
			(Get-NullableProperty $provenance 'commandHash'),
			(Get-NullableProperty $identity 'buildRecipeHash'))
	}
}

function Get-SampleIdentity($Sample)
{
	$identity = Get-NullableProperty $Sample 'identity'
	return @(
		[string](Get-NullableProperty $identity 'frameId'),
		[string](Get-NullableProperty $identity 'viewId'),
		[string](Get-NullableProperty $Sample 'queryIdentity'),
		[string](Get-NullableProperty $Sample 'executionEvidenceId'),
		[string](Get-NullableProperty $identity 'rendererGeneration')) -join '|'
}

function Get-ValidSamples($Capture)
{
	$rawSamples = @(Get-NullableProperty $Capture 'rawSamples')
	return @($rawSamples | Where-Object {
		$identity = Get-SampleIdentity $_
		(Get-NullableProperty $_ 'availability') -eq 'measured' -and
		(Get-NullableProperty $_ 'visualCorrect') -eq $true -and
		$null -ne (Get-NullableProperty $_ 'durationNanoseconds') -and
		$identity -notmatch '(^|\|)(\||$)' -and
		[string]::IsNullOrWhiteSpace(
			[string](Get-NullableProperty $_ 'rejectionReason'))
	} | Sort-Object { Get-SampleIdentity $_ })
}

function Get-Quantile([object[]]$Samples, [double]$Probability)
{
	if($Samples.Count -eq 0)
	{
		return $null
	}
	$values = @($Samples | ForEach-Object {
		[double](Get-NullableProperty $_ 'durationNanoseconds')
	} | Sort-Object)
	$index = [Math]::Min(
		$values.Count - 1,
		[Math]::Max(0, [Math]::Ceiling($values.Count * $Probability) - 1))
	return $values[[int]$index]
}

function Get-VisualGate([string]$Path)
{
	if([string]::IsNullOrWhiteSpace($Path) -or
		-not (Test-Path -LiteralPath $Path -PathType Leaf))
	{
		return [ordered]@{
			available = $false
			accepted = $false
			measuredFailure = $false
			reason = 'Matched decoded-pixel and correctness evidence is missing.'
			artifact = $null
		}
	}
	$visual = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
	$status = [string](Get-NullableProperty $visual 'status')
	$checks = Get-NullableProperty $visual 'checks'
	$requiredChecks = @(
		'imageTolerance',
		'silhouette',
		'coverage',
		'material',
		'shadow',
		'animation',
		'crackFree',
		'identity')
	$missingChecks = @($requiredChecks | Where-Object {
		(Get-NullableProperty $checks $_) -ne $true
	})
	$accepted = $status -eq 'accepted' -and $missingChecks.Count -eq 0
	return [ordered]@{
		available = $true
		accepted = $accepted
		measuredFailure = $status -eq 'mismatch' -or
			($status -eq 'accepted' -and $missingChecks.Count -ne 0)
		reason = $(if($accepted) {
			'All decoded-pixel, view, animation, crack, and identity checks passed.'
		} elseif($missingChecks.Count -ne 0) {
			'Visual/correctness checks missing or failed: ' + ($missingChecks -join ', ')
		} else {
			'Decoded-pixel visual comparison failed.'
		})
		artifact = (Resolve-Path -LiteralPath $Path).Path
		checks = $checks
	}
}

$outputDirectory = Split-Path -Parent $Output
if(-not [string]::IsNullOrWhiteSpace($outputDirectory))
{
	New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
}
try
{
	$terraCapture = Get-Content -Raw -LiteralPath $Terra | ConvertFrom-Json
	$referenceCapture = Get-Content -Raw -LiteralPath $Reference | ConvertFrom-Json
}
catch
{
	$rejected = [ordered]@{
		schemaVersion = 2
		classification = 'insufficient'
		reason = 'candidate-or-reference-capture-unreadable-or-malformed-json'
		visual = @{ visualFirst = $true; accepted = $false }
		unavailableIsNotZero = $true
		rejectedInputs = @($Terra, $Reference)
	}
	$rejected | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $Output -Encoding utf8
	exit 2
}
$terraProvenance = Get-Provenance $terraCapture
$referenceProvenance = Get-Provenance $referenceCapture
$normalizedReferenceClassification = [string](Get-NullableProperty $referenceCapture 'classification')
$normalizedReferenceSet = [string](Get-NullableProperty $referenceCapture 'status') -eq 'normalized-reference-set'
$provenanceMismatches = @($terraProvenance.Keys | Where-Object {
	[string]$terraProvenance[$_] -ne [string]$referenceProvenance[$_]
})
$terraSamples = @(Get-ValidSamples $terraCapture)
$referenceSamples = @(Get-ValidSamples $referenceCapture)
$pairMismatches = [Collections.Generic.List[object]]::new()
if($terraSamples.Count -ne $referenceSamples.Count)
{
	$pairMismatches.Add('sample-count')
}
else
{
	for($index = 0; $index -lt $terraSamples.Count; ++$index)
	{
		if((Get-SampleIdentity $terraSamples[$index]) -ne
			(Get-SampleIdentity $referenceSamples[$index]))
		{
			$pairMismatches.Add($index)
		}
	}
}

$visualGate = Get-VisualGate $VisualEvidence
$classification = 'insufficient'
$reason = 'At least six fully measured, visually accepted, exactly paired samples are required.'
$terraMedian = Get-Quantile $terraSamples 0.50
$referenceMedian = Get-Quantile $referenceSamples 0.50
$terraP95 = Get-Quantile $terraSamples 0.95
$referenceP95 = Get-Quantile $referenceSamples 0.95
$terraP99 = Get-Quantile $terraSamples 0.99
$referenceP99 = Get-Quantile $referenceSamples 0.99
$improvement = $null

if($normalizedReferenceSet -and $normalizedReferenceClassification -eq 'incomparable')
{
	$classification = 'incomparable'
	$reason = 'Normalized reference captures disagree on exact provenance.'
}
elseif($normalizedReferenceSet -and $normalizedReferenceClassification -eq 'insufficient')
{
	$classification = 'insufficient'
	$reason = 'Normalized reference captures retain insufficient or rejected evidence.'
}
elseif($referenceProvenance.graphicsPath.ToLowerInvariant() -ne 'vulkan')
{
	$classification = 'incomparable'
	$reason = 'The reference did not prove a supported Vulkan graphics path.'
}
elseif($provenanceMismatches.Count -ne 0)
{
	$classification = 'incomparable'
	$reason = 'Exact capture provenance mismatch: ' +
		($provenanceMismatches -join ', ')
}
elseif(-not $visualGate.available)
{
	$classification = 'insufficient'
	$reason = $visualGate.reason
}
elseif(-not $visualGate.accepted)
{
	$classification = $(if($visualGate.measuredFailure) {
		'regression'
	} else {
		'insufficient'
	})
	$reason = $visualGate.reason
}
elseif($terraSamples.Count -ge 6 -and $referenceSamples.Count -ge 6 -and
	$pairMismatches.Count -eq 0)
{
	if($null -eq $terraMedian -or $null -eq $referenceMedian -or
		$referenceMedian -le 0.0)
	{
		$classification = 'insufficient'
		$reason = 'A valid positive reference median is unavailable.'
	}
	else
	{
		$improvement = ($referenceMedian - $terraMedian) / $referenceMedian
		$tailRegression = $null -eq $referenceP95 -or $referenceP95 -le 0.0 -or
			(($terraP95 - $referenceP95) / $referenceP95) -gt
				$ParityRegressionLimit
		if($tailRegression)
		{
			$classification = 'regression'
			$reason = 'The protected p95 frame-time limit regressed.'
		}
		elseif($improvement -ge $SuperiorityMargin)
		{
			$classification = 'superiority'
			$reason = 'Visual/correctness gates passed and median frame time cleared the workload-specific superiority threshold.'
		}
		elseif($improvement -ge (-1.0 * $ParityRegressionLimit))
		{
			$classification = 'parity'
			$reason = 'Visual/correctness gates and protected median/tail margins passed.'
		}
		else
		{
			$classification = 'regression'
			$reason = 'The protected median frame-time limit regressed.'
		}
	}
}

$workload = Get-NullableProperty $terraCapture 'workload'
$result = [ordered]@{
	schemaVersion = 2
	pairIdentity = (
		[IO.Path]::GetFileNameWithoutExtension($Terra) + '--' +
		[IO.Path]::GetFileNameWithoutExtension($Reference))
	workloadIdentity = [string](Get-NullableProperty $workload 'name')
	classification = $classification
	reason = $reason
	visual = $visualGate
	metrics = @(
		[ordered]@{
			name = 'totalFrameNanoseconds'
			candidateMedian = $terraMedian
			referenceMedian = $referenceMedian
			candidateP95 = $terraP95
			referenceP95 = $referenceP95
			candidateP99 = $terraP99
			referenceP99 = $referenceP99
			improvement = $improvement
			protected = $true
		})
	thresholds = [ordered]@{
		superiorityMargin = $SuperiorityMargin
		parityRegressionLimit = $ParityRegressionLimit
		minimumValidSamplesPerSide = 6
	}
	rawCandidateSamples = @((Get-NullableProperty $terraCapture 'rawSamples'))
	rawReferenceSamples = @((Get-NullableProperty $referenceCapture 'rawSamples'))
	provenance = @{
		candidate = $terraProvenance
		reference = $referenceProvenance
		mismatches = $provenanceMismatches
	}
	pairMismatches = @($pairMismatches)
	unavailableIsNotZero = $true
}
$result | ConvertTo-Json -Depth 64 |
	Set-Content -LiteralPath $Output -Encoding utf8
if($classification -in @('regression', 'incomparable', 'insufficient'))
{
	exit 2
}
