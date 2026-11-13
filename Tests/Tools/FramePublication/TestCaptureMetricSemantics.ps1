# Exact production AST + bounded script/metric inputs; not a live capture.
param([string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$production = Join-Path $SourceRoot 'Tests/Tools/FramePublication/CaptureFramePublicationPerformance.ps1'
$parseTokens = $null; $parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($production, [ref]$parseTokens, [ref]$parseErrors)
if($parseErrors.Count -ne 0) { throw 'Actual capture producer must parse.' }
foreach($helper in @('Require','Get-PropertyValue')) {
	$nodes = @($ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $helper}, $true))
	if($nodes.Count -ne 1) { throw "Ambiguous production helper $helper" }
	. ([scriptblock]::Create($nodes[0].Extent.Text))
}
$invocations = @($ast.FindAll({param($node)
	if($node -isnot [Management.Automation.Language.IfStatementAst] -or $node.Clauses[0].Item1.Extent.Text -ne '-not $reuseCompletedRunSummary') { return $false }
	return $null -ne $node.Find({param($child) $child -is [Management.Automation.Language.CommandAst] -and $child.CommandElements[0].Extent.Text -eq '$LiveAdapter'}, $true)
}, $true))
$metrics = @($ast.FindAll({param($node)
	if($node -isnot [Management.Automation.Language.HashtableAst]) { return $false }
	$keys = @($node.KeyValuePairs | ForEach-Object { $_.Item1.Value })
	return ('p99Ms' -in $keys -and 'worstMs' -in $keys -and 'medianMs' -in $keys -and 'count' -in $keys)
}, $true))
if($invocations.Count -ne 1 -or $metrics.Count -ne 1) { throw 'Expected unique actual adapter guard and frameTiming map.' }
$invokeActual = [scriptblock]::Create($invocations[0].Extent.Text)
$metricActual = [scriptblock]::Create($metrics[0].Extent.Text)
$script:hostProcess = Get-Process -Id $PID
Write-Output ("sourceSHA256={0}; adapterAST={1}-{2}; metricAST={3}-{4}; actualScriptHostPid={5}; scope=unit-fixtures-no-runtime-worker-or-native-measurement" -f (Get-FileHash -LiteralPath $production -Algorithm SHA256).Hash, $invocations[0].Extent.StartLineNumber, $invocations[0].Extent.EndLineNumber, $metrics[0].Extent.StartLineNumber, $metrics[0].Extent.EndLineNumber, $script:hostProcess.Id)
$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$workspace = Join-Path $temporaryBase ('TerraCaptureMetric-' + [guid]::NewGuid().ToString('N'))
if(Test-Path -LiteralPath $workspace) { throw 'Exclusive fixture root unexpectedly exists.' }
$null = New-Item -ItemType Directory -Path $workspace
$script:cases = 0; $script:failures = 0
function Run-Case([string]$name, [scriptblock]$body) {
	$script:cases++
	try { & $body; Write-Output "[PASS] $name" } catch { $script:failures++; Write-Output "[FAIL] ${name}: $($_.Exception.Message)" }
}
function Invoke-ActualAdapter([string]$adapter) {
	$LiveAdapter = $adapter; $adapterArguments = @{}; $reuseCompletedRunSummary = $false
	$scenario = [pscustomobject]@{id='bounded-unit-fixture'}; $run = 1
	# This actual host process prevents the production guard from discovering or
	# launching an editor. It is labelled host-only, never recorded as runtime PID.
	$editor = $script:hostProcess
	& $invokeActual
}
function Actual-FrameTiming($summary, $disabled) {
	$profilerSummary = $summary; $disabledTiming = $disabled
	& $metricActual
}
try {
	$success = Join-Path $workspace 'success.ps1'; "Write-Output 'ACTUAL_ADAPTER_FIXTURE_SUCCESS'" | Set-Content -LiteralPath $success
	$failure = Join-Path $workspace 'failure.ps1'; "throw 'ACTUAL_ADAPTER_FIXTURE_FAILURE'" | Set-Content -LiteralPath $failure
	$nonterminating = Join-Path $workspace 'nonterminating.ps1'; "Write-Error 'ACTUAL_ADAPTER_FIXTURE_NONTERMINATING' -ErrorAction Continue" | Set-Content -LiteralPath $nonterminating
	Run-Case 'CaptureMetric.StaleNativeExitDoesNotRejectSuccessfulScript' {
		# Run the actual current PowerShell executable as a native child. Its37
		# exit is deliberately stale when the successful no-exit adapter returns.
		& $script:hostProcess.Path -NoProfile -Command 'exit 37'
		Require ($LASTEXITCODE -eq 37) 'Native control must establish a real stale exit37.'
		Invoke-ActualAdapter $success
	}
	Run-Case 'CaptureMetric.TerminatingAdapterFailureStillRejects' {
		$global:LASTEXITCODE = 0; $rejected = $false
		try { Invoke-ActualAdapter $failure } catch { $rejected = $_.Exception.Message -like '*ACTUAL_ADAPTER_FIXTURE_FAILURE*' }
		Require $rejected 'Actual failed script must not become accepted evidence.'
	}
	Run-Case 'CaptureMetric.NonterminatingAdapterFailureStillRejects' {
		$global:LASTEXITCODE = 0; $rejected = $false
		try { Invoke-ActualAdapter $nonterminating 2>$null } catch { $rejected = $true }
		Require $rejected 'Actual unsuccessful script invocation must reject even with native LASTEXITCODE zero.'
	}
	Run-Case 'CaptureMetric.FullFrameFactsRemainSeparateFromWindowValues' {
		$summary = '{"committedFrameCount":500,"averageFrameMs":8,"frameMedianMs":9,"frameP95Ms":13,"firstWindow":{"medianMs":700,"p95Ms":800},"finalWindow":{"medianMs":900,"p95Ms":999}}' | ConvertFrom-Json
		$actual = Actual-FrameTiming $summary $null
		Require ($actual.count -eq 500 -and $actual.meanMs -eq 8 -and $actual.medianMs -eq 9 -and $actual.p95Ms -eq 13) 'Whole-frame summary must not use window endpoints or their sum.'
	}
	Run-Case 'CaptureMetric.DistinctP95P99MaximumNeverAlias' {
		$summary = '{"committedFrameCount":500,"averageFrameMs":8,"frameMedianMs":9,"frameP95Ms":13,"frameP99Ms":17,"frameMaximumMs":23}' | ConvertFrom-Json
		$actual = Actual-FrameTiming $summary $null
		Require ($actual.p95Ms -eq 13 -and ($null -eq $actual.p99Ms -or $actual.p99Ms -eq 17) -and ($null -eq $actual.worstMs -or $actual.worstMs -eq 23)) 'P99/max must be their own measurements or explicit unavailable, never copied P95.'
	}
	Run-Case 'CaptureMetric.MissingRecordingPercentilesStayUnavailable' {
		$summary = '{"committedFrameCount":500,"averageFrameMs":8,"frameMedianMs":9,"frameP95Ms":13}' | ConvertFrom-Json
		$actual = Actual-FrameTiming $summary $null
		Require ($null -eq $actual.p99Ms -and $null -eq $actual.worstMs) 'Absent recording P99/max must remain null.'
	}
	Run-Case 'CaptureMetric.DisabledP95DoesNotBecomeP99' {
		$disabled = '{"sampleCount":4,"medianMs":5,"p95Ms":7,"maximumMs":11}' | ConvertFrom-Json
		$actual = Actual-FrameTiming $null $disabled
		Require ($actual.p95Ms -eq 7 -and $null -eq $actual.p99Ms -and $actual.worstMs -eq 11) 'Disabled stream has actual max11 and no P99 measurement.'
	}
} finally {
	# Delete only this exclusive fixture root, checked beneath the intended temp
	# directory. This unit script never owns an editor or changes its runtime.
	$resolvedFixture = [IO.Path]::GetFullPath($workspace)
	$prefix = $temporaryBase.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
	if(-not $resolvedFixture.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture cleanup escaped its intended temp directory.' }
	Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
}
Write-Output "$script:cases selected tests, $script:failures failures"
if($script:cases -ne 7 -or $script:failures -ne 0) { exit 1 }
