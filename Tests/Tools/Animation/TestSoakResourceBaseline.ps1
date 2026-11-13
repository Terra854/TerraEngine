# Bounded protocol regression only. Fixture numbers are NOT native performance evidence.
[CmdletBinding()]
param(
	[string]$RepoRoot = (Join-Path $PSScriptRoot '../../..'),
	[string]$WorkRoot,
	[string]$StressProducer,
	[string]$NavigationExecutable,
	[string]$NavigationManifest
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
if([string]::IsNullOrWhiteSpace($WorkRoot)) { $WorkRoot = Join-Path $script:RepoRoot ('out/validation/evidence-contract-fixtures/' + [guid]::NewGuid().ToString('N')) }
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot)
$prefix = (Join-Path $script:RepoRoot 'out').TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
if(-not $WorkRoot.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture work root must remain below repository out/.' }
if(Test-Path -LiteralPath $WorkRoot){throw 'A fresh nonexistent private fixture root is required; prior evidence is preserved.'}
[IO.Directory]::CreateDirectory($WorkRoot) | Out-Null
$script:Utf8NoBom = [Text.UTF8Encoding]::new($false)
$script:Cases = [Collections.Generic.List[object]]::new()
$script:FunctionProvenance = [Collections.Generic.List[object]]::new()
function Get-ActualProducerFunctions {
	param([string]$Path, [string[]]$Names)
	$tokens = $null; $errors = $null
	$ast = [Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
	if(@($errors).Count) { throw "Producer parse failed: $($errors[0].Message)" }
	$definitions = @()
	foreach($name in $Names) {
		$nodes = @($ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name}, $true))
		if($nodes.Count -ne 1) { throw "Required production callable seam '$name' was not found exactly once in '$Path'." }
		$definitions += $nodes[0].Extent.Text
		$sha=[Security.Cryptography.SHA256]::Create()
		try {$astHash=([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($nodes[0].Extent.Text)))).Replace('-','').ToLowerInvariant()}finally{$sha.Dispose()}
		$script:FunctionProvenance.Add([ordered]@{ source=$Path; sourceSha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash; function=$name; astSha256=$astHash; startLine=$nodes[0].Extent.StartLineNumber; endLine=$nodes[0].Extent.EndLineNumber })
	}
	# Execute exact AST extents, with no source replacement or copied calculation.
	# Dot-source this returned block in caller scope; never dot-source the monolith.
	return [scriptblock]::Create(($definitions -join "`n"))
}
function Assert-Protocol([bool]$Condition,[string]$Message) { if(-not $Condition) { throw $Message } }
function Invoke-ProtocolCase([string]$Name,[scriptblock]$Body) {
	try { & $Body; $script:Cases.Add([ordered]@{name=$Name;status='passed'}); Write-Output "[PASS] $Name" }
	catch { $script:Cases.Add([ordered]@{name=$Name;status='failed';error=$_.Exception.Message}); Write-Output "[FAIL] $Name : $($_.Exception.Message)" }
}
function Complete-ProtocolTests {
	$failures = @($script:Cases | Where-Object {$_.status -ne 'passed'}).Count
	[IO.File]::WriteAllText((Join-Path $WorkRoot 'result.json'), ([ordered]@{fixtureProtocolOnly=$true;nativePerformanceMeasured=$false;nativeResourcesMeasured=$false;selected=$script:Cases.Count;failures=$failures;cases=@($script:Cases.ToArray());functionProvenance=@($script:FunctionProvenance.ToArray())}|ConvertTo-Json -Depth 32),$script:Utf8NoBom)
	Write-Output "$($script:Cases.Count) selected protocol cases, $failures failures; native performance NOT measured."
	if($failures) { exit 1 }
}

