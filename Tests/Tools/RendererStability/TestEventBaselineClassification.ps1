# Run real production event policy/default query through its injected IO boundary.
# No Windows Event Log access/mutation, editor launch, or real process stop occurs.
param([string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$production = Join-Path $SourceRoot 'Tests/Tools/RendererStability/RunLiveSmoke.ps1'
$tokens = $null; $parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($production, [ref]$tokens, [ref]$parseErrors)
if($parseErrors.Count -ne 0) { throw 'Production event harness must parse.' }
$definitions = @($ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst]}, $true))
foreach($definition in $definitions) { . ([scriptblock]::Create($definition.Extent.Text)) }
$defaultQueries = @($ast.FindAll({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -eq '$SystemEventQuery'}, $true))
if($defaultQueries.Count -ne 1) { throw 'Expected one actual default SystemEventQuery assignment.' }
. ([scriptblock]::Create($defaultQueries[0].Extent.Text))
Write-Output ("sourceSHA256={0}; defaultQueryAST={1}-{2}; actualScriptHostPid={3}; scope=injected-IO-unit-no-native-event-or-worker-measurement" -f (Get-FileHash -LiteralPath $production -Algorithm SHA256).Hash, $defaultQueries[0].Extent.StartLineNumber, $defaultQueries[0].Extent.EndLineNumber, $PID)
$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$workspace = Join-Path $temporaryBase ('TerraEventClassification-' + [guid]::NewGuid().ToString('N'))
if(Test-Path -LiteralPath $workspace) { throw 'Exclusive fixture root unexpectedly exists.' }
$null = New-Item -ItemType Directory -Path $workspace
$script:cases = 0; $script:failures = 0
$script:baselineTime = [datetime]'2026-01-01T00:00:00Z'
$script:frozen = [pscustomobject]@{RecordId=100;TimeCreated=$script:baselineTime;ProviderName='Service Control Manager';Id=1;LevelDisplayName='Information';Message='Frozen fixture baseline'}
$script:queryMode = 'intact'; $script:currentHead = $script:frozen; $script:sinceEvents = @(); $script:stops = [Collections.Generic.List[int]]::new()
# This local boundary shadows only Get-WinEvent inside this isolated script.
# The unchanged production default-query block still chooses baseline/since and
# error handling. Fixtures are explicitly event-like values, never native facts.
function Get-WinEvent {
	[CmdletBinding()]
	param([string]$LogName, [hashtable]$FilterHashtable, [int]$MaxEvents)
	if($PSBoundParameters.ContainsKey('FilterHashtable')) {
		$script:capturedStartTime=$FilterHashtable.StartTime
		if($script:queryMode -eq 'no-match') {
			$record = [Management.Automation.ErrorRecord]::new([InvalidOperationException]::new('fixture: no matching events'), 'NoMatchingEventsFound', [Management.Automation.ErrorCategory]::ObjectNotFound, $null)
			$PSCmdlet.ThrowTerminatingError($record)
		}
		if($script:queryMode -eq 'denied') {
			$record = [Management.Automation.ErrorRecord]::new([UnauthorizedAccessException]::new('fixture: access denied'), 'SystemLogAccessDenied', [Management.Automation.ErrorCategory]::PermissionDenied, $null)
			$PSCmdlet.ThrowTerminatingError($record)
		}
		return $script:sinceEvents
	}
	return $script:currentHead
}
function Run-Case([string]$name, [scriptblock]$body) {
	$script:cases++
	$script:queryMode = 'intact'; $script:currentHead = $script:frozen; $script:sinceEvents = @(); $script:stops.Clear()
	try { & $body; Write-Output "[PASS] $name" } catch { $script:failures++; Write-Output "[FAIL] ${name}: $($_.Exception.Message)" }
}
function Require-Case([bool]$condition, [string]$reason) { if(-not $condition) { throw $reason } }
function Invoke-ActualGate {
	Invoke-TerraRendererSmokeDriverEventGate -Baseline $script:frozen -Query $SystemEventQuery -ProcessId $PID -EvidencePath $workspace -Operation 0 -Stage 'injected-unit' -StopProcessTree {param($processId) $script:stops.Add($processId)}
}
function Require-InvalidBaseline {
	$rejected = $false; $reason = ''
	try { Invoke-ActualGate } catch { $reason = $_.FullyQualifiedErrorId + ': ' + $_.Exception.Message; $rejected = $reason -match 'baseline|cleared|continuity|unverified|invalid.*log|log.*invalid' }
	Require-Case $rejected "Lost/replaced baseline must be explicitly invalid/unverified, not ordinary empty success or a generic no-match error; actual=$reason"
	Require-Case ($script:stops.Count -eq 0) 'Invalid evidence must not masquerade as an observed new driver event.'
}
try {
	Run-Case 'EventBaseline.IntactEmptyObservationIsValid' {
		$observed = Get-TerraRendererSmokeSystemBaseline -Query $SystemEventQuery
		Require-Case ($observed.RecordId -eq 100) 'Actual default query must establish frozen baseline.'
		Invoke-ActualGate
		Require-Case ($script:stops.Count -eq 0) 'Intact empty observation must not stop any process.'
	}
	Run-Case 'EventBaseline.NoMatchingEventsWithIntactBaselineIsValidEmpty' {
		$script:queryMode = 'no-match'
		Invoke-ActualGate
		Require-Case ($script:stops.Count -eq 0) 'No-match plus intact baseline is empty evidence, not a driver event.'
	}
	Run-Case 'EventBaseline.ClearedEmptyLogIsUnverified' {
		$script:currentHead = $null
		Require-InvalidBaseline
	}
	Run-Case 'EventBaseline.ReplacedRecordSequenceIsUnverified' {
		$script:currentHead = [pscustomobject]@{RecordId=1;TimeCreated=$script:baselineTime.AddSeconds(2);ProviderName='Service Control Manager';Id=1;LevelDisplayName='Information';Message='New unrelated log sequence'}
		Require-InvalidBaseline
	}
	Run-Case 'EventBaseline.NoMatchAfterClearIsNotOrdinaryEmpty' {
		$script:queryMode = 'no-match'; $script:currentHead = $null
		Require-InvalidBaseline
	}
	Run-Case 'EventBaseline.AccessDeniedRemainsQueryFailure' {
		$script:queryMode = 'denied'; $denied = $false
		try { Invoke-ActualGate } catch { $denied = $_.FullyQualifiedErrorId -like '*SystemLogAccessDenied*' -or $_.Exception -is [UnauthorizedAccessException] }
		Require-Case $denied 'Permission failure must propagate as actual query failure, never empty evidence.'
		Require-Case ($script:stops.Count -eq 0) 'Access error must not invent a driver event.'
	}
	Run-Case 'EventBaseline.TrueNewEventRetainsActualGateReceiptAndStopIdentity' {
		$event = [pscustomobject]@{RecordId=101;TimeCreated=$script:baselineTime.AddSeconds(2);ProviderName='Display';Id=13;LevelDisplayName='Error';Message='Injected graphics event fixture'}
		$script:currentHead = $event; $script:sinceEvents = @($event); $stopped = $false
		try { Invoke-ActualGate } catch { $stopped = $_.Exception.Message -like '*new qualifying*' }
		Require-Case ($stopped -and $script:stops.Count -eq 1 -and $script:stops[0] -eq $PID) 'Real gate must route the supplied actual script-host identity to the injected stop boundary exactly once.'
		$receipt = Get-Content -LiteralPath (Join-Path $workspace 'driver-event-failure.json') -Raw | ConvertFrom-Json
		Require-Case ($receipt.processId -eq $PID -and $receipt.events.Count -eq 1 -and $receipt.events[0].recordId -eq 101) 'Actual written receipt must contain the injected observed event and supplied identity.'
	}
	Run-Case 'EventBaseline.DstFoldUsesUtcInComparatorAndActualQueryBuilder' {
		if(-not $IsWindows) { throw 'The Windows event-query protocol requires its native cmdlet.' }
		# Only this owned child process's managed timezone cache is overridden.
		$cached=[TimeZoneInfo].GetField('s_cachedData',[Reflection.BindingFlags]'NonPublic,Static').GetValue($null)
		$field=$cached.GetType().GetField('_localTimeZone',[Reflection.BindingFlags]'NonPublic,Instance');$previous=$field.GetValue($cached)
		try {
			$field.SetValue($cached,[TimeZoneInfo]::FindSystemTimeZoneById('Eastern Standard Time'))
			$before=[datetime]::Parse('2026-11-01T05:30:00Z').ToUniversalTime();$after=[datetime]::Parse('2026-11-01T06:10:00Z').ToUniversalTime()
			Require-Case (Test-TerraRendererSmokeEventAfterBaseline -Baseline ([pscustomobject]@{RecordId=100;TimeCreated=$before.ToLocalTime()}) -Event ([pscustomobject]@{RecordId=101;TimeCreated=$after.ToLocalTime()})) 'A newer UTC event in the repeated local hour must qualify.'
			# Invoke the real default query with captured IO, then feed its exact
			# StartTime argument into the installed cmdlet's private XPath builder.
			$script:queryMode='intact';$null=& $SystemEventQuery 'since' $before
			$type=(Get-Command 'Microsoft.PowerShell.Diagnostics\Get-WinEvent').ImplementingType
			$instance=[Activator]::CreateInstance($type);$method=$type.GetMethod('HandleStartTimeHashValue',[Reflection.BindingFlags]'NonPublic,Instance')
			$xpath=$method.Invoke($instance,[object[]]@($script:capturedStartTime,@{}))
			Require-Case ($script:capturedStartTime -is [string] -and $xpath -like '*05:30:00*') 'The actual cmdlet must preserve the original UTC cutoff during DST fold.'
		} finally { $field.SetValue($cached,$previous) }
	}
	Run-Case 'EventBaseline.MissingInitialBaselineRejects'  {
		$script:currentHead = $null; $rejected = $false
		try { $null = Get-TerraRendererSmokeSystemBaseline -Query $SystemEventQuery } catch { $rejected = $_.Exception.Message -like '*baseline*' }
		Require-Case $rejected 'No initial record must not establish a valid baseline.'
	}
} finally {
	$resolvedFixture = [IO.Path]::GetFullPath($workspace)
	$prefix = $temporaryBase.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
	if(-not $resolvedFixture.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture cleanup escaped intended temp root.' }
	Remove-Item -LiteralPath $resolvedFixture -Recurse -Force
}
Write-Output "$script:cases selected tests, $script:failures failures"
if($script:cases -ne 9 -or $script:failures -ne 0) { exit 1 }
