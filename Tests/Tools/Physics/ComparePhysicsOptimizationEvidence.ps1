# ComparePhysicsOptimizationEvidence.ps1 - fail-closed physics acceptance comparison.

[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string]$BaselineDirectory,
	[Parameter(Mandatory=$true)][string]$CandidateDirectory,
	[Parameter(Mandatory=$true)][string]$Schema,
	[Parameter(Mandatory=$true)][string]$Output
)

$ErrorActionPreference='Stop'

function New-Gates {
	return [ordered]@{
		identityComparable=$false; sampleCoveragePassed=$false; unchangedPreparationPassed=$false
		mixedTickPassed=$false; emptyLowEventPassed=$false; contactEventPassed=$false
		ragdollPassed=$false; bulkAdmissionPassed=$false; workerScalingPassed=$false
		correctnessPassed=$false; repeatedRunsPassed=$false; qualityPassed=$false; buildInstallPassed=$false
	}
}

function Read-EvidenceSet {
	param([Parameter(Mandatory=$true)][string]$Directory)
	if(-not (Test-Path -LiteralPath $Directory -PathType Container)) { return @() }
	$values = @()
	foreach($file in Get-ChildItem -LiteralPath $Directory -Filter '*.json' -File | Sort-Object Name) {
		try { $value = Get-Content -Raw -LiteralPath $file.FullName | ConvertFrom-Json }
		catch { continue }
		if($value.schemaVersion -eq 1 -and $value.identity -and $value.runs -and $value.gates) {
			$value | Add-Member -NotePropertyName EvidencePath -NotePropertyValue $file.FullName -Force
			$values += $value
		}
	}
	return @($values)
}

function Read-CaptureIndex {
	param([string]$Directory)
	$path = Join-Path $Directory 'capture-index.json'
	if (-not (Test-Path -LiteralPath $path)) { $path = Join-Path $Directory 'baseline-index.json' }
	if (Test-Path -LiteralPath $path) { try { return Get-Content -Raw $path | ConvertFrom-Json } catch {} }
	return $null
}

function Add-Failure {
	param([System.Collections.Generic.List[string]]$Failures,[string]$Message)
	if(-not $Failures.Contains($Message)) { $Failures.Add($Message) }
}

function Test-Sha256([object]$Value) { return $Value -is [string] -and $Value -match '^[A-Fa-f0-9]{64}$' }

# Validate evidence with the committed Draft 2020-12 schema without depending on
# a machine-wide PowerShell module. The feature schema deliberately uses a small,
# stable keyword set; supporting that set here keeps source and installed-tree
# comparisons identical on Windows PowerShell 5.1 and newer hosts.
function Resolve-SchemaReference {
	param([object]$Root,[string]$Reference)
	if (-not $Reference.StartsWith('#/')) { return $null }
	$current=$Root
	foreach($encodedPart in $Reference.Substring(2).Split('/')) {
		$part=$encodedPart.Replace('~1','/').Replace('~0','~')
		$property=$current.PSObject.Properties[$part]
		if ($null -eq $property) { return $null }
		$current=$property.Value
	}
	return $current
}

function Test-JsonValueType {
	param([object]$Value,[string]$Expected)
	switch($Expected) {
		'null' { return $null -eq $Value }
		'boolean' { return $Value -is [bool] }
		'string' { return $Value -is [string] }
		'number' { return $Value -is [byte] -or $Value -is [sbyte] -or $Value -is [int16] -or $Value -is [uint16] -or $Value -is [int32] -or $Value -is [uint32] -or $Value -is [int64] -or $Value -is [uint64] -or $Value -is [single] -or $Value -is [double] -or $Value -is [decimal] }
		'integer' {
			if (-not (Test-JsonValueType $Value 'number')) { return $false }
			return [double]$Value -eq [Math]::Truncate([double]$Value)
		}
		'array' { return $Value -is [System.Collections.IList] -and $Value -isnot [string] }
		'object' { return $null -ne $Value -and $Value -isnot [string] -and $Value -isnot [System.Collections.IList] -and ($Value -is [System.Collections.IDictionary] -or $Value -is [pscustomobject]) }
		default { return $false }
	}
}

