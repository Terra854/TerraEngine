# Bounded path-policy regression. Run with PowerShell 7 from a clean checkout:
# pwsh -File Tests/Tools/CodeQuality/TestToolchainPaths.ps1 -RepoRoot <absolute-root>
# Executes exact verifier functions; it does not claim a complete source audit.
[CmdletBinding()]
param([string]$RepoRoot=(Join-Path $PSScriptRoot '../../..'), [string]$WorkRoot)
$ErrorActionPreference='Stop'
$RepoRoot=[IO.Path]::GetFullPath($RepoRoot)
if(!$WorkRoot){$WorkRoot=Join-Path $RepoRoot ('out/validation/toolchain-paths-'+[guid]::NewGuid().ToString('N'))}
if(Test-Path -LiteralPath $WorkRoot){throw 'WorkRoot must be fresh'}
[void][IO.Directory]::CreateDirectory($WorkRoot)
$source=Join-Path $RepoRoot 'Tests/Tools/CodeQuality/VerifySourceReviewQuality.ps1'
$tokens=$null; $parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($source,[ref]$tokens,[ref]$parseErrors)
if($parseErrors.Count){throw 'Verifier must parse'}
foreach($name in @('Require','Hash','StringValue','NoRedirect','Full','Under','ToolchainExecutable')){
	$nodes=@($ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name},$true))
	if($nodes.Count -ne 1){throw "Missing exact producer function: $name"}
	Invoke-Expression $nodes[0].Extent.Text
}
$cases=[Collections.Generic.List[object]]::new()
function Check([string]$Name,[scriptblock]$Body){
	try{& $Body; $cases.Add(@{name=$Name;status='passed'}); Write-Output "[PASS] $Name"}
	catch{$cases.Add(@{name=$Name;status='failed';error=$_.Exception.Message}); Write-Output "[FAIL] $Name : $_"}
}
function MustReject([scriptblock]$Body){$rejected=$false; try{& $Body | Out-Null}catch{$rejected=$true}; if(!$rejected){throw 'Expected path rejection'}}
$target=Join-Path $WorkRoot 'target'; [void][IO.Directory]::CreateDirectory($target)
$exe=Join-Path $target 'tool.exe'; [IO.File]::WriteAllText($exe,'first executable bytes')
$alias=Join-Path $WorkRoot 'alias'
# Windows junctions need no developer-mode or elevation. Unix directory links
# exercise the same reparse/redirect policy where this protocol is supported.
if($IsWindows){New-Item -ItemType Junction -Path $alias -Target $target | Out-Null}
else{New-Item -ItemType SymbolicLink -Path $alias -Target $target | Out-Null}
$linked=Join-Path $alias 'tool.exe'
Check 'Explicit executable alias is accepted and byte-pinned' {
	$resolved=ToolchainExecutable $linked
	if($resolved -cne [IO.Path]::GetFullPath($linked) -or (Hash $resolved) -cne (Hash $exe)){throw 'Alias must execute and hash the same explicit file'}
}
Check 'Relative executable path is rejected' {MustReject {ToolchainExecutable 'tool.exe'}}
Check 'Missing executable path is rejected' {MustReject {ToolchainExecutable (Join-Path $WorkRoot 'missing.exe')}}
Check 'Changed executable bytes invalidate a prior pin' {
	$before=Hash $linked; [IO.File]::WriteAllText($exe,'replacement executable bytes')
	if((Hash $linked) -ceq $before){throw 'Replacement bytes must invalidate the old SHA pin'}
}
# Exercise each real consumer entry: a source file, a relative artifact under
# an owned root, and the output parent itself. These are distinct path shapes.
Check 'Redirected source file is rejected by Full' {MustReject {Full $linked}}
Check 'Redirected artifact under a trusted root is rejected by Under' {MustReject {Under $WorkRoot 'alias/tool.exe'}}
Check 'Redirected output parent is rejected by NoRedirect' {MustReject {NoRedirect $alias}}
$failures=@($cases|Where-Object status -ne 'passed').Count
@{selected=$cases.Count;failures=$failures;fixtureProtocolOnly=$true;source=$source;sourceSha256=Hash $source;cases=@($cases.ToArray())}|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $WorkRoot 'result.json') -Encoding utf8
Write-Output "$($cases.Count) selected protocol cases, $failures failures"
if($failures){exit 1}
