# Actual soak argv transport through production native-argument/launch ASTs.
# The maintained native recorder records OS argv and never runs a soak.
param([string]$SourceRoot=(Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path,[string]$ArgumentRecorderExecutable='')
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
if([string]::IsNullOrWhiteSpace($ArgumentRecorderExecutable)) {
	# Installed tests resolve only their maintained sibling child. Source runs
	# supply an explicit installed executable instead of searching personal paths.
	$suffix=if([Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([Runtime.InteropServices.OSPlatform]::Windows)){'.exe'}else{''}
	$ArgumentRecorderExecutable=Join-Path $PSScriptRoot "../CodeQuality/TerraEngine-NativeArgvReceipt$suffix"
}
if(-not (Test-Path -LiteralPath $ArgumentRecorderExecutable -PathType Leaf)) {throw 'Maintained native argv-recorder prerequisite missing; supply -ArgumentRecorderExecutable or install the sibling TerraEngine-NativeArgvReceipt child.'}
$recorder=[IO.Path]::GetFullPath($ArgumentRecorderExecutable)
if((Get-Item -LiteralPath $recorder).Length -eq 0){throw 'Empty recorder cannot satisfy native prerequisite.'}
$production=Join-Path $SourceRoot 'Tests/Tools/Navigation/RunInstalledNavigationAcceptance.ps1';$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseFile($production,[ref]$tokens,[ref]$errors)
if($errors.Count -ne 0){throw 'Actual navigation producer must parse.'}
function Actual-Assignment([string]$name){
	$matches=@($ast.FindAll({param($node) $node -is [Management.Automation.Language.AssignmentStatementAst] -and $node.Left.Extent.Text -eq ('$'+$name)},$true))
	if($matches.Count -ne 1){throw "Expected unique actual assignment $name"};return $matches[0]
}
$durationAst=Actual-Assignment 'duration';$qualificationAst=Actual-Assignment 'qualificationArgument';$argumentsAst=Actual-Assignment 'arguments'
$launches=@($ast.FindAll({param($node)
	if($node -isnot [Management.Automation.Language.AssignmentStatementAst] -or $node.Left.Extent.Text -ne '$process'){return $false}
	return $null -ne $node.Find({param($command) $command -is [Management.Automation.Language.CommandAst] -and $command.GetCommandName() -eq 'Start-Process'},$true)
},$true))
if($launches.Count -ne 1){throw 'Expected one exact production native soak launch assignment.'}
$nativeHelpers=@($ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'ConvertTo-NavigationNativeArgumentLine'},$true))
if($nativeHelpers.Count -ne 1){throw 'Expected the actual repaired native argv helper.'}
. ([scriptblock]::Create($nativeHelpers[0].Extent.Text))
$actualLaunch=[scriptblock]::Create($launches[0].Extent.Text)
Write-Output ("sourceSHA256={0}; launchAST={1}-{2}; argumentAST={3}-{4}; recorderSHA256={5}; scope=native-argv-protocol-not-soak-performance" -f (Get-FileHash -LiteralPath $production -Algorithm SHA256).Hash,$launches[0].Extent.StartLineNumber,$launches[0].Extent.EndLineNumber,$argumentsAst.Extent.StartLineNumber,$argumentsAst.Extent.EndLineNumber,(Get-FileHash -LiteralPath $recorder -Algorithm SHA256).Hash)
$temporaryBase=[IO.Path]::GetFullPath([IO.Path]::GetTempPath());$workspace=Join-Path $temporaryBase ('TerraSoakArgv-'+[guid]::NewGuid().ToString('N'))
if(Test-Path -LiteralPath $workspace){throw 'Exclusive fixture root exists.'};$null=New-Item -ItemType Directory -Path $workspace
$install=Join-Path $workspace 'Installed Build With Spaces';$output=Join-Path $workspace 'Output Evidence With Spaces';$unicodeOutput=Join-Path $workspace ('Output '+[char]0x00E9+' Unicode With Spaces')
$null=New-Item -ItemType Directory -Path $install,$output,$unicodeOutput
$runner=Join-Path $install 'Native Argv Recorder.exe';Copy-Item -LiteralPath $recorder -Destination $runner
$script:cases=0;$script:failures=0;$script:processes=[Collections.Generic.List[object]]::new()
function Require-Case([bool]$ok,[string]$reason){if(-not $ok){throw $reason}}
function Case([string]$name,[scriptblock]$body){$script:cases++;try{& $body;Write-Output "[PASS] $name"}catch{$script:failures++;Write-Output "[FAIL] ${name}: $($_.Exception.Message)"}}
function Production-Arguments([string]$lane,[string]$manifestPath,[string]$destination,[int]$override=0){
	$manifest=$manifestPath;$laneResult=$destination;$matrix='soak';$DurationSecondsOverride=$override;$DurationMinutes=30
	$matrixCycles=1;$agents=256;$requests=100;$matrixArgument='soak';$IdlePairs=5;$evidence=[pscustomobject]@{qualification=($override -eq 0)}
	. ([scriptblock]::Create($durationAst.Extent.Text))
	. ([scriptblock]::Create($qualificationAst.Extent.Text))
	. ([scriptblock]::Create($argumentsAst.Extent.Text))
	return ,([string[]]$arguments)
}
function Observe-ActualTransport([string[]]$expected,[string]$tag){
	$arguments=$expected;$stdout=Join-Path $output ($tag+'.stdout');$stderr=Join-Path $output ($tag+'.stderr');$receipt=Join-Path $output ($tag+'.argv.json')
	$old=[Environment]::GetEnvironmentVariable('TERRAENGINE_SOAK_ARGV_RECEIPT','Process')
	try{
		[Environment]::SetEnvironmentVariable('TERRAENGINE_SOAK_ARGV_RECEIPT',$receipt,'Process')
		# Execute the actual producer assignment unchanged: no mocked Start-Process
		# or prefixed PS launcher whose own argv quoting could hide the defect.
		. $actualLaunch
		# Retain the native handle before waiting: older supported PowerShell hosts
		# otherwise lose a fast child's exit-code observation after it settles.
		$null = $process.Handle
		$script:processes.Add($process)
	}finally{[Environment]::SetEnvironmentVariable('TERRAENGINE_SOAK_ARGV_RECEIPT',$old,'Process')}
	if(-not $process.WaitForExit(5000)){ $process.Kill();$process.WaitForExit();throw 'Owned argv-recorder exceeded5second watchdog.' }
	$process.Refresh();Require-Case ($process.ExitCode -eq 0 -and (Test-Path -LiteralPath $receipt -PathType Leaf)) 'Actual recorder must publish real native argv and settle normally.'
	$observed=Get-Content -LiteralPath $receipt -Raw -Encoding utf8 | ConvertFrom-Json
	# Persist COMPLETE actual native receipt into captured unit stdout before any assertion.
	[Console]::WriteLine('NativeArgvReceipt='+($observed | ConvertTo-Json -Compress))
	Require-Case ($observed.scope -eq 'NativeArgvProtocol_NotNavigationSoak' -and $observed.processId -eq $process.Id) 'Receipt must belong to the actual owned native child, not predicted arguments.'
	Require-Case ($observed.argv.Count -eq $expected.Count) "Actual native argv count mismatch: expected=$($expected.Count), actual=$($observed.argv.Count); argv=$($observed.argv | ConvertTo-Json -Compress)"
	for($index=0;$index -lt $expected.Count;$index++){Require-Case ([string]$observed.argv[$index] -ceq $expected[$index]) "Real native argument$index must preserve exact Unicode/space content."}
	return $observed
}
try{
	Case 'SoakArgumentPaths.BoundedRelativeArgumentControl' {
		$expected=Production-Arguments 'export-rendered' 'manifest.json' 'result.json' 1
		$null=Observe-ActualTransport $expected 'relative-control'
	}
	Case 'SoakArgumentPaths.SpacedInstallAndOutputRemainSingleArguments' {
		$manifest=Join-Path $install 'Navigation Fixture Manifest.json';[IO.File]::WriteAllText($manifest,'{"scope":"argv-only-not-navigation-manifest"}')
		$result=Join-Path $output 'Rendered Lane Result.json';$expected=Production-Arguments 'export-rendered' $manifest $result 1
		$null=Observe-ActualTransport $expected 'spaced-rendered'
	}
	Case 'SoakArgumentPaths.UnicodeSpacedHeadlessPathsRemainExact' {
		$manifest=Join-Path $install ('Manifest '+[char]0x00E9+'.json');[IO.File]::WriteAllText($manifest,'{"scope":"argv-only"}')
		$result=Join-Path $unicodeOutput 'Headless Lane Result.json';$expected=Production-Arguments 'export-headless' $manifest $result 1
		$null=Observe-ActualTransport $expected 'unicode-headless'
	}
	Case 'SoakArgumentPaths.DeclaredQualificationAndOverrideStayHonest' {
		$qualified=Production-Arguments 'export-headless' 'manifest.json' 'result.json' 0
		$durationIndex=[Array]::IndexOf($qualified,'--duration-seconds');Require-Case ($qualified[$durationIndex+1] -eq '1800' -and '--qualification' -in $qualified) 'Actual qualification must retain required1800-second declaration.'
		$null=Observe-ActualTransport $qualified 'declared-duration-only'
		$short=Production-Arguments 'export-rendered' 'manifest.json' 'result.json' 1
		Require-Case ($short[$durationIndex+1] -eq '1' -and '--qualification' -notin $short) 'Bounded override must never masquerade as1800-second qualification.'
	}
}finally{
	foreach($owned in $script:processes){try{if(-not $owned.HasExited){$owned.Kill();$owned.WaitForExit()}}finally{$owned.Dispose()}}
	$resolved=[IO.Path]::GetFullPath($workspace);$prefix=$temporaryBase.TrimEnd([IO.Path]::DirectorySeparatorChar)+[IO.Path]::DirectorySeparatorChar
	if(-not $resolved.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){throw 'Cleanup escaped fixture temp base.'};Remove-Item -LiteralPath $resolved -Recurse -Force
}
Write-Output "$script:cases selected tests, $script:failures failures"
if($script:cases -ne 4 -or $script:failures -ne 0){exit 1}