function Test-JsonSchemaNode {
	param(
		[object]$Value,
		[object]$SchemaNode,
		[object]$SchemaRoot,
		[string]$Path,
		[System.Collections.Generic.List[string]]$Errors
	)
	if ($null -eq $SchemaNode) { $Errors.Add("$Path references a missing schema node"); return $false }
	if ($SchemaNode.PSObject.Properties['$ref']) {
		$target=Resolve-SchemaReference $SchemaRoot ([string]$SchemaNode.'$ref')
		return Test-JsonSchemaNode $Value $target $SchemaRoot $Path $Errors
	}

	$valid=$true
	if ($SchemaNode.PSObject.Properties['oneOf']) {
		$matches=0
		foreach($branch in @($SchemaNode.oneOf)) {
			$branchErrors=[System.Collections.Generic.List[string]]::new()
			if(Test-JsonSchemaNode $Value $branch $SchemaRoot $Path $branchErrors){++$matches}
		}
		if($matches -ne 1){$Errors.Add("$Path must match exactly one schema alternative");$valid=$false}
	}
	if ($SchemaNode.PSObject.Properties['allOf']) {
		foreach($branch in @($SchemaNode.allOf)) { if(-not (Test-JsonSchemaNode $Value $branch $SchemaRoot $Path $Errors)){$valid=$false} }
	}
	if ($SchemaNode.PSObject.Properties['if']) {
		$conditionErrors=[System.Collections.Generic.List[string]]::new()
		# JSON Schema conditionals are assertions, not ordinary validation
		# failures. Evaluate the condition in isolation and apply only its branch.
		$conditionMatches=Test-JsonSchemaNode $Value $SchemaNode.if $SchemaRoot $Path $conditionErrors
		if($conditionMatches -and $SchemaNode.PSObject.Properties['then']) {
			if(-not (Test-JsonSchemaNode $Value $SchemaNode.then $SchemaRoot $Path $Errors)){$valid=$false}
		}
	}

	if ($SchemaNode.PSObject.Properties['const'] -and $Value -ne $SchemaNode.const) { $Errors.Add("$Path does not equal the required constant"); $valid=$false }
	if ($SchemaNode.PSObject.Properties['enum']) {
		$found=$false
		foreach($allowed in @($SchemaNode.enum)){if($Value -eq $allowed){$found=$true;break}}
		if(-not $found){$Errors.Add("$Path is outside the allowed enumeration");$valid=$false}
	}

	if ($SchemaNode.PSObject.Properties['type']) {
		$typeMatches=$false
		foreach($expected in @($SchemaNode.type)){if(Test-JsonValueType $Value ([string]$expected)){$typeMatches=$true;break}}
		if(-not $typeMatches){$Errors.Add("$Path has the wrong JSON type");return $false}
	}
	if ($Value -is [string]) {
		if($SchemaNode.PSObject.Properties['minLength'] -and $Value.Length -lt [int]$SchemaNode.minLength){$Errors.Add("$Path is shorter than minLength");$valid=$false}
		if($SchemaNode.PSObject.Properties['pattern'] -and $Value -notmatch [string]$SchemaNode.pattern){$Errors.Add("$Path does not match the required pattern");$valid=$false}
	}
	if (Test-JsonValueType $Value 'number') {
		if($SchemaNode.PSObject.Properties['minimum'] -and [double]$Value -lt [double]$SchemaNode.minimum){$Errors.Add("$Path is below minimum");$valid=$false}
		if($SchemaNode.PSObject.Properties['maximum'] -and [double]$Value -gt [double]$SchemaNode.maximum){$Errors.Add("$Path is above maximum");$valid=$false}
		if($SchemaNode.PSObject.Properties['exclusiveMinimum'] -and [double]$Value -le [double]$SchemaNode.exclusiveMinimum){$Errors.Add("$Path is not above exclusiveMinimum");$valid=$false}
	}
	if (Test-JsonValueType $Value 'array') {
		if($SchemaNode.PSObject.Properties['minItems'] -and $Value.Count -lt [int]$SchemaNode.minItems){$Errors.Add("$Path has fewer than minItems");$valid=$false}
		if($SchemaNode.PSObject.Properties['items']) {
			for($index=0;$index -lt $Value.Count;++$index){if(-not (Test-JsonSchemaNode $Value[$index] $SchemaNode.items $SchemaRoot "$Path[$index]" $Errors)){$valid=$false}}
		}
	}
	if (Test-JsonValueType $Value 'object') {
		$names=@($Value.PSObject.Properties.Name)
		if ($SchemaNode.PSObject.Properties['required']) {
			foreach($required in @($SchemaNode.required)) {
				if($names -notcontains [string]$required){$Errors.Add("$Path is missing required property $required");$valid=$false}
			}
		}
		if($SchemaNode.PSObject.Properties['properties']) {
			foreach($property in $SchemaNode.properties.PSObject.Properties) {
				if($names -contains $property.Name){if(-not (Test-JsonSchemaNode $Value.($property.Name) $property.Value $SchemaRoot "$Path.$($property.Name)" $Errors)){$valid=$false}}
			}
		}
		if($SchemaNode.PSObject.Properties['additionalProperties'] -and $SchemaNode.additionalProperties -isnot [bool]) {
			$declared=if($SchemaNode.PSObject.Properties['properties']){@($SchemaNode.properties.PSObject.Properties.Name)}else{@()}
			foreach($name in $names | Where-Object { $declared -notcontains $_ -and -not ($Path -eq '$' -and $_ -eq 'EvidencePath') }) {
				if(-not (Test-JsonSchemaNode $Value.$name $SchemaNode.additionalProperties $SchemaRoot "$Path.$name" $Errors)){$valid=$false}
			}
		}
		elseif($SchemaNode.PSObject.Properties['additionalProperties'] -and -not [bool]$SchemaNode.additionalProperties) {
			$declared=if($SchemaNode.PSObject.Properties['properties']){@($SchemaNode.properties.PSObject.Properties.Name)}else{@()}
			foreach($name in $names | Where-Object { $declared -notcontains $_ -and -not ($Path -eq '$' -and $_ -eq 'EvidencePath') }) {$Errors.Add("$Path contains unsupported property $name");$valid=$false}
		}
	}
	return $valid
}

