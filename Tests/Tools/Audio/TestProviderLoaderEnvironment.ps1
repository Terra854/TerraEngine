# Audio provider loader protocol qualification of actual producer AST, not a fabricated Linux run.
# Prerequisite: PowerShell 7; run from a checkout with the actual audio producer.
# Example after integration (use a fresh nonexistent work directory):
# pwsh -NoProfile -File Tests/Tools/Audio/TestProviderLoaderEnvironment.ps1 -RepoRoot . -ProtocolOnly
# Omitting -ProtocolOnly deliberately returns 2 because this test alone cannot
# qualify a fresh native consumer build/link/run. Optional existing-artifact ELF
# inspection is read-only and does not turn that missing native proof into success.
[CmdletBinding()]
param(
	[string]$RepoRoot=(Join-Path $PSScriptRoot '../../..'),
	[string]$WorkRoot,
	[string]$ExistingLinuxConsumer='',
	[string]$WslDistribution='',
	[switch]$ProtocolOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$RepoRoot=[IO.Path]::GetFullPath($RepoRoot)
if(!$WorkRoot){$WorkRoot=Join-Path $RepoRoot ('out/validation/audio-provider-protocol/'+[guid]::NewGuid().ToString('N'))}
$WorkRoot=[IO.Path]::GetFullPath($WorkRoot)
$boundary=(Join-Path $RepoRoot 'out').TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
if(!$WorkRoot.StartsWith($boundary,[StringComparison]::OrdinalIgnoreCase)-or(Test-Path -LiteralPath $WorkRoot)){throw 'Use a fresh nonexistent private work root below repository out/.'}
[IO.Directory]::CreateDirectory($WorkRoot)|Out-Null
$producer=Join-Path $RepoRoot 'Tests/Tools/Audio/VerifyInstalledAudioProviderContract.ps1'
$tokens=$null;$errors=$null;$ast=[Management.Automation.Language.Parser]::ParseFile($producer,[ref]$tokens,[ref]$errors)
if(@($errors).Count){throw 'Actual audio provider producer must parse.'}
$script:cases=[Collections.Generic.List[object]]::new()
$script:pins=[Collections.Generic.List[object]]::new()
function Pin-Node($Node,[string]$Kind){
	$sha=[Security.Cryptography.SHA256]::Create()
	try{$hash=([BitConverter]::ToString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($Node.Extent.Text)))).Replace('-','')}finally{$sha.Dispose()}
	$script:pins.Add([ordered]@{source=$producer;sourceSha256=(Get-FileHash -LiteralPath $producer -Algorithm SHA256).Hash;kind=$Kind;astSha256=$hash;startLine=$Node.Extent.StartLineNumber;endLine=$Node.Extent.EndLineNumber})
}
function Select-ActualBlock([string]$Kind,[scriptblock]$Select){
	$nodes=@($ast.FindAll($Select,$true));if($nodes.Count-ne1){throw "Actual producer '$Kind' must occur once."}
	Pin-Node $nodes[0] $Kind
	return [scriptblock]::Create($nodes[0].Extent.Text)
}
$sourceAssignment=Select-ActualBlock 'source-path-assignment' {param($n)$n-is[Management.Automation.Language.AssignmentStatementAst]-and$n.Left-is[Management.Automation.Language.VariableExpressionAst]-and$n.Left.VariablePath.UserPath-eq'source'}
$sourceWrite=Select-ActualBlock 'actual-public-consumer-source-write' {param($n)
	$n-is[Management.Automation.Language.PipelineAst]-and @($n.PipelineElements|Where-Object{$_-is[Management.Automation.Language.CommandAst]-and$_.GetCommandName()-eq'Set-Content'}).Count-gt0
}
$platformNodes=@($ast.FindAll({param($n)$n-is[Management.Automation.Language.IfStatementAst]-and @($n.Clauses[0].Item1.FindAll({param($v)$v-is[Management.Automation.Language.VariableExpressionAst]-and$v.VariablePath.UserPath-eq'IsWindows'},$true)).Count-gt0},$true))
if($platformNodes.Count-ne1-or$null-eq$platformNodes[0].ElseClause){throw 'Actual Linux platform branch must be unique.'}
$linuxNode=$platformNodes[0].ElseClause;Pin-Node $linuxNode 'actual-linux-else-statements'
$linuxBlock=[scriptblock]::Create((@($linuxNode.Statements|ForEach-Object{$_.Extent.Text})-join"`n"))
$originalPathAssignment=Select-ActualBlock 'actual-original-path-assignment' {param($n)$n-is[Management.Automation.Language.AssignmentStatementAst]-and$n.Left-is[Management.Automation.Language.VariableExpressionAst]-and$n.Left.VariablePath.UserPath-eq'originalPath'}
$runtimeBlock=Select-ActualBlock 'actual-runtime-try-finally' {param($n)
	$n-is[Management.Automation.Language.TryStatementAst]-and @($n.Body.Statements|Where-Object{
		$_-is[Management.Automation.Language.PipelineAst]-and @($_.PipelineElements|Where-Object{$_-is[Management.Automation.Language.CommandAst]-and$_.CommandElements[0]-is[Management.Automation.Language.VariableExpressionAst]-and$_.CommandElements[0].VariablePath.UserPath-eq'consumer'}).Count-gt0
	}).Count-gt0
}
function Assert-Protocol([bool]$Value,[string]$Message){if(!$Value){throw $Message}}
function Run-Case([string]$Name,[scriptblock]$Body){
	try{&$Body;$script:cases.Add([ordered]@{name=$Name;status='passed';scope='actual AST with explicit protocol spy'});Write-Output "[PASS] $Name"}
	catch{$script:cases.Add([ordered]@{name=$Name;status='failed';error=$_.Exception.Message});Write-Output "[FAIL] $Name : $($_.Exception.Message)"}
}
function New-ProtocolFixture([string]$Name){
	$root=Join-Path $WorkRoot $Name;$include=Join-Path $root 'Installed Headers 日本語';$library=Join-Path $root 'Installed Runtime 日本語/libTerraEngine.so'
	[IO.Directory]::CreateDirectory($include)|Out-Null;[IO.Directory]::CreateDirectory((Split-Path -Parent $library))|Out-Null
	[IO.File]::WriteAllText($library,'PROTOCOL MARKER ONLY: NOT AN ELF LIBRARY')
	return [pscustomobject]@{root=$root;include=$include;library=$library;runtime=(Split-Path -Parent $library)}
}
function Invoke-ActualLinuxLink($fixture,[string]$LibraryPath){
	$temporary=$fixture.root;$IncludeRoot=$fixture.include;$RuntimeDirectory=$fixture.runtime
	$script:compilerCalls=0;$script:compilerArgs=@()
	# Only this local scope shadows c++ discovery. No global command, source or
	# installed tool is changed; the spy records argv and performs NO compilation.
	function Get-Command{param([string]$Name,[string]$ErrorAction)if($Name-ne'c++'){throw 'Only c++ discovery is admitted by this protocol spy.'};[pscustomobject]@{Source='Invoke-OwnedCompilerSpy'}}
	function Invoke-OwnedCompilerSpy{$script:compilerCalls++;$script:compilerArgs=@($args);$script:LASTEXITCODE=0}
	. $sourceAssignment
	. $sourceWrite
	. $linuxBlock
	return [pscustomobject]@{arguments=$script:compilerArgs;consumer=$consumer;source=$source;calls=$script:compilerCalls}
}
Run-Case 'AudioProvider.LinuxLinkPreservesExplicitSpacedUnicodeArguments' {
	$fixture=New-ProtocolFixture 'link';$observed=Invoke-ActualLinuxLink $fixture $fixture.library
	Assert-Protocol ($observed.calls-eq1-and$observed.arguments-contains$fixture.library-and$observed.arguments-contains$fixture.include-and$observed.arguments-contains$observed.source) 'Actual Linux compile command must preserve each explicit path as one argument.'
	Assert-Protocol ((Test-Path -LiteralPath $observed.source)-and$observed.arguments-contains'-std=c++20') 'Actual producer must write its public provider source and use its C++20 consumer command.'
}
Run-Case 'AudioProvider.MissingLibraryRejectsBeforeCompilerInvocation' {
	$fixture=New-ProtocolFixture 'missing';$rejected=$false
	try{$null=Invoke-ActualLinuxLink $fixture (Join-Path $fixture.root 'missing.so')}catch{if($_.Exception.Message-notlike'*shared library was not found*'){throw};$rejected=$true}
	Assert-Protocol ($rejected-and$script:compilerCalls-eq0) 'Actual missing-library prerequisite must fail before any compiler invocation.'
}
foreach($fails in @($false,$true)){
	Run-Case "AudioProvider.RuntimeEnvironmentRestored.failure=$fails" {
		$fixture=New-ProtocolFixture "runtime-$fails";$RuntimeDirectory=$fixture.runtime;$before=$env:PATH
		$beforeLd=$env:LD_LIBRARY_PATH;$beforePreload=$env:LD_PRELOAD;$script:runtimeObservation=$null
		function Invoke-OwnedRuntimeSpy{
			# Observation of the calling process environment only; no ELF loader or
			# runtime success is fabricated. A real child would inherit these values.
			$script:runtimeObservation=[ordered]@{path=$env:PATH;ldLibraryPath=$env:LD_LIBRARY_PATH;ldPreload=$env:LD_PRELOAD}
			if($fails){throw 'Owned runtime protocol refusal'}
		}
		$consumer='Invoke-OwnedRuntimeSpy';$rejected=$false
		. $originalPathAssignment
		try{. $runtimeBlock}catch{if($_.Exception.Message-ne'Owned runtime protocol refusal'){throw};$rejected=$true}
		Assert-Protocol ($rejected-eq$fails-and$env:PATH-ceq$before) 'Actual finally must restore PATH after both normal and exceptional invocation.'
		Assert-Protocol ($script:runtimeObservation.path.StartsWith($RuntimeDirectory+[IO.Path]::PathSeparator)) 'Actual producer runtime path-prefix operation must execute before the spy.'
		Assert-Protocol ($env:LD_LIBRARY_PATH-ceq$beforeLd-and$env:LD_PRELOAD-ceq$beforePreload) 'Protocol observation must not alter inherited Linux loader state.'
	}
}
# Real dependency retention is a prerequisite, never guessed from a library
# argument or marker file. Optional inspection executes readelf only, not code.
$retention=$null;$nativeObservation=$null
if(!$ExistingLinuxConsumer){
	$script:cases.Add([ordered]@{name='AudioProvider.ActualElfDependencyRetention';status='unavailable';reason='No preserved actual Linux consumer artifact supplied; no ELF retention claim.'})
}else{
	try{
		if($IsWindows-or$env:OS-eq'Windows_NT'){
			if(!$WslDistribution){throw 'Explicit existing WSL distribution is required for read-only ELF inspection on Windows.'}
			$sizeText=(& wsl.exe --distribution $WslDistribution --exec /usr/bin/stat --format=%s -- $ExistingLinuxConsumer 2>&1|Out-String).Trim();if($LASTEXITCODE-ne0){throw 'Actual WSL consumer artifact is unavailable.'}
			if([UInt64]$sizeText-gt67108864){throw 'ELF inspection is bounded to64MiB.'}
			$hashText=(& wsl.exe --distribution $WslDistribution --exec /usr/bin/sha256sum -- $ExistingLinuxConsumer 2>&1|Out-String).Trim();if($LASTEXITCODE-ne0){throw 'Actual consumer SHA256 inspection failed.'}
			$dynamic=(& wsl.exe --distribution $WslDistribution --exec /usr/bin/readelf --dynamic -- $ExistingLinuxConsumer 2>&1|Out-String);if($LASTEXITCODE-ne0){throw 'Actual consumer ELF dynamic-table inspection failed.'}
		}else{
			if(!(Test-Path -LiteralPath $ExistingLinuxConsumer -PathType Leaf)){throw 'Actual Linux consumer artifact is unavailable.'}
			if((Get-Item -LiteralPath $ExistingLinuxConsumer).Length-gt67108864){throw 'ELF inspection is bounded to64MiB.'}
			$hashText=(Get-FileHash -LiteralPath $ExistingLinuxConsumer -Algorithm SHA256).Hash
			$tool=Microsoft.PowerShell.Core\Get-Command readelf -ErrorAction Stop
			$dynamic=(&$tool.Source --dynamic -- $ExistingLinuxConsumer 2>&1|Out-String);if($LASTEXITCODE-ne0){throw 'Actual consumer ELF dynamic-table inspection failed.'}
		}
		[IO.File]::WriteAllText((Join-Path $WorkRoot 'actual-elf-dynamic.txt'),$dynamic)
		$needed=@([regex]::Matches($dynamic,'\(NEEDED\).*?\[(?<name>[^\]]+)\]')|ForEach-Object{$_.Groups['name'].Value})
		$retention=@($needed|Where-Object{$_-match'^libTerraEngine[.]so([.][0-9.]+)?$'}).Count-gt0
		$nativeObservation=[ordered]@{consumer=$ExistingLinuxConsumer;sha256Observation=$hashText;needed=$needed;engineDependencyRetained=$retention;consumerExecuted=$false;freshBuildProvenanceVerified=$false}
		$script:cases.Add([ordered]@{name='AudioProvider.ActualElfDependencyRetention';status='passed';scope='actual read-only ELF artifact classification, not link/run qualification';observation=$nativeObservation})
	}catch{$script:cases.Add([ordered]@{name='AudioProvider.ActualElfDependencyRetention';status='unavailable';reason=$_.Exception.Message})}
}
if($null-ne$retention-and-not$retention){
	$script:cases.Add([ordered]@{name='AudioProvider.RetainedDependencyCleanLinuxLinkRun';status='not-applicable';reason='Actual ELF does not retain engine dependency; no loader repair inferred. Fresh native consumer build/run remains separate parent evidence.'})
}else{
	$script:cases.Add([ordered]@{name='AudioProvider.RetainedDependencyCleanLinuxLinkRun';status='unavailable';reason='Fresh real installed-consumer compile/link/run with controlled loader environment was not executed. Protocol spies cannot establish this proof.'})
}
$failures=@($script:cases|Where-Object{$_.status-eq'failed'}).Count
$unavailable=@($script:cases|Where-Object{$_.status-eq'unavailable'}).Count
$report=[ordered]@{protocolOnly=[bool]$ProtocolOnly;nativeQualificationComplete=$false;nativeCompilationExecuted=$false;nativeConsumerExecuted=$false;selected=$script:cases.Count;protocolExecuted=4;failures=$failures;unavailable=$unavailable;cases=@($script:cases.ToArray());sourcePins=@($script:pins.ToArray());nativeArtifactObservation=$nativeObservation}
$report|ConvertTo-Json -Depth 32|Set-Content -LiteralPath (Join-Path $WorkRoot 'result.json') -Encoding utf8
Write-Output "4 protocol cases executed; $failures failures; $unavailable native conditions unavailable. No Linux consumer execution proof."
if($failures){exit 1}
if(!$ProtocolOnly){exit 2}
