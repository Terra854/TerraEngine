<# Installed editor loopback with owned process and retained raw evidence.
Uses existing diagnostics.get_summary, play lifecycle, and read-only object/state
APIs in a fresh generated project. Never changes a personal project or frozen tool.
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$InstallRoot,[Parameter(Mandatory)][string]$ToolInstallRoot,[Parameter(Mandatory)][string]$WorkRoot,[Parameter(Mandatory)][string]$OutputPath,[int]$McpPort=43187,[int]$StartupTimeoutSeconds=180)
$entry=@{InstallRoot=$InstallRoot;ToolInstallRoot=$ToolInstallRoot;WorkRoot=$WorkRoot;OutputPath=$OutputPath;McpPort=$McpPort;StartupTimeoutSeconds=$StartupTimeoutSeconds}
. (Join-Path $PSScriptRoot 'RunInstalledRuntimeMemorySafety.ps1')
foreach($key in $entry.Keys){Set-Variable $key $entry[$key]}
Set-StrictMode -Version Latest;$ErrorActionPreference='Stop'
$runtime=RmsLoadInstall $InstallRoot;$tools=RmsLoadInstall $ToolInstallRoot;$work=[IO.Path]::GetFullPath($WorkRoot);$output=[IO.Path]::GetFullPath($OutputPath)
RmsRequire ([IO.Path]::GetFullPath($PSCommandPath) -ieq (RmsInstalled $runtime 'Tests/Tools/RuntimeMemorySafety/InvokeRuntimeMemorySafetyEditorDiagnostics.ps1')) 'Invoke installed editor loopback helper'
RmsRequire (!(Test-Path $work) -and !(Test-Path $output)) 'Editor work and output paths must be fresh'
foreach($root in @($runtime.root,$tools.root)){RmsRequire ($work -ine $root -and !(RmsBelow $work $root) -and !(RmsBelow $root $work) -and !(RmsBelow $output $root)) 'Editor evidence overlaps installed data'}
RmsNoRedirect $work;RmsNoRedirect $output;$null=[IO.Directory]::CreateDirectory($work);$null=[IO.Directory]::CreateDirectory((Split-Path $output))
$editor=RmsInstalled $runtime 'Editor/TerraEngine-Editor.exe';$watchdog=RmsInstalled $runtime 'Tests/Tools/Correctness/RunWithWatchdog.ps1';$materializer=RmsInstalled $tools 'Tests/Tools/RuntimeMemorySafety/MaterializeRuntimeMemorySafetyFixture.ps1'
$rpcPath=Join-Path $work 'rpc.jsonl';$owner=$null;$editorProcess=$null;$rpcId=0;$failure=$null;$normalClose=$false;$processProof=$null;$watch=$null;$snapshots=@();$stableRead=$false;$roundTrip=$false;$sceneUnchanged=$false;$ready=$false;$project=''
function Quote([string]$Value){return "'"+$Value.Replace("'","''")+"'"}
function Rpc([string]$Method,$Parameters){
	$request=@{jsonrpc='2.0';id=(++$script:rpcId);method=$Method;params=$Parameters}
	$response=(Invoke-WebRequest -Uri "http://127.0.0.1:$McpPort/mcp" -Method Post -ContentType 'application/json' -Body ($request|ConvertTo-Json -Depth 100 -Compress) -TimeoutSec 30).Content|ConvertFrom-Json
	[IO.File]::AppendAllText($rpcPath,((@{utc=[DateTime]::UtcNow.ToString('o');request=$request;response=$response}|ConvertTo-Json -Depth 100 -Compress)+"`n"),[Text.UTF8Encoding]::new($false))
	RmsRequire ($null -eq $response.PSObject.Properties['error']) 'Editor JSON-RPC returned an error';return $response
}
function Tool([string]$Name,$Arguments=@{}){
	$response=Rpc 'tools/call' @{name=$Name;arguments=$Arguments};$texts=@($response.result.content|Where-Object type -eq 'text');RmsRequire ($texts.Count -eq 1) 'Missing unique native text envelope';$envelope=$texts[0].text|ConvertFrom-Json
	RmsRequire ($envelope.ok -eq $true) "Native editor tool failed: $Name";return $envelope.data
}
function AwaitPlay([string]$Expected){$clock=[Diagnostics.Stopwatch]::StartNew();do{$p=Tool 'play.state';if($p.currentState -ceq $Expected -and $null -eq $p.pendingTransition){return $p};Start-Sleep -Milliseconds 100}while($clock.Elapsed.TotalSeconds -lt 90);throw "Play did not reach $Expected"}
function AwaitProject([string]$Project){
	$clock=[Diagnostics.Stopwatch]::StartNew();do{
		$p=Tool 'project.state';RmsRequire ($p.hasProject -and [IO.Path]::GetFullPath($p.projectRoot) -ieq $Project) 'Owned editor project identity changed'
		if(!$p.assetImportsActive -and $p.readiness.projectSettingsReady -and $p.readiness.simulationEssentialReady -and $p.activeScenePath){return $p};Start-Sleep -Milliseconds 100
	}while($clock.Elapsed.TotalSeconds -lt 90);throw 'Owned project readiness deadline exceeded'
}
function AuthoringObjects(){
	$all=New-Object 'Collections.Generic.List[object]';$offset=0
	do{$page=Tool 'scene.list_objects' @{includeComponents=$true;offset=$offset;limit=256};foreach($object in @($page.objects)){
		# Entity handles and transform revisions can change on normal Play/Stop
		# reload. Stable authored identity, hierarchy and component names cannot.
		$all.Add([ordered]@{uuid=$object.uuid;name=$object.name;tag=$object.tag;layer=$object.layer;active=$object.active;static=$object.static;parentUuid=$object.parentUuid;components=@($object.components|Sort-Object)})
	};$next=[int]$page.nextOffset;if($page.hasMore){RmsRequire ($next -gt $offset) 'Object pagination did not advance'};$offset=$next}while($page.hasMore)
	return ,@($all.ToArray()|Sort-Object uuid)
}
function SameProject($Before,$After){
	foreach($key in @('projectGeneration','sceneGeneration','projectRoot','activeScenePath','sceneDirty')){RmsRequire ((RmsStableJson $Before.$key) -ceq (RmsStableJson $After.$key)) "Read-only diagnostics changed project/scene identity: $key"}
}
try{
	& $materializer -InstallRoot $runtime.root -WorkRoot (Join-Path $work 'fixture') -Scale small *> (Join-Path $work 'materialize.log');RmsRequire ($LASTEXITCODE -eq 0) 'Installed fixture materialization failed'
	$fixturePath=Join-Path $work 'fixture/materialization.json';$fixture=Get-Content $fixturePath -Raw|ConvertFrom-Json;$project=[IO.Path]::GetFullPath($fixture.projectRoot);RmsRequire (RmsBelow $project $work) 'Materialized editor project escapes owned root'
	$prefs=Join-Path $work 'editor-preferences.json';RmsWrite $prefs @{recentProjects=@();autoBuildScripts=$false;editorMcpEnabled=$true;editorMcpWriteModeDefault='writes_enabled';editorMcpListenPort=$McpPort;vulkanValidationLayers=$true;renderDebugDiagnostics=$true;rendererDiagnosticsPreferencesInitialized=$true}
	RmsRequire (@(Get-NetTCPConnection -LocalPort $McpPort -State Listen -ErrorAction SilentlyContinue).Count -eq 0) 'MCP port is already occupied'
	$logs=Join-Path $work 'process';$command='& '+(Quote $watchdog)+' -Executable '+(Quote $editor)+" -ArgumentList @('--project',"+(Quote $project)+') -WorkingDirectory '+(Quote (Split-Path $editor))+' -TimeoutSeconds 900 -LogDirectory '+(Quote $logs)
	$start=[Diagnostics.ProcessStartInfo]::new((Join-Path $PSHOME 'pwsh.exe'));$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
	$start.ArgumentList.Add('-NoProfile');$start.ArgumentList.Add('-EncodedCommand');$start.ArgumentList.Add([Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command)))
	$start.Environment['TERRAENGINE_EDITOR_PREFS_SESSION_PATH']=$prefs;$start.Environment['TERRAENGINE_SUPPRESS_PROJECT_RECENTS']='1';$owner=[Diagnostics.Process]::Start($start)
	$clock=[Diagnostics.Stopwatch]::StartNew();$ready=$false;$lastError='No owned endpoint'
	do{try{
		RmsRequire (!$owner.HasExited) 'Editor watchdog exited before readiness';$hello=Rpc 'initialize' @{protocolVersion='2025-03-26';clientInfo=@{name='runtime-memory-safety-editor-loopback';version='1'}}
		RmsRequire ($hello.result.serverInfo.name -ceq 'TerraEngine Editor') 'Unexpected MCP server'
		$listeners=@(Get-NetTCPConnection -LocalPort $McpPort -State Listen);RmsRequire ($listeners.Count -eq 1) 'Ambiguous loopback endpoint';$process=Get-CimInstance Win32_Process -Filter ("ProcessId="+$listeners[0].OwningProcess)
		RmsRequire ($process.ParentProcessId -eq $owner.Id -and $process.ExecutablePath -ieq $editor) 'MCP endpoint does not belong to owned installed editor'
		$editorProcess=[Diagnostics.Process]::GetProcessById($listeners[0].OwningProcess);$baselineProject=AwaitProject $project;$ready=$true;break
	}catch{$lastError=$_.Exception.Message;Start-Sleep -Milliseconds 200}}while($clock.Elapsed.TotalSeconds -lt $StartupTimeoutSeconds)
	RmsRequire $ready "Owned installed editor readiness failed: $lastError"
	$processProof=@{pid=$editorProcess.Id;startUtc=$editorProcess.StartTime.ToUniversalTime().ToString('o');parentPid=$owner.Id;executablePath=$editor}
	$beforePlay=AwaitPlay 'stopped';RmsRequire (!$baselineProject.sceneDirty) 'Owned baseline scene must be clean before Play'
	$baselineObjects=AuthoringObjects;$sceneBefore=RmsPin ([IO.Path]::GetFullPath($baselineProject.activeScenePath))
	$null=Tool 'play.enter' @{expectedState='stopped'};$null=AwaitPlay 'playing';$null=Tool 'play.pause' @{expectedState='playing'};$paused=AwaitPlay 'paused';$readBefore=AwaitProject $project;$objectsBeforeRead=AuthoringObjects
	$clock.Restart();$snapshot=$null
	do{try{$data=Tool 'diagnostics.get_summary' @{includeDetails=$true};$snapshot=RmsField $data 'rendererScratch';RmsCheckEditorScratchSnapshot $snapshot;break}catch{$lastError=$_.Exception.Message;$snapshot=$null;Start-Sleep -Milliseconds 100}}while($clock.Elapsed.TotalSeconds -lt 90)
	RmsRequire ($null -ne $snapshot) "UNVERIFIED: Renderer copied diagnostics unavailable: $lastError";$snapshots+=,$snapshot
	$data=Tool 'diagnostics.get_summary' @{includeDetails=$true};$snapshot=RmsField $data 'rendererScratch';RmsCheckEditorScratchSnapshot $snapshot;$snapshots+=,$snapshot
	$readAfter=AwaitProject $project;$objectsAfterRead=AuthoringObjects;SameProject $readBefore $readAfter
	RmsRequire ((RmsStableJson $objectsBeforeRead) -ceq (RmsStableJson $objectsAfterRead)) 'Read-only diagnostics changed paused authored object publication';$stableRead=$true
	$null=Tool 'play.exit' @{};$afterPlay=AwaitPlay 'stopped';$restored=AwaitProject $project;$restoredObjects=AuthoringObjects
	RmsRequire ((RmsStableJson $baselineObjects) -ceq (RmsStableJson $restoredObjects)) 'Play/Stop failed to restore authored object/hierarchy publication';$roundTrip=$true
	RmsCheckPin $sceneBefore;RmsRequire (!$restored.sceneDirty) 'Play/Stop changed authored scene dirty state';$sceneUnchanged=$true
	RmsWrite (Join-Path $work 'observations.json') @{beforePlay=$beforePlay;paused=$paused;afterPlay=$afterPlay;beforeProject=$baselineProject;readBefore=$readBefore;readAfter=$readAfter;restored=$restored;baselineObjects=$baselineObjects;objectsBeforeRead=$objectsBeforeRead;objectsAfterRead=$objectsAfterRead;restoredObjects=$restoredObjects;snapshots=$snapshots;sourceScene=$sceneBefore}
}catch{$failure=$_.Exception.Message}
finally{
	if($null -ne $editorProcess -and !$editorProcess.HasExited){try{$p=Tool 'play.state';if($p.currentState -cne 'stopped'){$null=Tool 'play.exit' @{};$null=AwaitPlay 'stopped'};$normalClose=$editorProcess.CloseMainWindow() -and $editorProcess.WaitForExit(10000)}catch{if(!$failure){$failure=$_.Exception.Message}}}
	if($null -ne $owner){if(!$normalClose -and !$owner.HasExited){$owner.Kill($true)};$null=$owner.WaitForExit(15000);$watchPath=Join-Path $work 'process/watchdog.json';if(Test-Path $watchPath){$watch=Get-Content $watchPath -Raw|ConvertFrom-Json};$normalClose=$normalClose -and $owner.HasExited -and $owner.ExitCode -eq 0 -and $null -ne $watch -and $watch.exitCode -eq 0 -and !$watch.timedOut;$owner.Dispose()}
}
$lifecycle=@{normalClose=$normalClose;exitCode=$(if($null -ne $watch){$watch.exitCode}else{125});timedOut=$(if($null -ne $watch){$watch.timedOut}else{$true})};RmsWrite (Join-Path $work 'lifecycle.json') $lifecycle
$raw=@();foreach($path in @($rpcPath,(Join-Path $work 'lifecycle.json'),(Join-Path $work 'observations.json'),(Join-Path $work 'fixture/materialization.json'),(Join-Path $work 'process/watchdog.json'),(Join-Path $work 'process/stdout.log'),(Join-Path $work 'process/stderr.log'))){if(Test-Path $path){$raw+=,(RmsPin $path)}}
$receipt=@{schemaVersion='runtime-memory-safety-editor-loopback-v1';status=$(if(!$failure -and $normalClose){'Passed'}elseif($failure -and $failure.StartsWith('UNVERIFIED:')){'Unverified'}else{'Failed'});reason=$failure;capability='diagnostics.get_summary';runtimeManifestSha256=$runtime.manifest.sha256;workRoot=$work;isolatedProjectRoot=$project;process=$processProof;endpointOwnershipVerified=$ready;isolatedProjectVerified=$ready;readOnlyAuthoringStable=$stableRead;playRoundTripAuthoredObjectsExact=$roundTrip;authoredSceneFileUnchanged=$sceneUnchanged;lifecycle=$lifecycle;snapshots=$snapshots;rawEvidence=$raw}
if($receipt.status -ceq 'Passed'){try{RmsCheckEditorLoopback $receipt $runtime.manifest.sha256 $runtime.root;foreach($p in $runtime.pins){RmsCheckPin $p};foreach($p in $tools.pins){RmsCheckPin $p}}catch{$receipt.status='Failed';$receipt.reason=$_.Exception.Message}}
RmsWrite $output $receipt
Write-Output "Installed editor scratch diagnostics loopback: $($receipt.status)"
exit $(if($receipt.status -ceq 'Passed'){0}elseif($receipt.status -ceq 'Unverified'){2}else{1})
