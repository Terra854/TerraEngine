<#
.SYNOPSIS
Checks installed renderer-efficiency tool contracts without launching the editor.
.DESCRIPTION
Prerequisite: a freshly built, installed and sealed bundle. Example:
pwsh -File Build/Tests/Tools/RendererEfficiency/Test-RendererEfficiencyFoundation.ps1 -InstallRoot Build -WorkRoot out/foundation-contracts
WorkRoot must not exist and must be outside InstallRoot. Cases preserve all logs
and owned temporary inputs. Success verifies tool contracts only, never GPU work.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string]$InstallRoot,
	[Parameter(Mandatory=$true)][string]$WorkRoot
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
# Desktop hosts may inherit another PowerShell edition's module search path.
# Resolve hashing/JSON cmdlets from this process's own shipped utility module;
# leave the caller's environment and unrelated module selection unchanged.
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -Force
$utf8=[Text.UTF8Encoding]::new($false)
$results=[Collections.Generic.List[object]]::new()
$ownsWork=$false
$powershellExecutable=(Get-Process -Id $PID).Path
function WriteJson([string]$Path,$Value) { [IO.File]::WriteAllText($Path,($Value|ConvertTo-Json -Depth 100),$utf8) }
function Assert([bool]$Condition,[string]$Message) { if(!$Condition){throw $Message} }
function NoRedirect([string]$Path) {
	$current=[IO.Path]::GetFullPath($Path)
	while($current) {
		if(Test-Path -LiteralPath $current) { Assert (((Get-Item -LiteralPath $current).Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) 'Redirected validation root is not allowed' }
		$next=Split-Path $current;if($next -eq $current){break};$current=$next
	}
}
function InvokeCase([string]$Name,[string]$Script,[string[]]$Arguments,[int]$Expected) {
	# Child PowerShell isolates scripts which intentionally use exit. All invoked
	# renderer runners carry DryRun; comparer cases have no editor launch surface.
	$log=Join-Path $work ($Name+'.log')
	$lines=& $powershellExecutable -NoLogo -NoProfile -File $Script @Arguments 2>&1
	$code=$LASTEXITCODE
	[IO.File]::WriteAllText($log,($lines|Out-String),$utf8)
	$results.Add([ordered]@{name=$Name;expectedExitCode=$Expected;actualExitCode=$code;log=$log})
	Assert ($code -eq $Expected) "$Name returned $code instead of $Expected"
}
function RunnerArguments([string]$Root,[string]$CaseWork,[string]$Output) {
	return @('-InstallRoot',$Root,'-WorkRoot',$CaseWork,'-OutputDirectory',$Output,'-Stage','P1','-DryRun')
}
function CheckInstalledRedirectHelper([string]$Runner,[string]$OwnedWork) {
	# Extract exactly the installed helper, never dot-source or launch the runner.
	# This keeps the test coupled to production behavior without its top-level IO.
	$tokens=$null;$parseErrors=$null
	$ast=[Management.Automation.Language.Parser]::ParseFile($Runner,[ref]$tokens,[ref]$parseErrors)
	$definitions=@($ast.FindAll({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'NoRedirect'},$false))
	Assert ($parseErrors.Count -eq 0 -and $definitions.Count -eq 1) 'Installed redirect helper extraction failed'
	$invoke=[scriptblock]::Create('param([string]$CandidatePath)'+[Environment]::NewLine+$definitions[0].Extent.Text+[Environment]::NewLine+'NoRedirect $CandidatePath')
	$sandbox=Join-Path $OwnedWork 'redirect-helper-cases'
	$target=Join-Path $sandbox 'target';$junction=Join-Path $sandbox 'junction';$file=Join-Path $target 'ordinary.txt'
	Assert (!(Test-Path -LiteralPath $sandbox)) 'Redirect helper sandbox must be fresh'
	[IO.Directory]::CreateDirectory($target)|Out-Null
	[IO.File]::WriteAllText($file,'Owned redirect contract input.',$utf8)
	try {
		# A real junction is required: mocked attributes cannot prove ancestor walking.
		New-Item -ItemType Junction -Path $junction -Target $target|Out-Null
		$cases=@(
			@{name='ordinary-file';path=$file;expected='Allowed'},
			@{name='ordinary-directory';path=$target;expected='Allowed'},
			@{name='missing-leaf';path=(Join-Path $target 'missing/child');expected='Allowed'},
			@{name='root';path=[IO.Path]::GetPathRoot($file);expected='Allowed'},
			@{name='empty';path='';expected='Rejected'},
			@{name='actual-junction';path=$junction;expected='Rejected'},
			@{name='missing-below-junction';path=(Join-Path $junction 'missing/child');expected='Rejected'}
		)
		foreach($case in $cases) {
			$actual='Allowed';try { & $invoke $case.path } catch { $actual='Rejected' }
			$results.Add([ordered]@{name=('redirect-helper-'+$case.name);kind='InstalledHelperBehavior';expectedResult=$case.expected;actualResult=$actual})
			Assert ($actual -ceq $case.expected) ('Redirect helper case failed: '+$case.name)
		}
	} finally {
		# Delete only this exact owned link, non-recursively; never traverse its target.
		# The target/file remain as bounded test inputs inside the caller-owned WorkRoot.
		if(Test-Path -LiteralPath $junction) { [IO.Directory]::Delete($junction,$false) }
	}
}
function NewUnavailableValue($Rule) {
	# This synthetic document follows the actual installed schema and explicitly
	# contains no frames or usable identities. It can only test failure handling.
	if($Rule.PSObject.Properties['$ref']) { return NewUnavailableValue $script:schema.'$defs'.($Rule.'$ref'.Split('/')[-1]) }
	if($Rule.PSObject.Properties['const']) {return $Rule.const}
	if($Rule.PSObject.Properties['enum']) {
		if($Rule.enum -contains 'Unavailable'){return 'Unavailable'}
		if($Rule.enum -contains 'Unverified'){return 'Unverified'}
		return $Rule.enum[0]
	}
	if($Rule.PSObject.Properties['anyOf']) {if(@($Rule.anyOf|Where-Object {$_.PSObject.Properties['type'] -and $_.type -eq 'null'}).Count){return $null};return NewUnavailableValue $Rule.anyOf[0]}
	if(!$Rule.PSObject.Properties['type'] -and $Rule.PSObject.Properties['allOf']) {return NewUnavailableValue $Rule.allOf[0]}
	if($Rule.type -eq 'object') {
		$value=[ordered]@{};foreach($property in $Rule.properties.PSObject.Properties){$value[$property.Name]=NewUnavailableValue $property.Value}
		if($value.Contains('reason')){$value.reason='NotAvailable'}
		return $value
	}
	if($Rule.type -eq 'array'){return ,@()}
	if($Rule.type -in @('number','integer')){if($Rule.PSObject.Properties['minimum']){return $Rule.minimum};return 0}
	if($Rule.type -eq 'boolean'){return $false}
	if($Rule.type -eq 'string'){return 'SyntheticUnavailable'}
	return $null
}
try {
	$install=[IO.Path]::GetFullPath($InstallRoot).TrimEnd('\','/')
	$work=[IO.Path]::GetFullPath($WorkRoot).TrimEnd('\','/')
	NoRedirect $install;NoRedirect $work
	Assert (!(Test-Path -LiteralPath $work)) 'Validation WorkRoot must be fresh'
	Assert ($work -ine $install -and !$work.StartsWith($install+'\',[StringComparison]::OrdinalIgnoreCase) -and !$install.StartsWith($work+'\',[StringComparison]::OrdinalIgnoreCase)) 'Validation WorkRoot and InstallRoot overlap'
	$runner=Join-Path $install 'Tests/Tools/RendererEfficiency/Invoke-RendererEfficiencyValidation.ps1'
	$comparer=Join-Path $install 'Tests/Tools/RendererEfficiency/Compare-RendererEfficiencyEvidence.ps1'
	$schemaPath=Join-Path $install 'Tests/Tools/RendererEfficiency/Schemas/evidence.schema.json'
	$manifest=Join-Path $install 'Tests/Fixtures/RendererEfficiency/workloads.json'
	$sealPath=Join-Path $install 'Tests/Tools/RendererEfficiency/renderer-efficiency-artifacts.json'
	foreach($path in @($runner,$comparer,$schemaPath,$manifest,$sealPath)){Assert (Test-Path -LiteralPath $path -PathType Leaf) "Installed contract prerequisite missing: $path"}
	# Snapshot every sealed input before the test; the maintained bundle is never
	# modified. Tampering uses one copied script and one owned sentinel instead.
	$seal=Get-Content -LiteralPath $sealPath -Raw|ConvertFrom-Json
	$before=@{};foreach($file in $seal.artifacts){$path=Join-Path $install $file.path;$before[$path]=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash}
	$before[$sealPath]=(Get-FileHash -LiteralPath $sealPath -Algorithm SHA256).Hash
	[IO.Directory]::CreateDirectory($work)|Out-Null
	$ownsWork=$true
	CheckInstalledRedirectHelper $runner $work
	$dryWork=Join-Path $work 'dry-work';$dryOutput=Join-Path $work 'dry-output'
	InvokeCase 'installed-dryrun' $runner (RunnerArguments $install $dryWork $dryOutput) 0
	Assert (!(Test-Path -LiteralPath $dryWork) -and !(Test-Path -LiteralPath $dryOutput)) 'DryRun wrote disposable project/output paths'
	# Authored CPU authority preserves both visible animated casters. This checks
	# generated inputs only; imported CPU output and GPU pixels need live evidence.
	$materializer=Join-Path $install 'Tests/Tools/RendererEfficiency/TerraEngine-RendererEfficiencyFixtureMaterializer.exe'
	$cpuFixtureWork=Join-Path $work 'cpu-authority-fixture'
	$cpuFixtureLog=Join-Path $work 'cpu-authority-fixture.log'
	$materialized=& $materializer --install-root $install --work-root $cpuFixtureWork --fixture shadow-invalidation 2>&1
	$materializedCode=$LASTEXITCODE
	[IO.File]::WriteAllText($cpuFixtureLog,($materialized|Out-String),$utf8)
	Assert ($materializedCode -eq 0) 'CPU-authority fixture materialization failed'
	$cpuProject=Join-Path $cpuFixtureWork 'project'
	$cpuScene=Get-Content (Join-Path $cpuProject 'Assets/Scenes/Scene.tescene') -Raw|ConvertFrom-Json
	$cpuSteps=Get-Content (Join-Path $cpuProject 'renderer-efficiency-steps.json') -Raw|ConvertFrom-Json
	$cpuMarker=Get-Content (Join-Path $cpuProject 'renderer-efficiency-project.json') -Raw|ConvertFrom-Json
	$animatedRoots=@($cpuScene.objects|Where-Object {$_.components.PSObject.Properties['AnimatorComponent']})
	Assert ($animatedRoots.Count -eq 2) 'The frozen skeletal and morph caster roots must both remain authored'
	foreach($root in $animatedRoots) {
		Assert ($root.active -eq $true -and @($root.children).Count -eq 1 -and $root.children[0].active -eq $true) 'CPU authority must not deactivate or exclude either caster'
		Assert ($root.components.AnimatorComponent.requestedMode -eq 3 -and $root.components.AnimatorComponent.playAutomatically -eq $false) 'Both roots must explicitly request the durable CPU authority policy'
	}
	Assert ($cpuSteps.animationAuthoritySetup.requestedMode -eq 3 -and $cpuSteps.animationAuthoritySetup.policy -eq 'ForceCpuDeterministicFallback' -and $cpuSteps.animationAuthoritySetup.appliesTo -eq 'AllP1Steps') 'Generated setup must disclose CPU authority for both configurations and all P1 steps'
	Assert (@($cpuSteps.steps).Count -eq 17 -and @($cpuSteps.assets).Count -eq 6 -and $cpuSteps.resetBaselineBeforeStep -eq $true) 'CPU authority must preserve the existing steps, asset closure and baseline reset'
	foreach($action in @('pose','morph')) {
		$seek=@($cpuSteps.steps|Where-Object {$_.action -eq $action})
		Assert ($seek.Count -eq 1 -and $seek[0].nativePayload.seekTick -eq 30 -and $seek[0].warmupFrames -eq 60 -and $seek[0].measuredFrames -eq 300) 'Actual pose/morph seek and measurement bounds must remain unchanged'
	}
	Assert ($cpuSteps.fixtureHash -eq (Get-FileHash $manifest -Algorithm SHA256).Hash.ToLowerInvariant() -and $cpuMarker.stepsHash -eq (Get-FileHash (Join-Path $cpuProject 'renderer-efficiency-steps.json') -Algorithm SHA256).Hash.ToLowerInvariant()) 'Generated policy must be sealed without rewriting the frozen manifest'
	$results.Add([ordered]@{name='authored-cpu-authority-fixture';expectedExitCode=0;actualExitCode=0;log=$cpuFixtureLog})
	InvokeCase 'invalid-install' $runner (RunnerArguments (Join-Path $work 'missing-install') (Join-Path $work 'invalid-work') (Join-Path $work 'invalid-output')) 3
	$reused=Join-Path $work 'reused-work';[IO.Directory]::CreateDirectory($reused)|Out-Null
	InvokeCase 'reused-work' $runner (RunnerArguments $install $reused (Join-Path $work 'reused-output')) 3
	$overlap=Join-Path $work 'overlap-work'
	InvokeCase 'overlapping-roots' $runner (RunnerArguments $install $overlap (Join-Path $overlap 'output')) 3
	InvokeCase 'installed-root-as-work' $runner (RunnerArguments $install $install (Join-Path $work 'inside-output')) 3
	$sourceRunner=Join-Path $work 'checkout-like-runner.ps1';Copy-Item -LiteralPath $runner -Destination $sourceRunner
	InvokeCase 'source-runner-rejected' $sourceRunner (RunnerArguments $install (Join-Path $work 'source-work') (Join-Path $work 'source-output')) 3
	# The isolated seal declares exactly one owned file. Mutate its bytes after
	# recording its hash, then require the inventory check to diagnose stale input.
	$fake=Join-Path $work 'seal-sandbox';$fakeTools=Join-Path $fake 'Tests/Tools/RendererEfficiency'
	[IO.Directory]::CreateDirectory($fakeTools)|Out-Null
	$fakeRunner=Join-Path $fakeTools 'Invoke-RendererEfficiencyValidation.ps1';Copy-Item -LiteralPath $runner -Destination $fakeRunner
	$sentinel=Join-Path $fake 'sentinel.txt';[IO.File]::WriteAllText($sentinel,'sealed',$utf8)
	$fakeSeal=@{schemaVersion=1;revision='synthetic-contract-input';buildConfiguration='RelWithDebInfo';sourceDirty=$true;artifacts=@(@{path='sentinel.txt';bytes=(Get-Item $sentinel).Length;sha256=(Get-FileHash $sentinel -Algorithm SHA256).Hash})}
	WriteJson (Join-Path $fakeTools 'renderer-efficiency-artifacts.json') $fakeSeal
	[IO.File]::WriteAllText($sentinel,'tamper',$utf8)
	InvokeCase 'tampered-isolated-seal' $fakeRunner (RunnerArguments $fake (Join-Path $work 'tamper-work') (Join-Path $work 'tamper-output')) 3
	Assert ((Get-Content -Raw (Join-Path $work 'tampered-isolated-seal.log')) -match 'Stale installed seal entry') 'Seal test did not reach stale input validation'
	InvokeCase 'absent-evidence' $comparer @('-Reference',(Join-Path $work 'absent-reference.json'),'-Candidate',(Join-Path $work 'absent-candidate.json'),'-Stage','P1','-Output',(Join-Path $work 'absent-summary.json')) 3
	$script:schema=Get-Content -Raw -LiteralPath $schemaPath|ConvertFrom-Json
	$referencePaths=@();$candidatePaths=@()
	foreach($mode in @('Reference','Candidate')) {for($run=0;$run -lt 3;$run++) {
		$document=NewUnavailableValue $script:schema
		$document.identity.mode=$mode;$document.identity.stage='P1';$document.identity.fixtureId='shadow-invalidation';$document.identity.seed=65001;$document.identity.runIndex=$run
		$path=Join-Path $work ($mode+'-unavailable-'+$run+'.json');WriteJson $path $document
		# Windows PowerShell lacks Test-Json; the installed comparer also validates
		# every document against its bundled interpreter before any acceptance gate.
		if(Get-Command Test-Json -ErrorAction SilentlyContinue) {
			Assert (Test-Json -Json (Get-Content -LiteralPath $path -Raw) -SchemaFile $schemaPath) 'Synthetic unavailable evidence is not schema-valid'
		}
		if($mode -eq 'Reference'){$referencePaths+=$path}else{$candidatePaths+=$path}
	}}
	$caseScript=Join-Path $work 'compare-unavailable.ps1'
	# Structured JSON stores arrays rather than risking argument-list quoting.
	$config=Join-Path $work 'unavailable-inputs.json';WriteJson $config @{comparer=$comparer;reference=$referencePaths;candidate=$candidatePaths;output=(Join-Path $work 'unavailable-summary.json')}
	$unavailableInvocation=@'
param([string]$Configuration)
$value=Get-Content -Raw -LiteralPath $Configuration|ConvertFrom-Json
& $value.comparer -Reference $value.reference -Candidate $value.candidate -Stage P1 -Output $value.output
exit $LASTEXITCODE
'@
	[IO.File]::WriteAllText($caseScript,$unavailableInvocation,$utf8)
	InvokeCase 'explicit-unavailable-evidence' $caseScript @('-Configuration',$config) 2
	foreach($path in $before.Keys){Assert ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ceq $before[$path]) "Maintained install changed: $path"}
	Assert ($results.Count -ge 16) 'Foundation contract case coverage is incomplete'
	$summary=[ordered]@{schemaVersion=1;status='Passed';scope='Foundation tool contracts only; no editor launch or GPU acceptance';installRoot=$install;caseCount=$results.Count;minimumCaseCount=16;cases=$results.ToArray()}
	WriteJson (Join-Path $work 'foundation-contracts.json') $summary
	Write-Output ($summary|ConvertTo-Json -Depth 100)
	exit 0
} catch {
	Write-Error $_.Exception.Message -ErrorAction Continue
	if($ownsWork -and (Test-Path -LiteralPath $work -PathType Container)) {
		WriteJson (Join-Path $work 'foundation-contracts-failed.json') @{schemaVersion=1;status='Failed';reason=$_.Exception.Message;cases=$results.ToArray()}
	}
	exit 1
}
