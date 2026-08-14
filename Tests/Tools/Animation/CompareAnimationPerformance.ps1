[CmdletBinding(DefaultParameterSetName = "Compare")]
param(
	[Parameter(Mandatory = $true, ParameterSetName = "Compare")]
	[ValidateNotNullOrEmpty()]
	[string]$CandidateManifest,

	[Parameter(Mandatory = $true, ParameterSetName = "Compare")]
	[ValidateNotNullOrEmpty()]
	[string]$OutputPath,

	[Parameter(ParameterSetName = "Compare")]
	[ValidateNotNullOrEmpty()]
	[string]$BaselinePath = "out/validation/animation/baseline/locked-baseline.json",

	[Parameter(ParameterSetName = "Compare")]
	[ValidateNotNullOrEmpty()]
	[string]$WorkloadsPath = "Tests/Tools/Animation/PerformanceWorkloads.json",

	[Parameter(ParameterSetName = "Compare")]
	[ValidateNotNullOrEmpty()]
	[string]$CoverageMatrixPath = "Tests/Tools/Animation/PerformanceCoverageMatrix.json",

	# Whole-engine coverage must opt in explicitly. The default comparator validates
	# matrix readiness but does not claim the whole-engine comparison was run.
	[Parameter(ParameterSetName = "Compare")]
	[switch]$FinalPhaseMatrix,

	# ValidateOnly intentionally keeps every acceptance check active. It labels a
	# dry comparison without creating a weaker route around a regression.
	[Parameter(ParameterSetName = "Compare")]
	[switch]$ValidateOnly,

	[Parameter(Mandatory = $true, ParameterSetName = "SelfTest")]
	[switch]$SelfTest
)

# This comparator consumes only repository files and normalized JSON evidence.
# StrictMode turns misspelled or absent properties into explicit failures instead
# of allowing a missing value to look like a measured zero.
Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$script:ExitAccepted = 0
$script:ExitRegression = 2
$script:ExitInvalidEvidence = 3
$script:ExitToolFailure = 4
$script:ExpectedBaselineSha256 = "cf4375934729e1afa745d9446faacfc1a7ba53a6d7db79b3d1e0e8164a5ced8f"
$script:ExpectedEvidenceLockSha256 = "0605b4d4241bda8ba3fd4743b2ba7a6c6b217aaf1c9d83fe669125493ddb4e41"
$script:ExpectedEvidenceAggregateSha256 = "4020949bd881f94f0395853e947e4512fb587c7bdd1ce5592317e75c01b5fcb5"
$script:ExpectedContractSha256 = "774c6c7a40da4ebb8fa67feb3649e956235669428d4c50ad43cdb2dceca1a801"
$script:ExpectedRendererManifestSha256 = "3230959fab8c628d9396bd31a7923672c3a1ebf5b51ff4998066a8f10c2b3e4c"
$script:ExpectedWorkloadsSha256 = "add34eb7debacbaddb03c07de115cf00f7935a3e62e849cf4b370f2cbf1bd707"
$script:ExpectedCoverageMatrixSha256 = "e9a861037edf63207368d9e31b3a85a32bdf3723cdfb28eeaa7b5223c5a64670"
$script:BaselineInventoryCache = $null

$script:RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot "../../.."))
$script:OutRoot = [System.IO.Path]::GetFullPath((Join-Path $script:RepoRoot "out"))
$script:BaselineRoot = [System.IO.Path]::GetFullPath((Join-Path $script:RepoRoot "out/validation/animation/baseline"))
$script:DefaultBaselinePath = [System.IO.Path]::GetFullPath((Join-Path $script:RepoRoot "out/validation/animation/baseline/locked-baseline.json"))
$script:DefaultWorkloadsPath = [System.IO.Path]::GetFullPath((Join-Path $script:RepoRoot "Tests/Tools/Animation/PerformanceWorkloads.json"))
$script:DefaultCoverageMatrixPath = [System.IO.Path]::GetFullPath((Join-Path $script:RepoRoot "Tests/Tools/Animation/PerformanceCoverageMatrix.json"))
$script:ContractPath = [System.IO.Path]::GetFullPath((Join-Path $script:RepoRoot "Docs/Animation/README.md"))
$script:RendererManifestPath = [System.IO.Path]::GetFullPath((Join-Path $script:RepoRoot "Tests/Fixtures/RendererPerformanceBaseline/workloads.json"))
$script:EvidenceLockPath = [System.IO.Path]::GetFullPath((Join-Path $script:RepoRoot "out/validation/animation/baseline/evidence-lock.json"))

function Resolve-RepositoryPath {
	param(
		[Parameter(Mandatory = $true)]
		[string]$Path
	)

	if ([System.IO.Path]::IsPathRooted($Path)) {
		return [System.IO.Path]::GetFullPath($Path)
	}
	return [System.IO.Path]::GetFullPath((Join-Path $script:RepoRoot $Path))
}