function Test-MinimumShape {
	param([object]$Evidence,[System.Collections.Generic.List[string]]$Failures)
	$schemaErrors=[System.Collections.Generic.List[string]]::new()
	$valid=Test-JsonSchemaNode $Evidence $script:EvidenceSchema $script:EvidenceSchema '$' $schemaErrors
	foreach($schemaError in $schemaErrors){Add-Failure $Failures "schema validation failed for $($Evidence.EvidencePath): $schemaError"}
	$valid = $valid -and $Evidence.schemaVersion -eq 1 -and $Evidence.identity.build -and $Evidence.identity.environment -and $Evidence.identity.workload -and $Evidence.identity.workers -and $Evidence.identity.quality -and $Evidence.identity.protocol
	$valid = $valid -and (Test-Sha256 $Evidence.identity.build.installedTreeHash)
	$valid = $valid -and (Test-Sha256 $Evidence.identity.workload.manifestSha256)
	$valid = $valid -and (Test-Sha256 $Evidence.identity.workload.sceneSha256)
	$valid = $valid -and @($Evidence.runs).Count -ge 5
	foreach($run in @($Evidence.runs)) {
		$valid = $valid -and $null -ne $run.jobs -and $null -ne $run.correctness -and $run.completedSteps -ge 1
		$stageNames = @('authoringPreparation','providerCommit','simulation','extraction','eventPublication','stateValidation','snapshotConstruction','transactionConstruction')
		foreach($stageName in $stageNames) {
			$metric = $run.stages.$stageName
			if($null -eq $metric -or @('available','unavailable','not-applicable') -notcontains $metric.availability) { $valid=$false }
			if($metric.availability -ne 'available' -and ($null -ne $metric.median -or $null -ne $metric.p95)) { $valid=$false }
		}
	}
	if(-not $valid) { Add-Failure $Failures "schema/shape validation failed for $($Evidence.EvidencePath)" }
	return $valid
}

function Get-MetricMedian {
	param([object]$Evidence,[string]$Stage,[string]$Field='median')
	$values = @($Evidence.runs | ForEach-Object { $_.stages.$Stage.$Field } | Where-Object { $null -ne $_ } | Sort-Object)
	if($values.Count -eq 0) { return $null }
	$middle=[int][Math]::Floor($values.Count/2)
	if(($values.Count % 2) -eq 1) { return [double]$values[$middle] }
	return ([double]$values[$middle-1] + [double]$values[$middle]) / 2.0
}

function Get-CombinedStageMedian {
	param([object]$Evidence,[string[]]$Stages,[string]$Field='median')
	$values = @($Evidence.runs | ForEach-Object {
		$total = 0.0; $valid = $true
		foreach($stage in $Stages) { $value = $_.stages.$stage.$Field; if($null -eq $value){$valid=$false}else{$total += [double]$value} }
		if($valid){$total}
	} | Sort-Object)
	if($values.Count -eq 0){return $null}
	$middle=[int][Math]::Floor($values.Count/2)
	if(($values.Count % 2)-eq 1){return [double]$values[$middle]}
	return ([double]$values[$middle-1]+[double]$values[$middle])/2.0
}

