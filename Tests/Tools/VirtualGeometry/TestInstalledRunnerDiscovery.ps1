# Discovery and command-protocol proof only; no codec/parity workload runs.
param([string]$SourceRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path)
$ErrorActionPreference='Stop'; Set-StrictMode -Version Latest
$production = Join-Path $SourceRoot 'Tests/Tools/VirtualGeometry/InvokeVirtualGeometryReleaseGates.ps1'
$tokens=$null; $errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($production,[ref]$tokens,[ref]$errors)
if($errors.Count -ne 0){throw 'Production release gate must parse.'}
foreach($function in @($ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst]},$true))){. ([scriptblock]::Create($function.Extent.Text))}
function Assignment([string]$name){
	$matches=@($ast.FindAll({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -eq ('$'+$name) -and $node.Operator -eq [Management.Automation.Language.TokenKind]::Equals},$true))
	if($matches.Count -ne 1){throw "Expected exact unique actual assignment $name"};return $matches[0]
}
function Executable-Statement($statement){
	# Copy the actual AST with its original file extent. Text-only Create loses
	# PSScriptRoot, which this producer needs for sibling parity manifest paths.
	# No expression or token is rewritten and no automatic variable is replaced.
	$body=[Management.Automation.Language.StatementBlockAst]::new($statement.Extent,[Management.Automation.Language.StatementAst[]]@($statement.Copy()),$null)
	$block=[Management.Automation.Language.ScriptBlockAst]::new($statement.Extent,$null,$body,$false).GetScriptBlock()
	if($block.File -ne $production){throw 'Executable AST must retain actual producer source-file context.'}
	return $block
}
$codecAssignment=Assignment 'codecRunner';$matrixAssignment=Assignment 'matrixArguments'
$codecBranches=@($ast.FindAll({param($node) $node -is [Management.Automation.Language.IfStatementAst] -and $node.Clauses[0].Item1.Extent.Text -like '*IsNullOrWhiteSpace($CodecInputMesh)*'},$true))
if($codecBranches.Count -ne 1){throw 'Expected actual codec availability/dispatch branch.'}
$codecDispatch=[scriptblock]::Create($codecBranches[0].Extent.Text)
Write-Output ("sourceSHA256={0}; codecAST={1}-{2}; matrixAST={3}-{4}; scope=discovery-and-protocol-not-codec-or-GPU" -f (Get-FileHash -LiteralPath $production -Algorithm SHA256).Hash,$codecAssignment.Extent.StartLineNumber,$codecAssignment.Extent.EndLineNumber,$matrixAssignment.Extent.StartLineNumber,$matrixAssignment.Extent.EndLineNumber)
$temporaryBase=[IO.Path]::GetFullPath([IO.Path]::GetTempPath());$workspace=Join-Path $temporaryBase ('TerraRunnerDiscovery-'+[guid]::NewGuid().ToString('N'))
if(Test-Path -LiteralPath $workspace){throw 'Exclusive fixture root exists.'};$null=New-Item -ItemType Directory -Path $workspace
$installedTree=Join-Path $workspace 'Installed Build With Spaces';$outputRoot=Join-Path $workspace 'Output Evidence With Spaces'
$null=New-Item -ItemType Directory -Path (Join-Path $installedTree 'Tests/Tools/VirtualGeometry') -Force
$null=New-Item -ItemType Directory -Path (Join-Path $installedTree 'Tools') -Force
$utf8NoBom=[Text.UTF8Encoding]::new($false);$repositoryRoot=$SourceRoot
$backendMatrix=Join-Path $SourceRoot 'Tests/Tools/VirtualGeometry/RunBackendParityMatrix.ps1'
foreach($name in @('TerraEngineVirtualGeometryCodecExperiment.exe','TerraEngineVirtualGeometryBenchmark.exe')){
	# Explicit non-executable placeholder bytes test only file discovery. Never
	# invoke these files or label them an installed/native codec implementation.
	[IO.File]::WriteAllText((Join-Path $installedTree "Tests/Tools/VirtualGeometry/$name"),'discovery-only-placeholder',$utf8NoBom)
	[IO.File]::WriteAllText((Join-Path $installedTree "Tools/$name"),'legacy-discovery-only-placeholder',$utf8NoBom)
}
$script:cases=0;$script:failures=0
function Require-Case([bool]$ok,[string]$reason){if(-not $ok){throw $reason}}
function Case([string]$name,[scriptblock]$body){$script:cases++;try{& $body;Write-Output "[PASS] $name"}catch{$script:failures++;Write-Output "[FAIL] ${name}: $($_.Exception.Message)"}}
try{
	Case 'RunnerDiscovery.CodecPrefersMaintainedTestsToolsPath' {
		. (Executable-Statement $codecAssignment)
		$expected=Join-Path $installedTree 'Tests/Tools/VirtualGeometry/TerraEngineVirtualGeometryCodecExperiment.exe'
		Require-Case ($codecRunner -eq $expected -and (Test-Path -LiteralPath $codecRunner -PathType Leaf)) "Actual codec discovery must prefer maintained path even with legacy file present; actual=$codecRunner"
	}
	Case 'RunnerDiscovery.ParityReceivesMaintainedBenchmarkPath' {
		$matrixOutput=Join-Path $outputRoot 'parity result.json'
		. (Executable-Statement $matrixAssignment)
		$runnerIndex=[Array]::IndexOf([object[]]$matrixArguments,'-Runner')
		$expected=Join-Path $installedTree 'Tests/Tools/VirtualGeometry/TerraEngineVirtualGeometryBenchmark.exe'
		Require-Case ($runnerIndex -ge 0 -and $matrixArguments[$runnerIndex+1] -eq $expected) 'Actual parity dispatch must receive maintained benchmark runner path as one argument.'
	}
	Case 'RunnerDiscovery.CommandIdentityRetainsSourcePathFallback' {
		$fixture=Join-Path $installedTree 'Tests/Tools/VirtualGeometry/TerraEngineVirtualGeometryCodecExperiment.exe'
		# Existing identity contract permits Get-Command.Source empty and .Path
		# present. Stub only discovery IO, then execute the actual hashing helper.
		function Get-Command {[CmdletBinding()]param([string]$Name) return [pscustomobject]@{Source='';Path=$fixture}}
		$identity=Get-CommandIdentity 'bounded-fixture-command'
		Require-Case ($identity.path -eq [IO.Path]::GetFullPath($fixture) -and $identity.sha256.Length -eq 64) 'Actual Path fallback must retain real fixture byte identity.'
	}
	Case 'RunnerDiscovery.ZeroSelectedPassRecordsCannotPass' {
		$hostExecutable=(Get-Process -Id $PID).Path
		$record=Invoke-GateCommand 'protocol-only' 'zero-selection' $hostExecutable @('-NoProfile','-Command',"Write-Output 'zero fixture selections'; exit 0") @{} $true
		Require-Case ($record.status -eq 'failed' -and $record.result.exitCode -eq 0 -and $record.result.selectedPassRecords -eq 0) 'Real exit-zero child with zero PASS records must fail selection gate.'
	}
	Case 'RunnerDiscovery.OneSelectedProtocolRecordControl' {
		$hostExecutable=(Get-Process -Id $PID).Path
		$record=Invoke-GateCommand 'protocol-only' 'one-selection' $hostExecutable @('-NoProfile','-Command',"Write-Output '[PASS] bounded-protocol-fixture'; exit 0") @{} $true
		Require-Case ($record.status -eq 'passed' -and $record.result.selectedPassRecords -eq 1) 'Actual command protocol positive control must retain one selected PASS; no codec proof claimed.'
	}
	Case 'RunnerDiscovery.DryRunCannotSkipInstalledValidation' {
		$hostExecutable=(Get-Process -Id $PID).Path
		& $hostExecutable -NoProfile -File (Join-Path $PSScriptRoot 'InvokeVirtualGeometryReleaseGates.ps1') -DryRun -ValidateInstalledRunners *> (Join-Path $workspace 'contradictory-flags.log')
		Require-Case ($LASTEXITCODE -ne 0) 'Actual entrypoint must reject contradictory flags before dry-run success.'
	}
	Case 'RunnerDiscovery.EmptyCommandCannotBecomeValidIdentity'  {
		$rejected=$false;try{$null=Get-CommandIdentity ''}catch{$rejected=$true};Require-Case $rejected 'Empty command must remain explicit discovery failure.'
	}
	Case 'RunnerDiscovery.MissingCodecRemainsExplicitUnavailable' {
		$CodecInputMesh=Join-Path $workspace 'path-only input.temf';[IO.File]::WriteAllText($CodecInputMesh,'path-only fixture',$utf8NoBom)
		$codecRunner=Join-Path $installedTree 'Tests/Tools/VirtualGeometry/missing-runner.exe';$rows=[Collections.Generic.List[object]]::new()
		& $codecDispatch
		Require-Case ($rows.Count -eq 1 -and $rows[0].status -eq 'unavailable') 'Actual missing-runner branch must emit unavailable, never accepted placeholder execution.'
	}
}finally{
	$resolved=[IO.Path]::GetFullPath($workspace);$prefix=$temporaryBase.TrimEnd([IO.Path]::DirectorySeparatorChar)+[IO.Path]::DirectorySeparatorChar
	if(-not $resolved.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Cleanup escaped intended fixture temp base.'};Remove-Item -LiteralPath $resolved -Recurse -Force
}
Write-Output "$script:cases selected tests, $script:failures failures"
if($script:cases -ne 8 -or $script:failures -ne 0){exit 1}
