# Bounded filesystem/AST protocol checks; no editor, Vulkan, or soak is launched.
# Run: pwsh -NoProfile -File Tests/Tools/VirtualGeometry/TestRuntimeAdapterPaths.ps1
# Requires Windows path semantics and a fresh private WorkRoot below repository out/.
# Source hashes and exact executed AST extents are retained in result.json.
#requires -Version 7.0
[CmdletBinding()]
param(
	[string]$RepoRoot = (Join-Path $PSScriptRoot '../../..'),
	[string]$WorkRoot
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'This protocol requires Windows path semantics.' }
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
if([string]::IsNullOrWhiteSpace($WorkRoot)) { $WorkRoot = Join-Path $RepoRoot ('out/validation/runtime-adapter-paths/' + [guid]::NewGuid().ToString('N')) }
$WorkRoot = [IO.Path]::GetFullPath($WorkRoot)
$boundary = (Join-Path $RepoRoot 'out').TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar
if(-not $WorkRoot.StartsWith($boundary, [StringComparison]::OrdinalIgnoreCase)) { throw 'WorkRoot must remain below repository out/.' }
if(Test-Path -LiteralPath $WorkRoot) { throw 'A fresh nonexistent WorkRoot is required; prior receipts are preserved.' }
[IO.Directory]::CreateDirectory($WorkRoot) | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
$cases = [Collections.Generic.List[object]]::new()
$provenance = [Collections.Generic.List[object]]::new()
function Assert-Protocol([bool]$Value, [string]$Message) { if(-not $Value) { throw $Message } }
function Invoke-Case([string]$Name, [scriptblock]$Body) {
	try { & $Body; $cases.Add([ordered]@{name=$Name;status='passed'}); Write-Output "[PASS] $Name" }
	catch { $cases.Add([ordered]@{name=$Name;status='failed';error=$_.Exception.Message}); Write-Output "[FAIL] $Name : $($_.Exception.Message)" }
}
function Get-PinnedBlock([string]$Path, [string]$Kind, [scriptblock]$Selector) {
	$tokens=$null; $errors=$null
	$ast=[Management.Automation.Language.Parser]::ParseFile($Path,[ref]$tokens,[ref]$errors)
	if(@($errors).Count) { throw "Production source must parse: $Path" }
	$nodes=@($ast.FindAll($Selector,$true))
	if($nodes.Count -ne 1) { throw "Expected one actual production AST for $Kind, found $($nodes.Count)." }
	$node=$nodes[0]; $sha=[Security.Cryptography.SHA256]::Create()
	try { $hash=([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($node.Extent.Text)))).Replace('-','').ToLowerInvariant() } finally { $sha.Dispose() }
	$provenance.Add([ordered]@{source=$Path;sourceSha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant();kind=$Kind;astSha256=$hash;startLine=$node.Extent.StartLineNumber;endLine=$node.Extent.EndLineNumber})
	return [scriptblock]::Create($node.Extent.Text)
}
$adapter=Join-Path $RepoRoot 'Tests/Tools/VirtualGeometry/CaptureVirtualGeometryRuntimeSample.ps1'
$capture=Join-Path $RepoRoot 'Tests/Tools/VirtualGeometry/CaptureVirtualGeometryLiveVisual.ps1'
. (Get-PinnedBlock $adapter 'actual-root-resolver' {param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Resolve-RuntimeAdapterRoots'})
$rootAssignments=@()
foreach($variable in @('runtimeRoots','sourceTree','repositoryRoot','installedRoot','workingRoot')) {
	$rootAssignments += Get-PinnedBlock $adapter "actual-$variable-assignment" {param($n) $n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Left -is [Management.Automation.Language.VariableExpressionAst] -and $n.Left.VariablePath.UserPath -eq $variable}
}
$cleanup=Get-PinnedBlock $capture 'actual-pre-cleanup-branch' {param($n)
	$n -is [Management.Automation.Language.IfStatementAst] -and
	@($n.Clauses[0].Item1.FindAll({param($v) $v -is [Management.Automation.Language.VariableExpressionAst] -and $v.VariablePath.UserPath -eq 'CreateValidationScene'},$true)).Count -gt 0 -and
	@($n.Clauses[0].Item1.FindAll({param($v) $v -is [Management.Automation.Language.VariableExpressionAst] -and $v.VariablePath.UserPath -eq 'CleanupValidationScene'},$true)).Count -gt 0
}
# Only fixture-owned directories/markers are created. Execute the actual helper
# and its actual callsite assignments, rather than a second layout calculation.
foreach($layout in @(
	[pscustomobject]@{name='Source';source=$true;directory='source'},
	[pscustomobject]@{name='Installed';source=$false;directory='installed/Build'},
	[pscustomobject]@{name='SourceSpacesUnicode';source=$true;directory='source with spaces 日本語'},
	[pscustomobject]@{name='InstalledSpacesUnicode';source=$false;directory='installed with spaces 日本語/Build'}
)) {
	Invoke-Case "RuntimeRoots.$($layout.name)" {
		$expectedRoot=[IO.Path]::GetFullPath((Join-Path $WorkRoot $layout.directory))
		$scriptRoot=Join-Path $expectedRoot 'Tests/Tools/VirtualGeometry'
		[IO.Directory]::CreateDirectory($scriptRoot) | Out-Null
		if($layout.source) { [IO.File]::WriteAllText((Join-Path $expectedRoot 'CMakeLists.txt'),'# Owned source-root marker only',$utf8) }
		foreach($assignment in $rootAssignments) { . $assignment }
		$expectedInstall=if($layout.source) { Join-Path $expectedRoot 'out/build/Testing/Build' } else { $expectedRoot }
		Assert-Protocol ($sourceTree -eq $layout.source) 'Actual resolver must distinguish source and installed layouts.'
		Assert-Protocol ($repositoryRoot -ceq $expectedRoot -and $workingRoot -ceq $expectedRoot -and $installedRoot -ceq $expectedInstall) 'Actual root assignments must preserve the expected layout including spaces/Unicode.'
	}
}
function Invoke-CleanupProtocol([string]$Name, [string]$RelativePath, [bool]$Accept, [bool]$RootedEscape=$false) {
	$caseRoot=Join-Path $WorkRoot $Name
	$projectPath=Join-Path $caseRoot 'Project with spaces 日本語'
	[IO.Directory]::CreateDirectory((Join-Path $projectPath 'Assets/Scenes')) | Out-Null
	$ValidationScenePath=if($RootedEscape) { Join-Path $caseRoot 'outside.tescene' } else { $RelativePath }
	# Preserve an actual outside-project sentinel for rooted input. Production
	# still executes its own Join-Path/GetFullPath and must reject before removal;
	# Windows PowerShell hosts can combine rooted child tokens differently.
	$candidate=if($RootedEscape) { [IO.Path]::GetFullPath($ValidationScenePath) } else { [IO.Path]::GetFullPath((Join-Path $projectPath $ValidationScenePath)) }
	Assert-Protocol ($candidate.StartsWith($WorkRoot.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) 'All protocol candidates must be fixture-owned.'
	[IO.Directory]::CreateDirectory((Split-Path -Parent $candidate)) | Out-Null
	foreach($path in @($candidate,"$candidate.meta")) { [IO.File]::WriteAllText($path,'OWNED PROTOCOL SENTINEL',$utf8) }
	$before=(Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash
	$removals=[Collections.Generic.List[string]]::new()
	# Observe exact production deletion requests without deleting anything. The
	# spy is scoped to this invocation, with no editor/process/global preference IO.
	function Remove-Item { param([string]$LiteralPath,[switch]$Force)
		Assert-Protocol ($LiteralPath.StartsWith($WorkRoot.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) 'A deletion request escaped the owned fixture.'
		$removals.Add($LiteralPath)
	}
	$CreateValidationScene=$true; $CleanupValidationScene=$true; $rejected=$false
	try { . $cleanup } catch { if($_.Exception.Message -notmatch 'validation scene|disposable') { throw }; $rejected=$true }
	if($Accept) {
		Assert-Protocol (-not $rejected -and $removals.Count -eq 2 -and $removals[0] -ceq $candidate -and $removals[1] -ceq "$candidate.meta") 'Exact pressure sample must request only its scene and sidecar cleanup.'
	} else {
		Assert-Protocol ($rejected -and $removals.Count -eq 0) 'Rejected candidate must never reach the deletion command.'
	}
	Assert-Protocol ((Get-FileHash -LiteralPath $candidate -Algorithm SHA256).Hash -ceq $before -and (Test-Path -LiteralPath "$candidate.meta" -PathType Leaf)) 'Protocol observation must preserve marker bytes.'
}
Invoke-Case 'PressureCleanup.ExactAccepted' { Invoke-CleanupProtocol 'pressure-exact' 'Assets/Scenes/.terra-validation/virtual-geometry-page-residency-pressure-1-123.tescene' $true }
foreach($rejection in @(
	[pscustomobject]@{name='NearMatch';path='Assets/Scenes/.terra-validation/virtual-geometry-page-residency-pressure-extra-1-123.tescene';rooted=$false},
	[pscustomobject]@{name='NonNumericSuffix';path='Assets/Scenes/.terra-validation/virtual-geometry-page-residency-pressure-x-123.tescene';rooted=$false},
	[pscustomobject]@{name='SiblingDirectory';path='Assets/Scenes-Other/.terra-validation/virtual-geometry-page-residency-pressure-1-123.tescene';rooted=$false},
	[pscustomobject]@{name='ParentEscape';path='../outside.tescene';rooted=$false},
	[pscustomobject]@{name='AuthoredScene';path='Assets/Scenes/Authored.tescene';rooted=$false},
	[pscustomobject]@{name='RootedEscape';path='';rooted=$true}
)) {
	Invoke-Case "PressureCleanup.$($rejection.name)Rejected" { Invoke-CleanupProtocol $rejection.name $rejection.path $false $rejection.rooted }
}
$failures=@($cases | Where-Object {$_.status -ne 'passed'}).Count
[IO.File]::WriteAllText((Join-Path $WorkRoot 'result.json'),([ordered]@{fixtureProtocolOnly=$true;nativeEditorLaunched=$false;nativePerformanceMeasured=$false;selected=$cases.Count;failures=$failures;cases=@($cases.ToArray());sourceProvenance=@($provenance.ToArray())}|ConvertTo-Json -Depth 20),$utf8)
Write-Output "$($cases.Count) selected protocol cases, $failures failures; no editor or native measurement."
if($failures -or $cases.Count -ne 11) { exit 1 }
