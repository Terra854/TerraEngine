<#
Run matching-header frame replay collectors against one explicitly pinned runtime.
Prerequisites: an installed correctness watchdog, a parent-built matching collector,
its reviewed engine path/SHA, and PreparedFrameReplayProjects' closed-input manifest.
Default executes all 15 scenarios at 1/2/8 workers, once each; -Scenario/-Workers
select an explicitly partial debugging campaign. Every child gets a fresh project
copy and cwd. The runner retains failures and stops; it never retries a favorable
subset or changes the original reference/timing controls.
Example: ./RunFrameStageReplay.ps1 -InstallRoot <Build> -CollectorExecutable <exe>
 -ExpectedCollectorSha256 <sha> -ExpectedEnginePath <actual DLL> -ExpectedEngineSha256 <sha>
 -WorkloadManifest <frame-replay-workloads.json> -ProfileManifest <preparation.json>
 -WorkRoot <fresh-directory> [-Scenario cold-direct-complete -Workers 1]
The original matching collector has no adjacent DLL: DependencyDirectory defaults
that original DLL's directory and is prepended to PATH. The collector itself checks
actual module path/hash before Engine construction. InstallRoot/Editor remains the
read-only engine asset root; logs/cache use the owned per-process cwd.
#>
[CmdletBinding()]
param(
	[string]$InstallRoot='', [string]$CollectorExecutable='', [string]$ExpectedCollectorSha256='',
	[string]$ExpectedEnginePath='', [string]$ExpectedEngineSha256='', [string]$WorkloadManifest='',
	[string]$ProfileManifest='', [string]$WorkRoot='', [string]$Scenario='',
	[int[]]$Workers=@(1,2,8), [string]$DependencyDirectory='',
	[ValidateRange(1,3600)][int]$TimeoutSeconds=300
)
$script:FrameReplayRunnerPath=$PSCommandPath
$script:FrameReplayRunnerArguments=@{};foreach($key in $PSBoundParameters.Keys){$script:FrameReplayRunnerArguments[$key]=$PSBoundParameters[$key]}
# Reuse the frozen parser/record validator. This import starts no process and
# deliberately keeps all runtime ABI-specific access inside the pinned collector.
. (Join-Path $PSScriptRoot 'CompareFrameStageReplay.ps1')
function FrrRequire([bool]$Condition,[string]$Message){if(!$Condition){throw "[FrameReplayRunner] $Message"}}
function FrrFull([string]$Path){[IO.Path]::GetFullPath($Path).TrimEnd('\','/')}
function FrrWithin([string]$Path,[string]$Root){$Path -ieq $Root -or $Path.StartsWith($Root+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)}
function FrrPin([string]$Path){FsrNoRedirect $Path;FrrRequire ([IO.File]::Exists($Path)) "Missing file: $Path";return [ordered]@{path=(FrrFull $Path);sha256=(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant();bytes=(Get-Item -LiteralPath $Path).Length}}
function FrrCheckPin($Pin){$actual=FrrPin $Pin.path;FrrRequire ($actual.sha256 -ceq $Pin.sha256 -and $actual.bytes -eq $Pin.bytes) "Pinned input changed: $($Pin.path)"}
function FrrRead([string]$Path){$null=FsrRead $Path;return [IO.File]::ReadAllText($Path)|ConvertFrom-Json -AsHashtable -Depth 100}
function FrrWrite([string]$Path,$Value){FsrNoRedirect $Path;$stream=[IO.File]::Open($Path,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read);try{$bytes=[Text.UTF8Encoding]::new($false).GetBytes(($Value|ConvertTo-Json -Depth 100));$stream.Write($bytes,0,$bytes.Length)}finally{$stream.Dispose()}}
function FrrRelative([string]$Root,[string]$Relative){
	FrrRequire (![string]::IsNullOrWhiteSpace($Relative) -and ![IO.Path]::IsPathRooted($Relative) -and $Relative -cnotmatch '\\' -and !($Relative.Split('/') -contains '..') -and !($Relative.Split('/') -contains '.') -and !($Relative.Split('/') -contains '')) 'Closure path must be a canonical project-relative forward-slash path.'
	$full=FrrFull (Join-Path $Root $Relative);FrrRequire ((FrrWithin $full $Root) -and $full -ine $Root) 'Closure file escaped project root.';FsrNoRedirect $full;return $full
}
function FrrVerifyClosure([string]$Root,$Closure){
	$Root=FrrFull $Root;FsrNoRedirect $Root;FrrRequire ([IO.Directory]::Exists($Root)) 'Missing closed project root.'
	FrrRequire ($Closure.schemaVersion -ceq 'frame-replay-project-closure-v1' -and $Closure.files -is [array] -and $Closure.files.Count -gt 0) 'Missing portable closed-input inventory.'
	$expected=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
	foreach($row in $Closure.files){$file=FrrRelative $Root $row.path;FrrRequire ($expected.Add($file)) 'Repeated closure file.';FrrRequire ($row.sha256 -cmatch '^[0-9a-f]{64}$' -and $row.bytes -is [ValueType] -and $row.bytes -isnot [bool] -and $row.bytes -ge 0) 'Malformed closed-input pin.';$actual=FrrPin $file;FrrRequire ($actual.sha256 -ceq $row.sha256 -and $actual.bytes -eq $row.bytes) "Closed project bytes changed: $file"}
	FrrRequire ($expected.Contains((FrrFull (Join-Path $Root 'Assets/Scenes/Scene.tescene')))) 'Closure omitted authored scene.'
	$actualFiles=@(Get-ChildItem -LiteralPath $Root -Recurse -File -Force);FrrRequire ($actualFiles.Count -eq $expected.Count) 'Closed project has missing/unsealed files.'
	foreach($file in $actualFiles){FsrNoRedirect $file.FullName;FrrRequire ($expected.Contains((FrrFull $file.FullName))) 'Closed project has an unsealed input.'}
	# Reject redirected directories even if they contain no files.
	foreach($directory in (Get-ChildItem -LiteralPath $Root -Recurse -Directory -Force)){FsrNoRedirect $directory.FullName}
}
function FrrCopyProject([string]$Source,[string]$Destination,$Closure){
	FrrVerifyClosure $Source $Closure;FrrRequire (!(Test-Path -LiteralPath $Destination)) 'Process project must be fresh.';$null=[IO.Directory]::CreateDirectory($Destination)
	foreach($row in $Closure.files){$from=FrrRelative $Source $row.path;$to=FrrRelative $Destination $row.path;$null=[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($to));[IO.File]::Copy($from,$to,$false)}
	FrrVerifyClosure $Destination $Closure;FrrVerifyClosure $Source $Closure
}
function FrrProfileName($ScenarioObject){if($ScenarioObject.id -ceq 'no-output'){return 'NoOutput'};if($ScenarioObject.id -ceq 'multi-output'){return 'MultiOutput'};return [string]$ScenarioObject.inputProfile}
function FrrArguments([string]$Install,[string]$Project,[string]$AssetManifest,[string]$Closure,[string]$Manifest,[string]$ScenarioId,[int]$WorkerCount,[string]$Output,[string]$Nonce,[string]$Engine,[string]$EngineHash){
	return @('--install-root',$Install,'--project-root',$Project,'--asset-manifest',$AssetManifest,'--project-closure',$Closure,'--manifest',$Manifest,'--scenario',$ScenarioId,'--workers',[string]$WorkerCount,'--output',$Output,'--run-nonce',$Nonce,'--expected-engine-path',$Engine,'--expected-engine-sha256',$EngineHash)
}
function FrrCheckReceipt([string]$Path,$Launch,[Text.Json.JsonElement]$ScenarioElement){
	$record=FsrRead $Path;$worker=FsrValidateRecord $record $ScenarioElement $false
	FrrRequire ($worker -eq $Launch.workers -and (FsrString (FsrField $record 'runNonce') '$/runNonce') -ceq $Launch.runNonce) 'Raw receipt identity does not match owned launch.'
	$controls=FsrField $record 'controls'
	foreach($kind in @('loadedEngine','hostExecutable','manifest','assetManifest','projectClosure')){FsrSamePin (FsrPin (FsrField $controls $kind) ('$/controls/'+$kind)) $Launch[$kind] $kind}
	FrrRequire ((FrrFull (FsrString (FsrField $controls 'projectRoot') '$/projectRoot')) -ieq $Launch.projectRoot) 'Collector used a different process project.'
	FsrBoolean (FsrField $controls 'engineHashVerified') $true '$/engineHashVerified'
	FrrRequire ((FrrFull (FsrString (FsrField $controls 'expectedEnginePath') '$/expectedEnginePath')) -ieq $Launch.loadedEngine.path -and (FsrString (FsrField $controls 'expectedEngineSha256') '$/expectedEngineSha256') -ceq $Launch.loadedEngine.sha256) 'Collector ABI guard differs from parent-pinned launch.'
	FsrCompareValue (FsrField $controls 'scenario') $ScenarioElement '/controls/scenario'
	$resolved=FsrArray (FsrField $controls 'resolvedAssetManifests') '$/resolvedAssetManifests';FrrRequire ($resolved.Count -gt 0) 'Collector omitted resolved asset inputs.'
	$resolvedPins=@(foreach($pin in $resolved){FsrPin $pin '$/resolvedAssetManifests'})
	$generatedDirectory=Join-Path $Launch.projectRoot 'Build/Assets/.terra-animation-manifests';FrrRequire ([IO.Directory]::Exists($generatedDirectory)) 'Prepared project omitted actual animation manifest directory.'
	[string[]]$generated=@([IO.Directory]::GetFiles($generatedDirectory)|Where-Object {[IO.Path]::GetExtension($_) -ceq '.json'});[Array]::Sort($generated,[StringComparer]::Ordinal)
	$expectedResolved=@($Launch.assetManifest)+@(foreach($file in $generated){FrrPin $file});FrrRequire ($resolvedPins.Count -eq $expectedResolved.Count) 'Collector resolved input closure differs from actual owned project.'
	for($i=0;$i -lt $expectedResolved.Count;$i++){FsrSamePin $resolvedPins[$i] $expectedResolved[$i] 'actual resolved input'}
	FsrSamePin $resolvedPins[0] $Launch.assetManifest 'first resolved input'
	return ,$resolvedPins
}
function InvokeFrameStageReplayCampaign {
	[CmdletBinding()]
	param([string]$InstallRoot,[string]$CollectorExecutable,[string]$ExpectedCollectorSha256,[string]$ExpectedEnginePath,[string]$ExpectedEngineSha256,[string]$WorkloadManifest,[string]$ProfileManifest,[string]$WorkRoot='',[string]$Scenario='',[int[]]$Workers=@(1,2,8),[string]$DependencyDirectory='',[int]$TimeoutSeconds=300)
	$ErrorActionPreference='Stop';$campaign=$null;$summaryPath=$null;$exitCode=1;$originalPath=$env:PATH
	try {
		foreach($required in @($InstallRoot,$CollectorExecutable,$ExpectedEnginePath,$WorkloadManifest,$ProfileManifest)){FrrRequire (![string]::IsNullOrWhiteSpace($required) -and [IO.Path]::IsPathRooted($required)) 'Install, collector, engine and manifest paths must be explicit and absolute.';FsrNoRedirect $required}
		$install=FrrFull $InstallRoot;$collector=FrrFull $CollectorExecutable;$engine=FrrFull $ExpectedEnginePath;$manifestPath=FrrFull $WorkloadManifest;$profilePath=FrrFull $ProfileManifest
		FrrRequire ((FrrWithin $engine $install) -and [IO.Directory]::Exists((Join-Path $install 'Editor'))) 'Actual engine DLL must belong to the explicit install root.'
		$work=if($WorkRoot){FrrRequire ([IO.Path]::IsPathRooted($WorkRoot)) 'WorkRoot must be absolute.';FrrFull $WorkRoot}else{Join-Path ([IO.Path]::GetTempPath()) ('terra-frame-replay-'+[guid]::NewGuid().ToString('N'))}
		FsrNoRedirect $work;FrrRequire (!(Test-Path -LiteralPath $work) -and !(FrrWithin $work $install) -and !(FrrWithin $install $work)) 'WorkRoot must be fresh and outside InstallRoot.'
		$null=[IO.Directory]::CreateDirectory($work);$summaryPath=Join-Path $work 'campaign.json'
		$campaign=[ordered]@{schemaVersion='frame-stage-replay-campaign-v1';status='Failed';fullMatrix=$false;expectedCases=0;completedCases=0;replayAcceptance=$false;gpuAcceptance=$false;realProcessesLaunched=0;launchAttempts=0;cases=[Collections.Generic.List[object]]::new()}
		$hostPin=FrrPin $collector;$enginePin=FrrPin $engine
		FrrRequire ($ExpectedCollectorSha256 -cmatch '^[0-9a-f]{64}$' -and $hostPin.sha256 -ceq $ExpectedCollectorSha256 -and $ExpectedEngineSha256 -cmatch '^[0-9a-f]{64}$' -and $enginePin.sha256 -ceq $ExpectedEngineSha256) 'Explicit parent collector/engine SHA256 pin does not match actual file.'
		$watchdog=Join-Path $install 'Tests/Tools/Correctness/RunWithWatchdog.ps1';$watchdogPin=FrrPin $watchdog
		$campaign.inputs=@{collector=$hostPin;engine=$enginePin;watchdog=$watchdogPin;workloadManifest=(FrrPin $manifestPath);profileManifest=(FrrPin $profilePath);runner=(FrrPin $script:FrameReplayRunnerPath)}
		$manifestElement=FsrRead $manifestPath;$manifest=FrrRead $manifestPath;$profiles=FrrRead $profilePath
		FrrRequire ($manifest.schemaVersion -ceq 'frame-replay-workloads-v1' -and $manifest.scenarios.Count -eq 15) 'Frozen workload must contain exactly fifteen scenarios.'
		$scenarioIds=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);foreach($item in $manifest.scenarios){FrrRequire ($item.id -cmatch '^[a-z][a-z0-9-]*$' -and $scenarioIds.Add($item.id)) 'Duplicate/unsafe scenario identity.'}
		foreach($expectedId in @('cold-direct-complete','warm-direct-complete','cold-virtual-compact','warm-virtual-compact','animated-mixed','multi-output','no-output','rejection-rollback','cancel-before-exchange','cancel-after-exchange','held-reader','worker-failure','scene-clear','shutdown','restart')){FrrRequire ($scenarioIds.Contains($expectedId)) 'Frozen workload scenario identities changed.'}
		FrrRequire ($profiles.schemaVersion -ceq 'frame-replay-project-preparation-v1' -and $profiles.status -ceq 'ClosedInputsPrepared' -and $profiles.profiles.Count -eq 5) 'Prepared five-profile closed-input manifest is required.'
		$profileRows=[Collections.Generic.Dictionary[string,object]]::new([StringComparer]::Ordinal)
		foreach($profile in $profiles.profiles){foreach($path in @($profile.projectRoot,$profile.assetManifest,$profile.projectClosure)){FrrRequire ([IO.Path]::IsPathRooted($path)) 'Prepared profile paths must be absolute.'};FrrRequire (!$profileRows.ContainsKey($profile.name)) 'Duplicate prepared profile.';$profileRows[$profile.name]=$profile;FrrCheckPin $profile.closurePin;FrrRequire ((FrrFull $profile.closurePin.path) -ieq (FrrFull $profile.projectClosure)) 'Profile closure pin belongs to another file.';$closure=FrrRead $profile.projectClosure;FrrRequire ($closure.status -ceq 'ClosedInputsPinned' -and $closure.profile -ceq $profile.profile -and $closure.variant -ceq $profile.variant -and $closure.closedEditor.normalClose -is [bool] -and $closure.closedEditor.normalClose -and $closure.closedEditor.exitCode -eq 0) 'Profile does not describe normally closed actual inputs.';FrrVerifyClosure $profile.projectRoot $closure;$profile['verifiedClosure']=$closure;$profile['assetPin']=FrrPin $profile.assetManifest;FrrRequire ($profile.assetPin.sha256 -ceq $closure.assetManifest.sha256 -and $profile.assetPin.bytes -eq $closure.assetManifest.bytes) 'Asset registration manifest differs from sealed preparation closure.'}
		foreach($name in @('DirectComplete','VirtualCompact','Mixed','NoOutput','MultiOutput')){FrrRequire ($profileRows.ContainsKey($name)) 'Prepared profile set is incomplete.';$expectedProfile=if($name -cin @('NoOutput','MultiOutput')){'Mixed'}else{$name};$expectedVariant=if($name -cin @('NoOutput','MultiOutput')){$name}else{'Default'};FrrRequire ($profileRows[$name].profile -ceq $expectedProfile -and $profileRows[$name].variant -ceq $expectedVariant) 'Prepared profile name/profile/variant join changed.'}
		$workerSet=[Collections.Generic.HashSet[int]]::new();foreach($count in $Workers){FrrRequire ($count -in @(1,2,8) -and $workerSet.Add($count)) 'Workers must be distinct values from 1,2,8.'};FrrRequire ($Workers.Count -gt 0) 'Worker selection is empty.'
		$selected=@($manifest.scenarios|Where-Object {!$Scenario -or $_.id -ceq $Scenario});FrrRequire ($selected.Count -gt 0 -and (!$Scenario -or $selected.Count -eq 1)) 'Unknown debugging scenario.'
		$campaign.fullMatrix=(!$Scenario -and $workerSet.Count -eq 3);$campaign.expectedCases=$selected.Count*$Workers.Count
		$dependencies=if($DependencyDirectory){FrrRequire ([IO.Path]::IsPathRooted($DependencyDirectory)) 'DependencyDirectory must be absolute.';FrrFull $DependencyDirectory}else{Split-Path $engine}
		FsrNoRedirect $dependencies;FrrRequire ([IO.Directory]::Exists($dependencies) -and (FrrWithin $dependencies $install)) 'Dependencies must come from the explicit runtime install.'
		$campaign.dependencyDirectory=$dependencies
		foreach($scenarioObject in $selected){foreach($count in $Workers){
			$id=$scenarioObject.id+'-workers-'+$count;$caseRoot=Join-Path $work $id;$null=[IO.Directory]::CreateDirectory($caseRoot);$cwd=Join-Path $caseRoot 'cwd';$null=[IO.Directory]::CreateDirectory($cwd)
			$case=[ordered]@{identity=$id;scenarioId=$scenarioObject.id;workers=$count;status='Failed';directory=$caseRoot};$campaign.cases.Add($case)
			$profile=$profileRows[(FrrProfileName $scenarioObject)];FrrRequire ($profile.profile -ceq $scenarioObject.inputProfile) 'Prepared input profile differs from workload.'
			FrrRequire (!(FrrWithin $work (FrrFull $profile.projectRoot)) -and !(FrrWithin (FrrFull $profile.projectRoot) $work)) 'Campaign work overlaps original prepared inputs.'
			$project=Join-Path $caseRoot 'project';FrrCopyProject $profile.projectRoot $project $profile.verifiedClosure
			foreach($pin in $campaign.inputs.Values){FrrCheckPin $pin};FrrCheckPin $profile.closurePin;FrrCheckPin $profile.assetPin
			$nonce=[guid]::NewGuid().ToString('N');$rawPath=Join-Path $caseRoot 'raw.json';$logs=Join-Path $caseRoot 'watchdog'
			$launch=[ordered]@{schemaVersion='frame-stage-replay-launch-v1';scenarioId=$scenarioObject.id;workers=$count;runNonce=$nonce;hostExecutable=$hostPin;loadedEngine=$enginePin;manifest=$campaign.inputs.workloadManifest;assetManifest=$profile.assetPin;projectClosure=$profile.closurePin;projectRoot=(FrrFull $project);sourceProjectRoot=(FrrFull $profile.projectRoot);workingDirectory=$cwd;watchdog=$watchdogPin;dependencyDirectory=$dependencies;timeoutSeconds=$TimeoutSeconds}
			$argumentValues=FrrArguments $install $project $profile.assetManifest $profile.projectClosure $manifestPath $scenarioObject.id $count $rawPath $nonce $engine $enginePin.sha256
			$launch.arguments=$argumentValues;FrrWrite (Join-Path $caseRoot 'launch.json') $launch;$case.launchPin=FrrPin (Join-Path $caseRoot 'launch.json')
			$env:PATH=$dependencies+[IO.Path]::PathSeparator+$originalPath
			$campaign.launchAttempts++;$watchdogMessage=& $watchdog -Executable $collector -ArgumentList $argumentValues -TimeoutSeconds $TimeoutSeconds -WorkingDirectory $cwd -LogDirectory $logs 2>&1;$childExit=$LASTEXITCODE;$env:PATH=$originalPath
			[IO.File]::WriteAllText((Join-Path $caseRoot 'watchdog-wrapper.log'),($watchdogMessage|Out-String),[Text.UTF8Encoding]::new($false))
			$case.exitCode=$childExit;foreach($name in @('stdout.log','stderr.log','watchdog.json')){if([IO.File]::Exists((Join-Path $logs $name))){$case[$name]=FrrPin (Join-Path $logs $name)}}
			$observedWatch=$null;if([IO.File]::Exists((Join-Path $logs 'watchdog.json'))){$observedWatch=FrrRead (Join-Path $logs 'watchdog.json');if($observedWatch.pid -gt 0){$campaign.realProcessesLaunched++;$case.processId=$observedWatch.pid}}
			if([IO.File]::Exists($rawPath)){$case.rawReceipt=FrrPin $rawPath}
			FrrVerifyClosure $project $profile.verifiedClosure;FrrVerifyClosure $profile.projectRoot $profile.verifiedClosure;foreach($pin in $campaign.inputs.Values){FrrCheckPin $pin};FrrCheckPin $profile.closurePin;FrrCheckPin $profile.assetPin;$case.copiedInputsUnchanged=$true;$case.sourceInputsUnchanged=$true
			FrrRequire ($null -ne $observedWatch -and $observedWatch.pid -gt 0 -and $observedWatch.timedOut -is [bool] -and !$observedWatch.timedOut) 'Watchdog timeout/missing owned process proof.'
			if([IO.File]::Exists($rawPath)){$case.rawReceipt=FrrPin $rawPath;$raw=FrrRead $rawPath;if($raw.status -ceq 'Incomplete'){$case.status='Incomplete';throw 'INCOMPLETE: collector could not observe required runtime capabilities.'}}
			FrrRequire ($childExit -eq 0 -and [IO.File]::Exists((Join-Path $logs 'watchdog.json'))) 'Collector/watchdog failed; preserved raw logs must be inspected.'
			$watch=FrrRead (Join-Path $logs 'watchdog.json');FrrRequire ($watch.exitCode -eq 0 -and $watch.timedOut -is [bool] -and !$watch.timedOut -and $watch.pid -gt 0) 'Watchdog did not observe successful owned process completion.'
			FrrRequire ([IO.File]::Exists($rawPath)) 'Collector omitted its raw receipt.'
			$scenarioElement=@((FsrArray (FsrField $manifestElement 'scenarios') '$/scenarios')|Where-Object {(FsrString (FsrField $_ 'id') '$/id') -ceq $scenarioObject.id})[0]
			$resolvedPins=FrrCheckReceipt $rawPath $launch $scenarioElement
			FrrVerifyClosure $project $profile.verifiedClosure;FrrVerifyClosure $profile.projectRoot $profile.verifiedClosure
			foreach($pin in $campaign.inputs.Values){FrrCheckPin $pin};FrrCheckPin $profile.closurePin;FrrCheckPin $profile.assetPin
			$verified=[ordered]@{schemaVersion='frame-stage-replay-owned-observation-v1';status='Passed';launchPin=$case.launchPin;receipt=$case.rawReceipt;runNonce=$nonce;loadedEngine=$enginePin;hostExecutable=$hostPin;projectRoot=(FrrFull $project);sourceProjectRoot=(FrrFull $profile.projectRoot);watchdogPin=(FrrPin (Join-Path $logs 'watchdog.json'));watchdogPid=$watch.pid;resolvedAssetManifests=$resolvedPins;copiedInputsUnchanged=$true;sourceInputsUnchanged=$true}
			FrrWrite (Join-Path $caseRoot 'owned-observation.json') $verified;$case.ownedObservation=FrrPin (Join-Path $caseRoot 'owned-observation.json');$case.status='Passed';$campaign.completedCases++
		}}
		FrrRequire ($campaign.cases.Count -eq $campaign.expectedCases -and $campaign.completedCases -eq $campaign.expectedCases -and (!$campaign.fullMatrix -or $campaign.expectedCases -eq 45)) 'Campaign is missing exact scenario/worker identities.'
		$campaign.status='Passed';$exitCode=0
	}catch{$reason=$_.Exception.Message;if($null -ne $campaign){$campaign.status=if($reason.StartsWith('INCOMPLETE:')){'Incomplete'}else{'Failed'};$campaign.error=$reason};$exitCode=if($reason.StartsWith('INCOMPLETE:')){2}else{1};Write-Output $reason}
	finally{$env:PATH=$originalPath;if($summaryPath -and $null -ne $campaign){FrrWrite $summaryPath $campaign}}
	Write-Output "Frame replay campaign exit $exitCode; evidence: $summaryPath"
	return $exitCode
}
if($MyInvocation.InvocationName -ne '.'){$runnerOutput=@(InvokeFrameStageReplayCampaign @script:FrameReplayRunnerArguments);for($i=0;$i -lt $runnerOutput.Count-1;$i++){Write-Output $runnerOutput[$i]};exit ([int]$runnerOutput[-1])}