function Test-PerStepCoverage {
	param([object]$Evidence)
	$required = [int]$Evidence.identity.protocol.measuredSteps
	if ($required -le 0) { return $false }
	return @($Evidence.runs | Where-Object { [int]$_.completedSteps -ge [Math]::Ceiling($required * 0.99) -and [double]$_.coverageRatio -ge 0.99 }).Count -eq @($Evidence.runs).Count
}

function Get-Improvement {
	param([Nullable[double]]$Baseline,[Nullable[double]]$Candidate)
	if($null -eq $Baseline -or $null -eq $Candidate -or $Baseline -le 0.0) { return $null }
	return ([double]$Baseline - [double]$Candidate) / [double]$Baseline
}

function Get-PhysicsTickP95 {
	param([object]$Evidence)
	$stageNames = @('authoringPreparation','providerCommit','simulation','extraction','eventPublication','stateValidation','snapshotConstruction','transactionConstruction')
	$totals = @($Evidence.runs | ForEach-Object {
		$run = $_
		$values = @($stageNames | ForEach-Object { $run.stages.$_.p95 })
		if (@($values | Where-Object { $null -eq $_ }).Count -eq 0) {
			[double](($values | Measure-Object -Sum).Sum)
		}
	} | Where-Object { $null -ne $_ } | Sort-Object)
	if ($totals.Count -eq 0) { return $null }
	$middle = [int][Math]::Floor($totals.Count / 2)
	if (($totals.Count % 2) -eq 1) { return [double]$totals[$middle] }
	return ([double]$totals[$middle - 1] + [double]$totals[$middle]) / 2.0
}

function Test-AllocationEvidence {
	param([object]$Evidence,[string]$Stage)
	$metrics = @($Evidence.runs | ForEach-Object { $_.stages.$Stage })
	return $metrics.Count -gt 0 -and @($metrics | Where-Object { $_.availability -eq 'available' -and $null -ne $_.allocationCount -and $null -ne $_.allocationBytes }).Count -eq $metrics.Count
}

function New-UnavailableComparisonRun {
	param([int]$RunNumber,[int]$CompletedSteps)
	$stages=[ordered]@{}
	foreach($stageName in @('authoringPreparation','providerCommit','simulation','extraction','eventPublication','stateValidation','snapshotConstruction','transactionConstruction')) {
		$stages[$stageName]=[ordered]@{availability='unavailable';reason='baseline or candidate evidence is missing';sampleCount=0;median=$null;p95=$null;p99=$null;worst=$null;workItems=$null;allocationCount=$null;allocationBytes=$null}
	}
	return [ordered]@{
		runId="unavailable-comparison-run-$RunNumber";runNumber=$RunNumber;completedSteps=[Math]::Max(1,$CompletedSteps);coverageRatio=0.0;stages=$stages
		jobs=[ordered]@{submitted=0;completed=0;rejected=0;observedWorkerThreads=0;barrierWaitMicroseconds=0;jobExecutionMicroseconds=0;idleAtEnd=$true}
		correctness=[ordered]@{stateHash='unavailable';eventHash='unavailable';poseHash='unavailable';overflowCount=0;droppedCount=0;rollbackPassed=$false;visibilityPassed=$false;leakedProviderWork=$false}
	}
}

if(-not (Test-Path -LiteralPath $Schema -PathType Leaf)) { throw "Evidence schema not found: $Schema" }
$schemaObject = Get-Content -Raw -LiteralPath $Schema | ConvertFrom-Json
if($schemaObject.title -ne 'TerraEngine Physics Performance Evidence V1') { throw 'The selected schema is not Physics Performance Evidence V1.' }
$script:EvidenceSchema=$schemaObject

$failures=[System.Collections.Generic.List[string]]::new()
$gates=New-Gates
$baselineSet=Read-EvidenceSet $BaselineDirectory
$candidateSet=Read-EvidenceSet $CandidateDirectory
$candidateIndex=Read-CaptureIndex $CandidateDirectory
$status='incomparable'
$comparisonRuns=@()

