<#
Run structural-validator contracts without compiling or loading engine/Vulkan code.
Requires PowerShell 7 and the bundled native SourceReviewAudit executable.
All generated source, manifests, output receipts and child logs are retained in a
fresh WorkRoot. Omit WorkRoot to generate a unique temporary directory.

pwsh -NoProfile -File TestStageOwnership.ps1 -AuditExecutable <absolute-audit-exe> `
	-ValidatorScript <absolute-VerifyStageOwnership.ps1> [-WorkRoot <fresh-directory>]

The fixture names model the expected stage closure. Native lexical spans are
measured, never guessed. Passing these contracts proves validator behavior on
synthetic inputs; it confers no runtime, semantic lifetime or replay acceptance.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$AuditExecutable,
	[Parameter(Mandatory)][string]$ValidatorScript,
	[string]$WorkRoot = ''
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Require([bool]$Condition, [string]$Message) { if(!$Condition) { throw $Message } }
function Pin([string]$Path) {
	return @{path=[IO.Path]::GetFullPath($Path);sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant();bytes=(Get-Item -LiteralPath $Path).Length}
}
function WriteJson([string]$Path, $Value) {
	$stream = [IO.File]::Open($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::None)
	try {
		$bytes = [Text.Encoding]::UTF8.GetBytes(($Value | ConvertTo-Json -Depth 40))
		$stream.Write($bytes,0,$bytes.Length)
	} finally { $stream.Dispose() }
}
function CopyManifest { return ($script:baseline | ConvertTo-Json -Depth 40) | ConvertFrom-Json -AsHashtable -Depth 40 }
function InvokeValidator([string]$Name, [string]$Manifest, [string]$Output, [string]$Tool) {
	# Launch only this PowerShell validator and the lexical audit it invokes.
	& $script:pwsh -NoProfile -File $script:validator -RepoRoot $script:repo -StageManifest $Manifest `
		-AuditExecutable $Tool -OutputPath $Output 1> (Join-Path $script:work "$Name.stdout.log") 2> (Join-Path $script:work "$Name.stderr.log")
	return $LASTEXITCODE
}

$summary = [ordered]@{schemaVersion='stage-ownership-contract-tests-v1';status='Failed';syntheticOnly=$true;
	engineProcessesLaunched=0;minimumTests=26;selectedTests=0;failures=0;cases=[Collections.Generic.List[object]]::new()}
