<#
Installed read-only editor capture/resize loopback. Requires Windows desktop,
Vulkan validation, the installed materializer and readback fixture/decoder.
Invoke the installed copy with fresh WorkRoot and OutputPath; no personal project
is opened. One owned editor session closes normally after all requested captures.
#>
[CmdletBinding()]
param([Parameter(Mandatory)][string]$InstallRoot,[Parameter(Mandatory)][string]$WorkRoot,
	[Parameter(Mandatory)][string]$OutputPath,[ValidateRange(100,100000)][int]$Iterations=100)
$entry=@{InstallRoot=$InstallRoot;WorkRoot=$WorkRoot;OutputPath=$OutputPath;Iterations=$Iterations}
. (Join-Path $PSScriptRoot 'RunInstalledReadbackLifetime.ps1')
foreach($key in $entry.Keys){Set-Variable $key $entry[$key]}
Set-StrictMode -Version Latest;$ErrorActionPreference='Stop'
$install=$null;$work=$null;$output=$null;$failure=$null;$exitCode=1;$owner=$null;$editorProcess=$null
$normalClose=$false;$ready=$false;$receipt=$null;$watch=$null;$rows=[Collections.Generic.List[object]]::new()
$nonce=[guid]::NewGuid().ToString('N');$rpcCounter=0;$resizeCount=0;$warmup=$null