if($baselineSet.Count -eq 0 -or $candidateSet.Count -eq 0) {
	Add-Failure $failures 'baseline or candidate evidence is missing'
} else {
	$shapeValid=$true
	foreach($evidence in @($baselineSet)+@($candidateSet)) { if(-not (Test-MinimumShape $evidence $failures)){ $shapeValid=$false } }
	$identityValid=$shapeValid
	$allCoveragePassed=$true
	$allCorrectnessPassed=$true
	$alternateCandidates=@()
	foreach($candidate in $candidateSet) {
		$id=$candidate.identity.workload.id
		$worker=[string]$candidate.identity.workers.requested
		$quality="$($candidate.identity.quality.velocityIterations)/$($candidate.identity.quality.positionIterations)"
		$isAlternate = $id -eq 'physics-worker-large-v1' -and $quality -ne '10/2'
		$baseline=@($baselineSet | Where-Object {
			$_.identity.workload.id -eq $id -and [string]$_.identity.workers.requested -eq $worker -and
			"$($_.identity.quality.velocityIterations)/$($_.identity.quality.positionIterations)" -eq $quality
		}) | Select-Object -First 1
		if($isAlternate) {
			# Alternate quality is a candidate-only leg; clean baselines predate
			# provider application and must not be required or hash-compared.
			$alternateCandidates += $candidate
			$baseline = $candidate
		}
		if($null -eq $baseline) { Add-Failure $failures "matching baseline missing for $id worker=$worker quality=$quality"; $identityValid=$false; continue }
		foreach($path in @('sourceRevision','configuration')) {
			if($baseline.identity.build.$path -ne $candidate.identity.build.$path) { Add-Failure $failures "$path mismatch for $id"; $identityValid=$false }
		}
		foreach($path in @('machineId','os','cpu','logicalProcessors','memoryBytes','powerContext')) {
			if($baseline.identity.environment.$path -ne $candidate.identity.environment.$path) { Add-Failure $failures "environment $path mismatch for $id"; $identityValid=$false }
		}
		foreach($path in @('version','manifestSha256','sceneSha256','population','activeBodies')) {
			if($baseline.identity.workload.$path -ne $candidate.identity.workload.$path) { Add-Failure $failures "workload $path mismatch for $id"; $identityValid=$false }
		}
		foreach($path in @('warmupSteps','measuredSteps','runCount','fixedDeltaSeconds','captureSettingsSha256')) {
			if($baseline.identity.protocol.$path -ne $candidate.identity.protocol.$path) { Add-Failure $failures "protocol $path mismatch for $id"; $identityValid=$false }
		}
		# A baseline artifact must describe the candidate's same source revision and
		# its pre-change parent relationship. The evidence document's own status is
		# allowed to differ because the comparator computes the final acceptance.
		if(-not $isAlternate -and -not [bool]$baseline.baseline.sameRevisionParent){Add-Failure $failures "baseline is not the frozen same-revision parent for $id";$identityValid=$false}

		$coveragePassed=(Test-PerStepCoverage $baseline) -and (Test-PerStepCoverage $candidate)
		if(-not $coveragePassed) { Add-Failure $failures "sample coverage below 99% for $id" }
		$allCoveragePassed = $allCoveragePassed -and $coveragePassed
		$correctnessPassed=$true
		for($index=0; $index -lt [Math]::Min(@($baseline.runs).Count,@($candidate.runs).Count); ++$index) {
			$b=$baseline.runs[$index]; $c=$candidate.runs[$index]
			# State is compared for every fixture. Ordered event identity is required for
			# the contact fixture, while ragdolls additionally require pose identity.
			# Other fixtures may legitimately suppress non-consumed provider events.
			$eventHashMismatch = $id -eq 'physics-contact-pairs-4096-v1' -and $b.correctness.eventHash -ne $c.correctness.eventHash
			$poseHashMismatch = $id -like 'physics-ragdoll-crowd-*' -and $b.correctness.poseHash -ne $c.correctness.poseHash
			if((-not $isAlternate -and ($b.correctness.stateHash -ne $c.correctness.stateHash -or $eventHashMismatch -or $poseHashMismatch)) -or $b.correctness.overflowCount -ne $c.correctness.overflowCount -or $b.correctness.droppedCount -ne $c.correctness.droppedCount -or -not $c.correctness.rollbackPassed -or -not $c.correctness.visibilityPassed -or $c.correctness.leakedProviderWork) { $correctnessPassed=$false }
		}
		if(-not $correctnessPassed) { Add-Failure $failures "correctness/hash mismatch for $id" }
		$allCorrectnessPassed = $allCorrectnessPassed -and $correctnessPassed
		$comparisonRuns += $candidate.runs

		switch($id) {
			'physics-unchanged-10000-v1' {
				$median=Get-Improvement (Get-MetricMedian $baseline 'authoringPreparation' 'median') (Get-MetricMedian $candidate 'authoringPreparation' 'median')
				$p95=Get-Improvement (Get-MetricMedian $baseline 'authoringPreparation' 'p95') (Get-MetricMedian $candidate 'authoringPreparation' 'p95')
				$gates.unchangedPreparationPassed=$null -ne $median -and $median -ge 0.80 -and $null -ne $p95 -and $p95 -ge 0.60
				# An unchanged world is also the canonical empty/low-event leg: no
				# contacts are introduced during the measured interval. Require both
				# the allocation-volume and median-time reductions from that same leg.
				$eventAllocationGain=Get-Improvement (Get-MetricMedian $baseline 'eventPublication' 'allocationBytes') (Get-MetricMedian $candidate 'eventPublication' 'allocationBytes')
				$eventMedianGain=Get-Improvement (Get-MetricMedian $baseline 'eventPublication' 'median') (Get-MetricMedian $candidate 'eventPublication' 'median')
				$gates.emptyLowEventPassed=$null -ne $eventAllocationGain -and $eventAllocationGain -ge 0.75 -and $null -ne $eventMedianGain -and $eventMedianGain -ge 0.40
			}
			'physics-contact-pairs-4096-v1' {
				$gain=Get-Improvement (Get-MetricMedian $baseline 'eventPublication' 'p95') (Get-MetricMedian $candidate 'eventPublication' 'p95')
				$gates.contactEventPassed=$correctnessPassed -and $null -ne $gain -and $gain -ge 0.25
			}
			'physics-ragdoll-crowd-64x16-v1' {
				# The ragdoll gate measures the complete animation-facing pose publication stage,
				# including its atomic immutable bridge swap and reacquisition.
				$poseStages=@('extraction','snapshotConstruction')
				$gain=Get-Improvement (Get-CombinedStageMedian $baseline $poseStages 'median') (Get-CombinedStageMedian $candidate $poseStages 'median')
				$double=@($candidateSet | Where-Object { $_.identity.workload.id -eq 'physics-ragdoll-crowd-128x16-v1' -and $_.identity.quality.velocityIterations -eq $candidate.identity.quality.velocityIterations }) | Select-Object -First 1
				$doubleMedian=if($double){Get-CombinedStageMedian $double $poseStages 'median'}else{$null}
				$gates.ragdollPassed=$correctnessPassed -and $null -ne $gain -and $gain -ge 0.30 -and $null -ne $doubleMedian -and $doubleMedian -le ((Get-CombinedStageMedian $candidate $poseStages 'median') * 2.5)
			}
			'physics-bulk-admission-10000-v1' {
				$gain=Get-Improvement (Get-MetricMedian $baseline 'providerCommit' 'median') (Get-MetricMedian $candidate 'providerCommit' 'median')
				$allocationGain=Get-Improvement (Get-MetricMedian $baseline 'providerCommit' 'allocationCount') (Get-MetricMedian $candidate 'providerCommit' 'allocationCount')
				$gates.bulkAdmissionPassed=$correctnessPassed -and $null -ne $gain -and $gain -ge 0.25 -and $null -ne $allocationGain -and $allocationGain -ge 0.30 -and (Test-AllocationEvidence $candidate 'providerCommit')
			}
		}
	}
	# Alternate-quality legs must be applied, complete, leak-free, and internally
	# deterministic across worker counts; they need not match the default profile.
	if($alternateCandidates.Count -gt 0) {
		$alternateInvalid=@($alternateCandidates | Where-Object {
			-not $_.identity.quality.applied -or -not (Test-PerStepCoverage $_) -or
			@($_.runs | Where-Object { -not $_.jobs.idleAtEnd -or $_.correctness.leakedProviderWork }).Count -gt 0
		})
		$altStateHashes = @($alternateCandidates | ForEach-Object { $_.runs | ForEach-Object { $_.correctness.stateHash } }) | Select-Object -Unique
		$altEventHashes = @($alternateCandidates | ForEach-Object { $_.runs | ForEach-Object { $_.correctness.eventHash } }) | Select-Object -Unique
		$altPoseHashes = @($alternateCandidates | ForEach-Object { $_.runs | ForEach-Object { $_.correctness.poseHash } }) | Select-Object -Unique
		if($alternateInvalid.Count -gt 0 -or $altStateHashes.Count -gt 1 -or $altEventHashes.Count -gt 1 -or $altPoseHashes.Count -gt 1) { Add-Failure $failures 'alternate quality legs are not applied, complete, idle, or deterministic across workers'; $allCorrectnessPassed=$false }
	}
	$gates.sampleCoveragePassed=$allCoveragePassed
	$gates.correctnessPassed=$allCorrectnessPassed
	$gates.identityComparable=$identityValid
	# The mixed-world gate uses one matching baseline/candidate leg. Sum the eight
	# independently measured stage p95 values for a conservative total, then fail
	# any >5% stage regression unless the total improvement reaches 30%.
	$workerMixed=@($candidateSet | Where-Object {
		$_.identity.workload.id -eq 'physics-unchanged-10000-v1' -and
		[string]$_.identity.workers.requested -eq 'max' -and
		$_.identity.quality.velocityIterations -eq 10 -and $_.identity.quality.positionIterations -eq 2
	}) | Select-Object -First 1
	$workerMixedBaseline=@($baselineSet | Where-Object {
		$_.identity.workload.id -eq 'physics-unchanged-10000-v1' -and
		[string]$_.identity.workers.requested -eq 'max' -and
		$_.identity.quality.velocityIterations -eq 10 -and $_.identity.quality.positionIterations -eq 2
	}) | Select-Object -First 1
	if($workerMixed -and $workerMixedBaseline){
		$totalGain=Get-Improvement (Get-PhysicsTickP95 $workerMixedBaseline) (Get-PhysicsTickP95 $workerMixed)
		$stageRegression=$false
		foreach($stageName in @('authoringPreparation','providerCommit','simulation','extraction','eventPublication','stateValidation','snapshotConstruction','transactionConstruction')) {
			$baselineStage=Get-MetricMedian $workerMixedBaseline $stageName 'p95'
			$candidateStage=Get-MetricMedian $workerMixed $stageName 'p95'
			# A zero-duration stage is a valid no-work measurement, not a regression.
			if($null -eq $baselineStage -or $null -eq $candidateStage) { $stageRegression=$true }
			elseif($baselineStage -gt 0.0 -and $candidateStage -gt ($baselineStage * 1.05)) { $stageRegression=$true }
		}
		$gates.mixedTickPassed=$null -ne $totalGain -and $totalGain -ge 0.20 -and (-not $stageRegression -or $totalGain -ge 0.30)
	}
	$requiredRepetitions=if($candidateIndex){[int]$candidateIndex.acceptanceRepetitions}else{10}
	$gates.repeatedRunsPassed=$null -ne $candidateIndex -and $requiredRepetitions -ge 10 -and
		@($candidateSet | Where-Object {
			@($_.runs).Count -lt $requiredRepetitions -or
			@($_.runs | Where-Object { -not $_.jobs.idleAtEnd -or $_.correctness.leakedProviderWork }).Count -gt 0
		}).Count -eq 0
	$gates.qualityPassed=@($candidateSet | Where-Object { [bool]$_.identity.quality.applied }).Count -eq $candidateSet.Count
	$gates.buildInstallPassed=@($candidateSet | Where-Object { Test-Sha256 $_.identity.build.installedTreeHash }).Count -eq $candidateSet.Count

	# Scaling compares one quality profile only. Alternate-quality legs prove their
	# own deterministic independence but must never be mixed into the 10/2 timing
	# matrix merely because they share the workload identity.
	$workerCandidates=@($candidateSet | Where-Object {
		$_.identity.workload.id -eq 'physics-worker-large-v1' -and
		$_.identity.quality.velocityIterations -eq 10 -and $_.identity.quality.positionIterations -eq 2
	})
	$one=@($workerCandidates | Where-Object { [string]$_.identity.workers.requested -eq '1' }) | Select-Object -First 1
	$four=@($workerCandidates | Where-Object { [string]$_.identity.workers.requested -eq '4' }) | Select-Object -First 1
	$max=@($workerCandidates | Where-Object { [string]$_.identity.workers.requested -eq 'max' }) | Select-Object -First 1
	if($one -and $four -and $max) {
		$oneTime=Get-MetricMedian $one 'simulation' 'median'; $fourTime=Get-MetricMedian $four 'simulation' 'median'; $maxTime=Get-MetricMedian $max 'simulation' 'median'
		$aggregates=@($candidateIndex.provenance.workerScalingAggregates)
		$oneAggregate=@($aggregates | Where-Object { [string]$_.worker -eq '1' -and $_.quality -eq 'default' }) | Select-Object -First 1
		$fourAggregate=@($aggregates | Where-Object { [string]$_.worker -eq '4' -and $_.quality -eq 'default' }) | Select-Object -First 1
		$maxAggregate=@($aggregates | Where-Object { [string]$_.worker -eq 'max' -and $_.quality -eq 'default' }) | Select-Object -First 1
		if($oneAggregate -and $fourAggregate -and $maxAggregate) { $oneTime=$oneAggregate.median; $fourTime=$fourAggregate.median; $maxTime=$maxAggregate.median }
		$gates.workerScalingPassed=$null -ne $oneTime -and $null -ne $fourTime -and $null -ne $maxTime -and $fourTime -gt 0 -and (($oneTime/$fourTime) -ge 1.5) -and ($maxTime -le $fourTime*1.10)
	}

	$failedGates=@($gates.GetEnumerator() | Where-Object { -not $_.Value } | ForEach-Object { $_.Key })
	foreach($gate in $failedGates) { Add-Failure $failures "acceptance gate failed: $gate" }
	$status=if(-not $identityValid){'incomparable'}elseif($failedGates.Count -eq 0){'passed'}else{'failed'}
}

