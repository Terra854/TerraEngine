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
	[IO.File]::WriteAllText((Join-Path $WorkRoot 'result.json'), ([ordered]@{fixtureProtocolOnly=$true;nativePerformanceMeasured=$false;nativeSceneAuthoringExecuted=$false;selected=$script:Cases.Count;failures=$failures;cases=@($script:Cases.ToArray());functionProvenance=@($script:FunctionProvenance.ToArray())}|ConvertTo-Json -Depth 32),$script:Utf8NoBom)
	Write-Output "$($script:Cases.Count) selected protocol cases, $failures failures; native performance NOT measured."
	if($failures) { exit 1 }
}

$capture=Join-Path $script:RepoRoot 'Tests/Tools/VirtualGeometry/CaptureVirtualGeometryLiveVisual.ps1'
$adapter=Join-Path $script:RepoRoot 'Tests/Tools/VirtualGeometry/CaptureVirtualGeometryRuntimeSample.ps1'
. (Get-ActualProducerFunctions $adapter @('Get-OptionalProperty'))
function Get-ScenePinnedBlock([string]$Path,[string]$Kind,[scriptblock]$Select) {
	$tokens=$null;$errors=$null;$ast=[Management.Automation.Language.Parser]::ParseFile($Path,[ref]$tokens,[ref]$errors)
	if(@($errors).Count){throw 'Actual disposable-scene producer must parse.'}
	$nodes=@($ast.FindAll($Select,$true));if($nodes.Count -ne 1){throw "Actual scene AST '$Kind' must occur once."}
	$node=$nodes[0];$sha=[Security.Cryptography.SHA256]::Create()
	try{$hash=([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($node.Extent.Text)))).Replace('-','')}finally{$sha.Dispose()}
	$script:FunctionProvenance.Add([ordered]@{source=$Path;sourceSha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash;astKind=$Kind;astSha256=$hash;startLine=$node.Extent.StartLineNumber;endLine=$node.Extent.EndLineNumber})
	return [scriptblock]::Create($node.Extent.Text)
}
$preCleanup=Get-ScenePinnedBlock $capture 'actual-pre-cleanup-if' {param($node)
	$node -is [Management.Automation.Language.IfStatementAst] -and
	@($node.Clauses[0].Item1.FindAll({param($p) $p -is [Management.Automation.Language.VariableExpressionAst] -and $p.VariablePath.UserPath -eq 'CreateValidationScene'},$true)).Count -gt 0 -and
	@($node.Clauses[0].Item1.FindAll({param($p) $p -is [Management.Automation.Language.VariableExpressionAst] -and $p.VariablePath.UserPath -eq 'CleanupValidationScene'},$true)).Count -gt 0
}
$postCleanup=Get-ScenePinnedBlock $capture 'actual-final-cleanup-if' {param($node)
	$node -is [Management.Automation.Language.IfStatementAst] -and
	@($node.Clauses[0].Item1.FindAll({param($p) $p -is [Management.Automation.Language.VariableExpressionAst] -and $p.VariablePath.UserPath -eq 'createdValidationScenePath'},$true)).Count -gt 0 -and
	@($node.Clauses[0].Item1.FindAll({param($p) $p -is [Management.Automation.Language.VariableExpressionAst] -and $p.VariablePath.UserPath -eq 'CleanupValidationScene'},$true)).Count -gt 0
}
$createdPath=Get-ScenePinnedBlock $capture 'actual-created-path-assignment' {param($node)
	$node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left -is [Management.Automation.Language.VariableExpressionAst] -and $node.Left.VariablePath.UserPath -eq 'createdValidationScenePath' -and
	@($node.Right.FindAll({param($p) $p -is [Management.Automation.Language.InvokeMemberExpressionAst] -and $p.Member.Value -eq 'GetFullPath'},$true)).Count -gt 0
}
$adapterBlocks=@{}
foreach($condition in @('useRawMesh','isAnimatedWorkload')) {
	$adapterBlocks[$condition]=Get-ScenePinnedBlock $adapter "actual-adapter-$condition-if" {param($node)
		$node -is [Management.Automation.Language.IfStatementAst] -and
		@($node.Clauses[0].Item1.FindAll({param($p) $p -is [Management.Automation.Language.VariableExpressionAst] -and $p.VariablePath.UserPath -eq $condition},$true)).Count -gt 0 -and
		@($node.FindAll({param($p) $p -is [Management.Automation.Language.IndexExpressionAst] -and $p.Target -is [Management.Automation.Language.VariableExpressionAst] -and $p.Target.VariablePath.UserPath -eq 'liveArguments' -and $p.Index.Value -eq 'ValidationScenePath'},$true)).Count -gt 0
	}
}
function New-OwnedProject([string]$Name) {
	$caseRoot=Join-Path $WorkRoot $Name
	$root=Join-Path $caseRoot 'Project with spaces 日本語'
	$boundary=$WorkRoot.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
	if(-not ([IO.Path]::GetFullPath($root)).StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)){throw 'Fixture must stay inside its unique owned root.'}
	$protected=@{}
	foreach($relative in @('Assets/Scenes/Authored.tescene','Assets/Scenes/Authored.tescene.meta','Library/LastScene.txt','UserSettings/Preferences.json','Assets/Scenes-Other/FramePublication-sibling.tescene')) {
		$path=Join-Path $root $relative;[IO.Directory]::CreateDirectory((Split-Path -Parent $path))|Out-Null
		[IO.File]::WriteAllText($path,"OWNED PROTECTED FIXTURE: $relative",$script:Utf8NoBom)
		$protected[$path]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
	}
	$outside=Join-Path $caseRoot 'outside.tescene';[IO.File]::WriteAllText($outside,'OWNED OUTSIDE-PROJECT SENTINEL',$script:Utf8NoBom)
	$protected[$outside]=(Get-FileHash -LiteralPath $outside -Algorithm SHA256).Hash
	return [pscustomobject]@{root=$root;protected=$protected}
}
function Assert-ProtectedBytes($fixture) {
	foreach($path in $fixture.protected.Keys){Assert-Protocol ((Test-Path -LiteralPath $path -PathType Leaf) -and (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ceq $fixture.protected[$path]) 'Actual cleanup must preserve all authored-scene, startup, preference and outside-project fixture bytes.'}
}
function Invoke-ActualSceneLifecycle($fixture,[string]$ValidationScenePath) {
	$projectPath=$fixture.root;$CreateValidationScene=$true;$CleanupValidationScene=$true
	$candidate=[IO.Path]::GetFullPath((Join-Path $projectPath $ValidationScenePath))
	# Positive candidate was produced by the actual supported adapter branch or
	# the existing FramePublication public fixture spelling, within this owned root.
	$boundary=$projectPath.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
	Assert-Protocol ($candidate.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)) 'Owned positive scene fixture must be inside its private project.'
	[IO.Directory]::CreateDirectory((Split-Path -Parent $candidate))|Out-Null
	foreach($path in @($candidate,"$candidate.meta")){[IO.File]::WriteAllText($path,'OWNED STALE PROTOCOL MARKER',$script:Utf8NoBom)}
	. $preCleanup
	Assert-Protocol (-not (Test-Path -LiteralPath $candidate) -and -not (Test-Path -LiteralPath "$candidate.meta")) 'Actual pre-cleanup must remove only the exact owned stale scene and sidecar.'
	# Marker save stands in only for filesystem protocol, not MCP scene authoring.
	foreach($path in @($candidate,"$candidate.meta")){[IO.File]::WriteAllText($path,'OWNED NEW PROTOCOL MARKER, NOT NATIVE SCENE',$script:Utf8NoBom)}
	. $createdPath
	. $postCleanup
	Assert-Protocol (-not (Test-Path -LiteralPath $candidate) -and -not (Test-Path -LiteralPath "$candidate.meta")) 'Actual final cleanup must remove the exact candidate and sidecar.'
	Assert-ProtectedBytes $fixture
}
foreach($kind in @('raw','animated','low-poly-unique-direct','low-poly-repeated-direct','low-poly-forced-virtual-geometry','low-poly-automatic-routing')) {
	Invoke-ProtocolCase "DisposableScene.ActualRuntimeAdapter.$kind" {
		$fixture=New-OwnedProject $kind;$liveArguments=[ordered]@{}
		$useRawMesh=$kind -ne 'animated';$isAnimatedWorkload=$kind -eq 'animated';$rawPressureGeometryRequired=$false
		$workload=if($kind -eq 'raw'){'repeated-static'}elseif($kind -eq 'animated'){'animated-hero'}else{$kind};$sampleIndex=1
		$runtimeCameraPosition=@(0.0,0.5,1.0)
		$rawStaticScenarioProfile=[pscustomobject]@{cameraPosition=@(0.0,0.5,1.0);cameraTarget=@(0.0,0.5,0.0)}
		if($useRawMesh){. $adapterBlocks['useRawMesh']}else{. $adapterBlocks['isAnimatedWorkload']}
		Assert-Protocol ($liveArguments.CreateValidationScene -and $liveArguments.CleanupValidationScene) 'Actual supported adapter must request owned disposable-scene lifecycle.'
		Invoke-ActualSceneLifecycle $fixture $liveArguments.ValidationScenePath
	}
}
Invoke-ProtocolCase 'DisposableScene.ExistingFramePublicationControl' {
	$fixture=New-OwnedProject 'frame-publication-control'
	Invoke-ActualSceneLifecycle $fixture 'Assets/Scenes/FramePublication-protocol-control.tescene'
}
foreach($candidate in @(
	[pscustomobject]@{name='UnknownWorkloadRejected';path='Assets/Scenes/.terra-validation/virtual-geometry-unknown-1-123.tescene'},
	[pscustomobject]@{name='AuthoredSceneRejected';path='Assets/Scenes/Authored.tescene'},
	[pscustomobject]@{name='ParentTraversalRejected';path='../outside.tescene'},
	[pscustomobject]@{name='SiblingPrefixRejected';path='Assets/Scenes-Other/FramePublication-sibling.tescene'}
)) {
	Invoke-ProtocolCase "DisposableScene.$($candidate.name)" {
		$fixture=New-OwnedProject $candidate.name;$projectPath=$fixture.root
		$CreateValidationScene=$true;$CleanupValidationScene=$true;$ValidationScenePath=$candidate.path;$rejected=$false
		try{. $preCleanup}catch{if($_.Exception.Message -notlike '*disposable*' -and $_.Exception.Message -notlike '*validation scene*'){throw};$rejected=$true}
		Assert-Protocol $rejected 'Actual producer must reject an authored/outside/sibling cleanup candidate before any deletion.'
		Assert-ProtectedBytes $fixture
	}
}
Complete-ProtocolTests