$exitCode = 1
$work = $null
$ownsWork = $false
try {
	foreach($path in @($AuditExecutable,$ValidatorScript)) {
		Require ([IO.Path]::IsPathRooted($path) -and [IO.File]::Exists($path)) "Expected existing absolute tool path: $path"
	}
	$tool = [IO.Path]::GetFullPath($AuditExecutable)
	$script:validator = [IO.Path]::GetFullPath($ValidatorScript)
	$script:pwsh = (Get-Process -Id $PID).Path
	Require ($PSVersionTable.PSVersion.Major -ge 7) 'PowerShell 7 is required.'
	$script:work = if($WorkRoot) {
		Require ([IO.Path]::IsPathRooted($WorkRoot)) 'WorkRoot must be absolute.'
		[IO.Path]::GetFullPath($WorkRoot)
	} else { Join-Path ([IO.Path]::GetTempPath()) ('TerraEngine-StageOwnership-'+[guid]::NewGuid().ToString('N')) }
	Require (!(Test-Path -LiteralPath $script:work)) 'WorkRoot must be fresh.'
	$null = [IO.Directory]::CreateDirectory($script:work)
	$ownsWork = $true
	$script:repo = Join-Path $script:work 'repo'
	$summary.auditExecutable=Pin $tool; $summary.validatorScript=Pin $script:validator
	$summary.workRoot=$script:work
	$definitions = @(
		@{path='Engine/Renderer/RenderSystem.cpp';text='struct RenderSystem { static void BuildFrameExecution() {} };';name='RenderSystem::BuildFrameExecution';limit=500},
		@{path='Engine/Renderer/Frame/Helper.h';text='namespace Fixture { inline void helper() {} }';name='Fixture::helper';limit=400},
		@{path='Engine/Animation/AnimationSystem.cpp';text='namespace TerraEngine::Animation { struct AnimationSystem { static void DeclareFrameWorkloads() {} }; }';name='TerraEngine::Animation::AnimationSystem::DeclareFrameWorkloads';limit=500},
		@{path='Engine/Animation/Frame/Helper.h';text='namespace Fixture { inline void helper() {} }';name='Fixture::helper';limit=400}
	)
	$sources=@(); $symbols=@(); $nonCallables=@()
	foreach($definition in $definitions) {
		$path = Join-Path $script:repo $definition.path
		$null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path))
		[IO.File]::WriteAllText($path,$definition.text)
		$hash = (Pin $path).sha256
		$nativeText = & $tool --inventory $path
		Require ($LASTEXITCODE -eq 0) "Fixture inventory failed: $path"
		$native = ($nativeText -join "`n") | ConvertFrom-Json -AsHashtable -Depth 40
		Require ($native.errorCount -eq 0 -and $native.files.Count -eq 1) 'Fixture native inventory must be valid.'
		$spans = $native.files[0].braceSpans
		# Every fixture has one function as the innermost/final opening brace.
		$body = $spans[-1]
		$source = @{path=$definition.path;sourceSha256=$hash}
		if($definition.path.EndsWith('System.cpp')) { $source.maxCodeLines=2000 }
		$sources += $source
		$symbols += @{path=$definition.path;qualifiedSymbol=$definition.name;sourceSha256=$hash;signatureLine=1;
			openingBraceOffset=$body.openingBraceOffset;closingBraceOffset=$body.spanEndOffsetExclusive-1;
			spanEndOffsetExclusive=$body.spanEndOffsetExclusive;firstLine=$body.firstLine;lastLine=$body.lastLine;
			codeLines=$body.codeLines;maxCodeLines=$definition.limit;symbolId=$definition.path+':'+$body.openingBraceOffset;ownershipContractIds=@('synchronous')}
		foreach($span in $spans | Select-Object -SkipLast 1) {
			$nonCallables += @{path=$definition.path;openingBraceOffset=$span.openingBraceOffset;
				spanEndOffsetExclusive=$span.spanEndOffsetExclusive;classification='Fixture namespace or class scope'}
		}
	}
	$script:baseline = @{baselineFact='Preserve this unrelated baseline fact';candidate=@{sources=$sources;symbols=$symbols;nonCallableBraces=$nonCallables;
		ownershipContracts=@(@{id='synchronous';owner='The fixture caller owns synchronous inputs';
			lastConsumer='The local function finishes before input retirement';settlement=@('No child work is admitted','Synchronous return settles the borrow')})}}
	$basePath = Join-Path $script:work 'baseline-manifest.json'; WriteJson $basePath $script:baseline
	$cases = @('valid','stale-hash','wrong-opening-offset','missing-source','duplicate-callable','missing-noncallable',
		'duplicate-noncallable','unknown-contract','unused-contract','empty-description','wrong-helper-limit','wrong-native-count',
		'unresolved-name','escaping-path','missing-candidate','duplicate-json-field')
	$names = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
	foreach($name in $cases) {
		$null = $names.Add($name)
		$document = CopyManifest
		switch($name) {
			'stale-hash' { $document.candidate.sources[0].sourceSha256='0'*64 }
			'wrong-opening-offset' { $document.candidate.symbols[0].openingBraceOffset++ }
			'missing-source' { $document.candidate.sources=@($document.candidate.sources | Select-Object -Skip 1) }
			'duplicate-callable' { $document.candidate.symbols+=,$document.candidate.symbols[0] }
			'missing-noncallable' { $document.candidate.nonCallableBraces=@($document.candidate.nonCallableBraces | Select-Object -Skip 1) }
			'duplicate-noncallable' { $document.candidate.nonCallableBraces+=,$document.candidate.nonCallableBraces[0] }
			'unknown-contract' { $document.candidate.symbols[0].ownershipContractIds=@('absent') }
			'unused-contract' { $document.candidate.ownershipContracts+=@{id='unused';owner='Caller owns input';lastConsumer='Synchronous local borrower';settlement='Return settles the input borrow'} }
			'empty-description' { $document.candidate.ownershipContracts[0].owner=@() }
			'wrong-helper-limit' { $document.candidate.symbols[1].maxCodeLines=500 }
			'wrong-native-count' { $document.candidate.symbols[0].codeLines++ }
			'unresolved-name' { $document.candidate.symbols[0].qualifiedSymbol='Unknown::?' }
			'escaping-path' { $document.candidate.sources[0].path='../escape.cpp' }
			'missing-candidate' { $document.Remove('candidate') }
		}
		$manifestPath=Join-Path $script:work "$name-manifest.json"; $output=Join-Path $script:work "$name-result.json"
		if($name -eq 'duplicate-json-field') {
			$json = ($document | ConvertTo-Json -Depth 40).Replace('"baselineFact":','"baselineFact":"duplicate","baselineFact":')
			[IO.File]::WriteAllText($manifestPath,$json)
		} else { WriteJson $manifestPath $document }
		$code = InvokeValidator $name $manifestPath $output $tool
		$receipt = [IO.File]::ReadAllText($output) | ConvertFrom-Json -AsHashtable -Depth 40
		$expectedExit=if($name -eq 'valid'){0}else{1}; $expectedStatus=if($name -eq 'valid'){'Passed'}else{'Failed'}
		$passed=$code -eq $expectedExit -and $receipt.status -ceq $expectedStatus -and $receipt.structuralOnly -eq $true -and $receipt.semanticCompletenessProven -eq $false
		$summary.cases.Add(@{name=$name;passed=$passed;expectedExit=$expectedExit;actualExit=$code;expectedStatus=$expectedStatus;observedStatus=$receipt.status;receipt=Pin $output})
	}
	$source=Join-Path $script:repo 'Engine/Renderer/Frame/Helper.h'; $original=[IO.File]::ReadAllBytes($source)
	try {
		foreach($extension in @('cpp','inl','inc','ipp','tpp','cxx','cc','malformed')) {
			$name=if($extension -eq 'malformed'){'malformed-native-source'}else{"include-$extension"}
			Require ($names.Add($name)) "Duplicate case name: $name"
			$text=if($extension -eq 'malformed'){'void unterminated() {'}else{'#include "Hidden.'+$extension+'"'+"`n"+[Text.Encoding]::UTF8.GetString($original)}
			[IO.File]::WriteAllText($source,$text)
			$document=CopyManifest
			($document.candidate.sources | Where-Object path -CEQ 'Engine/Renderer/Frame/Helper.h').sourceSha256=(Pin $source).sha256
			$manifestPath=Join-Path $script:work "$name-manifest.json"; WriteJson $manifestPath $document
			$output=Join-Path $script:work "$name-result.json"; $code=InvokeValidator $name $manifestPath $output $tool
			$receipt=[IO.File]::ReadAllText($output) | ConvertFrom-Json -AsHashtable -Depth 40
			$prefix=if($extension -eq 'malformed'){'Native inventory failed'}else{'Included implementation fragment is forbidden:'}
			$summary.cases.Add(@{name=$name;passed=($code -eq 1 -and $receipt.status -ceq 'Failed' -and $receipt.error.StartsWith($prefix));
				expectedExit=1;actualExit=$code;expectedStatus='Failed';observedStatus=$receipt.status;receipt=Pin $output})
		}
	} finally { [IO.File]::WriteAllBytes($source,$original) }
	$name='existing-output-preserved'; Require ($names.Add($name)) 'Duplicate output-preservation case.'
	$output=Join-Path $script:work 'valid-result.json'; $before=Pin $output
	$code=InvokeValidator $name $basePath $output $tool
	$after=Pin $output
	$summary.cases.Add(@{name=$name;passed=($code -eq 1 -and $before.sha256 -ceq $after.sha256 -and $before.bytes -eq $after.bytes);expectedExit=1;actualExit=$code;receipt=$after})
	$name='output-created-after-initial-check'; Require ($names.Add($name)) 'Duplicate output-race case.'
	$output=Join-Path $script:work 'competing-receipt.json'; $wrapper=Join-Path $script:work 'race-audit.ps1'
	# This synthetic tool wrapper creates a competing receipt during the first
	# native invocation, after the validator's initial freshness check has passed.
	$quotedOutput=$output.Replace("'","''"); $quotedTool=$tool.Replace("'","''")
	$wrapperText="if(!(Test-Path -LiteralPath '$quotedOutput')) { [IO.File]::WriteAllText('$quotedOutput','COMPETING-RECEIPT') }`n& '$quotedTool' @args`nexit `$LASTEXITCODE`n"
	[IO.File]::WriteAllText($wrapper,$wrapperText)
	$code=InvokeValidator $name $basePath $output $wrapper
	$summary.cases.Add(@{name=$name;passed=($code -eq 1 -and [IO.File]::ReadAllText($output) -ceq 'COMPETING-RECEIPT');expectedExit=1;actualExit=$code;receipt=Pin $output})
	$summary.selectedTests=$summary.cases.Count
	$summary.failures=@($summary.cases | Where-Object { !$_.passed }).Count
	Require ($names.Count -eq $summary.selectedTests -and $summary.selectedTests -ge $summary.minimumTests) 'Contract selection below the26-case floor or names duplicated.'
	Require ($summary.failures -eq 0) "$($summary.failures) structural validator contracts failed."
	Require ((Pin $tool).sha256 -ceq $summary.auditExecutable.sha256 -and (Pin $script:validator).sha256 -ceq $summary.validatorScript.sha256) 'Tool bytes changed during contracts.'
	$summary.status='Passed'; $exitCode=0
} catch {
	$summary.error=$_.Exception.Message
	$summary.selectedTests=$summary.cases.Count
	$summary.failures=[Math]::Max(1,@($summary.cases | Where-Object { !$_.passed }).Count)
} finally {
	if($ownsWork) {
		WriteJson (Join-Path $work 'result.json') $summary
	}
}
Write-Output "Stage ownership contracts: $($summary.status); $($summary.selectedTests) selected; $($summary.failures) failures; evidence: $work"
exit $exitCode