function Test-PathIsBelow {
	param(
		[Parameter(Mandatory = $true)]
		[string]$Parent,

		[Parameter(Mandatory = $true)]
		[string]$Child
	)

	$parentPrefix = [System.IO.Path]::GetFullPath($Parent).TrimEnd(
		[System.IO.Path]::DirectorySeparatorChar,
		[System.IO.Path]::AltDirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
	$childPath = [System.IO.Path]::GetFullPath($Child)
	return $childPath.StartsWith($parentPrefix, [System.StringComparison]::OrdinalIgnoreCase)
}

function Assert-SafeCandidatePath {
	param(
		[Parameter(Mandatory = $true)]
		[string]$Path,

		[Parameter(Mandatory = $true)]
		[string]$Description
	)

	if (-not (Test-PathIsBelow -Parent $script:OutRoot -Child $Path)) {
		throw "$Description must stay below '$($script:OutRoot)'; received '$Path'."
	}
	if (Test-PathIsBelow -Parent $script:BaselineRoot -Child $Path) {
		throw "$Description may not use the immutable baseline directory; received '$Path'."
	}
}

function ConvertTo-PortablePath {
	param(
		[Parameter(Mandatory = $true)]
		[string]$Path
	)

	return [System.IO.Path]::GetFullPath($Path).Replace("\", "/")
}

function ConvertTo-RepositoryRelativePath {
	param(
		[Parameter(Mandatory = $true)]
		[string]$Path
	)

	$absolutePath = [System.IO.Path]::GetFullPath($Path)
	if (-not (Test-PathIsBelow -Parent $script:RepoRoot -Child $absolutePath)) {
		return (ConvertTo-PortablePath -Path $absolutePath)
	}
	return $absolutePath.Substring($script:RepoRoot.Length + 1).Replace("\", "/")
}

function Get-FileSha256 {
	param(
		[Parameter(Mandatory = $true)]
		[string]$Path
	)

	if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
		throw "Required file was not found: '$Path'."
	}
	return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

function Get-TextSha256 {
	param(
		[Parameter(Mandatory = $true)]
		[string]$Text
	)

	$algorithm = [System.Security.Cryptography.SHA256]::Create()
	try {
		$encoding = New-Object System.Text.UTF8Encoding($false)
		$bytes = $encoding.GetBytes($Text)
		return (($algorithm.ComputeHash($bytes) | ForEach-Object { $_.ToString("x2") }) -join "")
	}
	finally {
		$algorithm.Dispose()
	}
}

function Read-JsonFile {
	param(
		[Parameter(Mandatory = $true)]
		[string]$Path
	)

	if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
		throw "Required JSON file was not found: '$Path'."
	}
	try {
		return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
	}
	catch {
		throw "JSON file '$Path' could not be parsed: $($_.Exception.Message)"
	}
}

function Write-DeterministicJson {
	param(
		[Parameter(Mandatory = $true)]
		[string]$Path,

		[Parameter(Mandatory = $true)]
		[AllowNull()]
		$Value
	)

	$absolutePath = [System.IO.Path]::GetFullPath($Path)
	Assert-SafeCandidatePath -Path $absolutePath -Description "JSON output"
	$directory = Split-Path -Parent $absolutePath
	New-Item -ItemType Directory -Force -Path $directory | Out-Null
	$json = $Value | ConvertTo-Json -Depth 100
	$json = $json.Replace("`r`n", "`n") + "`n"
	$encoding = New-Object System.Text.UTF8Encoding($false)
	[System.IO.File]::WriteAllText($absolutePath, $json, $encoding)
}

function Test-HasProperty {
	param(
		[Parameter(Mandatory = $true)]
		$Object,

		[Parameter(Mandatory = $true)]
		[string]$Name
	)

	if ($null -eq $Object) {
		return $false
	}
	if ($Object -is [System.Collections.IDictionary]) {
		return $Object.Contains($Name)
	}
	return $null -ne $Object.PSObject.Properties[$Name]
}

function Get-PropertyValue {
	param(
		[Parameter(Mandatory = $true)]
		$Object,

		[Parameter(Mandatory = $true)]
		[string]$Name
	)

	if (-not (Test-HasProperty -Object $Object -Name $Name)) {
		throw "Required property '$Name' is missing."
	}
	if ($Object -is [System.Collections.IDictionary]) {
		return $Object[$Name]
	}
	return $Object.PSObject.Properties[$Name].Value
}

function Get-NestedValue {
	param(
		[Parameter(Mandatory = $true)]
		$Object,

		[Parameter(Mandatory = $true)]
		[string]$Path
	)

	$current = $Object
	foreach ($part in $Path.Split(".")) {
		$current = Get-PropertyValue -Object $current -Name $part
	}
	return $current
}

function ConvertTo-CanonicalValue {
	param(
		[AllowNull()]
		$Value
	)

	if ($null -eq $Value) {
		return $null
	}
	if ($Value -is [string] -or $Value -is [char] -or $Value -is [bool] -or $Value -is [ValueType]) {
		return $Value
	}
	if ($Value -is [System.Collections.IDictionary]) {
		$ordered = [ordered]@{}
		$keys = [string[]]@($Value.Keys | ForEach-Object { [string]$_ })
		[Array]::Sort($keys, [System.StringComparer]::Ordinal)
		foreach ($key in $keys) {
			$ordered[$key] = ConvertTo-CanonicalValue -Value $Value[$key]
		}
		return $ordered
	}
	if ($Value -is [System.Collections.IEnumerable]) {
		$items = @()
		foreach ($item in $Value) {
			$items += ,(ConvertTo-CanonicalValue -Value $item)
		}
		return ,$items
	}
	$properties = [string[]]@($Value.PSObject.Properties | ForEach-Object { $_.Name })
	[Array]::Sort($properties, [System.StringComparer]::Ordinal)
	$canonicalObject = [ordered]@{}
	foreach ($property in $properties) {
		$canonicalObject[$property] = ConvertTo-CanonicalValue -Value $Value.PSObject.Properties[$property].Value
	}
	return $canonicalObject
}

function Get-CanonicalJson {
	param(
		[AllowNull()]
		$Value
	)

	return (ConvertTo-CanonicalValue -Value $Value) | ConvertTo-Json -Compress -Depth 100
}

function Test-DeepEqual {
	param(
		[AllowNull()]
		$Left,

		[AllowNull()]
		$Right
	)

	return (Get-CanonicalJson -Value $Left) -ceq (Get-CanonicalJson -Value $Right)
}

function Test-FiniteNumber {
	param(
		[AllowNull()]
		$Value
	)

	if ($null -eq $Value -or $Value -is [bool] -or $Value -is [string] -or $Value -is [char]) {
		return $false
	}
	try {
		$number = [double]$Value
		return -not [double]::IsNaN($number) -and -not [double]::IsInfinity($number)
	}
	catch {
		return $false
	}
}

function Get-ForbiddenCandidateProperties {
	param(
		[AllowNull()]
		$Value,

		[string]$Path = "$"
	)

	$forbiddenNames = @(
		"threshold",
		"thresholds",
		"thresholdOverride",
		"thresholdOverrides",
		"comparisonPolicy",
		"minimumCandidateRuns",
		"minimumMeasuredRuns",
		"warmupRequired",
		"acceptanceLimits",
		"tolerances",
		"limits"
	)
	$matches = @()
	if ($null -eq $Value -or $Value -is [string] -or $Value -is [ValueType]) {
		return $matches
	}
	if ($Value -is [System.Collections.IEnumerable] -and -not ($Value -is [System.Collections.IDictionary])) {
		$index = 0
		foreach ($item in $Value) {
			$matches += @(Get-ForbiddenCandidateProperties -Value $item -Path "$Path[$index]")
			++$index
		}
		return $matches
	}
	$propertyNames = if ($Value -is [System.Collections.IDictionary]) {
		@($Value.Keys | ForEach-Object { [string]$_ })
	}
	else {
		@($Value.PSObject.Properties | ForEach-Object { $_.Name })
	}
	foreach ($propertyName in $propertyNames) {
		$propertyPath = "$Path.$propertyName"
		if ($forbiddenNames -ccontains $propertyName) {
			$matches += $propertyPath
		}
		$propertyValue = if ($Value -is [System.Collections.IDictionary]) {
			$Value[$propertyName]
		}
		else {
			$Value.PSObject.Properties[$propertyName].Value
		}
		$matches += @(Get-ForbiddenCandidateProperties -Value $propertyValue -Path $propertyPath)
	}
	return $matches
}

function Get-PathRecord {
	param(
		[Parameter(Mandatory = $true)]
		[string]$DeclaredPath,

		[Parameter(Mandatory = $true)]
		[string]$Kind,

		[switch]$CandidatePath
	)

	$resolvedPath = Resolve-RepositoryPath -Path $DeclaredPath
	if ($CandidatePath) {
		Assert-SafeCandidatePath -Path $resolvedPath -Description $Kind
	}
	if (-not (Test-Path -LiteralPath $resolvedPath -PathType Leaf)) {
		throw "$Kind was not found: '$resolvedPath'."
	}
	return [ordered]@{
		kind = $Kind
		declaredPath = $DeclaredPath
		resolvedPath = ConvertTo-PortablePath -Path $resolvedPath
		sha256 = Get-FileSha256 -Path $resolvedPath
		bytes = (Get-Item -LiteralPath $resolvedPath).Length
	}
}

function Get-BaselineEvidenceInventory {
	if ($null -ne $script:BaselineInventoryCache) {
		return $script:BaselineInventoryCache
	}

	# evidence-lock.json excludes generated project subtrees because they contain
	# large disposable build products. Reproduce the exact retained selection so a
	# changed log, summary, capture, or lock manifest invalidates comparability.
	$recordsByPath = @{}
	$totalBytes = [uint64]0
	$files = @(Get-ChildItem -LiteralPath $script:BaselineRoot -Recurse -File | Where-Object {
		$relativePath = $_.FullName.Substring($script:BaselineRoot.Length + 1).Replace("\", "/")
		$relativePath -cne "evidence-lock.json" -and $relativePath -notmatch "(^|/)project/"
	})
	# The reference-baseline lock used PowerShell's stable Sort-Object ordering. Reuse it byte
	# for byte so the documented aggregate can be independently reproduced.
	$relativePaths = [string[]]@($files | ForEach-Object { $_.FullName.Substring($script:BaselineRoot.Length + 1).Replace("\", "/") } | Sort-Object)
	$aggregateBuilder = New-Object System.Text.StringBuilder
	$inventory = @()
	foreach ($relativePath in $relativePaths) {
		$absolutePath = [System.IO.Path]::GetFullPath((Join-Path $script:BaselineRoot $relativePath))
		$file = Get-Item -LiteralPath $absolutePath
		$hash = Get-FileSha256 -Path $absolutePath
		[void]$aggregateBuilder.Append($relativePath).Append("`t").Append($hash).Append("`n")
		$totalBytes += [uint64]$file.Length
		$record = [ordered]@{
			relativePath = $relativePath
			resolvedPath = ConvertTo-PortablePath -Path $absolutePath
			sha256 = $hash
			bytes = $file.Length
		}
		$inventory += ,$record
		$recordsByPath[$relativePath.ToLowerInvariant()] = $record
	}
	$script:BaselineInventoryCache = [PSCustomObject][ordered]@{
		fileCount = $inventory.Count
		totalBytes = $totalBytes
		aggregateSha256 = Get-TextSha256 -Text $aggregateBuilder.ToString()
		files = $inventory
		recordsByPath = $recordsByPath
	}
	return $script:BaselineInventoryCache
}

function Get-Percentile {
	param(
		[Parameter(Mandatory = $true)]
		[double[]]$Values,

		[Parameter(Mandatory = $true)]
		[ValidateRange(0.0, 100.0)]
		[double]$Percentile
	)

	if ($Values.Count -eq 0) {
		throw "A percentile requires at least one value."
	}
	$sorted = [double[]]@($Values | Sort-Object)
	$rank = [Math]::Ceiling(($Percentile / 100.0) * $sorted.Count)
	$index = [Math]::Max(0, [Math]::Min($sorted.Count - 1, [int]$rank - 1))
	return $sorted[$index]
}

function Get-Statistics {
	param(
		[Parameter(Mandatory = $true)]
		[double[]]$Values,

		[Parameter(Mandatory = $true)]
		[ValidateSet("lowerIsBetter", "higherIsBetter", "exact")]
		[string]$Direction
	)

	$median = Get-Percentile -Values $Values -Percentile 50
	$minimum = ($Values | Measure-Object -Minimum).Minimum
	$maximum = ($Values | Measure-Object -Maximum).Maximum
	$worst = if ($Direction -eq "higherIsBetter") { [double]$minimum } else { [double]$maximum }
	$rangePercent = if ([Math]::Abs($median) -gt 0.0) {
		(([double]$maximum - [double]$minimum) / [Math]::Abs($median)) * 100.0
	}
	else {
		if ([double]$maximum -eq [double]$minimum) { 0.0 } else { [double]::PositiveInfinity }
	}
	$maximumConsecutiveDeltaPercent = 0.0
	for ($index = 1; $index -lt $Values.Count; ++$index) {
		$denominator = [Math]::Abs($Values[$index - 1])
		$currentDelta = if ($denominator -gt 0.0) {
			([Math]::Abs($Values[$index] - $Values[$index - 1]) / $denominator) * 100.0
		}
		else {
			if ($Values[$index] -eq 0.0) { 0.0 } else { [double]::PositiveInfinity }
		}
		$maximumConsecutiveDeltaPercent = [Math]::Max($maximumConsecutiveDeltaPercent, $currentDelta)
	}
	return [ordered]@{
		count = $Values.Count
		values = @($Values)
		median = $median
		p95 = Get-Percentile -Values $Values -Percentile 95
		p99 = Get-Percentile -Values $Values -Percentile 99
		worst = $worst
		tail = [ordered]@{
			p95 = Get-Percentile -Values $Values -Percentile 95
			p99 = Get-Percentile -Values $Values -Percentile 99
			worst = $worst
		}
		repeatability = [ordered]@{
			minimum = [double]$minimum
			maximum = [double]$maximum
			range = [double]$maximum - [double]$minimum
			rangePercentOfMedian = $rangePercent
			maximumConsecutiveDeltaPercent = $maximumConsecutiveDeltaPercent
		}
	}
}

function Get-RegressionPercent {
	param(
		[Parameter(Mandatory = $true)]
		[double]$Baseline,

		[Parameter(Mandatory = $true)]
		[double]$Candidate,

		[Parameter(Mandatory = $true)]
		[ValidateSet("lowerIsBetter", "higherIsBetter")]
		[string]$Direction
	)

	if ([Math]::Abs($Baseline) -eq 0.0) {
		if ($Candidate -eq $Baseline) {
			return 0.0
		}
		return [double]::PositiveInfinity
	}
	if ($Direction -eq "higherIsBetter") {
		return (($Baseline - $Candidate) / [Math]::Abs($Baseline)) * 100.0
	}
	return (($Candidate - $Baseline) / [Math]::Abs($Baseline)) * 100.0
}

function New-Check {
	param(
		[Parameter(Mandatory = $true)]
		[string]$Id,

		[Parameter(Mandatory = $true)]
		[bool]$Passed,

		[AllowNull()]
		$Actual,

		[AllowNull()]
		$Limit
	)

	return [ordered]@{
		id = $Id
		passed = $Passed
		actual = $Actual
		limit = $Limit
	}
}

function Compare-MetricSamples {
	param(
		[Parameter(Mandatory = $true)]
		$Metric,

		[Parameter(Mandatory = $true)]
		[double[]]$BaselineValues,

		[Parameter(Mandatory = $true)]
		[double[]]$CandidateValues
	)

	$direction = [string]$Metric.direction
	$baselineStatistics = Get-Statistics -Values $BaselineValues -Direction $direction
	$candidateStatistics = Get-Statistics -Values $CandidateValues -Direction $direction
	$pairedRuns = @()
	$pairedRegressionValues = @()
	for ($index = 0; $index -lt $BaselineValues.Count; ++$index) {
		$regressionPercent = if ($direction -eq "exact") {
			$null
		}
		else {
			Get-RegressionPercent -Baseline $BaselineValues[$index] -Candidate $CandidateValues[$index] -Direction $direction
		}
		if ($null -ne $regressionPercent) {
			$pairedRegressionValues += [double]$regressionPercent
		}
		$pairedRuns += ,[ordered]@{
			runIndex = $index + 1
			baseline = $BaselineValues[$index]
			candidate = $CandidateValues[$index]
			delta = $CandidateValues[$index] - $BaselineValues[$index]
			regressionPercent = $regressionPercent
		}
	}

	$checks = @()
	if ([string]$Metric.gate -eq "exact") {
		$exactRuns = $true
		for ($index = 0; $index -lt $BaselineValues.Count; ++$index) {
			if ($BaselineValues[$index] -ne $CandidateValues[$index]) {
				$exactRuns = $false
				break
			}
		}
		$checks += ,(New-Check -Id "paired-values-exact" -Passed $exactRuns -Actual @($CandidateValues) -Limit @($BaselineValues))
		foreach ($statisticName in @("median", "p95", "p99", "worst")) {
			$checks += ,(New-Check -Id "$statisticName-exact" -Passed ($candidateStatistics[$statisticName] -eq $baselineStatistics[$statisticName]) -Actual $candidateStatistics[$statisticName] -Limit $baselineStatistics[$statisticName])
		}
		$pairedStatistics = $null
	}
	else {
		$allowedRepeatabilityPercent = [double]$baselineStatistics.repeatability.rangePercentOfMedian
		$pairedStatistics = Get-Statistics -Values ([double[]]$pairedRegressionValues) -Direction "lowerIsBetter"
		foreach ($statisticName in @("median", "p95", "p99", "worst")) {
			$baselineEnvelope = [double]$baselineStatistics.worst
			$actual = [double]$candidateStatistics[$statisticName]
			$insideEnvelope = if ($direction -eq "higherIsBetter") {
				$actual -ge $baselineEnvelope
			}
			else {
				$actual -le $baselineEnvelope
			}
			$checks += ,(New-Check -Id "$statisticName-within-baseline-envelope" -Passed $insideEnvelope -Actual $actual -Limit $baselineEnvelope)
			$pairedActual = [double]$pairedStatistics[$statisticName]
			$checks += ,(New-Check -Id "paired-$statisticName-within-repeatability" -Passed ($pairedActual -le $allowedRepeatabilityPercent) -Actual $pairedActual -Limit $allowedRepeatabilityPercent)
		}
		$checks += ,(New-Check -Id "candidate-repeatability-within-baseline" `
			-Passed ([double]$candidateStatistics.repeatability.rangePercentOfMedian -le $allowedRepeatabilityPercent) `
			-Actual $candidateStatistics.repeatability.rangePercentOfMedian -Limit $allowedRepeatabilityPercent)
	}

	return [ordered]@{
		id = [string]$Metric.id
		unit = [string]$Metric.unit
		direction = $direction
		gate = [string]$Metric.gate
		baseline = $baselineStatistics
		candidate = $candidateStatistics
		paired = [ordered]@{
			runs = $pairedRuns
			regressionStatistics = $pairedStatistics
		}
		checks = $checks
		passed = @($checks | Where-Object { -not $_.passed }).Count -eq 0
	}
}

function Add-UniqueDiagnostic {
	param(
		[Parameter(Mandatory = $true)]
		[AllowEmptyCollection()]
		[System.Collections.Generic.List[string]]$List,

		[Parameter(Mandatory = $true)]
		[string]$Message
	)

	if (-not $List.Contains($Message)) {
		$List.Add($Message)
	}
}

function Invoke-PerformanceComparison {
	param(
		[Parameter(Mandatory = $true)]
		[string]$CandidateManifestPath,

		[Parameter(Mandatory = $true)]
		[string]$ResultPath,

		[Parameter(Mandatory = $true)]
		[string]$LockedBaselinePath,

		[Parameter(Mandatory = $true)]
		[string]$PerformanceWorkloadsPath,

		[Parameter(Mandatory = $true)]
		[bool]$IsValidationOnly,

		[string]$WholeEngineCoverageMatrixPath = $script:DefaultCoverageMatrixPath,

		[bool]$RequireWholeEngineCoverage = $false
	)

	$invalidDiagnostics = New-Object System.Collections.Generic.List[string]
	$regressionDiagnostics = New-Object System.Collections.Generic.List[string]
	$thresholdChecks = @()
	$workloadReports = @()
	$candidateRawPaths = @()
	$candidateArtifactPaths = @()
	$coverageReports = @()
	$coverageSourceRecords = @()
	$coverageMatrixReady = $false

	$resolvedOutputPath = Resolve-RepositoryPath -Path $ResultPath
	Assert-SafeCandidatePath -Path $resolvedOutputPath -Description "Comparison output"
	$resolvedCandidateManifest = Resolve-RepositoryPath -Path $CandidateManifestPath
	Assert-SafeCandidatePath -Path $resolvedCandidateManifest -Description "Candidate manifest"
	$resolvedBaselinePath = Resolve-RepositoryPath -Path $LockedBaselinePath
	$resolvedWorkloadsPath = Resolve-RepositoryPath -Path $PerformanceWorkloadsPath
	$resolvedCoverageMatrixPath = Resolve-RepositoryPath -Path $WholeEngineCoverageMatrixPath

	if ($resolvedBaselinePath -cne $script:DefaultBaselinePath) {
		Add-UniqueDiagnostic -List $invalidDiagnostics -Message "The baseline path is not the authoritative locked baseline path."
	}
	if ($resolvedWorkloadsPath -cne $script:DefaultWorkloadsPath) {
		Add-UniqueDiagnostic -List $invalidDiagnostics -Message "The workload path is not the source-controlled PerformanceWorkloads.json path."
	}
	if ($resolvedCoverageMatrixPath -cne $script:DefaultCoverageMatrixPath) {
		Add-UniqueDiagnostic -List $invalidDiagnostics -Message "The whole-engine coverage path is not the reviewed source-controlled matrix."
	}

	$inputRecords = [ordered]@{}
	foreach ($inputDefinition in @(
		@("candidateManifest", $CandidateManifestPath, $resolvedCandidateManifest, "candidate manifest"),
		@("lockedBaseline", $LockedBaselinePath, $resolvedBaselinePath, "locked baseline"),
		@("performanceWorkloads", $PerformanceWorkloadsPath, $resolvedWorkloadsPath, "performance workload configuration"),
		@("wholeEngineCoverageMatrix", $WholeEngineCoverageMatrixPath, $resolvedCoverageMatrixPath, "whole-engine coverage matrix"),
		@("runtimeContract", (ConvertTo-RepositoryRelativePath -Path $script:ContractPath), $script:ContractPath, "runtime validation contract"),
		@("rendererWorkloadManifest", (ConvertTo-RepositoryRelativePath -Path $script:RendererManifestPath), $script:RendererManifestPath, "renderer workload manifest"),
		@("evidenceLock", (ConvertTo-RepositoryRelativePath -Path $script:EvidenceLockPath), $script:EvidenceLockPath, "baseline evidence lock")
	)) {
		try {
			$inputRecords[$inputDefinition[0]] = [ordered]@{
				kind = $inputDefinition[3]
				declaredPath = $inputDefinition[1]
				resolvedPath = ConvertTo-PortablePath -Path $inputDefinition[2]
				sha256 = Get-FileSha256 -Path $inputDefinition[2]
				bytes = (Get-Item -LiteralPath $inputDefinition[2]).Length
			}
		}
		catch {
			Add-UniqueDiagnostic -List $invalidDiagnostics -Message $_.Exception.Message
		}
	}

	$config = $null
	$baseline = $null
	$evidenceLock = $null
	$rendererManifest = $null
	$coverageMatrix = $null
	$candidate = $null
	try { $config = Read-JsonFile -Path $resolvedWorkloadsPath } catch { Add-UniqueDiagnostic -List $invalidDiagnostics -Message $_.Exception.Message }
	try { $baseline = Read-JsonFile -Path $resolvedBaselinePath } catch { Add-UniqueDiagnostic -List $invalidDiagnostics -Message $_.Exception.Message }
	try { $evidenceLock = Read-JsonFile -Path $script:EvidenceLockPath } catch { Add-UniqueDiagnostic -List $invalidDiagnostics -Message $_.Exception.Message }
	try { $rendererManifest = Read-JsonFile -Path $script:RendererManifestPath } catch { Add-UniqueDiagnostic -List $invalidDiagnostics -Message $_.Exception.Message }
	try { $coverageMatrix = Read-JsonFile -Path $resolvedCoverageMatrixPath } catch { Add-UniqueDiagnostic -List $invalidDiagnostics -Message $_.Exception.Message }
	try { $candidate = Read-JsonFile -Path $resolvedCandidateManifest } catch { Add-UniqueDiagnostic -List $invalidDiagnostics -Message $_.Exception.Message }

	if ($inputRecords.Contains("lockedBaseline")) {
		$passed = [string]$inputRecords.lockedBaseline.sha256 -ceq $script:ExpectedBaselineSha256
		$thresholdChecks += ,(New-Check -Id "locked-baseline-sha256" -Passed $passed -Actual $inputRecords.lockedBaseline.sha256 -Limit $script:ExpectedBaselineSha256)
		if (-not $passed) { Add-UniqueDiagnostic -List $invalidDiagnostics -Message "The locked baseline SHA-256 does not match the reference-baseline contract." }
	}
	if ($inputRecords.Contains("evidenceLock")) {
		$passed = [string]$inputRecords.evidenceLock.sha256 -ceq $script:ExpectedEvidenceLockSha256
		$thresholdChecks += ,(New-Check -Id "evidence-lock-sha256" -Passed $passed -Actual $inputRecords.evidenceLock.sha256 -Limit $script:ExpectedEvidenceLockSha256)
		if (-not $passed) { Add-UniqueDiagnostic -List $invalidDiagnostics -Message "The evidence-lock SHA-256 does not match the reference-baseline contract." }
	}
	if ($inputRecords.Contains("runtimeContract")) {
		$passed = [string]$inputRecords.runtimeContract.sha256 -ceq $script:ExpectedContractSha256
		$thresholdChecks += ,(New-Check -Id "runtime-contract-sha256" -Passed $passed -Actual $inputRecords.runtimeContract.sha256 -Limit $script:ExpectedContractSha256)
		if (-not $passed) { Add-UniqueDiagnostic -List $invalidDiagnostics -Message "The frozen runtime validation contract changed." }
	}
	if ($inputRecords.Contains("rendererWorkloadManifest")) {
		$passed = [string]$inputRecords.rendererWorkloadManifest.sha256 -ceq $script:ExpectedRendererManifestSha256
		$thresholdChecks += ,(New-Check -Id "renderer-workload-manifest-sha256" -Passed $passed -Actual $inputRecords.rendererWorkloadManifest.sha256 -Limit $script:ExpectedRendererManifestSha256)
		if (-not $passed) { Add-UniqueDiagnostic -List $invalidDiagnostics -Message "The source-controlled renderer workload manifest changed." }
	}
	if ($inputRecords.Contains("performanceWorkloads")) {
		$passed = [string]$inputRecords.performanceWorkloads.sha256 -ceq $script:ExpectedWorkloadsSha256
		$thresholdChecks += ,(New-Check -Id "performance-workloads-sha256" -Passed $passed -Actual $inputRecords.performanceWorkloads.sha256 -Limit $script:ExpectedWorkloadsSha256)
		if (-not $passed) { Add-UniqueDiagnostic -List $invalidDiagnostics -Message "PerformanceWorkloads.json changed without updating the comparator's reviewed identity." }
	}
	if ($inputRecords.Contains("wholeEngineCoverageMatrix")) {
		$passed = [string]$inputRecords.wholeEngineCoverageMatrix.sha256 -ceq $script:ExpectedCoverageMatrixSha256
		$thresholdChecks += ,(New-Check -Id "whole-engine-coverage-matrix-sha256" -Passed $passed -Actual $inputRecords.wholeEngineCoverageMatrix.sha256 -Limit $script:ExpectedCoverageMatrixSha256)
		if (-not $passed) { Add-UniqueDiagnostic -List $invalidDiagnostics -Message "The final whole-engine coverage matrix changed without review." }
	}

	$baselineInventory = $null
	try {
		$baselineInventory = Get-BaselineEvidenceInventory
		foreach ($inventoryCheck in @(
			@("baseline-evidence-file-count", $baselineInventory.fileCount, 178),
			@("baseline-evidence-total-bytes", $baselineInventory.totalBytes, [uint64]371567204),
			@("baseline-evidence-aggregate-sha256", $baselineInventory.aggregateSha256, $script:ExpectedEvidenceAggregateSha256)
		)) {
			$passed = [string]$inventoryCheck[1] -ceq [string]$inventoryCheck[2]
			$thresholdChecks += ,(New-Check -Id $inventoryCheck[0] -Passed $passed -Actual $inventoryCheck[1] -Limit $inventoryCheck[2])
			if (-not $passed) { Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Immutable baseline evidence failed '$($inventoryCheck[0])'." }
		}
	}
	catch {
		Add-UniqueDiagnostic -List $invalidDiagnostics -Message "The immutable baseline inventory could not be verified: $($_.Exception.Message)"
	}

	if ($null -ne $config -and $null -ne $rendererManifest) {
		$rendererThresholdsMatch = Test-DeepEqual -Left $config.sourceControlledThresholds.rendererPerformanceBaseline -Right $rendererManifest.thresholds
		$rendererWorkloadsMatch = Test-DeepEqual -Left $config.rendererControlWorkloads -Right $rendererManifest.workloads
		$thresholdChecks += ,(New-Check -Id "renderer-thresholds-source-controlled" -Passed $rendererThresholdsMatch -Actual (Get-CanonicalJson $rendererManifest.thresholds) -Limit (Get-CanonicalJson $config.sourceControlledThresholds.rendererPerformanceBaseline))
		$thresholdChecks += ,(New-Check -Id "renderer-workloads-source-controlled" -Passed $rendererWorkloadsMatch -Actual (Get-CanonicalJson $rendererManifest.workloads) -Limit (Get-CanonicalJson $config.rendererControlWorkloads))
		if (-not $rendererThresholdsMatch) { Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Renderer thresholds differ from the reviewed source-controlled values." }
		if (-not $rendererWorkloadsMatch) { Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Renderer workloads differ from the reviewed source-controlled values." }
		foreach ($control in @($config.setupBaselineControls)) {
			$actualSettingsHash = Get-TextSha256 -Text (Get-CanonicalJson -Value $control.settings)
			$settingsHashMatches = $actualSettingsHash -ceq [string]$control.settingsHash
			$thresholdChecks += ,(New-Check -Id "settings-hash-$($control.id)" -Passed $settingsHashMatches -Actual $actualSettingsHash -Limit $control.settingsHash)
			if (-not $settingsHashMatches) { Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Source-controlled settings hash mismatch for '$($control.id)'." }
		}
	}

	if ($null -ne $config -and $null -ne $coverageMatrix) {
		$coverageInvalidStart = $invalidDiagnostics.Count
		try {
			$requiredDomainIds = @("renderer", "camera", "ui", "physics", "scripting", "resource", "virtual-geometry", "scheduler", "profiler", "mcp", "startup", "export")
			$matrixDomains = @($coverageMatrix.domains)
			$actualDomainIds = @($matrixDomains | ForEach-Object { [string]$_.id })
			if ($coverageMatrix.schemaVersion -ne 1 -or $coverageMatrix.feature -cne "animation" -or
				$coverageMatrix.task -cne "animation-performance-coverage" -or $coverageMatrix.comparisonTask -cne "animation-performance-comparison" -or
				-not $coverageMatrix.finalPhaseOnly -or $matrixDomains.Count -ne $requiredDomainIds.Count) {
				throw "The whole-engine matrix identity, phase guard, or domain count is invalid."
			}
			if ([string]$coverageMatrix.performanceWorkloads.path -cne "Tests/Tools/Animation/PerformanceWorkloads.json" -or
				[string]$coverageMatrix.performanceWorkloads.sha256 -cne $script:ExpectedWorkloadsSha256 -or
				[string]$coverageMatrix.lockedBaseline.sha256 -cne $script:ExpectedBaselineSha256) {
				throw "The whole-engine matrix does not retain the immutable workload and baseline identities."
			}
			$policy = $coverageMatrix.comparisonPolicy
			if ($policy.minimumPairedMeasuredRuns -ne 5 -or -not $policy.pairedWarmupRequired -or
				[string]$policy.thresholdSource -cne "PerformanceWorkloads.json-only" -or
				[string]$policy.missingDomain -cne "reject" -or [string]$policy.missingMetric -cne "reject" -or
				[string]$policy.missingFact -cne "reject" -or [string]$policy.failedMappedWorkload -cne "reject" -or
				[string]$policy.identityMismatch -cne "reject" -or
				-not (Test-DeepEqual -Left $policy.statistics -Right @("median", "p95", "p99", "worst"))) {
				throw "The whole-engine comparison policy is incomplete or weakened."
			}
			foreach ($requiredDomainId in $requiredDomainIds) {
				$domainMatches = @($matrixDomains | Where-Object { $_.id -ceq $requiredDomainId })
				if ($domainMatches.Count -ne 1) { throw "Coverage domain '$requiredDomainId' is missing or duplicated." }
				$domain = $domainMatches[0]
				$controlMatches = @($config.setupBaselineControls | Where-Object { $_.id -ceq [string]$domain.baselineControlId })
				if ($controlMatches.Count -ne 1 -or -not $controlMatches[0].required) {
					throw "Coverage domain '$requiredDomainId' does not map to one required reference-baseline control."
				}
				$availableMetricIds = @($controlMatches[0].metrics | ForEach-Object { [string]$_.id })
				foreach ($metricId in @($domain.requiredMetricIds)) {
					if ($availableMetricIds -cnotcontains [string]$metricId) { throw "Coverage domain '$requiredDomainId' references unknown metric '$metricId'." }
				}
				$availableFactIds = @($controlMatches[0].requiredFacts | ForEach-Object { [string]$_.id })
				foreach ($factId in @($domain.requiredFactIds)) {
					if ($availableFactIds -cnotcontains [string]$factId) { throw "Coverage domain '$requiredDomainId' references unknown fact '$factId'." }
				}
				$sourceContracts = @($domain.sourceContracts)
				if ($sourceContracts.Count -eq 0) { throw "Coverage domain '$requiredDomainId' has no source contract." }
				foreach ($sourceContract in $sourceContracts) {
					$coverageSourceRecords += ,(Get-PathRecord -DeclaredPath ([string]$sourceContract) -Kind "whole-engine $requiredDomainId source contract")
				}
			}
		}
		catch {
			Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Whole-engine coverage matrix is invalid: $($_.Exception.Message)"
		}
		$coverageMatrixReady = $invalidDiagnostics.Count -eq $coverageInvalidStart
		$thresholdChecks += ,(New-Check -Id "whole-engine-coverage-contract" -Passed $coverageMatrixReady -Actual "parsed animation performance coverage matrix" -Limit "12 fail-closed domains mapped to immutable reference-baseline controls")
	}

	if ($null -ne $baseline) {
		$baselineContractPassed = $baseline.schemaVersion -eq 1 -and $baseline.feature -eq "animation" -and $baseline.task -eq "animation-reference-baseline" -and $baseline.state -eq "locked" -and $baseline.result.passed -and $baseline.result.warmupsPassed -and $baseline.result.measuredRunsPerControl -eq 5 -and $baseline.comparisonContract.immutable -and $baseline.comparisonContract.minimumCandidateRuns -eq 5 -and $baseline.comparisonContract.pairedWarmupRequired -and $baseline.comparisonContract.missingRequiredMetric -eq "reject" -and $baseline.comparisonContract.identityMismatch -eq "reject" -and $baseline.comparisonContract.thresholdWeakening -eq "reject" -and $baseline.comparisonContract.candidateMayNotOverwriteThisFile
		$thresholdChecks += ,(New-Check -Id "locked-baseline-contract" -Passed $baselineContractPassed -Actual "parsed locked baseline contract" -Limit "immutable, warm-up, five runs, and fail-closed rules")
		if (-not $baselineContractPassed) { Add-UniqueDiagnostic -List $invalidDiagnostics -Message "The locked baseline comparison contract is incomplete or weakened." }
	}
	if ($null -ne $evidenceLock) {
		$lockMatches = $evidenceLock.schemaVersion -eq 1 -and $evidenceLock.feature -eq "animation" -and $evidenceLock.task -eq "animation-reference-baseline" -and $evidenceLock.fileCount -eq 178 -and [uint64]$evidenceLock.totalBytes -eq [uint64]371567204 -and $evidenceLock.aggregateSha256 -eq $script:ExpectedEvidenceAggregateSha256 -and $evidenceLock.lockedBaselineSha256 -eq $script:ExpectedBaselineSha256
		$thresholdChecks += ,(New-Check -Id "evidence-lock-contract" -Passed $lockMatches -Actual "parsed evidence lock" -Limit "reference-baseline locked aggregate identity")
		if (-not $lockMatches) { Add-UniqueDiagnostic -List $invalidDiagnostics -Message "evidence-lock.json does not describe the immutable reference-baseline selection." }
	}

	if ($null -ne $candidate -and $null -ne $config) {
		$forbiddenProperties = @(Get-ForbiddenCandidateProperties -Value $candidate)
		if ($forbiddenProperties.Count -gt 0) {
			Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Candidate evidence contains forbidden threshold or policy properties: $($forbiddenProperties -join ', ')."
		}
		try {
			if ($candidate.schemaVersion -ne 1 -or $candidate.feature -ne "animation") {
				Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Candidate manifest schemaVersion or feature identity is not supported."
			}
			$expectedReference = [ordered]@{}
			foreach ($property in $config.candidateTemplate.reference.PSObject.Properties) {
				$expectedReference[$property.Name] = $property.Value
			}
			$expectedReference["performanceWorkloadsSha256"] = $script:ExpectedWorkloadsSha256
			if (-not (Test-DeepEqual -Left $candidate.reference -Right $expectedReference)) {
				Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Candidate reference hashes or immutable baseline identity do not match."
			}
			if (-not (Test-DeepEqual -Left $candidate.comparisonIdentity -Right $config.candidateTemplate.comparisonIdentity)) {
				Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Candidate hardware, platform, driver, or toolchain identity does not match the frozen reference host."
			}
			if (-not (Test-DeepEqual -Left $candidate.comparisonSettings -Right $config.candidateTemplate.comparisonSettings)) {
				Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Candidate build, graphics, power, sampling, layout, or ambient-process settings do not match."
			}
			if (-not (Test-DeepEqual -Left $candidate.observations -Right $config.candidateTemplate.requiredObservations)) {
				Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Candidate observations do not prove AC power, discrete routing, warm-up, and comparable ambient load."
			}
		}
		catch {
			Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Candidate identity is incomplete: $($_.Exception.Message)"
		}

		try {
			$build = $candidate.candidateBuild
			if ([string]$build.sourceCommit -notmatch "^[0-9a-f]{40}$") { throw "candidateBuild.sourceCommit must be a full lowercase commit hash." }
			if ($build.sourceTreeDirty -isnot [bool]) { throw "candidateBuild.sourceTreeDirty must be Boolean." }
			if ($build.sourceTreeDirty) {
				if ([string]$build.sourceDiffSha256 -notmatch "^[0-9a-f]{64}$") { throw "A dirty candidate requires sourceDiffSha256." }
			}
			elseif ($null -ne $build.sourceDiffSha256) { throw "A clean candidate must use null sourceDiffSha256." }
			if ($build.buildConfiguration -ne "RelWithDebInfo") { throw "Candidate build configuration must be RelWithDebInfo." }
			$installRoot = Resolve-RepositoryPath -Path ([string]$build.installedTreeRoot)
			Assert-SafeCandidatePath -Path $installRoot -Description "Candidate installed tree"
			if (-not (Test-Path -LiteralPath $installRoot -PathType Container)) { throw "Candidate installed tree was not found: '$installRoot'." }
			$artifacts = @($build.artifacts)
			if ($artifacts.Count -eq 0) { throw "Candidate build must preserve at least one installed artifact hash." }
			foreach ($artifact in $artifacts) {
				$record = Get-PathRecord -DeclaredPath ([string]$artifact.path) -Kind "candidate installed artifact" -CandidatePath
				$artifactPath = Resolve-RepositoryPath -Path ([string]$artifact.path)
				if (-not (Test-PathIsBelow -Parent $installRoot -Child $artifactPath)) { throw "Candidate artifact '$artifactPath' is outside the installed tree." }
				if ([string]$artifact.sha256 -cne [string]$record.sha256) { throw "Candidate artifact hash mismatch for '$($artifact.path)'." }
				$candidateArtifactPaths += ,$record
			}
		}
		catch {
			Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Candidate build identity is invalid: $($_.Exception.Message)"
		}

		try {
			$rawInputPaths = @($candidate.rawInputPaths)
			if ($rawInputPaths.Count -eq 0) { throw "Candidate manifest must preserve at least one raw identity/settings input path." }
			$seenRawInputs = @{}
			foreach ($rawInputPath in $rawInputPaths) {
				$record = Get-PathRecord -DeclaredPath ([string]$rawInputPath) -Kind "candidate raw input" -CandidatePath
				$key = ([string]$record.resolvedPath).ToLowerInvariant()
				if ($seenRawInputs.ContainsKey($key)) { throw "Candidate raw input paths contain a duplicate: '$rawInputPath'." }
				$seenRawInputs[$key] = $true
				$candidateRawPaths += ,$record
			}
		}
		catch {
			Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Candidate raw input paths are invalid: $($_.Exception.Message)"
		}
	}

	if ($null -ne $candidate -and $null -ne $config -and $null -ne $baseline) {
		$candidateWorkloads = @($candidate.workloads)
		$configuredControls = @($config.setupBaselineControls | Where-Object { $_.required })
		if ($candidateWorkloads.Count -ne $configuredControls.Count) {
			Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Candidate must report exactly the $($configuredControls.Count) required setup baseline controls."
		}
		$allNormalizedRunPaths = @{}
		$allSourceResultPaths = @{}
		foreach ($control in $configuredControls) {
			$controlId = [string]$control.id
			$matchingWorkloads = @($candidateWorkloads | Where-Object { $_.id -eq $controlId })
			if ($matchingWorkloads.Count -ne 1) {
				Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Candidate workload '$controlId' must appear exactly once."
				continue
			}
			$candidateWorkload = $matchingWorkloads[0]
			$workloadInvalidStart = $invalidDiagnostics.Count
			$normalizedRunRecords = @()
			$sourceResultRecords = @()
			$candidateRuns = @()
			$warmupRun = $null
			try {
				if (-not (Test-DeepEqual -Left $candidateWorkload.settings -Right $control.settings)) { throw "Settings object differs from the frozen workload." }
				$computedCandidateSettingsHash = Get-TextSha256 -Text (Get-CanonicalJson -Value $candidateWorkload.settings)
				if ([string]$candidateWorkload.settingsHash -cne [string]$control.settingsHash -or $computedCandidateSettingsHash -cne [string]$control.settingsHash) { throw "Settings hash differs from the frozen workload." }
				$measuredPaths = @($candidateWorkload.measuredResultPaths)
				$baselineRuns = @(Get-NestedValue -Object $baseline -Path ([string]$control.baselineRunsPath))
				if ($measuredPaths.Count -lt [int]$config.comparisonPolicy.minimumCandidateRuns) { throw "At least five measured candidate runs are required." }
				if ($config.comparisonPolicy.mustMatchLockedBaselineRunCount -and $measuredPaths.Count -ne $baselineRuns.Count) { throw "Candidate and baseline measured run counts differ." }
				$allDeclaredRunPaths = @([string]$candidateWorkload.warmupResultPath) + @($measuredPaths | ForEach-Object { [string]$_ })
				for ($pathIndex = 0; $pathIndex -lt $allDeclaredRunPaths.Count; ++$pathIndex) {
					$declaredRunPath = $allDeclaredRunPaths[$pathIndex]
					$runPath = Resolve-RepositoryPath -Path $declaredRunPath
					Assert-SafeCandidatePath -Path $runPath -Description "Candidate normalized run"
					$runKey = (ConvertTo-PortablePath -Path $runPath).ToLowerInvariant()
					if ($allNormalizedRunPaths.ContainsKey($runKey)) { throw "Normalized run path is duplicated: '$declaredRunPath'." }
					$allNormalizedRunPaths[$runKey] = $true
					$runRecord = Get-PathRecord -DeclaredPath $declaredRunPath -Kind "candidate normalized run" -CandidatePath
					$normalizedRunRecords += ,$runRecord
					$runData = Read-JsonFile -Path $runPath
					$forbiddenRunProperties = @(Get-ForbiddenCandidateProperties -Value $runData)
					if ($forbiddenRunProperties.Count -gt 0) { throw "Normalized run supplies forbidden threshold properties: $($forbiddenRunProperties -join ', ')." }
					$expectedKind = if ($pathIndex -eq 0) { "warmup" } else { "measured" }
					$expectedIndex = if ($pathIndex -eq 0) { 0 } else { $pathIndex }
					if ($runData.schemaVersion -ne 1 -or $runData.workloadId -ne $controlId -or $runData.runKind -ne $expectedKind -or $runData.runIndex -ne $expectedIndex -or -not $runData.succeeded -or $runData.settingsHash -cne $control.settingsHash) { throw "Run '$declaredRunPath' has mismatched schema, identity, index, success, or settings hash." }
					$sourcePaths = @($runData.sourceResultPaths)
					if ($sourcePaths.Count -eq 0) { throw "Run '$declaredRunPath' did not preserve a raw source result path." }
					foreach ($sourcePath in $sourcePaths) {
						$sourceRecord = Get-PathRecord -DeclaredPath ([string]$sourcePath) -Kind "candidate raw result" -CandidatePath
						$sourceKey = ([string]$sourceRecord.resolvedPath).ToLowerInvariant()
						if ($allSourceResultPaths.ContainsKey($sourceKey)) { throw "Raw result path is duplicated across runs: '$sourcePath'." }
						$allSourceResultPaths[$sourceKey] = $true
						$sourceResultRecords += ,$sourceRecord
					}
					if ($expectedKind -eq "warmup") { $warmupRun = $runData } else { $candidateRuns += ,$runData }
				}
			}
			catch {
				Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Candidate workload '$controlId' is invalid: $($_.Exception.Message)"
			}

			$baselineRunsForReport = @()
			try {
				$baselineWarmupDeclared = [string](Get-NestedValue -Object $baseline -Path ([string]$control.baselineWarmupPath))
				$baselineWarmupAbsolute = [System.IO.Path]::GetFullPath((Join-Path $script:BaselineRoot $baselineWarmupDeclared))
				$baselineWarmupRelative = $baselineWarmupAbsolute.Substring($script:BaselineRoot.Length + 1).Replace("\", "/")
				if ($null -eq $baselineInventory -or -not $baselineInventory.recordsByPath.ContainsKey($baselineWarmupRelative.ToLowerInvariant())) { throw "Locked warm-up path is absent from the evidence selection: '$baselineWarmupDeclared'." }
				$baselineWarmupRecord = $baselineInventory.recordsByPath[$baselineWarmupRelative.ToLowerInvariant()]
				$baselineRuns = @(Get-NestedValue -Object $baseline -Path ([string]$control.baselineRunsPath))
				foreach ($baselineRun in $baselineRuns) {
					if (-not [bool](Get-PropertyValue -Object $baselineRun -Name ([string]$control.baselineSuccessProperty))) { throw "A locked baseline run is not successful." }
					$resultDeclared = [string](Get-PropertyValue -Object $baselineRun -Name ([string]$control.baselineResultPathProperty))
					$resultAbsolute = [System.IO.Path]::GetFullPath((Join-Path $script:BaselineRoot $resultDeclared))
					$resultRelative = $resultAbsolute.Substring($script:BaselineRoot.Length + 1).Replace("\", "/")
					if (-not $baselineInventory.recordsByPath.ContainsKey($resultRelative.ToLowerInvariant())) { throw "Locked result path is absent from the evidence selection: '$resultDeclared'." }
					$baselineRunsForReport += ,$baselineInventory.recordsByPath[$resultRelative.ToLowerInvariant()]
				}
			}
			catch {
				Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Locked workload '$controlId' evidence paths are invalid: $($_.Exception.Message)"
				$baselineWarmupRecord = $null
				$baselineRuns = @()
			}

			$metricReports = @()
			if ($invalidDiagnostics.Count -eq $workloadInvalidStart) {
				foreach ($fact in @($control.requiredFacts)) {
					try {
						$expectedFact = if (Test-HasProperty -Object $fact -Name "constant") { $fact.constant } else { Get-NestedValue -Object $baseline -Path ([string]$fact.baselinePath) }
						foreach ($candidateRun in $candidateRuns) {
							if (-not (Test-HasProperty -Object $candidateRun -Name "facts") -or -not (Test-HasProperty -Object $candidateRun.facts -Name ([string]$fact.id))) { throw "Required fact '$($fact.id)' is missing." }
							if (-not (Test-DeepEqual -Left (Get-PropertyValue -Object $candidateRun.facts -Name ([string]$fact.id)) -Right $expectedFact)) { throw "Required fact '$($fact.id)' does not match the locked correctness result." }
						}
					}
					catch {
						Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Candidate workload '$controlId' fact failure: $($_.Exception.Message)"
					}
				}

				foreach ($metric in @($control.metrics)) {
					try {
						$baselineValues = @()
						$candidateValues = @()
						for ($runIndex = 0; $runIndex -lt $baselineRuns.Count; ++$runIndex) {
							$baselineValue = if (Test-HasProperty -Object $metric -Name "baselineConstant") { $metric.baselineConstant } else { Get-PropertyValue -Object $baselineRuns[$runIndex] -Name ([string]$metric.baselineProperty) }
							if (-not (Test-FiniteNumber -Value $baselineValue)) { throw "Locked baseline metric '$($metric.id)' is missing or non-finite at run $($runIndex + 1)." }
							$candidateRun = $candidateRuns[$runIndex]
							if (-not (Test-HasProperty -Object $candidateRun -Name "metrics") -or -not (Test-HasProperty -Object $candidateRun.metrics -Name ([string]$metric.id))) { throw "Required candidate metric '$($metric.id)' is missing at run $($runIndex + 1)." }
							$candidateMetric = Get-PropertyValue -Object $candidateRun.metrics -Name ([string]$metric.id)
							if ($candidateMetric.availability -cne "measured") { throw "Required candidate metric '$($metric.id)' is unavailable at run $($runIndex + 1)." }
							if ($candidateMetric.unit -cne $metric.unit) { throw "Required candidate metric '$($metric.id)' uses unit '$($candidateMetric.unit)' instead of '$($metric.unit)'." }
							if (-not (Test-FiniteNumber -Value $candidateMetric.value)) { throw "Required candidate metric '$($metric.id)' is non-finite at run $($runIndex + 1)." }
							$baselineValues += [double]$baselineValue
							$candidateValues += [double]$candidateMetric.value
						}
						$metricReport = Compare-MetricSamples -Metric $metric -BaselineValues ([double[]]$baselineValues) -CandidateValues ([double[]]$candidateValues)
						$metricReports += ,$metricReport
						if (-not $metricReport.passed) {
							$failedCheckIds = @($metricReport.checks | Where-Object { -not $_.passed } | ForEach-Object { $_.id })
							Add-UniqueDiagnostic -List $regressionDiagnostics -Message "Workload '$controlId' metric '$($metric.id)' failed: $($failedCheckIds -join ', ')."
						}
					}
					catch {
						Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Candidate workload '$controlId' metric evidence is invalid: $($_.Exception.Message)"
					}
				}
			}

			$workloadReports += ,[ordered]@{
				id = $controlId
				settingsHash = [string]$control.settingsHash
				settings = $control.settings
				baselineWarmup = $baselineWarmupRecord
				baselineMeasuredResults = $baselineRunsForReport
				candidateNormalizedResults = $normalizedRunRecords
				candidateRawResults = $sourceResultRecords
				metrics = $metricReports
				passed = ($invalidDiagnostics.Count -eq $workloadInvalidStart -and @($metricReports | Where-Object { -not $_.passed }).Count -eq 0)
			}
		}
	}

	if ($null -ne $coverageMatrix) {
		foreach ($domain in @($coverageMatrix.domains)) {
			$domainId = [string]$domain.id
			$controlId = [string]$domain.baselineControlId
			$matchingReports = @($workloadReports | Where-Object { $_.id -ceq $controlId })
			$missingMetrics = @()
			if ($matchingReports.Count -eq 1) {
				$reportedMetricIds = @($matchingReports[0].metrics | ForEach-Object { [string]$_.id })
				$missingMetrics = @($domain.requiredMetricIds | Where-Object { $reportedMetricIds -cnotcontains [string]$_ })
			}
			else {
				$missingMetrics = @($domain.requiredMetricIds)
			}
			$domainPassed = $coverageMatrixReady -and $matchingReports.Count -eq 1
			if ($domainPassed) {
				$domainPassed = [bool]$matchingReports[0].passed -and $missingMetrics.Count -eq 0
			}
			$coverageReports += ,[ordered]@{
				id = $domainId
				baselineControlId = $controlId
				sourceContracts = @($domain.sourceContracts)
				requiredMetricIds = @($domain.requiredMetricIds)
				requiredFactIds = @($domain.requiredFactIds)
				missingMetricIds = $missingMetrics
				mappedWorkloadPassed = $matchingReports.Count -eq 1 -and [bool]$matchingReports[0].passed
				passed = $domainPassed
			}
			if ($RequireWholeEngineCoverage -and $matchingReports.Count -ne 1) {
				Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Final whole-engine domain '$domainId' has no unique mapped candidate workload."
			}
			if ($RequireWholeEngineCoverage -and $missingMetrics.Count -gt 0) {
				Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Final whole-engine domain '$domainId' is missing metrics: $($missingMetrics -join ', ')."
			}
		}
	}
	elseif ($RequireWholeEngineCoverage) {
		Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Whole-engine coverage was requested without a readable animation performance coverage matrix."
	}
	if ($RequireWholeEngineCoverage -and -not $coverageMatrixReady) {
		Add-UniqueDiagnostic -List $invalidDiagnostics -Message "Final whole-engine coverage cannot run because matrix readiness failed."
	}

	$exitCode = if ($invalidDiagnostics.Count -gt 0) {
		$script:ExitInvalidEvidence
	}
	elseif ($regressionDiagnostics.Count -gt 0) {
		$script:ExitRegression
	}
	else {
		$script:ExitAccepted
	}
	$outcome = switch ($exitCode) {
		0 { "passed" }
		2 { "failed" }
		3 { "invalid" }
		default { "error" }
	}
	$sortedInvalidDiagnostics = [string[]]@($invalidDiagnostics | Sort-Object -Unique)
	$sortedRegressionDiagnostics = [string[]]@($regressionDiagnostics | Sort-Object -Unique)
	$result = [ordered]@{
		schemaVersion = 1
		tool = "CompareAnimationPerformance"
		feature = "animation"
		task = "animation-performance-evidence"
		mode = if ($RequireWholeEngineCoverage) { "final-phase-whole-engine-comparison" } elseif ($IsValidationOnly) { "validate-only-with-full-gates" } else { "comparison" }
		outcome = $outcome
		passed = $exitCode -eq $script:ExitAccepted
		exitCode = $exitCode
		inputs = $inputRecords
		outputPath = [ordered]@{
			declaredPath = $ResultPath
			resolvedPath = ConvertTo-PortablePath -Path $resolvedOutputPath
		}
		thresholdIntegrity = [ordered]@{
			passed = @($thresholdChecks | Where-Object { -not $_.passed }).Count -eq 0
			checks = $thresholdChecks
		}
		identity = [ordered]@{
			passed = @($sortedInvalidDiagnostics | Where-Object { $_ -match "identity|hardware|platform|driver|toolchain|settings|reference|ambient|power|routing" }).Count -eq 0
			mismatches = @($sortedInvalidDiagnostics | Where-Object { $_ -match "identity|hardware|platform|driver|toolchain|settings|reference|ambient|power|routing" })
		}
		baselineEvidence = if ($null -eq $baselineInventory) { $null } else { [ordered]@{
			fileCount = $baselineInventory.fileCount
			totalBytes = $baselineInventory.totalBytes
			aggregateSha256 = $baselineInventory.aggregateSha256
			files = $baselineInventory.files
		} }
		candidateEvidence = [ordered]@{
			rawInputs = $candidateRawPaths
			installedArtifacts = $candidateArtifactPaths
			wholeEngineSourceContracts = $coverageSourceRecords
		}
		workloads = $workloadReports
		wholeEngineCoverage = [ordered]@{
			matrixReady = $coverageMatrixReady
			enforced = $RequireWholeEngineCoverage
			performanceExecutionDeferred = -not $RequireWholeEngineCoverage
			passed = $coverageMatrixReady -and @($coverageReports | Where-Object { -not $_.passed }).Count -eq 0
			domains = $coverageReports
		}
		diagnostics = [ordered]@{
			invalidEvidence = @($sortedInvalidDiagnostics)
			regressions = @($sortedRegressionDiagnostics)
		}
	}
	Write-DeterministicJson -Path $resolvedOutputPath -Value $result
	return [PSCustomObject][ordered]@{
		exitCode = $exitCode
		outcome = $outcome
		resultPath = $resolvedOutputPath
	}
}

function Invoke-ComparatorSelfTest {
	$selfTestRoot = Resolve-RepositoryPath -Path "out/validation/animation/comparator-self-test"
	Assert-SafeCandidatePath -Path $selfTestRoot -Description "Comparator self-test root"
	New-Item -ItemType Directory -Force -Path $selfTestRoot | Out-Null
	$config = Read-JsonFile -Path $script:DefaultWorkloadsPath
	$baseline = Read-JsonFile -Path $script:DefaultBaselinePath

	$installedRoot = Join-Path $selfTestRoot "installed"
	New-Item -ItemType Directory -Force -Path $installedRoot | Out-Null
	$artifactPath = Join-Path $installedRoot "TerraEngine-self-test.bin"
	[System.IO.File]::WriteAllText($artifactPath, "deterministic comparator self-test artifact`n", (New-Object System.Text.UTF8Encoding($false)))
	$identityEvidencePath = Join-Path $selfTestRoot "raw-host-and-settings.json"
	Write-DeterministicJson -Path $identityEvidencePath -Value ([ordered]@{ purpose = "synthetic comparator identity evidence"; comparable = $true })

	$expectedReference = [ordered]@{}
	foreach ($property in $config.candidateTemplate.reference.PSObject.Properties) {
		$expectedReference[$property.Name] = $property.Value
	}
	$expectedReference["performanceWorkloadsSha256"] = $script:ExpectedWorkloadsSha256
	$manifestWorkloads = @()
	foreach ($control in @($config.setupBaselineControls)) {
		$controlDirectory = Join-Path $selfTestRoot ([string]$control.id)
		New-Item -ItemType Directory -Force -Path $controlDirectory | Out-Null
		$warmupSourcePath = Join-Path $controlDirectory "warmup-raw.json"
		Write-DeterministicJson -Path $warmupSourcePath -Value ([ordered]@{ workloadId = $control.id; runKind = "warmup"; succeeded = $true })
		$warmupPath = Join-Path $controlDirectory "warmup.json"
		Write-DeterministicJson -Path $warmupPath -Value ([ordered]@{
			schemaVersion = 1
			workloadId = $control.id
			runKind = "warmup"
			runIndex = 0
			succeeded = $true
			settingsHash = $control.settingsHash
			sourceResultPaths = @((ConvertTo-RepositoryRelativePath -Path $warmupSourcePath))
			facts = [ordered]@{}
			metrics = [ordered]@{}
		})

		$baselineRuns = @(Get-NestedValue -Object $baseline -Path ([string]$control.baselineRunsPath))
		$measuredPaths = @()
		for ($runIndex = 0; $runIndex -lt $baselineRuns.Count; ++$runIndex) {
			$rawPath = Join-Path $controlDirectory ("run-{0}-raw.json" -f ($runIndex + 1))
			Write-DeterministicJson -Path $rawPath -Value ([ordered]@{ workloadId = $control.id; run = $runIndex + 1; synthetic = $true })
			$metricValues = [ordered]@{}
			foreach ($metric in @($control.metrics)) {
				$value = if (Test-HasProperty -Object $metric -Name "baselineConstant") { $metric.baselineConstant } else { Get-PropertyValue -Object $baselineRuns[$runIndex] -Name ([string]$metric.baselineProperty) }
				$metricValues[[string]$metric.id] = [ordered]@{ availability = "measured"; unit = [string]$metric.unit; value = $value }
			}
			$facts = [ordered]@{}
			foreach ($fact in @($control.requiredFacts)) {
				$facts[[string]$fact.id] = if (Test-HasProperty -Object $fact -Name "constant") { $fact.constant } else { Get-NestedValue -Object $baseline -Path ([string]$fact.baselinePath) }
			}
			$runPath = Join-Path $controlDirectory ("run-{0}.json" -f ($runIndex + 1))
			Write-DeterministicJson -Path $runPath -Value ([ordered]@{
				schemaVersion = 1
				workloadId = $control.id
				runKind = "measured"
				runIndex = $runIndex + 1
				succeeded = $true
				settingsHash = $control.settingsHash
				sourceResultPaths = @((ConvertTo-RepositoryRelativePath -Path $rawPath))
				facts = $facts
				metrics = $metricValues
			})
			$measuredPaths += ConvertTo-RepositoryRelativePath -Path $runPath
		}
		$manifestWorkloads += ,[ordered]@{
			id = $control.id
			settingsHash = $control.settingsHash
			settings = $control.settings
			warmupResultPath = ConvertTo-RepositoryRelativePath -Path $warmupPath
			measuredResultPaths = $measuredPaths
		}
	}

	$passManifest = [ordered]@{
		schemaVersion = 1
		feature = "animation"
		reference = $expectedReference
		comparisonIdentity = $config.candidateTemplate.comparisonIdentity
		comparisonSettings = $config.candidateTemplate.comparisonSettings
		observations = $config.candidateTemplate.requiredObservations
		candidateBuild = [ordered]@{
			sourceCommit = "6d12e3a99d7c0d41899fc96fa18b02b61e5a0aa9"
			sourceTreeDirty = $false
			sourceDiffSha256 = $null
			buildConfiguration = "RelWithDebInfo"
			installedTreeRoot = ConvertTo-RepositoryRelativePath -Path $installedRoot
			artifacts = @([ordered]@{ path = ConvertTo-RepositoryRelativePath -Path $artifactPath; sha256 = Get-FileSha256 -Path $artifactPath })
		}
		rawInputPaths = @((ConvertTo-RepositoryRelativePath -Path $identityEvidencePath))
		workloads = $manifestWorkloads
	}
	$passManifestPath = Join-Path $selfTestRoot "candidate-pass.json"
	$passResultPath = Join-Path $selfTestRoot "result-pass.json"
	Write-DeterministicJson -Path $passManifestPath -Value $passManifest
	$passResult = Invoke-PerformanceComparison -CandidateManifestPath $passManifestPath -ResultPath $passResultPath -LockedBaselinePath $script:DefaultBaselinePath -PerformanceWorkloadsPath $script:DefaultWorkloadsPath -IsValidationOnly $false

	$regressionManifest = Read-JsonFile -Path $passManifestPath
	$regressionRunPath = Join-Path $selfTestRoot "focused-control-matrix/run-1-regression.json"
	$regressionRun = Read-JsonFile -Path (Resolve-RepositoryPath -Path ([string]$regressionManifest.workloads[0].measuredResultPaths[0]))
	$regressionRun.metrics.totalSeconds.value = [double]$baseline.focusedControlMatrix.aggregate.worstSeconds * 10.0
	Write-DeterministicJson -Path $regressionRunPath -Value $regressionRun
	$regressionManifest.workloads[0].measuredResultPaths[0] = ConvertTo-RepositoryRelativePath -Path $regressionRunPath
	$regressionManifestPath = Join-Path $selfTestRoot "candidate-regression.json"
	$regressionResultPath = Join-Path $selfTestRoot "result-regression.json"
	Write-DeterministicJson -Path $regressionManifestPath -Value $regressionManifest
	$regressionResult = Invoke-PerformanceComparison -CandidateManifestPath $regressionManifestPath -ResultPath $regressionResultPath -LockedBaselinePath $script:DefaultBaselinePath -PerformanceWorkloadsPath $script:DefaultWorkloadsPath -IsValidationOnly $false

	$invalidManifest = Read-JsonFile -Path $passManifestPath
	$invalidManifest | Add-Member -NotePropertyName thresholds -NotePropertyValue ([ordered]@{ weakened = $true })
	$invalidManifestPath = Join-Path $selfTestRoot "candidate-invalid-threshold.json"
	$invalidResultPath = Join-Path $selfTestRoot "result-invalid-threshold.json"
	Write-DeterministicJson -Path $invalidManifestPath -Value $invalidManifest
	$invalidResult = Invoke-PerformanceComparison -CandidateManifestPath $invalidManifestPath -ResultPath $invalidResultPath -LockedBaselinePath $script:DefaultBaselinePath -PerformanceWorkloadsPath $script:DefaultWorkloadsPath -IsValidationOnly $true

	$selfTestPassed = $passResult.exitCode -eq $script:ExitAccepted -and $regressionResult.exitCode -eq $script:ExitRegression -and $invalidResult.exitCode -eq $script:ExitInvalidEvidence
	$selfTestSummaryPath = Join-Path $selfTestRoot "self-test-summary.json"
	Write-DeterministicJson -Path $selfTestSummaryPath -Value ([ordered]@{
		schemaVersion = 1
		tool = "CompareAnimationPerformance"
		passed = $selfTestPassed
		expectedExitCodes = [ordered]@{ accepted = 0; regression = 2; invalidEvidence = 3 }
		observedExitCodes = [ordered]@{ accepted = $passResult.exitCode; regression = $regressionResult.exitCode; invalidEvidence = $invalidResult.exitCode }
		resultPaths = @(
			(ConvertTo-RepositoryRelativePath -Path $passResult.resultPath)
			(ConvertTo-RepositoryRelativePath -Path $regressionResult.resultPath)
			(ConvertTo-RepositoryRelativePath -Path $invalidResult.resultPath)
		)
	})
	Write-Host "Comparator self-test: $(if ($selfTestPassed) { 'PASS' } else { 'FAIL' })"
	Write-Host "Summary: $(ConvertTo-PortablePath -Path $selfTestSummaryPath)"
	if ($selfTestPassed) { return $script:ExitAccepted }
	return $script:ExitToolFailure
}

try {
	if ($PSCmdlet.ParameterSetName -eq "SelfTest") {
		exit (Invoke-ComparatorSelfTest)
	}
	$comparison = Invoke-PerformanceComparison -CandidateManifestPath $CandidateManifest -ResultPath $OutputPath -LockedBaselinePath $BaselinePath -PerformanceWorkloadsPath $WorkloadsPath -IsValidationOnly ([bool]$ValidateOnly) -WholeEngineCoverageMatrixPath $CoverageMatrixPath -RequireWholeEngineCoverage ([bool]$FinalPhaseMatrix)
	Write-Host "Animation performance comparison outcome: $($comparison.outcome)"
	Write-Host "Result: $(ConvertTo-PortablePath -Path $comparison.resultPath)"
	exit $comparison.exitCode
}
catch {
	# Invocation and tool failures are distinct from invalid evidence. When the
	# output boundary is valid, preserve a deterministic machine-readable error.
	$errorMessage = $_.Exception.Message
	if ($PSCmdlet.ParameterSetName -eq "Compare") {
		try {
			$resolvedFailureOutput = Resolve-RepositoryPath -Path $OutputPath
			Write-DeterministicJson -Path $resolvedFailureOutput -Value ([ordered]@{
				schemaVersion = 1
				tool = "CompareAnimationPerformance"
				feature = "animation"
				task = "animation-performance-evidence"
				outcome = "error"
				passed = $false
				exitCode = $script:ExitToolFailure
				diagnostics = @($errorMessage)
			})
		}
		catch {
			# A path-boundary or filesystem error can prevent the fallback report.
			# The nonzero exit code and console message remain unambiguous.
		}
	}
	Write-Error "Animation performance comparator failed: $errorMessage"
	exit $script:ExitToolFailure
}
