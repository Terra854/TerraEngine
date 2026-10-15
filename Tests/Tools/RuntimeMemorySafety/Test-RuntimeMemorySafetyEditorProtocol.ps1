# Synthetic endpoint/receipt protocol checks. No editor is launched.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$WorkRoot)
Set-StrictMode -Version Latest;$ErrorActionPreference='Stop'
$editorTestRoot=[IO.Path]::GetFullPath($WorkRoot);if(Test-Path $editorTestRoot){throw 'Editor protocol root must be fresh'};$null=[IO.Directory]::CreateDirectory($editorTestRoot)
. (Join-Path $PSScriptRoot 'RunInstalledRuntimeMemorySafety.ps1')
$script:editorChecks=0
function Check([string]$Name,[scriptblock]$Body){& $Body;$script:editorChecks++;Write-Output "[PASS] $Name"}
function Reject([string]$Name,[scriptblock]$Body){$caught=$false;try{& $Body|Out-Null}catch{$caught=$true};if(!$caught){throw "Expected rejection $Name"};$script:editorChecks++;Write-Output "[PASS] $Name rejected"}
$episode=@{lifetimeId=10;episodeId=2;frameId=2;capacityBytes=16777216;usedBytes=1024;peakBytes=1024;lifetimeHighWaterBytes=2048;reuseAllocationCount=1;fallbackAllocationCount=0;resetCount=2;outcome='Completed';outcomeCode=2}
$previous=@{lifetimeId=0;episodeId=0;frameId=0;capacityBytes=0;usedBytes=0;peakBytes=0;lifetimeHighWaterBytes=0;reuseAllocationCount=0;fallbackAllocationCount=0;resetCount=0;outcome='None';outcomeCode=0}
$snapshot=@{latest=$episode;previous=$previous;reclamation=@{lifetimeId=10;totalFallbackAllocationCount=2;totalFallbackDeallocationCount=2;outstandingFallbackAllocationCount=0;outstandingFallbackBytes=0;resetCount=2}}|ConvertTo-Json -Depth 20|ConvertFrom-Json
$scene=Join-Path $editorTestRoot 'scene.tescene';[IO.File]::WriteAllText($scene,'SYNTHETIC authored scene bytes')
$obsPath=Join-Path $editorTestRoot 'observations.json';$projectState=@{projectGeneration=1;sceneGeneration=2;projectRoot=$editorTestRoot;activeScenePath=$scene;sceneDirty=$false};$objects=@(@{uuid='SYNTHETIC';name='Light';components=@('Light')})
RmsWrite $obsPath @{readBefore=$projectState;readAfter=$projectState;objectsBeforeRead=$objects;objectsAfterRead=$objects;baselineObjects=$objects;restoredObjects=$objects;sourceScene=(RmsPin $scene)}
$rpcPath=Join-Path $editorTestRoot 'rpc.jsonl';$events=@()
foreach($name in @('project.state','scene.list_objects','play.state','play.enter','play.pause','diagnostics.get_summary','diagnostics.get_summary','play.exit')){
	$data=$(if($name -ceq 'diagnostics.get_summary'){@{rendererScratch=$snapshot}}else{@{}})
	$events+=(@{request=@{method='tools/call';params=@{name=$name;arguments=@{}}};response=@{result=@{content=@(@{type='text';text=(@{ok=$true;data=$data}|ConvertTo-Json -Depth 30 -Compress)})}}}|ConvertTo-Json -Depth 40 -Compress)
}
[IO.File]::WriteAllText($rpcPath,($events -join "`n")+"`n")
$manifestHash='1'*64;$runtime=Join-Path $editorTestRoot 'mock-install'
$receipt=@{schemaVersion='runtime-memory-safety-editor-loopback-v1';status='Passed';runtimeManifestSha256=$manifestHash;capability='diagnostics.get_summary';workRoot=$editorTestRoot;isolatedProjectRoot=(Join-Path $editorTestRoot 'fixture/project');process=@{pid=123;startUtc='2020-01-01T00:00:00Z';executablePath=(Join-Path $runtime 'Editor/TerraEngine-Editor.exe')};endpointOwnershipVerified=$true;isolatedProjectVerified=$true;readOnlyAuthoringStable=$true;playRoundTripAuthoredObjectsExact=$true;authoredSceneFileUnchanged=$true;lifecycle=@{normalClose=$true;exitCode=0;timedOut=$false};snapshots=@($snapshot,$snapshot);rawEvidence=@((RmsPin $rpcPath),(RmsPin $obsPath),(RmsPin $scene))}|ConvertTo-Json -Depth 50|ConvertFrom-Json
Check 'native-rpc-snapshot-joined-to-authoring-and-normal-close' {RmsCheckEditorLoopback $receipt $manifestHash $runtime}
foreach($fault in @('wrong-manifest','wrong-capability','wrong-executable','unowned-endpoint','project-escape','authoring-mutated','play-restore-failed','scene-file-mutated','watchdog-kill','missing-snapshot','outcome-code','allocator-control','reclamation-lifetime','reclamation-count','snapshot-not-in-native-rpc','missing-raw-rpc')){
	$r=$receipt|ConvertTo-Json -Depth 100|ConvertFrom-Json
	switch($fault){
		'wrong-manifest'{$r.runtimeManifestSha256='2'*64}
		'wrong-capability'{$r.capability='diagnostics.summary'}
		'wrong-executable'{$r.process.executablePath=Join-Path $editorTestRoot 'different.exe'}
		'unowned-endpoint'{$r.endpointOwnershipVerified=$false}
		'project-escape'{$r.isolatedProjectRoot=Split-Path $editorTestRoot}
		'authoring-mutated'{$r.readOnlyAuthoringStable=$false}
		'play-restore-failed'{$r.playRoundTripAuthoredObjectsExact=$false}
		'scene-file-mutated'{$r.authoredSceneFileUnchanged=$false}
		'watchdog-kill'{$r.lifecycle.timedOut=$true}
		'missing-snapshot'{$r.snapshots=@($r.snapshots|Select-Object -First 1)}
		'outcome-code'{$r.snapshots[0].latest.outcomeCode=3}
		'allocator-control'{$r.snapshots[0]|Add-Member allocatorReset 'FORBIDDEN'}
		'reclamation-lifetime'{$r.snapshots[0].reclamation.lifetimeId++}
		'reclamation-count'{$r.snapshots[0].reclamation.outstandingFallbackAllocationCount=1}
		'snapshot-not-in-native-rpc'{$r.snapshots[0].latest.episodeId++}
		'missing-raw-rpc'{$r.rawEvidence=@($r.rawEvidence|Where-Object {[IO.Path]::GetFileName($_.path) -cne 'rpc.jsonl'})}
	}
	Reject "editor-$fault" {RmsCheckEditorLoopback $r $manifestHash $runtime}
}
Check 'new-runner-canonical-preserves-wrapped-scalars-and-empty-object' {$value=@{path=(Join-Path $editorTestRoot 'Editor.exe');empty=('{}'|ConvertFrom-Json);values=@(0,1,$false,$null)};$copy=RmsStableJson $value|ConvertFrom-Json;RmsRequire ($copy.path -ceq $value.path -and (RmsStableJson $copy.empty) -ceq '{}' -and (RmsStableJson $copy.values) -ceq '[0,1,false,null]') 'Runner canonical serializer corrupted primitives'}
RmsWrite (Join-Path $editorTestRoot 'result.json') @{schemaVersion=1;status='EditorProtocolPassed';caseCount=$script:editorChecks;syntheticEndpointOnly=$true;nativeAcceptance=$false;performanceAcceptance=$false;sourceIntegrationReleased=$false}
Write-Output "$($script:editorChecks) editor loopback protocol cases passed; synthetic endpoint only"
exit 0
