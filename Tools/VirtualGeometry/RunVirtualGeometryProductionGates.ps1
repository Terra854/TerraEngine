[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)]
	[string]$EvidenceDirectory,
	[Parameter(Mandatory = $true)]
	[string]$Output,
	[string]$InstallRoot = '',
	[string]$ProjectPath = '',
	[ValidateRange(6, 1000)]
	[int]$MinimumValidSamples = 6,
	[ValidateRange(30, 10080)]
	[int]$SoakMinutes = 30,
	[switch]$AllowUnavailableReference
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$resolvedEvidence = [IO.Path]::GetFullPath(
	$(if([IO.Path]::IsPathRooted($EvidenceDirectory)) {
		$EvidenceDirectory
	} else {
		Join-Path $repositoryRoot $EvidenceDirectory
	}))
$resolvedOutput = [IO.Path]::GetFullPath(
	$(if([IO.Path]::IsPathRooted($Output)) {
		$Output
	} else {
		Join-Path $repositoryRoot $Output
	}))
if([string]::IsNullOrWhiteSpace($InstallRoot))
{
	$InstallRoot = Join-Path $repositoryRoot 'out/build/RelWithDebInfo/Build'
}
$resolvedInstall = [IO.Path]::GetFullPath($InstallRoot)

function Read-JsonFile([string]$Path)
{
	try
	{
		return Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
	}
	catch
	{
		return $null
	}
}

# Evidence directories intentionally contain several JSON schemas. Read optional
# fields through the PSObject property table so strict mode can reject malformed
# values without aborting when a valid, unrelated evidence document omits them.
function Get-OptionalProperty($Document, [string]$Name)
{
	if($null -eq $Document)
	{
		return $null
	}
	$property = $Document.PSObject.Properties[$Name]
	if($null -eq $property)
	{
		return $null
	}
	return $property.Value
}

function New-Stage(
	[string]$Name,
	[bool]$Accepted,
	[string]$Status,
	[string[]]$Artifacts,
	[string[]]$Diagnostics)
{
	return [ordered]@{
		name = $Name
		accepted = $Accepted
		status = $Status
		artifacts = @($Artifacts)
		diagnostics = @($Diagnostics)
	}
}

function Get-ArtifactHash([string]$Path)
{
	if(-not (Test-Path -LiteralPath $Path -PathType Leaf))
	{
		return $null
	}
	return [ordered]@{
		path = [IO.Path]::GetFullPath($Path)
		bytes = (Get-Item -LiteralPath $Path).Length
		sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
	}
}

New-Item -ItemType Directory -Force -Path $resolvedEvidence | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $resolvedOutput) | Out-Null
$stages = [Collections.Generic.List[object]]::new()