$sourceRevision=if($baselineSet.Count){$baselineSet[0].identity.build.sourceRevision}else{'unavailable'}
$artifactHash=if($baselineSet.Count){$baselineSet[0].identity.build.installedTreeHash}else{'0'*64}
$identity=if($candidateSet.Count){$candidateSet[0].identity}else{[ordered]@{
	build=[ordered]@{sourceRevision='unavailable';dirtyDiffHash=$null;configuration='RelWithDebInfo';installedTreeHash=('0'*64)}
	environment=[ordered]@{machineId='unavailable';os='unavailable';cpu='unavailable';logicalProcessors=1;memoryBytes=1;powerContext='unavailable'}
	workload=[ordered]@{id='physics-unchanged-10000-v1';version=1;manifestSha256=('0'*64);sceneSha256=('0'*64);population=10000;activeBodies=1000}
	workers=[ordered]@{requested='max';resolved=1;adapterLimit=1}
	quality=[ordered]@{settingsSchemaVersion=1;settingsGeneration=1;worldGeneration=1;velocityIterations=10;positionIterations=2;applied=$false}
	protocol=[ordered]@{warmupSteps=30;measuredSteps=100;runCount=5;fixedDeltaSeconds=(1.0/60.0);captureSettingsSha256=('0'*64)}
}}
$resultRuns=@($comparisonRuns)
if($resultRuns.Count -lt 5) {
	# The contract keeps five runs in every document so even an incomparable
	# result is schema-valid and machine-readable. These records are explicit
	# unavailable placeholders; they cannot satisfy coverage or correctness.
	$resultRuns=@(1..5 | ForEach-Object { New-UnavailableComparisonRun $_ ([int]$identity.protocol.measuredSteps) })
}
$result=[ordered]@{
	schemaVersion=1; status=$status; identity=$identity
	baseline=[ordered]@{ evidenceId="physics-baseline-$sourceRevision"; sourceRevision=$sourceRevision; artifactSha256=$artifactHash; sameRevisionParent=$gates.identityComparable }
	runs=$resultRuns; gates=$gates; failures=@($failures)
}
$outputSchemaErrors=[System.Collections.Generic.List[string]]::new()
$resultForValidation=$result | ConvertTo-Json -Depth 24 | ConvertFrom-Json
if(-not (Test-JsonSchemaNode $resultForValidation $script:EvidenceSchema $script:EvidenceSchema '$' $outputSchemaErrors)) {
	throw "Comparator generated invalid evidence: $($outputSchemaErrors -join '; ')"
}
$parent=Split-Path -Parent $Output
if(-not [string]::IsNullOrWhiteSpace($parent)){New-Item -ItemType Directory -Force -Path $parent|Out-Null}
$failures | ForEach-Object { Write-Verbose $_ }
$result|ConvertTo-Json -Depth 24|Set-Content -LiteralPath $Output -Encoding UTF8
if($status -ne 'passed'){exit 1}