function RblEditorQuote([string]$Value){return "'"+$Value.Replace("'","''")+"'"}
function RblEditorRpc([string]$Method,$Parameters,[string]$Id='') {
	if(!$Id){$Id="$nonce-rpc-"+(++$script:rpcCounter)}
	$request=[pscustomobject]@{jsonrpc='2.0';id=$Id;method=$Method;params=$Parameters}
	$response=(Invoke-WebRequest -Uri "http://127.0.0.1:$port/mcp" -Method Post -ContentType 'application/json' -Body (ConvertTo-Json $request -Depth 100 -Compress) -TimeoutSec 35).Content | ConvertFrom-Json
	$path=Join-Path $work ('rpc-'+[guid]::NewGuid().ToString('N')+'.json')
	RblWrite $path ([pscustomobject]@{utc=[DateTime]::UtcNow.ToString('o');request=$request;response=$response})
	RblRequire ($response.id -ceq $Id -and $null -eq $response.PSObject.Properties['error']) 'Owned editor JSON-RPC identity/error mismatch.'
	return [pscustomobject]@{request=$request;response=$response;pin=(RblPin $path)}
}
function RblEditorTool([string]$Name,$Arguments=@{}) {
	$rpc=RblEditorRpc 'tools/call' @{name=$Name;arguments=$Arguments}
	$texts=@($rpc.response.result.content | Where-Object type -CEQ 'text')
	RblRequire ($texts.Count -eq 1) "Missing unique editor text envelope: $Name"
	$envelope=$texts[0].text | ConvertFrom-Json
	RblRequire ($envelope.ok -eq $true) "Editor tool failed: $Name"
	return $envelope.data
}
function RblEditorProject([string]$ExpectedProject) {
	$clock=[Diagnostics.Stopwatch]::StartNew()
	do {
		$state=RblEditorTool 'project.state'
		RblRequire ($state.hasProject -and [IO.Path]::GetFullPath($state.projectRoot) -ieq [IO.Path]::GetFullPath($ExpectedProject)) 'Owned editor project identity changed.'
		if(!$state.assetImportsActive -and $state.readiness.projectSettingsReady -and $state.readiness.simulationEssentialReady -and $state.activeScenePath){return $state}
		Start-Sleep -Milliseconds 100
	} while($clock.Elapsed.TotalSeconds -lt 120)
	throw 'INCOMPLETE: Owned isolated editor project did not become ready.'
}
function RblEditorObjects() {
	$list=[Collections.Generic.List[object]]::new();$offset=0
	do {
		$page=RblEditorTool 'scene.list_objects' @{includeComponents=$true;offset=$offset;limit=256}
		foreach($object in @($page.objects)) {
			$list.Add([ordered]@{uuid=$object.uuid;name=$object.name;tag=$object.tag;layer=$object.layer;active=$object.active;static=$object.static;parentUuid=$object.parentUuid;components=@($object.components | Sort-Object)})
		}
		$next=[int]$page.nextOffset;RblRequire (!$page.hasMore -or $next -gt $offset) 'Object pagination did not advance.';$offset=$next
	} while($page.hasMore)
	return ,@($list.ToArray() | Sort-Object uuid)
}
function RblEditorCapture([int]$Iteration,[uint32]$Width,[uint32]$Height,[string]$Operation) {
	$id="$nonce-capture-$Iteration"
	$captureArguments=@{width=$Width;height=$Height;freshness='nextFrame';includeOverlays=$false;transparentBackground=$false;allowBackendFallback=$false;timeoutMs=10000;correlationId=$id}
	if($Operation -ceq 'camera.capture_custom') {
		$captureArguments.camera=@{position=@{cell=@{x='0';y='0';z='0'};offsetMetres=@(0.0,2.0,5.0)};lookAt=@{cell=@{x='0';y='0';z='0'};offsetMetres=@(0.0,1.0,0.0)};viewType='Game'}
	}
	$rpc=RblEditorRpc 'tools/call' @{name=$Operation;arguments=$captureArguments} $id
	$texts=@($rpc.response.result.content | Where-Object type -CEQ 'text');$images=@($rpc.response.result.content | Where-Object type -CEQ 'image')
	RblRequire ($texts.Count -eq 1) 'Capture returned no unique native text envelope.'
	$envelope=$texts[0].text | ConvertFrom-Json
	if(!$envelope.ok){
		if($envelope.error.category -in @('unavailable','unsupported_readback','backend_readback_unavailable')){throw "INCOMPLETE: Real editor capture prerequisite failed: $($envelope.message)"}
		throw "Real editor capture failed: $($envelope.message)"
	}
	RblRequire ($images.Count -eq 1 -and $images[0].mimeType -ceq 'image/png') 'Capture must transmit exactly one PNG image.'
	RblRequire ($envelope.data.backendReadback -eq $true -and $envelope.data.deterministicFallback -eq $false -and $null -ne $envelope.data.PSObject.Properties['backendTarget']) 'Actual editor pixels lack real-backend target provenance.'
	$imagePath=Join-Path $work ("capture-$Iteration.png")
	# These are exactly the MCP image bytes, not a re-encoded bitmap or a copy
	# manufactured from the JSON extent. The native decoder verifies their PNG.
	$bytes=[Convert]::FromBase64String($images[0].data)
	$stream=[IO.File]::Open($imagePath,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
	try{$stream.Write($bytes,0,$bytes.Length)}finally{$stream.Dispose()}
	$decodePath=Join-Path $work ("decode-$Iteration.json");$logs=Join-Path $work ("decode-$Iteration-process")
	& $watchdog -Executable $decoder -ArgumentList @('--mode','decode','--input',$imagePath,'--output',$decodePath) -WorkingDirectory (Split-Path $decoder) -TimeoutSeconds 30 -LogDirectory $logs | Out-Host
	$decodeExit=$LASTEXITCODE;$decodeWatch=RblCheckWatchdog $logs $decodeExit
	$decoded=Get-Content -LiteralPath $decodePath -Raw | ConvertFrom-Json
	RblRequire ($decoded.status -ceq 'Decoded' -and $decoded.exitCode -eq 0 -and $decoded.processId -eq $decodeWatch.pid -and [IO.Path]::GetFullPath($decoded.executablePath) -ieq [IO.Path]::GetFullPath($decoder)) 'Independent installed decoder identity/exit mismatch.'
	return [pscustomobject]@{iteration=$Iteration;requestId=$id;responseId=$rpc.response.id;responseCount=1;correlationId=$envelope.correlationId;operation=$Operation;
		backendReadback=$envelope.data.backendReadback;deterministicFallback=$envelope.data.deterministicFallback;requestedWidth=$Width;requestedHeight=$Height;
		imagePin=(RblPin $imagePath);rpcPin=$rpc.pin;decoderReceiptPin=(RblPin $decodePath);decoder=$decoded;backendTarget=$envelope.data.backendTarget;
		decoderWatchdogPin=(RblPin (Join-Path $logs 'watchdog.json'));decoderStdoutPin=(RblPin (Join-Path $logs 'stdout.log'));decoderStderrPin=(RblPin (Join-Path $logs 'stderr.log'))}
}

try {
	$install=RblLoadInstall $InstallRoot;foreach($required in @(RblRequiredArtifacts)){$null=RblInstalled $install $required}
	RblRequire ([IO.Path]::GetFullPath($PSCommandPath) -ieq [IO.Path]::GetFullPath((RblInstalled $install 'Tests/Tools/ReadbackLifetime/InvokeReadbackEditorAcceptance.ps1'))) 'Invoke the installed editor helper.'
	$work=[IO.Path]::GetFullPath($WorkRoot);RblValidateWorkRoot $work $install
	$output=[IO.Path]::GetFullPath($OutputPath);RblNoRedirect $output
	RblRequire (!(Test-Path -LiteralPath $output) -and !(RblBelow $output $install.root)) 'Editor output must be fresh and outside install.'
	$null=[IO.Directory]::CreateDirectory($work);$null=[IO.Directory]::CreateDirectory((Split-Path $output))
	Import-Module (RblInstalled $install 'Tests/Tools/ReadbackLifetime/ReadbackLifetimeProtocol.psm1') -Force
	$editor=RblInstalled $install 'Editor/TerraEngine-Editor.exe';$decoder=RblInstalled $install 'Tests/Tools/ReadbackLifetime/ReadbackLifetimeFixture.exe'
	$watchdog=RblInstalled $install 'Tests/Tools/Correctness/RunWithWatchdog.ps1'
	$materializer=RblInstalled $install 'Tests/Tools/RuntimeMemorySafety/MaterializeRuntimeMemorySafetyFixture.ps1'
	& $materializer -InstallRoot $install.root -WorkRoot (Join-Path $work 'fixture') -Scale small *> (Join-Path $work 'materialize.log')
	RblRequire ($LASTEXITCODE -eq 0) 'Installed detached project materialization failed.'
	$materializationPath=Join-Path $work 'fixture/materialization.json';$materialization=Get-Content -LiteralPath $materializationPath -Raw | ConvertFrom-Json
	$project=[IO.Path]::GetFullPath($materialization.projectRoot);RblRequire (RblBelow $project $work) 'Materializer project escaped the owned root.'
	$expectedScene=Join-Path $project 'Assets/Scenes/Scene.tescene';$sceneBefore=RblPin $expectedScene
	# Choose an available loopback port. A bind race cannot attach us to another
	# editor: listener PID ancestry and exact installed executable are checked first.
	$probe=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,0);$probe.Start()
	try{$port=$probe.LocalEndpoint.Port}finally{$probe.Stop()}
	$prefs=Join-Path $work 'editor-preferences.json'
	RblWrite $prefs @{recentProjects=@();autoBuildScripts=$false;editorMcpEnabled=$true;editorMcpWriteModeDefault='read_only';editorMcpListenPort=$port;vulkanValidationLayers=$true;renderDebugDiagnostics=$true;rendererDiagnosticsPreferencesInitialized=$true}
	$logs=Join-Path $work 'editor-process'
	$command='& '+(RblEditorQuote $watchdog)+' -Executable '+(RblEditorQuote $editor)+" -ArgumentList @('--project',"+(RblEditorQuote $project)+') -WorkingDirectory '+(RblEditorQuote (Split-Path $editor))+' -TimeoutSeconds 3600 -LogDirectory '+(RblEditorQuote $logs)
	$start=[Diagnostics.ProcessStartInfo]::new((Join-Path $PSHOME 'pwsh.exe'));$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
	$start.ArgumentList.Add('-NoProfile');$start.ArgumentList.Add('-EncodedCommand');$start.ArgumentList.Add([Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command)))
	$start.Environment['TERRAENGINE_EDITOR_PREFS_SESSION_PATH']=$prefs;$start.Environment['TERRAENGINE_SUPPRESS_PROJECT_RECENTS']='1'
	$owner=[Diagnostics.Process]::Start($start)
	$clock=[Diagnostics.Stopwatch]::StartNew();$lastError='No owned editor listener.'
	do {
		try {
			RblRequire (!$owner.HasExited) 'Editor watchdog exited before readiness.'
			$listeners=@(Get-NetTCPConnection -LocalPort $port -State Listen -ErrorAction SilentlyContinue)
			RblRequire ($listeners.Count -eq 1) 'Editor endpoint is absent or ambiguous.'
			$process=Get-CimInstance Win32_Process -Filter ('ProcessId='+$listeners[0].OwningProcess)
			RblRequire ($process.ParentProcessId -eq $owner.Id -and [IO.Path]::GetFullPath($process.ExecutablePath) -ieq [IO.Path]::GetFullPath($editor)) 'Listener is not the owned installed editor.'
			$editorProcess=[Diagnostics.Process]::GetProcessById($listeners[0].OwningProcess)
			$hello=RblEditorRpc 'initialize' @{protocolVersion='2025-03-26';clientInfo=@{name='readback-lifetime-editor';version='1'}}
			RblRequire ($hello.response.result.serverInfo.name -ceq 'TerraEngine Editor') 'Unexpected owned MCP server identity.'
			$beforeProject=RblEditorProject $project
			RblRequire ([IO.Path]::GetFullPath($beforeProject.activeScenePath) -ieq [IO.Path]::GetFullPath($expectedScene) -and !$beforeProject.sceneDirty) 'Owned authored scene is not the clean fixture scene.'
			$ready=$true;break
		} catch {$lastError=$_.Exception.Message;Start-Sleep -Milliseconds 200}
	} while($clock.Elapsed.TotalSeconds -lt 180)
	RblRequire $ready "INCOMPLETE: Owned installed editor readiness failed: $lastError"
	$processProof=[pscustomobject]@{pid=$editorProcess.Id;startUtc=$editorProcess.StartTime.ToUniversalTime().ToString('o');parentPid=$owner.Id;executablePath=$editor;port=$port}
	$beforeObjects=RblEditorObjects
	$warmup=RblEditorCapture 0 2 3 'camera.capture_custom';$previous=$warmup.backendTarget
	for($i=1;$i -le $Iterations;$i++) {
		$width=if($i%2){3}else{2};$height=if($i%2){2}else{3};$operation=if($i%2){'game_view.capture'}else{'camera.capture_custom'}
		$row=RblEditorCapture $i $width $height $operation
		RblRequire ($row.backendTarget.rendererSessionGeneration -eq $previous.rendererSessionGeneration -and $row.backendTarget.targetGeneration -gt $previous.targetGeneration -and ($row.backendTarget.sourceWidth -ne $previous.sourceWidth -or $row.backendTarget.sourceHeight -ne $previous.sourceHeight)) 'Actual dedicated capture target did not resize within the same renderer session.'
		$resizeCount++;$rows.Add($row);$previous=$row.backendTarget
	}
	$afterProject=RblEditorProject $project;$afterObjects=RblEditorObjects;$sceneAfter=RblPin $expectedScene
	foreach($key in @('projectGeneration','sceneGeneration','projectRoot','activeScenePath','sceneDirty')){RblRequire ((ConvertTo-Json -InputObject $beforeProject.$key -Compress) -ceq (ConvertTo-Json -InputObject $afterProject.$key -Compress)) "Read-only captures changed project/scene state: $key"}
	RblRequire ((ConvertTo-Json -InputObject $beforeObjects -Depth 50 -Compress) -ceq (ConvertTo-Json -InputObject $afterObjects -Depth 50 -Compress)) 'Read-only captures changed authored object identity/hierarchy/components.'
	RblCheckPin $sceneBefore
	RblWrite (Join-Path $work 'authoring-observations.json') @{beforeProject=$beforeProject;afterProject=$afterProject;beforeObjects=$beforeObjects;afterObjects=$afterObjects;sceneBeforePin=$sceneBefore;sceneAfterPin=$sceneAfter}
	$exitCode=0
} catch {$failure=$_.Exception.Message;$exitCode=if($failure.StartsWith('INCOMPLETE:')){2}else{1}}
finally {
	# Normal close is one session boundary, after all captures. Killing a process
	# tree is bounded failure cleanup and can never qualify as normal shutdown.
	if($null -ne $editorProcess -and !$editorProcess.HasExited) {
		try{$normalClose=$editorProcess.CloseMainWindow() -and $editorProcess.WaitForExit(10000)}catch{if(!$failure){$failure=$_.Exception.Message}}
	}
	if($null -ne $owner) {
		if(!$normalClose -and !$owner.HasExited){$owner.Kill($true)}
		$null=$owner.WaitForExit(15000)
		if($null -ne $work -and (Test-Path -LiteralPath (Join-Path $work 'editor-process/watchdog.json'))) {
			$watch=Get-Content -LiteralPath (Join-Path $work 'editor-process/watchdog.json') -Raw | ConvertFrom-Json
		}
		$normalClose=$normalClose -and $owner.HasExited -and $owner.ExitCode -eq 0 -and $null -ne $watch -and $watch.exitCode -eq 0 -and !$watch.timedOut
		$owner.Dispose()
	}
}
if($exitCode -eq 0 -and !$normalClose){$exitCode=1;$failure='Owned editor did not close normally after captures.'}
if($exitCode -eq 0) {
	try{$validationObservation=RblCheckEditorValidationText ([IO.File]::ReadAllText((Join-Path $work 'editor-process/stdout.log'))) ([IO.File]::ReadAllText((Join-Path $work 'editor-process/stderr.log')))}
	catch{$failure=$_.Exception.Message;$exitCode=if($failure.StartsWith('INCOMPLETE:')){2}else{1}}
}
if($null -ne $work -and (Test-Path -LiteralPath $work)) {
	$status=if($exitCode -eq 0){'Passed'}elseif($exitCode -eq 2){'Incomplete'}else{'Failed'}
	$rawPath=Join-Path $work 'editor-observations.json'
	RblWrite $rawPath @{runNonce=$nonce;observations=$rows.ToArray();warmup=$warmup;resizeCount=$resizeCount;normalCloseCount=$(if($normalClose){1}else{0});capturesBeforeNormalClose=$rows.Count;status=$status;reason=$failure}
	if($exitCode -eq 0) {
		try {
			$receipt=[pscustomobject]@{schemaVersion='readback-lifetime-editor-v1';status='Passed';protocolFixture=$false;runNonce=$nonce;installManifestSha256=$install.manifestPin.sha256;
				executablePath=$editor;processId=$processProof.pid;process=$processProof;watchdogPin=(RblPin (Join-Path $work 'editor-process/watchdog.json'));rawObservationsPin=(RblPin $rawPath);
				iterations=$Iterations;observations=$rows.ToArray();warmup=$warmup;resizeCount=$resizeCount;normalCloseCount=1;capturesBeforeNormalClose=$rows.Count;
				endpointOwnershipVerified=$ready;isolatedProjectVerified=$ready;authoredObjectsUnchanged=$true;sceneFileUnchanged=$true;sceneDirtyUnchanged=$true;normalClose=$normalClose;
				sceneBeforePin=$sceneBefore;sceneAfterPin=$sceneAfter;authoringPin=(RblPin (Join-Path $work 'authoring-observations.json'));materializationPin=(RblPin $materializationPath);
				validationObservation=$validationObservation;
				stdoutPin=(RblPin (Join-Path $work 'editor-process/stdout.log'));stderrPin=(RblPin (Join-Path $work 'editor-process/stderr.log'))}
			$context=RblContext $install $work $nonce $editor $Iterations
			$verification=Test-ReadbackEditorReceipt $receipt $context
			foreach($pin in @($install.manifestPin)+@($install.pins)){RblCheckPin $pin}
			RblWrite (Join-Path $work 'verification.json') $verification
		} catch {$exitCode=1;$failure=$_.Exception.Message;$receipt=$null}
	}
	if($null -eq $receipt){$receipt=[pscustomobject]@{schemaVersion='readback-lifetime-editor-v1';status=$(if($exitCode -eq 2){'Incomplete'}else{'Failed'});reason=$failure;protocolFixture=$false;runNonce=$nonce;rawObservationsPin=(RblPin $rawPath);iterations=$Iterations;observations=$rows.ToArray();normalClose=$normalClose}}
	if($null -ne $output -and !(Test-Path -LiteralPath $output)){RblWrite $output $receipt}
}
Write-Output "Installed editor readback exit $exitCode; evidence: $output"
if($failure){Write-Output $failure}
exit $exitCode