$requiredInstalledFiles = @(
	'Editor/TerraEngine-Editor.exe',
	'Tools/TerraEngineVirtualGeometryAssetTool.exe',
	'Tools/TerraEngineVirtualGeometryBenchmark.exe',
	'Tools/TerraEngineVirtualGeometryCodecExperiment.exe',
	'Tools/VirtualGeometry/workloads.json',
	'Tools/VirtualGeometry/quality-profiles.json',
	'Tools/VirtualGeometry/hardware-matrix.json',
	'Tools/VirtualGeometry/CheckSemanticProductNaming.ps1',
	'Editor/Shaders/Includes/VirtualGeometry/VirtualGeometryProductionCodecAbi.hlsli',
	'Editor/Shaders/VirtualGeometry/VirtualGeometryProductionDecode.comp.spv',
	'Docs/VirtualGeometry.md'
)
$missingInstalled = @($requiredInstalledFiles | Where-Object {
	-not (Test-Path -LiteralPath (Join-Path $resolvedInstall $_) -PathType Leaf)
})
$stages.Add((New-Stage -Name 'installed-preflight' -Accepted ($missingInstalled.Count -eq 0) `
	-Status $(if($missingInstalled.Count -eq 0) { 'accepted' } else { 'missing-artifacts' }) `
	-Artifacts @($requiredInstalledFiles | ForEach-Object { Join-Path $resolvedInstall $_ }) `
	-Diagnostics @($missingInstalled | ForEach-Object { "missing-installed-artifact:$_" })))

$semanticScript = Join-Path $resolvedInstall 'Tools/VirtualGeometry/CheckSemanticProductNaming.ps1'
$semanticConfig = Join-Path $resolvedInstall 'Tools/VirtualGeometry/semantic-product-naming.json'
$semanticReport = Join-Path $resolvedEvidence 'semantic-product-naming.json'
$semanticAccepted = $false
$semanticDiagnostics = @()
if((Test-Path -LiteralPath $semanticScript -PathType Leaf) -and
	(Test-Path -LiteralPath $semanticConfig -PathType Leaf))
{
	& $semanticScript -Configuration $semanticConfig -ReportPath $semanticReport
	$semanticInvocationSucceeded = $?
	$semanticResult = Read-JsonFile $semanticReport
	$semanticAccepted = $semanticInvocationSucceeded -and
		(Get-OptionalProperty $semanticResult 'status') -eq 'passed' -and
		@((Get-OptionalProperty $semanticResult 'violations')).Count -eq 0
	if(-not $semanticAccepted) { $semanticDiagnostics += 'semantic-product-naming-failed' }
}
else
{
	$semanticDiagnostics += 'semantic-product-naming-unavailable'
}
$stages.Add((New-Stage -Name 'semantic-product-naming' -Accepted $semanticAccepted `
	-Status $(if($semanticAccepted) { 'accepted' } else { 'failed' }) `
	-Artifacts @($semanticReport) -Diagnostics $semanticDiagnostics))

$workloadPath = Join-Path $resolvedInstall 'Tools/VirtualGeometry/workloads.json'
$workloads = Read-JsonFile $workloadPath
$requiredWorkloads = @(
	'low-poly-unique-direct',
	'low-poly-repeated-direct',
	'low-poly-forced-virtual-geometry',
	'low-poly-automatic-routing',
	'dense-photogrammetry',
	'hard-surface-city',
	'foliage-forest',
	'production-geometry-classes',
	'adversarial-integrity-and-bounds'
)
$availableWorkloads = @(
	if($null -ne $workloads) {
		@($workloads.workloads | ForEach-Object { [string]$_.id })
	}
)
$missingWorkloads = @($requiredWorkloads | Where-Object { $_ -notin $availableWorkloads })
$stages.Add((New-Stage -Name 'workload-contract' -Accepted ($missingWorkloads.Count -eq 0) `
	-Status $(if($missingWorkloads.Count -eq 0) { 'accepted' } else { 'incomplete' }) `
	-Artifacts @($workloadPath) `
	-Diagnostics @($missingWorkloads | ForEach-Object { "missing-workload:$_" })))