$producer=Join-Path $script:RepoRoot 'Tests/Tools/Animation/RunAnimationSoak.ps1'
. (Get-ActualProducerFunctions $producer @('Get-RequiredUInt64','Assert-IterationResult','Assert-BoundedGrowth'))
$tokens=$null;$parseErrors=$null
$actualAst=[Management.Automation.Language.Parser]::ParseFile($producer,[ref]$tokens,[ref]$parseErrors)
if(@($parseErrors).Count){throw 'Actual soak producer must parse before bounded AST execution.'}
function Get-SoakPinnedBlock([string]$Kind,[scriptblock]$Select) {
	$nodes=@($actualAst.FindAll($Select,$true));if($nodes.Count -ne 1){throw "Actual soak AST '$Kind' must occur once."}
	$node=$nodes[0];$sha=[Security.Cryptography.SHA256]::Create()
	try{$hash=([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($node.Extent.Text)))).Replace('-','')}finally{$sha.Dispose()}
	$script:FunctionProvenance.Add([ordered]@{source=$producer;sourceSha256=(Get-FileHash -LiteralPath $producer -Algorithm SHA256).Hash;astKind=$Kind;astSha256=$hash;startLine=$node.Extent.StartLineNumber;endLine=$node.Extent.EndLineNumber})
	return [scriptblock]::Create($node.Extent.Text)
}
foreach($name in @('PrivateGrowthLimitBytes','GpuGrowthLimitBytes','HandleGrowthLimit','ThreadGrowthLimit','RetainedHistoryLimit')) {
	$assignment=Get-SoakPinnedBlock $name {param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left -is [Management.Automation.Language.VariableExpressionAst] -and $node.Left.VariablePath.UserPath -eq "script:$name"}
	. $assignment
}
$baselineBlock=Get-SoakPinnedBlock 'actual-baseline-if' {param($node)
	$node -is [Management.Automation.Language.IfStatementAst] -and
	@($node.Clauses[0].Item1.FindAll({param($part) $part -is [Management.Automation.Language.MemberExpressionAst] -and $part.Member.Value -eq 'baselineResources'},$true)).Count -gt 0 -and
	@($node.Clauses[0].Item1.FindAll({param($part) $part -is [Management.Automation.Language.VariableExpressionAst] -and $part.VariablePath.UserPath -eq 'null'},$true)).Count -gt 0
}
$highWaterBlock=Get-SoakPinnedBlock 'actual-high-water-if' {param($node)
	$node -is [Management.Automation.Language.IfStatementAst] -and
	@($node.Clauses[0].Item1.FindAll({param($part) $part -is [Management.Automation.Language.MemberExpressionAst] -and $part.Member.Value -eq 'highWaterResources'},$true)).Count -gt 0 -and
	@($node.Clauses[0].Item1.FindAll({param($part) $part -is [Management.Automation.Language.VariableExpressionAst] -and $part.VariablePath.UserPath -eq 'null'},$true)).Count -gt 0
}
function New-RawResources {
	# Explicit fixture counters only; no allocation/native resource measurement.
	return [ordered]@{privateBytes=[UInt64]1000000;gpuResidentBytes=[UInt64]2000000;handleCount=[UInt64]100;threadCount=[UInt64]16;retainedHistoryRecords=[UInt64]0}
}
function Invoke-ActualResourceIteration($state,$resources,[UInt64]$iteration) {
	$result=[pscustomobject]@{schemaVersion=1;runId='resource-protocol-fixture';iteration=$iteration;passed=$true;heartbeatSequence=$iteration;counters=[ordered]@{deadlocks=0;crashes=0;stalePublications=0;partialPublications=0;unrecoverableRendererFailures=0;unrecoverablePhysicsFailures=0};resources=$resources}
	$validated=Assert-IterationResult $result 'resource-protocol-fixture' $iteration ($iteration-1)
	# These exact existing producer branches own baseline capture, rejection and
	# high-water mutation. There is no copied growth/update policy in this test.
	. $baselineBlock
	. $highWaterBlock
}
Invoke-ProtocolCase 'Soak.FreshBaselineAndHighWaterHaveIndependentOwnership' {
	$state=[ordered]@{baselineResources=$null;highWaterResources=$null}
	Invoke-ActualResourceIteration $state (New-RawResources) 1
	Assert-Protocol (-not [object]::ReferenceEquals($state.baselineResources,$state.highWaterResources)) 'First successful baseline must not alias the mutable high-water record.'
}
foreach($dimension in @(
	[pscustomobject]@{name='privateBytes';step=[UInt64]($script:PrivateGrowthLimitBytes/2)},
	[pscustomobject]@{name='gpuResidentBytes';step=[UInt64]($script:GpuGrowthLimitBytes/2)},
	[pscustomobject]@{name='handleCount';step=[UInt64]($script:HandleGrowthLimit/2)},
	[pscustomobject]@{name='threadCount';step=[UInt64]($script:ThreadGrowthLimit/2)}
)) {
	Invoke-ProtocolCase "Soak.CumulativeGrowthRejects.$($dimension.name)" {
		$state=[ordered]@{baselineResources=$null;highWaterResources=$null};$seed=New-RawResources
		$initial=[UInt64]$seed[$dimension.name];$growthRejected=$false
		for([UInt64]$iteration=1;$iteration -le 4;$iteration++) {
			$raw=New-RawResources;$raw[$dimension.name]=$initial+($iteration-1)*$dimension.step
			try {Invoke-ActualResourceIteration $state $raw $iteration}
			catch {if($_.Exception.Message -notlike '*growth limit*'){throw};$growthRejected=$true;break}
		}
		Assert-Protocol $growthRejected 'Repeated sub-limit increments must still exceed and reject the immutable first baseline.'
		Assert-Protocol ((Get-RequiredUInt64 $state.baselineResources $dimension.name 'fixture baseline') -eq $initial) 'Rejected cumulative growth must not rewrite the warm baseline.'
	}
}
Invoke-ProtocolCase 'Soak.ResumedJsonStatePreservesBaselineAndRejectsCumulativeGrowth' {
	$raw=New-RawResources
	# Actual resume serialization duplicates value records; it must not silently
	# receive stronger bounded-growth behavior than an otherwise identical fresh run.
	$state=([ordered]@{baselineResources=$raw;highWaterResources=$raw}|ConvertTo-Json -Depth 8|ConvertFrom-Json)
	$rejected=$false
	for([UInt64]$iteration=2;$iteration -le 4;$iteration++) {
		$next=New-RawResources;$next.handleCount=100+($iteration-1)*32
		try {Invoke-ActualResourceIteration $state $next $iteration}catch{if($_.Exception.Message -notlike '*growth limit*'){throw};$rejected=$true;break}
	}
	Assert-Protocol ($rejected -and $state.baselineResources.handleCount -eq 100) 'Resumed actual branches must reject cumulative growth against their unchanged initial state.'
}
Invoke-ProtocolCase 'Soak.StableCounterControlRemainsAccepted' {
	$state=[ordered]@{baselineResources=$null;highWaterResources=$null}
	for([UInt64]$iteration=1;$iteration -le 6;$iteration++){Invoke-ActualResourceIteration $state (New-RawResources) $iteration}
	Assert-Protocol ($state.baselineResources.handleCount -eq 100 -and $state.highWaterResources.handleCount -eq 100) 'Actual soak branches must continue to accept stable resource counters.'
}
Complete-ProtocolTests
