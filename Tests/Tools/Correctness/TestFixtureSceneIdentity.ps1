#requires -Version 7.0
# Exercise the actual installed-runner helper and scene-open/refresh AST only.
# Usage: pwsh -NoProfile -File Tests/Tools/Correctness/TestFixtureSceneIdentity.ps1
#   -SourceRoot <checkout> -EvidenceDirectory <fresh directory below checkout/out>
# Marker files and controlled tool replies are protocol inputs, not live scenes,
# approval receipts, editor execution, physical input or GPU acceptance.
[CmdletBinding()]
param(
	[string]$SourceRoot=(Join-Path $PSScriptRoot '../../..'),
	[string]$EvidenceDirectory
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if (!$IsWindows) { throw 'Fixture scene path protocols require the Windows runner path semantics.' }
$SourceRoot=[IO.Path]::GetFullPath($SourceRoot)
if (!$EvidenceDirectory) { $EvidenceDirectory=Join-Path $SourceRoot ('out/validation/fixture-scene-identity-'+[guid]::NewGuid().ToString('N')) }
$EvidenceDirectory=[IO.Path]::GetFullPath($EvidenceDirectory)
$boundary=(Join-Path $SourceRoot 'out').TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
if (!$EvidenceDirectory.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase) -or (Test-Path -LiteralPath $EvidenceDirectory)) { throw 'Use a fresh owned evidence directory below checkout/out.' }
[IO.Directory]::CreateDirectory($EvidenceDirectory)|Out-Null
$producer=Join-Path $SourceRoot 'Tests/Tools/Correctness/RunInstalledTextureUiAcceptance.ps1'
$parseErrors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($producer,[ref]$null,[ref]$parseErrors)
if (@($parseErrors).Count) { throw 'The actual producer must parse before protocol execution.' }
$pins=[Collections.Generic.List[object]]::new()
function Actual-Block($Node,[string]$Kind) {
	$bytes=[Text.Encoding]::UTF8.GetBytes($Node.Extent.Text)
	$sha=[Security.Cryptography.SHA256]::Create()
	try { $hash=([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-','') }
	finally { $sha.Dispose() }
	$pins.Add([ordered]@{kind=$Kind;source=$producer;sourceSha256=(Get-FileHash -LiteralPath $producer).Hash;astSha256=$hash;startLine=$Node.Extent.StartLineNumber;endLine=$Node.Extent.EndLineNumber})
	# Execute the exact source extent. No expression/token replacement, copied
	# path policy or live monolithic launcher body is admitted by this test.
	return [scriptblock]::Create($Node.Extent.Text)
}
$helpers=@($ast.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Test-FixtureSceneIdentity'},$true))
if ($helpers.Count -ne 1) { throw 'The producer must own one scene identity helper.' }
. (Actual-Block $helpers[0] 'actual-helper')
$checks=@($ast.FindAll({param($n)
	$n -is [Management.Automation.Language.IfStatementAst] -and
	@($n.Clauses[0].Item1.FindAll({param($part)$part -is [Management.Automation.Language.CommandAst] -and $part.GetCommandName() -eq 'Test-FixtureSceneIdentity'},$true)).Count -eq 1
},$true)|Sort-Object {$_.Extent.StartOffset})
if ($checks.Count -ne 2) { throw 'The actual producer must use identity at both scene branch gates.' }
$expected=@($ast.FindAll({param($n)$n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Left -is [Management.Automation.Language.VariableExpressionAst] -and $n.Left.VariablePath.UserPath -eq 'expectedFixtureScenePath'},$true))
if ($expected.Count -ne 1) { throw 'The producer must own one expected fixture path assignment.' }
$stateAssignments=@($ast.FindAll({param($n)$n -is [Management.Automation.Language.AssignmentStatementAst] -and $n.Left -is [Management.Automation.Language.VariableExpressionAst] -and $n.Left.VariablePath.UserPath -eq 'state'},$true))
$before=@($stateAssignments|Where-Object{$_.Extent.StartOffset -gt $expected[0].Extent.EndOffset -and $_.Extent.EndOffset -lt $checks[0].Extent.StartOffset})
$refresh=@($stateAssignments|Where-Object{$_.Extent.StartOffset -gt $checks[0].Extent.EndOffset -and $_.Extent.EndOffset -lt $checks[1].Extent.StartOffset})
if ($before.Count -ne 1 -or $refresh.Count -ne 1) { throw 'The actual scene branch must read state initially and refresh it before the final gate.' }
$blocks=@(
	(Actual-Block $expected[0] 'actual-expected-path'),
	(Actual-Block $before[0] 'actual-initial-state-read'),
	(Actual-Block $checks[0] 'actual-open-branch'),
	(Actual-Block $refresh[0] 'actual-state-refresh'),
	(Actual-Block $checks[1] 'actual-final-identity-gate')
)
$script:cases=[Collections.Generic.List[object]]::new()
function Require([bool]$Value,[string]$Message) { if (!$Value) { throw $Message } }
function Case([string]$Name,[scriptblock]$Body) {
	try { &$Body; $script:cases.Add([ordered]@{name=$Name;status='passed'}); Write-Output "[PASS] $Name" }
	catch { $script:cases.Add([ordered]@{name=$Name;status='failed';error=$_.Exception.Message}); Write-Output "[FAIL] $Name : $($_.Exception.Message)" }
}
function New-State([string]$Name,$Path,[switch]$OmitPath) {
	$value=[pscustomobject]@{activeScene=$Name}
	if (!$OmitPath) { $value|Add-Member -NotePropertyName activeScenePath -NotePropertyValue $Path }
	return $value
}
function Invoke-ActualBranch($Initial,$After,[int]$ExpectedOpen,[bool]$Reject,[switch]$RefuseRefresh) {
	$project=Join-Path $EvidenceDirectory ([guid]::NewGuid().ToString('N'))
	$scene=Join-Path $project 'Assets/Scenes/Startup.tescene'
	[IO.Directory]::CreateDirectory((Split-Path -Parent $scene))|Out-Null
	[IO.File]::WriteAllText($scene,'PROTOCOL FILE MARKER: never loaded by an editor.')
	# Inputs are factories so each case binds to this actual fresh owned path.
	$script:initialState=&$Initial $scene
	$script:afterState=&$After $scene
	$script:readCount=0; $script:openCalls=@(); $script:records=@()
	function Tool([string]$Name,$Arguments=@{}) {
		if ($Name -eq 'project.state') {
			$script:readCount++
			if ($script:readCount -eq 1) { return [pscustomobject]@{data=$script:initialState} }
			if ($script:readCount -eq 2) {
				if ($RefuseRefresh) { throw 'Owned state refresh refusal' }
				return [pscustomobject]@{data=$script:afterState}
			}
			throw 'Unexpected extra state read.'
		}
		if ($Name -eq 'scene.open') {
			$script:openCalls+=@{name=$Name;arguments=$Arguments}
			return [pscustomobject]@{ok=$true;data=@{protocolOnly=$true}}
		}
		throw "Unexpected protocol boundary: $Name"
	}
	function Record($Name,$Data) { $script:records+=@{name=$Name;data=$Data} }
	$rejected=$false; $failure=$null
	try { foreach ($block in $blocks) { . $block } }
	catch { $rejected=$true; $failure=$_.Exception.Message }
	Require ($rejected -eq $Reject) 'Actual producer branch acceptance/rejection differs.'
	if ($Reject) { Require ($failure -eq 'Authored Startup scene source is not active.' -or ($RefuseRefresh -and $failure -eq 'Owned state refresh refusal')) 'Failure must come from the real identity gate or explicitly refused state refresh.' }
	Require ($script:readCount -eq 2) 'Actual source must read and refresh state; retained pre-open state cannot establish success.'
	Require ($script:openCalls.Count -eq $ExpectedOpen -and $script:records.Count -eq $ExpectedOpen) 'Actual source must open exactly when current source identity is unproven.'
	foreach ($call in $script:openCalls) { Require ($call.arguments.path -ceq 'Assets/Scenes/Startup.tescene') 'The real open branch must request the exact owned fixture.' }
}
Case 'FixtureSceneIdentity.AuthoredDisplayNameCorrectPathAccepted' {
	$correct={param($p)New-State 'Installed Texture and UI Observation' $p}
	Invoke-ActualBranch $correct $correct 0 $false
}
Case 'FixtureSceneIdentity.StartupDisplayNameWrongPathRejected' {
	$wrong={param($p)New-State 'Startup' ($p+'.different')}
	Invoke-ActualBranch $wrong $wrong 1 $true
}
Case 'FixtureSceneIdentity.MissingSourcePathRejected' {
	$missing={param($p)New-State 'Startup' $null -OmitPath}
	Invoke-ActualBranch $missing $missing 1 $true
}
Case 'FixtureSceneIdentity.RelativeSourcePathRejected' {
	$relative={param($p)New-State 'Startup' 'Assets/Scenes/Startup.tescene'}
	Invoke-ActualBranch $relative $relative 1 $true
}
Case 'FixtureSceneIdentity.NormalSlashCaseSpellingAccepted' {
	$spelling={param($p)New-State 'Another display name' ($p.Replace('\','/').ToUpperInvariant())}
	Invoke-ActualBranch $spelling $spelling 0 $false
}
Case 'FixtureSceneIdentity.OpenThenRefreshedCorrectPathAccepted' {
	$wrong={param($p)New-State 'Startup' ($p+'.different')}
	$correct={param($p)New-State 'Installed Texture and UI Observation' $p}
	Invoke-ActualBranch $wrong $correct 1 $false
}
Case 'FixtureSceneIdentity.FailedRefreshCannotReusePriorSuccess' {
	$correct={param($p)New-State 'Installed Texture and UI Observation' $p}
	Invoke-ActualBranch $correct $correct 0 $true -RefuseRefresh
}
$failed=@($script:cases|Where-Object{$_.status -ne 'passed'}).Count
[ordered]@{scope='Exact producer AST protocol; no editor/native approval/physical input/GPU execution';selected=($script:cases.Count);failures=$failed;cases=@($script:cases.ToArray());sourcePins=@($pins.ToArray());lexicalPathIdentityOnly=$true;junctionAliasCanonicalityClaimed=$false}|ConvertTo-Json -Depth 20|Set-Content (Join-Path $EvidenceDirectory 'result.json') -Encoding utf8
Write-Output "$($script:cases.Count) selected tests, $failed failures"
if ($script:cases.Count -ne 7 -or $failed -ne 0) { exit 1 }