$captureFiles = @(
	Get-ChildItem -LiteralPath $resolvedEvidence -Recurse -File -Filter '*.json' |
		Where-Object { $_.FullName -ne $resolvedOutput -and $_.FullName -ne $semanticReport }
)
$validCaptures = [Collections.Generic.List[object]]::new()
$captureRejections = [Collections.Generic.List[string]]::new()
foreach($captureFile in $captureFiles)
{
	$capture = Read-JsonFile $captureFile.FullName
	if($null -eq $capture) { continue }
	$rawSamples = Get-OptionalProperty $capture 'rawSamples'
	$comparability = Get-OptionalProperty $capture 'baselineAndComparability'
	$visibility = Get-OptionalProperty $capture 'visibilityAndShading'
	$sampleCount = 0
	if($null -ne $rawSamples) { $sampleCount = @($rawSamples).Count }
	$comparable = $null -ne $comparability -and
		(Get-OptionalProperty $comparability 'status') -in @('measured', 'comparable')
	$visualPassed = $null -ne $visibility -and
		(Get-OptionalProperty $visibility 'visualGatePassed') -eq $true
	if($sampleCount -ge $MinimumValidSamples -and $comparable -and $visualPassed)
	{
		$validCaptures.Add([ordered]@{
			path = $captureFile.FullName
			samples = $sampleCount
		})
	}
	elseif($null -ne $rawSamples)
	{
		$captureRejections.Add(
			"$($captureFile.Name): samples=$sampleCount comparable=$comparable visual=$visualPassed")
	}
}
$totalValidSamples = 0
foreach($validCapture in $validCaptures)
{
	$totalValidSamples += [int]$validCapture.samples
}
$matrixAccepted = $totalValidSamples -ge $MinimumValidSamples
$stages.Add((New-Stage -Name 'six-sample-performance-matrix' -Accepted $matrixAccepted `
	-Status $(if($matrixAccepted) { 'accepted' } else { 'insufficient' }) `
	-Artifacts @($validCaptures | ForEach-Object { $_.path }) `
	-Diagnostics @($captureRejections + @("accepted-measured-samples:$totalValidSamples"))))

$soakFiles = @(
	Get-ChildItem -LiteralPath $resolvedEvidence -Recurse -File -Filter '*soak*.json'
)
$acceptedSoak = $null
foreach($soakFile in $soakFiles)
{
	$soak = Read-JsonFile $soakFile.FullName
	if($null -eq $soak) { continue }
	$durationMinutes = Get-OptionalProperty $soak 'durationMinutes'
	$requiredDurationMinutes = Get-OptionalProperty $soak 'requiredDurationMinutes'
	$status = Get-OptionalProperty $soak 'status'
	$boundedGrowth = Get-OptionalProperty $soak 'boundedGrowth'
	$boundedCounters = Get-OptionalProperty $soak 'boundedCounters'
	$gateDurationSatisfied = Get-OptionalProperty $soak 'gateDurationSatisfied'
	$duration = if($null -ne $durationMinutes) {
		[int]$durationMinutes
	} elseif($null -ne $requiredDurationMinutes) {
		[int]$requiredDurationMinutes
	} else {
		0
	}
	# The production soak wrapper publishes measured-result-published only when
	# the full wall-clock duration, runtime duration, and bounded counters pass.
	$bounded = $boundedGrowth -eq $true -or
		$status -eq 'accepted' -or
		($status -eq 'measured-result-published' -and
			$boundedCounters -eq $true -and
			$gateDurationSatisfied -eq $true)
	if($duration -ge $SoakMinutes -and $bounded)
	{
		$acceptedSoak = $soakFile.FullName
		break
	}
}
$stages.Add((New-Stage -Name 'streaming-soak' -Accepted ($null -ne $acceptedSoak) `
	-Status $(if($null -ne $acceptedSoak) { 'accepted' } else { 'insufficient' }) `
	-Artifacts @($acceptedSoak) `
	-Diagnostics $(if($null -eq $acceptedSoak) { @("No bounded soak reached $SoakMinutes minutes.") } else { @() })))

