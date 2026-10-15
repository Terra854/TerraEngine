<#
Collects reference calibration or candidate timings from owned installed editors.
Requires installed runtime-memory tools, PowerShell 7, a usable desktop/Vulkan
adapter, and a fresh WorkRoot outside both installs. No user project is opened.
Example: & ./InvokeRuntimeMemorySafetyTimingMatrix.ps1 -Mode Calibrate
  -RuntimeInstallRoot C:/Reference/Build -ToolInstallRoot C:/Candidate/Build
  -WorkRoot C:/Evidence/fresh -OutputPath C:/Evidence/reference-calibration.json
The fixed three-scope timing contract is separate from complete feature parity.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory)][ValidateSet('Calibrate','Compare')][string]$Mode,
	[Parameter(Mandatory)][string]$RuntimeInstallRoot,
	[Parameter(Mandatory)][string]$ToolInstallRoot,
	[Parameter(Mandatory)][string]$WorkRoot,
	[Parameter(Mandatory)][string]$OutputPath,
	[string]$CalibrationPath='',
	[string]$RuntimeIdentityPath='',
	[ValidateRange(1024,65535)][int]$McpPort=43186,
	[ValidateRange(30,600)][int]$StartupTimeoutSeconds=180,
	[switch]$ProbeOnly
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$utf8=[Text.UTF8Encoding]::new($false)
function Require([bool]$Condition,[string]$Message){if(!$Condition){throw $Message}}
function Hash([string]$Path){(Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()}
function Pin([string]$Path){[pscustomobject]@{path=[IO.Path]::GetFullPath($Path);sha256=(Hash $Path);bytes=(Get-Item -LiteralPath $Path).Length}}
function Canonical($Value){
	# Hash table iteration can differ between PowerShell processes. Canonical
	# property order makes identical settings hash identically on both builds.
	if($null -eq $Value){return $null}
	# Command results can wrap scalar strings in PSObject. Preserve scalar
	# values before considering their adapted Length/other object properties.
	if($Value -is [string] -or $Value -is [ValueType]){return $Value}
	if($Value -is [Collections.IDictionary]){
		$result=[ordered]@{};$keys=[string[]]@($Value.Keys);[Array]::Sort($keys,[StringComparer]::Ordinal)
		foreach($key in $keys){$result[$key]=Canonical $Value[$key]};return $result
	}
	if($Value -is [Array] -or $Value -is [Collections.IList]){return ,@($Value|ForEach-Object {Canonical $_})}
	if($Value -is [pscustomobject]){
		# Empty native JSON objects have no properties; enumerate safely under
		# strict mode instead of asking an empty property collection for Name.
		$result=[ordered]@{};$keys=[string[]]@($Value.PSObject.Properties|ForEach-Object {$_.Name});[Array]::Sort($keys,[StringComparer]::Ordinal)
		foreach($key in $keys){$result[$key]=Canonical $Value.$key};return $result
	}
	return $Value
}
function JsonHash($Value){
	$bytes=$utf8.GetBytes(((Canonical $Value)|ConvertTo-Json -Depth 60 -Compress))
	[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
}
function JsonWrite([string]$Path,$Value){
	Require (!(Test-Path -LiteralPath $Path)) 'Evidence output must be fresh.'
	[IO.File]::WriteAllText($Path,((Canonical $Value)|ConvertTo-Json -Depth 100),$utf8)
}
function NoRedirect([string]$Path){
	$cursor=[IO.Path]::GetFullPath($Path)
	while($cursor){
		if(Test-Path -LiteralPath $cursor){Require (!((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) 'Redirected validation path.'}
		$cursor=[IO.Path]::GetDirectoryName($cursor)
	}
}
function Below([string]$Child,[string]$Root){$Child.StartsWith($Root.TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)}
function Quote([string]$Value){"'"+$Value.Replace("'","''")+"'"}
$runtime=[IO.Path]::GetFullPath($RuntimeInstallRoot)
$toolInstall=[IO.Path]::GetFullPath($ToolInstallRoot)
$work=[IO.Path]::GetFullPath($WorkRoot)
$output=[IO.Path]::GetFullPath($OutputPath)
foreach($path in @($runtime,$toolInstall,$work,$output)){NoRedirect $path}
Require ((Test-Path -LiteralPath $runtime -PathType Container) -and (Test-Path -LiteralPath $toolInstall -PathType Container)) 'Missing installed runtime/tools.'
Require (!(Test-Path -LiteralPath $work) -and !(Test-Path -LiteralPath $output)) 'WorkRoot and OutputPath must be fresh.'
foreach($install in @($runtime,$toolInstall)){
	Require ($work -ine $install -and !(Below $work $install) -and !(Below $install $work)) 'WorkRoot overlaps installed data.'
	Require ($output -ine $install -and !(Below $output $install)) 'OutputPath overlaps installed data.'
}
$tools=Join-Path $toolInstall 'Tests/Tools/RuntimeMemorySafety'
Require ([IO.Path]::GetFullPath($PSCommandPath) -ieq (Join-Path $tools 'InvokeRuntimeMemorySafetyTimingMatrix.ps1')) 'Invoke the installed timing matrix tool.'
$editor=Join-Path $runtime 'Editor/TerraEngine-Editor.exe'
$inspector=Join-Path $tools 'TerraEngine-RuntimeMemorySafetyTimingInspector.exe'
$materializer=Join-Path $tools 'MaterializeRuntimeMemorySafetyFixture.ps1'
$watchdog=Join-Path $toolInstall 'Tests/Tools/Correctness/RunWithWatchdog.ps1'
foreach($path in @($editor,$inspector,$materializer,$watchdog)){Require (Test-Path -LiteralPath $path -PathType Leaf) 'Installed matrix dependency missing.'}
$inventory=Get-Content (Join-Path $tools 'runtime-memory-safety-artifacts.json') -Raw|ConvertFrom-Json
foreach($row in $inventory.files){
	$path=[IO.Path]::GetFullPath((Join-Path $toolInstall $row.path));Require (Below $path $toolInstall) 'Manifest path escaped install.'
	NoRedirect $path;Require ((Get-Item -LiteralPath $path).Length -eq $row.length -and (Hash $path) -ceq $row.sha256.ToLowerInvariant()) ('Installed hash mismatch: '+$row.path)
}
# The comparator's CLI parameters share the dot-source scope. Preserve entry
# choices before importing its library functions through the capture adapter.
$matrixEntryMode=$Mode;$matrixEntryCalibration=$CalibrationPath
. (Join-Path $tools 'CaptureRuntimeMemorySafetyTiming.ps1')
$Mode=$matrixEntryMode;$CalibrationPath=$matrixEntryCalibration
if($Mode -ceq 'Compare'){Require (Test-Path -LiteralPath $CalibrationPath -PathType Leaf) 'Candidate collection requires a sealed calibration.'}
if($Mode -ceq 'Calibrate'){
	Require (Test-Path -LiteralPath $RuntimeIdentityPath -PathType Leaf) 'Reference collection requires the immutable archive receipt.'
	$referenceIdentity=Get-Content -LiteralPath $RuntimeIdentityPath -Raw|ConvertFrom-Json
	Require ([IO.Path]::GetFullPath($referenceIdentity.referenceInstall) -ieq $runtime) 'Archive receipt identifies a different archive.'
}
[IO.Directory]::CreateDirectory($work)|Out-Null
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($output))|Out-Null
$environment=[ordered]@{
	os=[Environment]::OSVersion.VersionString;processors=@(Get-CimInstance Win32_Processor|Select-Object Name,NumberOfLogicalProcessors)
	adapters=@(Get-CimInstance Win32_VideoController|Select-Object Name,DriverVersion,PNPDeviceID)
	powerScheme=(& powercfg /getactivescheme|Out-String).Trim();workerPolicy='Unchanged hardware-default executor configuration'
}
JsonWrite (Join-Path $work 'environment.json') $environment
$runtimePins=@($editor,(Join-Path $runtime 'Editor/TerraEngine.dll'),(Join-Path $runtime 'Editor/glfw3.dll'))|ForEach-Object{Pin $_}
$selfHash=Hash $PSCommandPath
$receipts=[Collections.Generic.List[string]]::new()
$script:rpcId=0;$script:sessionDirectory='';$script:editorProcess=$null
function Rpc([string]$Method,$Parameters=@{},[int]$Timeout=30){
	$request=@{jsonrpc='2.0';id=(++$script:rpcId);method=$Method;params=$Parameters}
	$response=(Invoke-WebRequest -Uri "http://127.0.0.1:$McpPort/mcp" -Method Post -ContentType 'application/json' -Body ($request|ConvertTo-Json -Depth 100 -Compress) -TimeoutSec $Timeout).Content|ConvertFrom-Json
	# Preserve native envelopes and timestamps; images remain separate artifacts.
	$line=@{utc=[DateTime]::UtcNow.ToString('o');request=$request;response=$response}|ConvertTo-Json -Depth 100 -Compress
	[IO.File]::AppendAllText((Join-Path $script:sessionDirectory 'rpc.jsonl'),$line+"`n",$utf8)
	Require ($null -eq $response.PSObject.Properties['error']) ('MCP error for '+$Method)
	return $response
}
function Tool([string]$Name,$Arguments=@{}){
	$response=Rpc 'tools/call' @{name=$Name;arguments=$Arguments}
	$text=@($response.result.content|Where-Object type -eq 'text')
	Require ($text.Count -eq 1) ('Missing native envelope for '+$Name)
	$envelope=$text[0].text|ConvertFrom-Json
	Require ($envelope.ok -eq $true) ('Native tool failed: '+$Name+' '+$text[0].text)
	return $envelope.data
}
function WaitPlay([string]$Expected){
	$clock=[Diagnostics.Stopwatch]::StartNew()
	do{$state=Tool 'play.state';if($state.currentState -ceq $Expected -and $null -eq $state.pendingTransition){return $state};Start-Sleep -Milliseconds 100}while($clock.Elapsed.TotalSeconds -lt 90)
	throw ('Play transition did not settle: '+$Expected)
}
function AwaitClean([string]$Project){
	$clock=[Diagnostics.Stopwatch]::StartNew()
	do{
		$state=Tool 'project.state'
		Require ($state.hasProject -and [IO.Path]::GetFullPath($state.projectRoot) -ieq $Project) 'Owned project identity changed.'
		$expectedScene=[IO.Path]::GetFullPath((Join-Path $Project 'Assets/Scenes/Scene.tescene'))
		if(!$state.assetImportsActive -and $state.readiness.projectSettingsReady -and $state.readiness.simulationEssentialReady -and
			[IO.Path]::GetFullPath($state.activeScenePath) -ieq $expectedScene){return $state};Start-Sleep -Milliseconds 100
	}while($clock.Elapsed.TotalSeconds -lt 90)
	throw 'Project imports did not settle.'
}
function Position($Vector){
	$cells=@();$offsets=@()
	foreach($value in $Vector){$cell=[Math]::Floor(([double]$value+32768.0)/65536.0);$cells+=$cell.ToString('0',[Globalization.CultureInfo]::InvariantCulture);$offsets+=([double]$value-$cell*65536.0)}
	return @{cell=@{x=$cells[0];y=$cells[1];z=$cells[2]};offsetMetres=$offsets}
}
function CaptureReadiness([string]$Directory){
	$recipe=(Get-Content (Join-Path $runtime 'Tests/Fixtures/RendererEfficiency/workloads.json') -Raw|ConvertFrom-Json).cameras[0]
	$arguments=@{camera=@{viewType=$recipe.viewType;position=(Position $recipe.position);lookAt=(Position $recipe.target);up=$recipe.up;projection='perspective';verticalFovDegrees=$recipe.verticalFovDegrees;nearPlane=$recipe.nearPlane;farPlane=$recipe.farPlane};width=1920;height=1080;format='image/png';freshness='nextFrame';allowBackendFallback=$false;timeoutMs=30000}
	$response=Rpc 'tools/call' @{name='camera.capture_custom';arguments=$arguments} 45
	$text=@($response.result.content|Where-Object type -eq 'text');Require ($text.Count -eq 1) 'Missing readback envelope.'
	$envelope=$text[0].text|ConvertFrom-Json
	Require ($envelope.ok -and $envelope.data.backendReadback -and !$envelope.data.deterministicFallback) 'Readiness lacks a genuine GPU readback.'
	Require ($envelope.data.width -eq 1920 -and $envelope.data.height -eq 1080 -and $envelope.data.frameId -gt 0) 'Dedicated readiness output identity/extent is invalid.'
	$images=@($response.result.content|Where-Object {$_.type -eq 'image' -and $_.mimeType -eq 'image/png'})
	Require ($images.Count -eq 1) 'Readiness omitted native PNG.'
	[IO.File]::WriteAllBytes((Join-Path $Directory 'readiness.png'),[Convert]::FromBase64String($images[0].data))
	JsonWrite (Join-Path $Directory 'readiness.json') @{camera=$arguments;native=$envelope.data;imageSha256=(Hash (Join-Path $Directory 'readiness.png'));fullCollectionParityAcceptance=$false}
}
function ObserveNormalViewBindings([string]$Directory){
	# The common endpoint retains protected target metadata, which may be stale.
	# Preserve that limitation: it is not completion-qualified GPU extent proof.
	$observations=@();$bindings=@()
	foreach($view in @(@{type='Scene';id=1},@{type='Game';id=2})){
		$observed=Tool 'renderer.efficiency.stats' @{viewType=$view.type;viewId=$view.id}
		$observations+=@{view=$view;native=$observed}
		$resolution=@($observed.resolution)
		$bindings+=@{viewType=$view.type;viewId=$view.id;width=$(if($resolution.Count -eq 2){$resolution[0]}else{0});height=$(if($resolution.Count -eq 2){$resolution[1]}else{0})}
	}
	JsonWrite (Join-Path $Directory 'normal-view-bindings.json') @{bindings=$bindings;observations=$observations;proof='Common protected target metadata; freshness/completed GPU extent unavailable';fixedLayoutPolicy='Installed templates and isolated preferences; no UI interactions during collection'}
	Require (@($bindings|Where-Object {$_.width -gt 0 -and $_.height -gt 0}).Count -gt 0) 'Common normal-view target metadata unavailable.'
	return ,$bindings
}
$failure=$null
try{
	foreach($scale in @('small','normal','stress')){
		$roles=if($Mode -ceq 'Calibrate'){@('A','Aprime')}else{@('C')}
		foreach($ordinal in 1..3){foreach($role in $roles){
			if($ProbeOnly -and $receipts.Count -gt 0){break}
			$session=Join-Path $work "$scale-$role$ordinal"
			[IO.Directory]::CreateDirectory($session)|Out-Null;$script:sessionDirectory=$session
			& $materializer -InstallRoot $runtime -WorkRoot (Join-Path $session 'fixture') -Scale $scale *> (Join-Path $session 'materialize.log')
			$fixture=Get-Content (Join-Path $session 'fixture/materialization.json') -Raw|ConvertFrom-Json
			$project=[IO.Path]::GetFullPath($fixture.projectRoot)
			$preferences=Join-Path $session 'editor-preferences.json'
			$settings=[ordered]@{recentProjects=@();autoBuildScripts=$false;editorMcpEnabled=$true;editorMcpWriteModeDefault='writes_enabled';editorMcpListenPort=$McpPort;vulkanValidationLayers=$false;renderDebugDiagnostics=$false;rendererDiagnosticsPreferencesInitialized=$true}
			JsonWrite $preferences $settings
			$owner=$null;$normalClose=$false;$lifecycleError=$null;$ownerExitCode=$null;$watchdogReceipt=$null
			try{
				Require (@(Get-NetTCPConnection -LocalPort $McpPort -State Listen -ErrorAction SilentlyContinue).Count -eq 0) 'MCP port already occupied.'
				# The existing watchdog launches suspended, assigns a Windows process job,
				# and owns the complete child tree. Its parent wrapper stays hidden.
				$command='& '+(Quote $watchdog)+' -Executable '+(Quote $editor)+" -ArgumentList @('--project',"+(Quote $project)+') -WorkingDirectory '+(Quote (Split-Path $editor))+' -TimeoutSeconds 900 -LogDirectory '+(Quote (Join-Path $session 'process'))
				$start=[Diagnostics.ProcessStartInfo]::new((Join-Path $PSHOME 'pwsh.exe'))
				$start.UseShellExecute=$false;$start.CreateNoWindow=$true;$start.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
				$start.ArgumentList.Add('-NoProfile');$start.ArgumentList.Add('-EncodedCommand');$start.ArgumentList.Add([Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command)))
				$start.Environment['TERRAENGINE_EDITOR_PREFS_SESSION_PATH']=$preferences;$start.Environment['TERRAENGINE_SUPPRESS_PROJECT_RECENTS']='1'
				$owner=[Diagnostics.Process]::Start($start)
				$clock=[Diagnostics.Stopwatch]::StartNew();$ready=$false;$startupError='No MCP response'
				do{
					if($owner.HasExited){throw 'Owned editor watchdog exited before readiness.'}
					try{
						$hello=Rpc 'initialize' @{protocolVersion='2025-03-26';clientInfo=@{name='runtime-memory-safety';version='1'}}
						Require ($hello.result.serverInfo.name -ceq 'TerraEngine Editor') 'Unexpected MCP server.'
						$listeners=@(Get-NetTCPConnection -LocalPort $McpPort -State Listen)
						Require ($listeners.Count -eq 1) 'MCP listener ownership ambiguous.'
						$nativeOwner=Get-CimInstance Win32_Process -Filter ("ProcessId="+$listeners[0].OwningProcess)
						Require ($nativeOwner.ParentProcessId -eq $owner.Id -and $nativeOwner.ExecutablePath -ieq $editor) 'MCP listener is not the owned archived/candidate editor.'
						$script:editorProcess=[Diagnostics.Process]::GetProcessById($listeners[0].OwningProcess)
						$state=AwaitClean $project
						if($state.activeScene){$ready=$true;break}
					}catch{$startupError=$_.Exception.Message;Start-Sleep -Milliseconds 200}
				}while($clock.Elapsed.TotalSeconds -lt $StartupTimeoutSeconds)
				Require $ready ('Installed editor/project did not become ready: '+$startupError)
				$null=Tool 'play.enter' @{expectedState='stopped'};$null=WaitPlay 'playing'
				$null=Tool 'play.pause' @{expectedState='playing'};$null=WaitPlay 'paused'
				$null=AwaitClean $project
				CaptureReadiness $session
				$normalBindings=ObserveNormalViewBindings $session
				# These independently retained parent receipts bind the measurements to
				# real process instances, installed bytes and frozen authored inputs.
				$processProof=Join-Path $session 'process-identity.json'
				JsonWrite $processProof @{pid=$script:editorProcess.Id;startUtc=$script:editorProcess.StartTime.ToUniversalTime().ToString('o');parentPid=$owner.Id;executablePath=$editor}
				$fixtureProof=Join-Path $session 'fixture-identity.json'
				JsonWrite $fixtureProof ([ordered]@{manifest=$fixture.fixtureManifestSha256;generator=$fixture.generatorSha256;generated=$fixture.generatedHashes;inputs=$fixture.inputClosure})
				$settingsProof=Join-Path $session 'settings-identity.json'
				JsonWrite $settingsProof ([ordered]@{preferences=$settings;render=$fixture.generatedHashes.'ProjectSettings/RenderSettings.json';normalViewBindings=$normalBindings;layoutIni=(Hash (Join-Path $project 'UserSettings/EditorLayout.ini'));layoutTerra=(Hash (Join-Path $project 'UserSettings/EditorLayout.terra'));matrixTool=$selfHash;watchdog=(Hash $watchdog)})
				$sourceIdentity='runtime-binary-sha256:'+(Hash (Join-Path $runtime 'Editor/TerraEngine.dll'))
				$installIdentity='editor-sha256:'+(Hash $editor)
				$runtimeProof=Join-Path $session 'runtime-identity.json'
				JsonWrite $runtimeProof @{sourceIdentity=$sourceIdentity;installIdentity=$installIdentity;files=$runtimePins;archiveReceipt=$(if($Mode -ceq 'Calibrate'){Pin $RuntimeIdentityPath}else{$null})}
				$metadata=[ordered]@{scale=$scale;role=$role;ordinal=$ordinal;fixtureSha256=(Hash $fixtureProof);settingsSha256=(Hash $settingsProof);cameraLightSha256=(JsonHash @{cameras=$fixture.generatedHashes.'runtime-memory-safety-cameras.json';lights=$fixture.generatedHashes.'runtime-memory-safety-lights.json'});environmentSha256=(Hash (Join-Path $work 'environment.json'));sourceIdentity=$sourceIdentity;installIdentity=$installIdentity;parentEvidence=@{process=(Pin $processProof);environment=(Pin (Join-Path $work 'environment.json'));settings=(Pin $settingsProof);fixture=(Pin $fixtureProof);runtime=(Pin $runtimeProof);orchestrator=(Pin $PSCommandPath)};lifecyclePath=(Join-Path $session 'lifecycle.json')}
				$receipt=Invoke-RuntimeMemorySafetyTimingCapture -Tool {param($name,$argsValue) Tool $name $argsValue} -InspectorPath $inspector -ProjectRoot $project -OutputRoot (Join-Path $session 'capture') -Metadata $metadata
				$receipts.Add((Join-Path $session 'capture/capture-receipt.json'))
			}finally{
				if($null -ne $script:editorProcess -and !$script:editorProcess.HasExited){
					try{
						$play=Tool 'play.state';if($play.currentState -cne 'stopped'){$null=Tool 'play.exit' @{};$null=WaitPlay 'stopped'}
						$state=AwaitClean $project
						if($state.sceneDirty){$null=Tool 'scene.save' @{};$state=AwaitClean $project}
						Require (!$state.sceneDirty) 'Owned scene remained dirty before normal close.'
						$normalClose=$script:editorProcess.CloseMainWindow() -and $script:editorProcess.WaitForExit(10000)
					}catch{$lifecycleError=$_.Exception.Message}
				}
				if($null -ne $owner){
					if(!$normalClose -and !$owner.HasExited){$owner.Kill($true)}
					$null=$owner.WaitForExit(15000)
					if($owner.HasExited){$ownerExitCode=$owner.ExitCode}
					$watchdogPath=Join-Path $session 'process/watchdog.json'
					if(Test-Path -LiteralPath $watchdogPath){$watchdogReceipt=Get-Content -LiteralPath $watchdogPath -Raw|ConvertFrom-Json}
					$normalClose=$normalClose -and $ownerExitCode -eq 0 -and $null -ne $watchdogReceipt -and $watchdogReceipt.exitCode -eq 0 -and !$watchdogReceipt.timedOut
					JsonWrite (Join-Path $session 'lifecycle.json') @{normalClose=$normalClose;reason=$lifecycleError;watchdogExited=$owner.HasExited;exitCode=$ownerExitCode;timedOut=$(if($null -ne $watchdogReceipt){$watchdogReceipt.timedOut}else{$true});watchdog=$watchdogReceipt}
					$owner.Dispose()
				}
				$script:editorProcess=$null
			}
			Require ($normalClose -and !$lifecycleError) 'Owned editor failed normal lifecycle retirement; timings cannot be accepted.'
		}}
		if($ProbeOnly){break}
	}
	foreach($pin in $runtimePins){Require ((Hash $pin.path) -ceq $pin.sha256) 'Archived/candidate runtime changed during collection.'}
	Require ((Hash $PSCommandPath) -ceq $selfHash) 'Matrix tool changed during collection.'
	if($ProbeOnly){JsonWrite $output @{status='ProbeOnly';capture=$receipts.ToArray();performanceAcceptance=$false;calibrationComplete=$false};exit 2}
	$result=if($Mode -ceq 'Calibrate'){New-RuntimeMemorySafetyCalibration -Reference $receipts.ToArray()}else{Compare-RuntimeMemorySafetyTiming -CalibrationPath $CalibrationPath -Candidate $receipts.ToArray()}
	JsonWrite $output $result
	if($Mode -ceq 'Compare' -and !$result.timingPassed){exit 1}
	exit 0
}catch{
	$failure=$_.Exception.Message
	if(!(Test-Path -LiteralPath $output)){JsonWrite $output @{status='Failed';reason=$failure;stack=$_.ScriptStackTrace;captures=$receipts.ToArray();performanceAcceptance=$false;calibrationComplete=$false}}
	Write-Error $failure -ErrorAction Continue
	exit 1
}
