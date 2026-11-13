# Executes exact production AST, without launching the benchmark or editor.
param([string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$production = Join-Path $SourceRoot 'Tests/Tools/Profiling/CaptureProfilerReworkBenchmark.ps1'
$tokens = $null; $parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($production, [ref]$tokens, [ref]$parseErrors)
if($parseErrors.Count -ne 0) { throw 'Production benchmark script must parse.' }
foreach($name in @('Require','Get-PropertyValue')) {
	$definitions = @($ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name}, $true))
	if($definitions.Count -ne 1) { throw "Expected one actual production helper: $name" }
	. ([scriptblock]::Create($definitions[0].Extent.Text))
}
$branches = @($ast.FindAll({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and $node.Clauses[0].Item1.Extent.Text -eq '$windowStatus -eq ''not-requested'''}, $true))
if($branches.Count -ne 1) { throw 'Expected one unambiguous production normalization branch.' }
$branch = [scriptblock]::Create($branches[0].Extent.Text)
Write-Output ("Production SHA256={0}; exact AST lines={1}-{2}; scope=normalization-only" -f (Get-FileHash -LiteralPath $production -Algorithm SHA256).Hash, $branches[0].Extent.StartLineNumber, $branches[0].Extent.EndLineNumber)
$script:failures = 0; $script:cases = 0
function Run-Case([string]$name, [scriptblock]$body) {
	$script:cases++
	try { & $body; Write-Output "[PASS] $name" } catch { $script:failures++; Write-Output "[FAIL] ${name}: $($_.Exception.Message)" }
}
function Normalize-ActualBranch($measurement, [string]$mode, [string]$metric) {
	# Set only actual production branch inputs; execute the unchanged AST extent.
	$windowMeasurement = $measurement; $windowStatus = [string](Get-PropertyValue $measurement 'status')
	$Mode = $mode; $metricMode = $metric; $_ = [pscustomobject]@{FullName='owned-normalization-fixture.json'}
	& $branch
	return $windowMeasurement
}
Run-Case 'WindowGate.MissingDeserializedPropertyBecomesFalseUnavailable' {
	$value = '{"status":"not-requested"}' | ConvertFrom-Json
	$result = Normalize-ActualBranch $value 'DetailedRecording' 'recording-window-closed'
	Require ($result.status -eq 'unavailable') 'Non-window row must become unavailable.'
	Require ($null -ne $result.PSObject.Properties['twoPercentGate'] -and $result.twoPercentGate -eq $false) 'Missing gate must be added false.'
}
Run-Case 'WindowGate.ExistingPropertyRemainsStable' {
	foreach($gate in @($false,$true)) {
		$value = ('{"status":"not-requested","twoPercentGate":'+$gate.ToString().ToLowerInvariant()+'}') | ConvertFrom-Json
		$result = Normalize-ActualBranch $value 'DetailedRecording' 'recording-window-closed'
		Require ($result.status -eq 'unavailable' -and $result.twoPercentGate -eq $gate) 'Existing gate must remain stable with unavailable status.'
	}
}
Run-Case 'WindowGate.MeasuredRowRemainsMeasured' {
	$value = '{"status":"measured","twoPercentGate":false}' | ConvertFrom-Json
	$result = Normalize-ActualBranch $value 'MatchedWindowOpen' 'recording-window-open'
	Require ($result.status -eq 'measured' -and $result.twoPercentGate -eq $false) 'Measured failure must remain a failure.'
}
Run-Case 'WindowGate.TimedOpenUnmeasuredRowStillRejects' {
	$value = '{"status":"not-requested"}' | ConvertFrom-Json; $rejected = $false
	try { $null = Normalize-ActualBranch $value 'MatchedWindowOpen' 'recording-window-open' }
	catch { $rejected = $_.Exception.Message -like '*requires measured windowMeasurement*' }
	Require $rejected 'Timed-open recording requires measured evidence.'
	Require ($value.status -eq 'not-requested') 'Rejected timed-open input must not normalize to success.'
}
Write-Output "$script:cases selected tests, $script:failures failures"
if($script:cases -ne 4 -or $script:failures -ne 0) { exit 1 }