$visualArtifacts = @(
	Get-ChildItem -LiteralPath $resolvedEvidence -Recurse -File |
		Where-Object { $_.Extension -in @('.png', '.bmp') }
)
$requiredVisualKinds = @('camera', 'shadow', 'capture', 'picking', 'editor')
$missingVisualKinds = @($requiredVisualKinds | Where-Object {
	$kind = $_
	-not ($visualArtifacts | Where-Object { $_.BaseName -match $kind })
})
$stages.Add((New-Stage -Name 'live-visual' -Accepted ($missingVisualKinds.Count -eq 0) `
	-Status $(if($missingVisualKinds.Count -eq 0) { 'accepted' } else { 'insufficient' }) `
	-Artifacts @($visualArtifacts | ForEach-Object { $_.FullName }) `
	-Diagnostics @($missingVisualKinds | ForEach-Object { "missing-visual:$_" })))

$referenceFiles = @(
	Get-ChildItem -LiteralPath $resolvedEvidence -Recurse -File -Filter '*reference*.json'
)
$referenceClassification = 'insufficient'
$referenceArtifact = $null
foreach($referenceFile in $referenceFiles)
{
	$reference = Read-JsonFile $referenceFile.FullName
	$classification = Get-OptionalProperty $reference 'classification'
	if($null -eq $classification) { continue }
	$referenceClassification = [string]$classification
	$referenceArtifact = $referenceFile.FullName
	break
}
$referenceAccepted = $referenceClassification -in @('superiority', 'parity')
if($AllowUnavailableReference -and
	$referenceClassification -in @('incomparable', 'insufficient'))
{
	# The opt-in allows the aggregate product gate to finish when no eligible
	# external reference exists. The original fail-closed classification stays
	# in the evidence, so an unavailable comparison can never become a parity or
	# superiority claim.
	$referenceAccepted = $true
}
$stages.Add((New-Stage -Name 'matched-reference' -Accepted $referenceAccepted `
	-Status $referenceClassification `
	-Artifacts @($referenceArtifact) `
	-Diagnostics $(if($referenceAccepted) { @() } else { @('matched-reference-evidence-failed-closed') })))

$vendorNeutralityFiles = @(
	Get-ChildItem -LiteralPath $resolvedEvidence -Recurse -File -Filter '*.json' |
		Where-Object {
			$_.FullName -match '(?i)vendor[-_ ]?neutral'
		}
)
$vendorNeutralityAccepted = $false
foreach($vendorFile in $vendorNeutralityFiles)
{
	$vendor = Read-JsonFile $vendorFile.FullName
	if($null -ne $vendor -and
		(Get-OptionalProperty $vendor 'status') -eq 'accepted' -and
		(Get-OptionalProperty $vendor 'dx12References') -eq 0 -and
		(Get-OptionalProperty $vendor 'nvidiaSpecificReferences') -eq 0)
	{
		$vendorNeutralityAccepted = $true
		break
	}
}
$stages.Add((New-Stage -Name 'vendor-neutrality' -Accepted $vendorNeutralityAccepted `
	-Status $(if($vendorNeutralityAccepted) { 'accepted' } else { 'insufficient' }) `
	-Artifacts @($vendorNeutralityFiles | ForEach-Object { $_.FullName }) `
	-Diagnostics $(if($vendorNeutralityAccepted) { @() } else { @('vendor-neutrality-proof-missing') })))

$requiredStageNames = @(
	'installed-preflight',
	'semantic-product-naming',
	'workload-contract',
	'six-sample-performance-matrix',
	'streaming-soak',
	'live-visual',
	'matched-reference',
	'vendor-neutrality'
)
$failedStages = @($stages | Where-Object {
	$_.name -in $requiredStageNames -and -not $_.accepted
})
$artifactHashes = @(
	$stages.artifacts |
		Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
		Sort-Object -Unique |
		ForEach-Object { Get-ArtifactHash $_ } |
		Where-Object { $null -ne $_ }
)
$result = [ordered]@{
	schemaVersion = 1
	product = 'virtual-geometry-production'
	status = if($failedStages.Count -eq 0) { 'accepted' } else { 'failed' }
	generatedAtUtc = [DateTime]::UtcNow.ToString('o')
	installRoot = $resolvedInstall
	projectPath = $ProjectPath
	graphicsPath = 'Vulkan'
	dx12Fallback = $false
	vendorExtrapolation = $false
	minimumValidSamples = $MinimumValidSamples
	requiredSoakMinutes = $SoakMinutes
	command = $MyInvocation.Line
	stages = @($stages)
	artifactHashes = $artifactHashes
	failedStages = @($failedStages | ForEach-Object { $_.name })
}
$result | ConvertTo-Json -Depth 64 |
	Set-Content -LiteralPath $resolvedOutput -Encoding utf8
if($failedStages.Count -ne 0) { exit 2 }
