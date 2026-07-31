[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string]$BaselineDirectory,
	[Parameter(Mandatory=$true)][string]$CandidateDirectory,
	[Parameter(Mandatory=$true)][string]$Output,
	[int]$MinimumRuns = 6,
	[double]$MaxMedianRegressionRatio = 1.10
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
function Read-Captures([string]$Directory) {
	if(-not (Test-Path -LiteralPath $Directory -PathType Container)){ throw "Evidence directory missing: $Directory" }
	return @(Get-ChildItem -LiteralPath $Directory -File -Filter '*.json' | ForEach-Object { Get-Content -Raw $_.FullName | ConvertFrom-Json })
}
function Get-Measured([object[]]$Captures) {
	$out = [Collections.Generic.List[double]]::new()
	foreach($capture in $Captures){
		$identity = $capture.PSObject.Properties['identity']
		$comparability = $capture.PSObject.Properties['baselineAndComparability']
		$executionId = if($null -ne $identity -and $null -ne $identity.Value.executionId){[string]$identity.Value.executionId}else{''}
		$status = if($null -ne $comparability){[string]$comparability.Value.status}else{''}
		if([string]::IsNullOrWhiteSpace($executionId) -or $executionId -eq 'setup-unavailable' -or $status -notin @('measured','comparable')){ continue }
		$raw = $capture.PSObject.Properties['rawSamples']
		if($null -eq $raw -or $null -eq $raw.Value) { continue }
		foreach($sample in @($raw.Value)){
			$availability = $sample.PSObject.Properties['availability']
			$duration = $sample.PSObject.Properties['durationNanoseconds']
			$sampleExecution = $sample.PSObject.Properties['executionId']
			if($null -ne $availability -and $availability.Value -eq 'measured' -and $null -ne $duration -and $null -ne $duration.Value -and $null -ne $sampleExecution -and [string]$sampleExecution.Value -eq $executionId){ $out.Add([double]$duration.Value) }
		}
	}
	# Let PowerShell enumerate the typed array here. The callers wrap the function
	# result in @(), which preserves zero or one samples while keeping every
	# measured duration as its own run for the minimum-run gate.
	return $out.ToArray()
}
function Median([double[]]$Values){ $sorted=@($Values|Sort-Object); return $sorted[[Math]::Floor(($sorted.Count-1)/2)] }
function Quantile([double[]]$Values, [double]$Probability){ $sorted=@($Values|Sort-Object); return $sorted[[Math]::Min($sorted.Count-1,[Math]::Ceiling($sorted.Count*$Probability)-1)] }
$baseline = Read-Captures $BaselineDirectory; $candidate = Read-Captures $CandidateDirectory
$baseValues = @(Get-Measured $baseline); $candidateValues = @(Get-Measured $candidate)
if($baseValues.Count -lt $MinimumRuns -or $candidateValues.Count -lt $MinimumRuns){ throw "At least $MinimumRuns measured comparable runs are required; pending/unavailable samples do not count." }
$baseMedian = Median $baseValues; $candidateMedian = Median $candidateValues
$ratio = $candidateMedian / $baseMedian
$result = [ordered]@{ schemaVersion=1; status= if($ratio -le $MaxMedianRegressionRatio){'accepted'}else{'regression'}; baselineMeasuredSamples=$baseValues.Count; candidateMeasuredSamples=$candidateValues.Count; baselineStatistics=[ordered]@{medianNanoseconds=$baseMedian;p95Nanoseconds=(Quantile $baseValues 0.95);p99Nanoseconds=(Quantile $baseValues 0.99)}; candidateStatistics=[ordered]@{medianNanoseconds=$candidateMedian;p95Nanoseconds=(Quantile $candidateValues 0.95);p99Nanoseconds=(Quantile $candidateValues 0.99)}; medianRatio=$ratio; maxMedianRegressionRatio=$MaxMedianRegressionRatio; unavailableIsNotZero=$true; note='Comparison uses only measured samples; capability or timing unavailable is never treated as zero.' }
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Output -Encoding utf8
if($result.status -eq 'regression'){ exit 2 }; Write-Output "comparison accepted: median ratio $ratio"
