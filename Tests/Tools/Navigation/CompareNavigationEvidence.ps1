[CmdletBinding()]
param(
	[string[]]$EvidenceFiles = @(),
	[string]$Manifest,
	[string]$OutputPath,
	[string[]]$BaselineFiles = @(),
	[string[]]$CandidateFiles = @(),
	[string]$BaselineDirectory,
	[string]$CandidateDirectory,
	[ValidateRange(5, 100)][int]$MinimumRuns = 5
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

# Keep the gates in one place so a review can distinguish a failed measurement
# from a changed threshold. These values are the frozen request-latency targets.
$p95LimitMilliseconds = 100.0
$p99LimitMilliseconds = 250.0
$sha256Pattern = '^[A-Fa-f0-9]{64}$'

function Add-Failure {
	param([System.Collections.Generic.List[string]]$Failures, [string]$Message)
	$Failures.Add($Message)
}

function Get-PropertyValue {
	param([object]$Object, [string]$Name)
	if($null -eq $Object) { return $null }
	$property = $Object.PSObject.Properties[$Name]
	if($null -eq $property) { return $null }
	return $property.Value
}

function ConvertTo-CanonicalValue {
	param([object]$Value)
	if($null -eq $Value -or $Value -is [string] -or $Value -is [ValueType]) { return $Value }
	if($Value -is [System.Collections.IEnumerable]) {
		# The unary comma preserves a single-element JSON array. Without it,
		# PowerShell pipeline unrolling changes `["semantic"]` into `"semantic"`
		# and makes a valid canonical digest impossible to reproduce.
		return ,@($Value | ForEach-Object { ConvertTo-CanonicalValue $_ })
	}

	# JSON object order is not semantic. Sorting every property makes the checked
	# content digest stable across the C++ runner and PowerShell tooling.
	$ordered = [ordered]@{}
	foreach($property in @($Value.PSObject.Properties | Sort-Object Name)) {
		$ordered[$property.Name] = ConvertTo-CanonicalValue $property.Value
	}
	return [pscustomobject]$ordered
}

function Get-CanonicalSha256 {
	param([object]$Value)
	$canonicalJson = (ConvertTo-CanonicalValue $Value | ConvertTo-Json -Compress -Depth 64)
	$bytes = [System.Text.UTF8Encoding]::new($false).GetBytes($canonicalJson)
	$hash = [System.Security.Cryptography.SHA256]::HashData($bytes)
	return [Convert]::ToHexString($hash).ToLowerInvariant()
}

function Get-ManifestFiles {
	param([string]$ManifestPath)
	if([string]::IsNullOrWhiteSpace($ManifestPath)) { return @() }
	$absoluteManifest = [IO.Path]::GetFullPath($ManifestPath)
	# Windows PowerShell 5.1 has no ConvertFrom-Json -Depth parameter; nested
	# manifest/evidence objects are still fully parsed by its JSON reader.
	$manifestObject = Get-Content -LiteralPath $absoluteManifest -Raw | ConvertFrom-Json
	$manifestFiles = @(Get-PropertyValue $manifestObject 'evidenceFiles')
	if($manifestFiles.Count -eq 0) { throw "Manifest '$absoluteManifest' has no evidenceFiles entries." }
	$manifestDirectory = [IO.Path]::GetDirectoryName($absoluteManifest)
	return @($manifestFiles | ForEach-Object {
		if([IO.Path]::IsPathFullyQualified([string]$_)) { [IO.Path]::GetFullPath([string]$_) }
		else { [IO.Path]::GetFullPath((Join-Path $manifestDirectory ([string]$_))) }
	})
}

function Test-BooleanTrue {
	param([object]$Value)
	return $Value -is [bool] -and $Value
}

function Test-NonNegativeInteger {
	param([object]$Value, [bool]$MustBePositive = $false)
	if($null -eq $Value -or $Value -isnot [ValueType]) { return $false }
	try {
		$number = [decimal]$Value
		return [decimal]::Truncate($number) -eq $number -and $(if($MustBePositive) { $number -gt 0 } else { $number -ge 0 })
	} catch { return $false }
}

$inputFailures = [System.Collections.Generic.List[string]]::new()
$qualificationRequested = $BaselineFiles.Count -gt 0 -or $CandidateFiles.Count -gt 0 -or -not [string]::IsNullOrWhiteSpace($BaselineDirectory) -or -not [string]::IsNullOrWhiteSpace($CandidateDirectory)
$paths = [System.Collections.Generic.List[string]]::new()
foreach($path in @($EvidenceFiles)) {
	if(-not [string]::IsNullOrWhiteSpace($path)) { $paths.Add([IO.Path]::GetFullPath($path)) }
}
try { foreach($path in Get-ManifestFiles $Manifest) { $paths.Add($path) } }
catch { Add-Failure $inputFailures $_.Exception.Message }

$uniquePaths = @($paths | Sort-Object -Unique)
if($uniquePaths.Count -eq 0 -and -not $qualificationRequested) { Add-Failure $inputFailures 'No evidence files were supplied.' }
if($uniquePaths.Count -gt 64) { Add-Failure $inputFailures 'At most 64 evidence files may be compared at once.' }

$records = [System.Collections.Generic.List[object]]::new()
foreach($path in $uniquePaths) {
	$failures = [System.Collections.Generic.List[string]]::new()
	$evidence = $null
	try {
		$evidence = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
	} catch {
		Add-Failure $failures "Unreadable JSON: $($_.Exception.Message)"
	}

	if($null -ne $evidence) {
		if((Get-PropertyValue $evidence 'schemaVersion') -ne 1) { Add-Failure $failures 'schemaVersion must be 1.' }
		$identity = Get-PropertyValue $evidence 'identity'
		$identityNames = if($null -eq $identity) { @() } else { @($identity.PSObject.Properties.Name | Sort-Object) }
		$expectedIdentityNames = @('buildSha256', 'powerMode', 'renderer', 'settingsSha256', 'sourceSha256', 'workerCount', 'workloadSha256') | Sort-Object
		if((ConvertTo-Json $identityNames -Compress) -ne (ConvertTo-Json $expectedIdentityNames -Compress)) { Add-Failure $failures 'identity contains an unexpected or missing provenance field.' }
		foreach($name in @('buildSha256', 'sourceSha256', 'settingsSha256', 'workloadSha256')) {
			if((Get-PropertyValue $identity $name) -notmatch $sha256Pattern) { Add-Failure $failures "identity.$name is not a SHA-256 digest." }
		}
		foreach($name in @('renderer', 'powerMode')) {
			if([string]::IsNullOrWhiteSpace([string](Get-PropertyValue $identity $name))) { Add-Failure $failures "identity.$name is required." }
		}
		if(-not (Test-NonNegativeInteger (Get-PropertyValue $identity 'workerCount') $true)) { Add-Failure $failures 'identity.workerCount must be a positive integer.' }

		$content = Get-PropertyValue $evidence 'content'
		$contentHash = Get-PropertyValue $evidence 'contentSha256'
		if($contentHash -notmatch $sha256Pattern) { Add-Failure $failures 'contentSha256 is not a SHA-256 digest.' }
		elseif($null -eq $content) { Add-Failure $failures 'content is required to verify contentSha256.' }
		elseif((Get-CanonicalSha256 $content) -ne $contentHash.ToLowerInvariant()) { Add-Failure $failures 'contentSha256 does not match canonical content.' }

		if((Get-PropertyValue $evidence 'lane') -notin @('rendered', 'headless')) { Add-Failure $failures 'lane must be rendered or headless.' }
		if([string]::IsNullOrWhiteSpace([string](Get-PropertyValue $evidence 'workloadId'))) { Add-Failure $failures 'workloadId is required.' }
		if(-not (Test-BooleanTrue (Get-PropertyValue $evidence 'success'))) { Add-Failure $failures 'success must be true.' }

		$denominator = Get-PropertyValue $evidence 'denominator'
		$denominatorNames = @('attempted', 'completed', 'cancelled', 'rejected', 'failed', 'timedOut')
		foreach($name in $denominatorNames) {
			if(-not (Test-NonNegativeInteger (Get-PropertyValue $denominator $name) ($name -eq 'attempted'))) { Add-Failure $failures "denominator.$name is invalid." }
		}
		if($failures.Count -eq 0) {
			$accounted = [decimal]$denominator.completed + [decimal]$denominator.cancelled + [decimal]$denominator.rejected + [decimal]$denominator.failed + [decimal]$denominator.timedOut
			if($accounted -ne [decimal]$denominator.attempted) { Add-Failure $failures 'denominator outcomes do not account for every attempt.' }
		}

		$latency = Get-PropertyValue $evidence 'latency'
		try {
			$p95 = [double](Get-PropertyValue $latency 'p95Milliseconds')
			$p99 = [double](Get-PropertyValue $latency 'p99Milliseconds')
			if([double]::IsNaN($p95) -or [double]::IsInfinity($p95) -or $p95 -lt 0.0 -or $p95 -gt $p95LimitMilliseconds) { Add-Failure $failures "p95 exceeds $p95LimitMilliseconds ms or is invalid." }
			if([double]::IsNaN($p99) -or [double]::IsInfinity($p99) -or $p99 -lt $p95 -or $p99 -gt $p99LimitMilliseconds) { Add-Failure $failures "p99 exceeds $p99LimitMilliseconds ms or is invalid." }
		} catch { Add-Failure $failures 'latency p95/p99 values are required numeric measurements.' }

		$safety = Get-PropertyValue $evidence 'safety'
		$safetyNames = if($null -eq $safety) { @() } else { @($safety.PSObject.Properties.Name | Sort-Object) }
		$expectedSafetyNames = @('boundedStatus', 'noCrash', 'noDeadlock', 'noStaleOwnerPublications', 'removalSafety')
		if((ConvertTo-Json $safetyNames -Compress) -ne (ConvertTo-Json $expectedSafetyNames -Compress)) { Add-Failure $failures 'safety contains an unexpected or missing field.' }
		foreach($name in @('noCrash', 'noDeadlock', 'noStaleOwnerPublications', 'removalSafety', 'boundedStatus')) {
			if(-not (Test-BooleanTrue (Get-PropertyValue $safety $name))) { Add-Failure $failures "safety.$name must be true." }
		}
	}

	$records.Add([pscustomobject][ordered]@{
		file = $path
		passed = $failures.Count -eq 0
		lane = Get-PropertyValue $evidence 'lane'
		workloadId = Get-PropertyValue $evidence 'workloadId'
		identity = Get-PropertyValue $evidence 'identity'
		failures = @($failures)
	})
}

# Qualification deliberately has a separate, fail-closed input path. A single
# valid record is useful for absolute gates, but cannot establish a performance
# claim. Require five identity-matched runs on each side and require the
# runner-emitted dynamic publication metric; sample counts are not a timing
# substitute and must never be treated as one.
$qualificationFailures = [System.Collections.Generic.List[string]]::new()
$qualification = $null
if($qualificationRequested) {
	function Get-Median([double[]]$Values) {
		$sorted = @($Values | Sort-Object)
		if($sorted.Count -eq 0) { throw 'Cannot calculate a median for an empty set.' }
		$middle = [int][Math]::Floor($sorted.Count / 2)
		if($sorted.Count % 2 -eq 1) { return [double]$sorted[$middle] }
		return ([double]$sorted[$middle - 1] + [double]$sorted[$middle]) / 2.0
	}
	function Get-IdentityKey([object]$Evidence, [bool]$IncludeBuild) {
		$identity = Get-PropertyValue $Evidence 'identity'
		$parts = [System.Collections.Generic.List[string]]::new()
		$parts.Add([string](Get-PropertyValue $Evidence 'workloadId'))
		$parts.Add([string](Get-PropertyValue $Evidence 'lane'))
		if($IncludeBuild) { $parts.Add([string](Get-PropertyValue $identity 'buildSha256')) }
		foreach($name in @('sourceSha256', 'settingsSha256', 'workloadSha256', 'renderer', 'powerMode', 'workerCount')) {
			$parts.Add([string](Get-PropertyValue $identity $name))
		}
		return [string]::Join('|', $parts)
	}
	function Get-QualificationPaths([string[]]$Files, [string]$Directory, [string]$Label) {
		$paths = [System.Collections.Generic.List[string]]::new()
		foreach($file in @($Files)) { if(-not [string]::IsNullOrWhiteSpace($file)) { $paths.Add([IO.Path]::GetFullPath($file)) } }
		if(-not [string]::IsNullOrWhiteSpace($Directory)) {
			$root = [IO.Path]::GetFullPath($Directory)
			if(-not (Test-Path -LiteralPath $root -PathType Container)) { Add-Failure $qualificationFailures "$Label directory does not exist: $root" }
			else { foreach($file in @(Get-ChildItem -LiteralPath $root -Recurse -File -Filter '*.json')) { $paths.Add($file.FullName) } }
		}
		return @($paths | Sort-Object -Unique)
	}
	$baselinePaths = @(Get-QualificationPaths $BaselineFiles $BaselineDirectory 'Baseline')
	$candidatePaths = @(Get-QualificationPaths $CandidateFiles $CandidateDirectory 'Candidate')
	function Read-QualificationRecords([string[]]$Paths, [string]$Label) {
		$result = [System.Collections.Generic.List[object]]::new()
		foreach($path in $Paths) {
			try { $value = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json }
			catch { Add-Failure $qualificationFailures "$Label file '$path' is unreadable: $($_.Exception.Message)"; continue }
			if($null -eq $value -or (Get-PropertyValue $value 'schemaVersion') -ne 1 -or -not (Test-BooleanTrue (Get-PropertyValue $value 'success'))) { Add-Failure $qualificationFailures "$Label file '$path' is not a successful schema-v1 evidence record."; continue }
			$identity = Get-PropertyValue $value 'identity'
			$identityValid = $true
			foreach($name in @('buildSha256', 'sourceSha256', 'settingsSha256', 'workloadSha256')) {
				if((Get-PropertyValue $identity $name) -notmatch $sha256Pattern) { Add-Failure $qualificationFailures "$Label file '$path' has invalid identity.$name."; $identityValid = $false }
			}
			foreach($name in @('renderer', 'powerMode')) {
				if([string]::IsNullOrWhiteSpace([string](Get-PropertyValue $identity $name))) { Add-Failure $qualificationFailures "$Label file '$path' lacks identity.$name."; $identityValid = $false }
			}
			if(-not (Test-NonNegativeInteger (Get-PropertyValue $identity 'workerCount') $true)) { Add-Failure $qualificationFailures "$Label file '$path' has invalid identity.workerCount."; $identityValid = $false }
			if((Get-PropertyValue $value 'lane') -notin @('rendered', 'headless') -or [string]::IsNullOrWhiteSpace([string](Get-PropertyValue $value 'workloadId'))) { Add-Failure $qualificationFailures "$Label file '$path' has invalid lane or workloadId."; $identityValid = $false }
			$content = Get-PropertyValue $value 'content'
			$contentHash = Get-PropertyValue $value 'contentSha256'
			if($contentHash -notmatch $sha256Pattern -or $null -eq $content -or (Get-CanonicalSha256 $content) -ne $contentHash.ToLowerInvariant()) { Add-Failure $qualificationFailures "$Label file '$path' has invalid content evidence."; $identityValid = $false }
			$denominator = Get-PropertyValue $value 'denominator'
			try {
				$attempted = [decimal](Get-PropertyValue $denominator 'attempted')
				$accounted = [decimal](Get-PropertyValue $denominator 'completed') + [decimal](Get-PropertyValue $denominator 'cancelled') + [decimal](Get-PropertyValue $denominator 'rejected') + [decimal](Get-PropertyValue $denominator 'failed') + [decimal](Get-PropertyValue $denominator 'timedOut')
				if($attempted -le 0 -or $accounted -ne $attempted) { throw 'invalid denominator' }
			} catch { Add-Failure $qualificationFailures "$Label file '$path' has an invalid denominator."; $identityValid = $false }
			$latency = Get-PropertyValue $value 'latency'
			try {
				$p95 = [double](Get-PropertyValue $latency 'p95Milliseconds'); $p99 = [double](Get-PropertyValue $latency 'p99Milliseconds')
				if([double]::IsNaN($p95) -or [double]::IsInfinity($p95) -or $p95 -lt 0 -or $p95 -gt $p95LimitMilliseconds -or [double]::IsNaN($p99) -or [double]::IsInfinity($p99) -or $p99 -lt $p95 -or $p99 -gt $p99LimitMilliseconds) { throw 'invalid latency' }
			} catch { Add-Failure $qualificationFailures "$Label file '$path' fails the absolute latency gates."; $identityValid = $false }
			$safety = Get-PropertyValue $value 'safety'
			foreach($name in @('noCrash', 'noDeadlock', 'noStaleOwnerPublications', 'removalSafety', 'boundedStatus')) {
				if(-not (Test-BooleanTrue (Get-PropertyValue $safety $name))) { Add-Failure $qualificationFailures "$Label file '$path' fails safety.$name."; $identityValid = $false }
			}
			$metrics = Get-PropertyValue $value 'metrics'
			$dynamic = Get-PropertyValue $metrics 'dynamicPublicationResponsivenessMilliseconds'
			$route = Get-PropertyValue $metrics 'routeLatencyMilliseconds'
			if($null -eq $route) { Add-Failure $qualificationFailures "$Label file '$path' lacks metrics.routeLatencyMilliseconds."; continue }
			if($null -eq $dynamic) { Add-Failure $qualificationFailures "$Label file '$path' lacks metrics.dynamicPublicationResponsivenessMilliseconds."; continue }
			try { $route = [double]$route; $dynamic = [double]$dynamic } catch { Add-Failure $qualificationFailures "$Label file '$path' has non-numeric qualification metrics."; continue }
			if([double]::IsNaN($route) -or [double]::IsInfinity($route) -or $route -lt 0 -or [double]::IsNaN($dynamic) -or [double]::IsInfinity($dynamic) -or $dynamic -lt 0) { Add-Failure $qualificationFailures "$Label file '$path' has invalid qualification metrics."; continue }
			if(-not $identityValid) { continue }
			$result.Add([pscustomobject]@{ cohortKey = Get-IdentityKey $value $true; comparisonKey = Get-IdentityKey $value $false; buildSha256 = Get-PropertyValue $identity 'buildSha256'; route = $route; dynamic = $dynamic; path = $path })
		}
		return @($result)
	}
	$baseline = @(Read-QualificationRecords $baselinePaths 'Baseline')
	$candidate = @(Read-QualificationRecords $candidatePaths 'Candidate')
	if($baseline.Count -lt $MinimumRuns) { Add-Failure $qualificationFailures "Baseline requires at least $MinimumRuns valid runs; found $($baseline.Count)." }
	if($candidate.Count -lt $MinimumRuns) { Add-Failure $qualificationFailures "Candidate requires at least $MinimumRuns valid runs; found $($candidate.Count)." }
	$comparisons = [System.Collections.Generic.List[object]]::new()
	$baselineCohorts = @($baseline | Group-Object cohortKey)
	$candidateCohorts = @($candidate | Group-Object cohortKey)
	if($baselineCohorts.Count -ne 1) { Add-Failure $qualificationFailures "Baseline must contain exactly one internally identity-matched cohort; found $($baselineCohorts.Count)." }
	if($candidateCohorts.Count -ne 1) { Add-Failure $qualificationFailures "Candidate must contain exactly one internally identity-matched cohort; found $($candidateCohorts.Count)." }
	if($baselineCohorts.Count -eq 1 -and $candidateCohorts.Count -eq 1) {
		$baselineGroup = @($baselineCohorts[0].Group)
		$candidateGroup = @($candidateCohorts[0].Group)
		if($baselineGroup.Count -lt $MinimumRuns) { Add-Failure $qualificationFailures "Baseline cohort requires at least $MinimumRuns runs; found $($baselineGroup.Count)." }
		if($candidateGroup.Count -lt $MinimumRuns) { Add-Failure $qualificationFailures "Candidate cohort requires at least $MinimumRuns runs; found $($candidateGroup.Count)." }
		if($baselineGroup[0].comparisonKey -ne $candidateGroup[0].comparisonKey) { Add-Failure $qualificationFailures 'Baseline and candidate fixture/settings/workload/runtime identities do not match.' }
		elseif($baselineGroup.Count -ge $MinimumRuns -and $candidateGroup.Count -ge $MinimumRuns) {
			$bRoute = Get-Median @($baselineGroup | Select-Object -ExpandProperty route)
			$cRoute = Get-Median @($candidateGroup | Select-Object -ExpandProperty route)
			$bDynamic = Get-Median @($baselineGroup | Select-Object -ExpandProperty dynamic)
			$cDynamic = Get-Median @($candidateGroup | Select-Object -ExpandProperty dynamic)
			$comparisons.Add([ordered]@{ identity = $baselineGroup[0].comparisonKey; baselineBuildSha256 = $baselineGroup[0].buildSha256; candidateBuildSha256 = $candidateGroup[0].buildSha256; baselineRouteLatencyMilliseconds = $bRoute; candidateRouteLatencyMilliseconds = $cRoute; routeLimitMilliseconds = $bRoute * 1.05; routePassed = $cRoute -le ($bRoute * 1.05); baselineDynamicPublicationResponsivenessMilliseconds = $bDynamic; candidateDynamicPublicationResponsivenessMilliseconds = $cDynamic; dynamicLimitMilliseconds = $bDynamic * 1.05; dynamicPassed = $cDynamic -le ($bDynamic * 1.05) })
		}
	}
	if($comparisons.Count -eq 0) { Add-Failure $qualificationFailures 'No identity-matched baseline/candidate workload group was available.' }
	$qualification = [ordered]@{ minimumRuns = $MinimumRuns; passed = $qualificationFailures.Count -eq 0 -and @($comparisons | Where-Object { -not $_.routePassed -or -not $_.dynamicPassed }).Count -eq 0; failures = @($qualificationFailures); comparisons = @($comparisons) }
}

# Semantic and soak workloads must be represented in both installed lanes with
# the same frozen provenance. Performance and lifecycle/usability records are
# intentionally single-lane measurements.
$parityFailures = [System.Collections.Generic.List[string]]::new()
$workloadIds = @($records | ForEach-Object { $_.workloadId } | Where-Object { -not [string]::IsNullOrWhiteSpace([string]$_) } | Sort-Object -Unique)
foreach($workloadId in $workloadIds) {
	$workloadRecords = @($records | Where-Object { $_.workloadId -eq $workloadId })
	$requiresParity = $workloadId -in @('semantic', 'soak')
	if(-not $requiresParity) { continue }
	$workloadLanes = @($workloadRecords.lane | Sort-Object -Unique)
	if(@($workloadLanes | Where-Object { $_ -eq 'rendered' }).Count -lt 1 -or @($workloadLanes | Where-Object { $_ -eq 'headless' }).Count -lt 1) {
		Add-Failure $parityFailures "workload '$workloadId' is missing a rendered or headless lane."
		continue
	}
	# Renderer is lane identity by design (rendered versus headless); all other
	# build/source/settings/workload provenance must remain byte-identical.
	$identityValues = @($workloadRecords | ForEach-Object {
		$record = $_
		$comparable = [ordered]@{}
		foreach($propertyName in @($record.identity.PSObject.Properties.Name | Where-Object { $_ -ne 'renderer' })) {
			$comparable[$propertyName] = Get-PropertyValue $record.identity $propertyName
		}
		ConvertTo-CanonicalValue ([pscustomobject]$comparable) | ConvertTo-Json -Compress -Depth 64
	} | Sort-Object -Unique)
	if($identityValues.Count -ne 1) { Add-Failure $parityFailures "workload '$workloadId' has non-identical lane identities." }
}

$hasRequiredRecords = $records.Count -gt 0 -or $qualificationRequested
$passed = $inputFailures.Count -eq 0 -and $hasRequiredRecords -and @($records | Where-Object { -not $_.passed }).Count -eq 0 -and $parityFailures.Count -eq 0
$passed = $passed -and ($null -eq $qualification -or $qualification.passed)
$comparison = [pscustomobject][ordered]@{
	schemaVersion = 1
	passed = $passed
	thresholds = [ordered]@{ p95Milliseconds = $p95LimitMilliseconds; p99Milliseconds = $p99LimitMilliseconds }
	inputFailures = @($inputFailures)
	parityFailures = @($parityFailures)
	qualification = $qualification
	evidence = @($records)
}

$comparisonJson = $comparison | ConvertTo-Json -Depth 32
if(-not [string]::IsNullOrWhiteSpace($OutputPath)) {
	$absoluteOutput = [IO.Path]::GetFullPath($OutputPath)
	$directory = [IO.Path]::GetDirectoryName($absoluteOutput)
	if(-not [string]::IsNullOrWhiteSpace($directory)) { [IO.Directory]::CreateDirectory($directory) | Out-Null }
	[IO.File]::WriteAllText($absoluteOutput, $comparisonJson, [System.Text.UTF8Encoding]::new($false))
}

Write-Output $comparisonJson
if(-not $passed) { exit 1 }
