<#
.SYNOPSIS
Run sealed installed texture, settlement and physical-pointer acceptance.
.DESCRIPTION
Prerequisites: BUILD_TESTING configure, full build/install and complete CTest.
Invoke only the installed copy with -AllowLiveAcceptance and a fresh WorkRoot.
The editor uses a fresh authored project, both default layout files, isolated
preferences and real managed callbacks. GPU capture must report backendReadback
and never deterministicFallback. Physical input remains an observed interactive
gate: the parent records screen/logical coordinates and screenshots, compares
texture regions with independent generator colors, and writes live-review.json.
The runner does not inject clicks or infer acceptance from synthetic labels.
Normal approvals remain in the editor UI; no token/policy bypass. Forced cleanup
is failure. Preserve work/evidence; no caller files are removed.
Example: ./Build/Tests/Tools/Correctness/RunInstalledTextureUiAcceptance.ps1
 -InstallRoot ./Build -WorkRoot ./out/validation/texture-ui-live-new -AllowLiveAcceptance
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string]$InstallRoot,
	[Parameter(Mandatory=$true)][string]$WorkRoot,
	[ValidateRange(1024,65535)][int]$McpPort=43149,
	[ValidateRange(30,600)][int]$StageTimeoutSeconds=180,
	[ValidateRange(30,600)][int]$ApprovalTimeoutSeconds=180,
	[ValidateRange(120,3600)][int]$EditorWatchdogSeconds=1800,
	[switch]$AllowLiveAcceptance
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if (!$AllowLiveAcceptance) { throw 'Pass -AllowLiveAcceptance after integrated gates pass.' }
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'Physical installed acceptance requires Windows.' }
$install=[IO.Path]::GetFullPath($InstallRoot)
$ArtifactManifest=Join-Path $install 'correctness-artifacts.json'
$work=[IO.Path]::GetFullPath($WorkRoot)
if (Test-Path -LiteralPath $work) { throw 'WorkRoot must be fresh.' }
function Below([string]$Root,[string]$Child) {
	$prefix=[IO.Path]::GetFullPath($Root).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
	if (![IO.Path]::GetFullPath($Child).StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) { throw "Path escapes owned root: $Child" }
}
Below $install ([IO.Path]::GetFullPath($ArtifactManifest))
$manifest=Get-Content -LiteralPath $ArtifactManifest -Raw | ConvertFrom-Json
if ($manifest.schemaVersion -ne 1 -or !$manifest.builtAtUtc -or !$manifest.files -or !$manifest.minimumCases) { throw 'Unsupported/incomplete artifact manifest.' }
$null=[DateTimeOffset]::Parse($manifest.builtAtUtc)
$inventory=@{}
foreach ($entry in $manifest.files) {
	if ([IO.Path]::IsPathRooted($entry.relativePath) -or $inventory.ContainsKey($entry.relativePath)) { throw 'Invalid/duplicate manifest path.' }
	$path=[IO.Path]::GetFullPath((Join-Path $install $entry.relativePath)); Below $install $path
	if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing installed artifact: $path" }
	if ((Get-Item -LiteralPath $path).Length -ne [long]$entry.length -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Stale/hash-mismatched installed artifact: $path" }
	$inventory[$entry.relativePath]=$path
}
function Installed([string]$Relative) {
	if (!$inventory.ContainsKey($Relative)) { throw "Required asset is not declared in the artifact manifest: $Relative" }
	return $inventory[$Relative]
}

$editor=Installed 'Editor/TerraEngine-Editor.exe'
$engineTests=Installed 'Tests/Engine/TerraEngine-EngineTests.exe'
$watchdog=Installed 'Tests/Tools/Correctness/RunWithWatchdog.ps1'
$slice=Installed 'Tests/Tools/Correctness/RunNativeCorrectnessSlice.ps1'
$generator=Installed 'Tests/Tools/Correctness/TerraEngineTextureUiFixtureGenerator.exe'
$runner=Installed 'Tests/Tools/Correctness/RunInstalledTextureUiAcceptance.ps1'
if ([IO.Path]::GetFullPath($PSCommandPath) -ne $runner) { throw 'Invoke the sealed installed runner.' }
$fixturePrefix='Tests/Tools/Correctness/Fixtures/TextureUiProject/'
$fixtureFiles=@('game.json','Assembly-CSharp.csproj','ProjectSettings/ProjectSettings.terra','ProjectSettings/RenderSettings.json','Assets/Scenes/Startup.tescene','Assets/UI/TranslatedControls.tuiml','Assets/Scripts/TextureUiObserver.cs')
foreach ($file in $fixtureFiles) { $null=Installed ($fixturePrefix+$file) }
foreach ($relative in @('Editor/Managed/TerraEngine.CSharp.dll','Editor/Managed/Analyzers/TerraEngine.Export.Generator.dll','Editor/DotNet/dotnet.exe','Editor/NativeAotNuGet/global.json','Editor/Project/Templates/UserSettings/EditorLayout.ini','Editor/Project/Templates/UserSettings/EditorLayout.terra')) { $null=Installed $relative }
$sdkVersion=(Get-Content -LiteralPath (Installed 'Editor/NativeAotNuGet/global.json') -Raw | ConvertFrom-Json).sdk.version
$null=Installed ('Editor/DotNet/sdk/'+$sdkVersion+'/dotnet.dll')
$null=Installed ('Editor/DotNet/sdk/'+$sdkVersion+'/Sdks/Microsoft.NET.Sdk/Sdk/Sdk.props')
New-Item -ItemType Directory -Path $work | Out-Null
$rpcLog=Join-Path $work 'mcp.jsonl'; $summaryPath=Join-Path $work 'summary.json'
$summary=[ordered]@{schemaVersion=1;passed=$false;builtAtUtc=$manifest.builtAtUtc;manifestSha256=(Get-FileHash -LiteralPath $ArtifactManifest).Hash;installRoot=$install;workRoot=$work;native=@();live=@();failure=$null}
function Record([string]$Name,$Data) { $summary.live+=@{name=$Name;data=$Data} }
function JsonFile([string]$Path,$Value) { $Value | ConvertTo-Json -Depth 40 | Set-Content -LiteralPath $Path -Encoding UTF8 }
function PsQuote([string]$Value) { return "'"+$Value.Replace("'","''")+"'" }
$shell=(Get-Process -Id $PID).Path
# The independent watchdog already owns a suspended-launched child job. Add a
# read-only job-accounting observation to its fresh disposable copy so normal
# close cannot be credited merely because the outer job killed leaked children.
$wrapper=Get-Content -LiteralPath $watchdog -Raw
$anchor='public int Pid { get; private set; }'
if (!$wrapper.Contains($anchor) -or !$wrapper.Contains('pid = $ownedProcess.Pid;')) { throw 'Installed watchdog support schema changed.' }
$accounting=@'
public int Pid { get; private set; }
[StructLayout(LayoutKind.Sequential)] struct Accounting { public long a,b,c,d; public uint faults,total,active,terminated; }
[DllImport("kernel32.dll", SetLastError=true)] static extern bool QueryInformationJobObject(IntPtr job,int info,ref Accounting accounting,int size,IntPtr returned);
public uint ActiveProcesses { get { var value=new Accounting(); if(!QueryInformationJobObject(job,1,ref value,Marshal.SizeOf(typeof(Accounting)),IntPtr.Zero)) throw Error("Job accounting"); return value.active; } }
'@
$wrapper=$wrapper.Replace($anchor,$accounting).Replace('pid = $ownedProcess.Pid;', 'activeProcessesAfterRootExit = $ownedProcess.ActiveProcesses; pid = $ownedProcess.Pid;')
$normalExit='if ($watchdogExitCode -ne 124) { $watchdogExitCode = $ownedProcess.ExitCode() }'
if (!$wrapper.Contains($normalExit)) { throw 'Installed watchdog completion schema changed.' }
$wrapper=$wrapper.Replace($normalExit,$normalExit+@'

	if (-not $watchdogTimedOut) {
		$settlementClock = [Diagnostics.Stopwatch]::StartNew()
		while ($ownedProcess.ActiveProcesses -ne 0 -and $settlementClock.ElapsedMilliseconds -lt 1000) { Start-Sleep -Milliseconds 10 }
	}
'@)
$ownedWrapper=Join-Path $work 'owned-editor-watchdog.ps1'; Set-Content -LiteralPath $ownedWrapper -Value $wrapper -Encoding UTF8
$script:rpcId=0; $script:editorProcess=$null; $script:wrapperProcess=$null
function Rpc([string]$Method,$Parameters=@{}) {
	$script:rpcId++
	$request=@{jsonrpc='2.0';id=$script:rpcId;method=$Method;params=$Parameters}
	try {
		$response=Invoke-WebRequest -UseBasicParsing -Uri "http://127.0.0.1:$McpPort/mcp" -Method Post -ContentType 'application/json' -Body ($request | ConvertTo-Json -Depth 40 -Compress) -TimeoutSec 30
		$result=$response.Content | ConvertFrom-Json
		@{request=$request;response=$result;utc=[DateTime]::UtcNow.ToString('o')} | ConvertTo-Json -Depth 40 -Compress | Add-Content -LiteralPath $rpcLog -Encoding UTF8
		if ($result.PSObject.Properties['error']) { throw ('JSON-RPC failure: '+($result.error | ConvertTo-Json -Compress)) }
		return $result
	} catch { @{request=$request;error=$_.Exception.Message} | ConvertTo-Json -Depth 40 -Compress | Add-Content -LiteralPath $rpcLog -Encoding UTF8; throw }
}
function Envelope($Response) {
	if (!$Response.result.content -or !$Response.result.content[0].text) { throw 'Missing MCP text envelope.' }
	return $Response.result.content[0].text | ConvertFrom-Json
}
function Tool([string]$Name,$Arguments=@{},[switch]$ExpectFailure) {
	$response=Envelope (Rpc 'tools/call' @{name=$Name;arguments=$Arguments})
	if ($response.PSObject.Properties['status'] -and $response.status -eq 'waiting_approval') {
		$id=$response.data.approvalId
		Write-Host "Approve $Name ($id) in the installed editor's normal approval UI."
		$deadline=[DateTime]::UtcNow.AddSeconds($ApprovalTimeoutSeconds); $resolved=$false
		while ([DateTime]::UtcNow -lt $deadline) {
			$resource=Rpc 'resources/read' @{uri='terraengine://approvals/recent'}
			$recent=$resource.result.contents[0].text | ConvertFrom-Json
			$match=@($recent.approvals | Where-Object { $_.approvalId -eq $id -and $_.dispatchCompleted })
			if ($match.Count -eq 1) { if (!$match[0].approved) { throw "Approval $id was denied/stale." }; $response=$match[0].dispatchResult; $resolved=$true; break }
			Start-Sleep -Milliseconds 100
		}
		if (!$resolved) { throw "Approval $id did not resolve before the deadline." }
	}
	if ($ExpectFailure) { if ($response.ok) { throw "$Name unexpectedly succeeded." } }
	elseif (!$response.ok) { throw "$Name failed: $($response | ConvertTo-Json -Depth 20 -Compress)" }
	return $response
}
function Job([string]$Id,[string]$Correlation,[string[]]$Allowed=@('succeeded')) {
	$deadline=[DateTime]::UtcNow.AddSeconds($StageTimeoutSeconds)
	while ([DateTime]::UtcNow -lt $deadline) {
		$value=(Tool 'jobs.get' @{jobId=$Id}).data.job
		if ($value.correlationId -ne $Correlation) { throw 'Job correlation was lost.' }
		if ($value.state -in @('succeeded','failed','canceled')) { if ($value.state -notin $Allowed) { throw "Unexpected job outcome $($value.state)." }; return $value }
		Start-Sleep -Milliseconds 100
	}
	throw "Job $Id did not settle before its deadline."
}
function ReadyScripts {
	# Startup and an explicit rebuild can overlap. A completed job receipt is
	# not proof that a later queued build has finished; observe the live owner.
	$deadline=[DateTime]::UtcNow.AddSeconds($StageTimeoutSeconds)
	while ([DateTime]::UtcNow -lt $deadline) {
		$value=(Tool 'script.status').data
		if ($value.coordinatorState -eq 'ready' -and $value.readiness.state -eq 'ready' -and
			!$value.readiness.queuedBuild -and $value.readiness.gameplayAssemblyLoaded -and $value.loadedOutput.published) { return $value }
		Start-Sleep -Milliseconds 100
	}
	throw 'The installed scripting owner did not reach settled published readiness.'
}


function CloseEditor {
	if (!$script:wrapperProcess) { return }
	if (!$script:editorProcess) { throw 'Editor identity unavailable; normal close cannot be proved.' }
	$script:editorProcess.Refresh()
	if (!$script:editorProcess.HasExited -and !$script:editorProcess.CloseMainWindow()) { throw 'Normal editor window close was not accepted.' }
	if (!$script:wrapperProcess.WaitForExit(60000)) { throw 'Normal editor close did not drain within 60 seconds.' }
	$watch=Get-Content -LiteralPath (Join-Path $script:editorLogs 'watchdog.json') -Raw | ConvertFrom-Json
	Record 'normal-close' $watch
	if ($watch.timedOut -or $watch.exitCode -ne 0 -or $watch.activeProcessesAfterRootExit -ne 0) { throw 'Editor close used forced cleanup or left active descendants.' }
	$script:editorProcess=$null; $script:wrapperProcess.Dispose(); $script:wrapperProcess=$null
}

# The observer reads real production UI state. Retain delete sharing while it
# atomically replaces its JSON, so evidence collection cannot fault the script.
function Observation {
	$stream=$null; $reader=$null
	try {
		$stream=[IO.File]::Open((Join-Path $script:observation 'texture-ui.json'),[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
		$reader=[IO.StreamReader]::new($stream)
		return ($reader.ReadToEnd() | ConvertFrom-Json)
	} finally { if ($reader) { $reader.Dispose() } elseif ($stream) { $stream.Dispose() } }
}
function Capture([string]$Name,[switch]$Orthographic) {
	$toolName='game_view.capture'
	# Read the displayed Game target: nextFrame uses a dedicated camera render
	# whose intentionally empty UI batch cannot verify these flat UI textures.
	$arguments=@{width=1280;height=720;format='image/png';freshness='latestCompleted';includeOverlays=$true;allowBackendFallback=$false}
	if ($Orthographic) {
		# This separate camera/readback control intentionally omits UI. Only the
		# observed live Game target establishes the texture and pointer witnesses.
		$toolName='camera.capture_custom'
		$arguments.freshness='nextFrame'
		$arguments.camera=@{position=@{cell=@{x='0';y='0';z='0'};offsetMetres=@(0,1,4)};forward=@(0,0,-1);up=@(0,1,0);projection='orthographic';orthographicHeight=10;viewType='Game'}
	}
	$raw=Rpc 'tools/call' @{name=$toolName;arguments=$arguments}
	$value=Envelope $raw
	if (!$value.ok -or !$value.data.backendReadback -or $value.data.deterministicFallback) { throw 'Capture is not a genuine backend GPU readback.' }
	$images=@($raw.result.content | Where-Object { $_.type -eq 'image' -and $_.mimeType -eq 'image/png' })
	if ($images.Count -ne 1) { throw 'Expected one GPU PNG.' }
	$bytes=[Convert]::FromBase64String($images[0].data)
	if ($bytes.Length -lt 8 -or [BitConverter]::ToString($bytes,0,8) -ne '89-50-4E-47-0D-0A-1A-0A') { throw 'Invalid PNG readback.' }
	$path=Join-Path $work ($Name+'.png'); [IO.File]::WriteAllBytes($path,$bytes)
	Record $Name @{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash;capture=$value.data}
	return $path
}
try {
	# Execute installed lanes against the colocated, sealed DLL; source-tree
	# checks cannot establish installed runtime closure or corpus packaging.
	foreach ($lane in @(@('Engine.Resource.Texture.Bounds',8),@('Engine.Resource.Texture.Rgba',5),@('Engine.JobGraph.FailureSettlement',6),@('Engine.UI.HitTesting',6))) {
		$property=$manifest.minimumCases.PSObject.Properties[$lane[0]]
		if (!$property -or [int]$property.Value -ne $lane[1]) { throw 'Installed native floor changed.' }
		& $slice -Executable $engineTests -Filter $lane[0] -MinimumCases $lane[1] -TimeoutSeconds 120 -LogDirectory (Join-Path $work ('native-'+$lane[0]))
		if ($LASTEXITCODE -ne 0) { throw "Native lane failed: $($lane[0])" }
		$summary.native+=@{filter=$lane[0];minimumCases=$lane[1]}
	}
	$project=Join-Path $work 'project'; $script:observation=Join-Path $work 'observations'
	New-Item -ItemType Directory -Path $project,$script:observation | Out-Null
	foreach ($relative in $fixtureFiles) {
		$target=Join-Path $project $relative
		New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
		Copy-Item -LiteralPath (Installed ($fixturePrefix+$relative)) -Destination $target
	}
	# Generator owns a new directory and produces legal TETF inputs, rather than
	# copying historical texture packages whose provenance cannot be proved.
	& $watchdog -Executable $generator -ArgumentList @('--output-root',(Join-Path $project 'Assets/Textures')) -WorkingDirectory (Split-Path $generator) -LogDirectory (Join-Path $work 'generator') -TimeoutSeconds 30
	if ($LASTEXITCODE -ne 0) { throw 'Installed fixture generation failed.' }
	$generated=@(Get-ChildItem -LiteralPath (Join-Path $project 'Assets/Textures') -Filter '*.tetf')
	if ($generated.Count -ne 8) { throw 'Generator did not produce the eight authored textures.' }
	foreach ($name in @('witness-linear.tetf','quadrants-linear.tetf','edges-linear.tetf','alpha-linear.tetf','midtone-srgb.tetf','mips-linear.tetf','control-bc7.tetf','control-astc.tetf')) {
		if ($name -notin $generated.Name) { throw "Generated texture missing: $name" }
	}
	Record 'generated-textures' @($generated | ForEach-Object { @{name=$_.Name;length=$_.Length;sha256=(Get-FileHash -LiteralPath $_.FullName).Hash} })
	$userSettings=Join-Path $project 'UserSettings'; New-Item -ItemType Directory -Path $userSettings | Out-Null
	foreach ($name in @('EditorLayout.ini','EditorLayout.terra')) { Copy-Item -LiteralPath (Installed ('Editor/Project/Templates/UserSettings/'+$name)) -Destination (Join-Path $userSettings $name) }
	$references=Join-Path $project 'Build/Assembly-CSharp/References'; New-Item -ItemType Directory -Force -Path $references | Out-Null
	Copy-Item -LiteralPath (Installed 'Editor/Managed/TerraEngine.CSharp.dll') -Destination $references
	Copy-Item -LiteralPath (Installed 'Editor/Managed/Analyzers/TerraEngine.Export.Generator.dll') -Destination $references
	New-Item -ItemType Directory -Path (Join-Path $project 'Library') | Out-Null
	Set-Content -LiteralPath (Join-Path $project 'Library/LastScene.txt') -Value 'Scenes/Startup.tescene' -Encoding UTF8
	$prefs=Join-Path $work 'editor-preferences.json'
	JsonFile $prefs @{recentProjects=@();autoBuildScripts=$false;editorMcpEnabled=$true;editorMcpWriteModeDefault='writes_enabled';editorMcpListenPort=$McpPort;vulkanValidationLayers=$true;renderDebugDiagnostics=$true;rendererDiagnosticsPreferencesInitialized=$true}
	$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,$McpPort); try { $listener.Start() } finally { $listener.Stop() }
	$script:editorLogs=Join-Path $work 'editor-logs'
	$command='& '+(PsQuote $ownedWrapper)+' -Executable '+(PsQuote $editor)+' -ArgumentList @('+(PsQuote '--project')+','+(PsQuote $project)+') -VisibleWindow -WorkingDirectory '+(PsQuote (Split-Path $editor))+' -LogDirectory '+(PsQuote $script:editorLogs)+' -TimeoutSeconds '+$EditorWatchdogSeconds+'; exit $LASTEXITCODE'
	$start=[Diagnostics.ProcessStartInfo]::new(); $start.FileName=$shell
	$start.Arguments='-NoProfile -ExecutionPolicy Bypass -EncodedCommand '+[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
	$start.UseShellExecute=$false; $start.CreateNoWindow=$true; $start.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
	$start.EnvironmentVariables['TERRAENGINE_EDITOR_PREFS_SESSION_PATH']=$prefs
	$start.EnvironmentVariables['TERRAENGINE_SUPPRESS_PROJECT_RECENTS']='1'
	$start.EnvironmentVariables['TERRAENGINE_TEXTURE_UI_OBSERVATION_ROOT']=$script:observation
	$editorStartedUtc=[DateTime]::UtcNow
	$script:wrapperProcess=[Diagnostics.Process]::Start($start)
	$deadline=[DateTime]::UtcNow.AddSeconds($StageTimeoutSeconds); $ready=$false
	while ([DateTime]::UtcNow -lt $deadline) {
		if ($script:wrapperProcess.HasExited) { throw 'Owned editor exited before bridge readiness.' }
		if (!$script:editorProcess) {
			$children=@(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($script:wrapperProcess.Id)" | Where-Object { $_.ExecutablePath -eq $editor })
			if ($children.Count -eq 1) { $script:editorProcess=Get-Process -Id $children[0].ProcessId }
		}
		try { $hello=Rpc 'initialize' @{protocolVersion='2025-03-26';clientInfo=@{name='texture-ui-acceptance';version='1'}}; if ($script:editorProcess -and $hello.result.serverInfo.name -eq 'TerraEngine Editor') { $ready=$true; break } } catch { Start-Sleep -Milliseconds 250 }
	}
	if (!$ready) { throw 'Installed editor/bridge did not become ready.' }
	$listeners=@(Get-NetTCPConnection -LocalPort $McpPort -State Listen -ErrorAction Stop)
	if (!$listeners.Count -or @($listeners | Where-Object { $_.OwningProcess -ne $script:editorProcess.Id }).Count) { throw 'Loopback endpoint is not owned by the editor.' }
	$state=(Tool 'project.state').data
	if (!$state.projectRoot -or [IO.Path]::GetFullPath($state.projectRoot) -ne $project) { throw 'Editor did not load the fresh fixture.' }
	Record 'owned-project-and-endpoint' @{editorPid=$script:editorProcess.Id;projectRoot=$project;port=$McpPort}
	$compile=Tool 'script.compile' @{profile='RelWithDebInfo';correlationId='texture-ui-managed-build'}
	Record 'managed-build' (Job $compile.data.jobId 'texture-ui-managed-build')
	Record 'scripts-ready' (ReadyScripts)
	$state=(Tool 'project.state').data
	if ($state.activeScene -ne 'Startup') { Record 'open-fixture' (Tool 'scene.open' @{path='Assets/Scenes/Startup.tescene'}) }
	$state=(Tool 'project.state').data
	if ($state.activeScene -ne 'Startup') { throw 'Authored Startup scene is not active.' }
	Tool 'play.enter' @{expectedState='stopped'} | Out-Null
	Record 'play-ready' (ReadyScripts)
	$deadline=[DateTime]::UtcNow.AddSeconds($StageTimeoutSeconds); $observed=$null
	while ([DateTime]::UtcNow -lt $deadline) { try { $observed=Observation; if ($observed.ready -and $observed.updates -gt 0) { break } } catch {}; Start-Sleep -Milliseconds 100 }
	if (!$observed -or !$observed.ready) { throw 'No real managed UI observation arrived.' }
	if ($observed.translatedClicks -ne 0 -or $observed.frontClicks -ne 0 -or $observed.backClicks -ne 0 -or $observed.hiddenClicks -ne 0) { throw 'Physical review must start from fresh zero counters.' }
	Record 'initial-observation' $observed
	# Fresh default layouts select Scene View. Wait for the screenshot reviewer
	# to activate and observe Game View before reading its displayed color target.
	JsonFile (Join-Path $work 'pointer-ready.json') @{editorPid=$script:editorProcess.Id;port=$McpPort;projectRoot=$project;observationRoot=$script:observation;logicalTargets=@(@{name='translated';x=90;y=10},@{name='front';x=90;y=140},@{name='hidden';x=90;y=180})}
	Write-Host "Activate and observe Game View: $work. Record game-view-ready.json before GPU capture."
	$viewReadyPath=Join-Path $work 'game-view-ready.json'
	$deadline=$editorStartedUtc.AddSeconds($EditorWatchdogSeconds-60)
	while (!(Test-Path -LiteralPath $viewReadyPath) -and [DateTime]::UtcNow -lt $deadline) {
		if ($script:wrapperProcess.HasExited) { throw 'Editor exited before displayed Game View readiness.' }
		Start-Sleep -Milliseconds 250
	}
	if (!(Test-Path -LiteralPath $viewReadyPath)) { throw 'Displayed Game View review remains pending.' }
	$viewReady=Get-Content -LiteralPath $viewReadyPath -Raw | ConvertFrom-Json
	if ($viewReady.observed -ne $true -or $viewReady.editorPid -ne $script:editorProcess.Id) { throw 'Game View readiness must refer to the observed owned editor.' }
	Record 'displayed-game-view-ready' $viewReady
	$null=Capture 'initial-gpu'
	$null=Capture 'orthographic-gpu' -Orthographic
	# A screenshot reviewer controls this gate after actual physical actions and
	# independent GPU color checks. Pending is deliberately never a green result.
	Write-Host "Physical/GPU review ready: $work. Record real observations in live-review.json."
	$deadline=$editorStartedUtc.AddSeconds($EditorWatchdogSeconds-60)
	$reviewPath=Join-Path $work 'live-review.json'
	while (!(Test-Path -LiteralPath $reviewPath) -and [DateTime]::UtcNow -lt $deadline) {
		if ($script:wrapperProcess.HasExited) { throw 'Editor exited before live review.' }
		Start-Sleep -Milliseconds 250
	}
	if (!(Test-Path -LiteralPath $reviewPath)) { throw 'Physical/GPU review remains pending.' }
	$review=Get-Content -LiteralPath $reviewPath -Raw | ConvertFrom-Json
	foreach ($flag in @('physicalPointer','hoverObserved','holdDidNotRepeat','secondPressObserved','frontOnly','hiddenExcluded','outsideExcluded','rgbaRegionsMatch','edgeRegionsMatch','mipRegionsMatch','alphaAndSrgbMatch','compressedControlMatch')) { if ($review.PSObject.Properties[$flag].Value -ne $true) { throw "Live review missing: $flag" } }
	if (@($review.observations).Count -lt 6 -or @($review.screenshots).Count -lt 3) { throw 'Live review must retain pointer/hold/negative observations and screenshots.' }
	$held=@($review.observations | Where-Object { $_.kind -eq 'held-sample' -and $_.state.pointerDown -eq $true -and $_.state.translatedClicks -eq 1 })
	if (@($held.state.updates | Sort-Object -Unique).Count -lt 2) { throw 'Hold review needs two distinct actual held frames with count one.' }
	$released=@($review.observations | Where-Object { $_.kind -eq 'release' -and $_.state.pointerDown -eq $false -and $_.state.translatedClicks -eq 1 })
	if (!$released.Count) { throw 'Release review must observe the real pointer release before a second press.' }
	foreach ($relative in $review.screenshots) { $path=[IO.Path]::GetFullPath((Join-Path $work $relative)); Below $work $path; if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw 'Missing reviewed screenshot.' } }
	$final=Observation
	if ($final.translatedClicks -ne 2 -or $final.frontClicks -ne 1 -or $final.backClicks -ne 0 -or $final.hiddenClicks -ne 0) { throw 'Real callback counters do not match physical observations.' }
	Record 'physical-review' $review; Record 'final-observation' $final
	$null=Capture 'final-gpu'
	Tool 'play.exit' @{expectedState='playing'} | Out-Null
	CloseEditor
	$summary.passed=$true
} catch {
	$summary.failure=$_.Exception.Message
	if ($script:editorProcess -and $script:wrapperProcess) { try { CloseEditor } catch { Record 'close-failure' $_.Exception.Message } }
	throw
} finally {
	# On failure the still-owned watchdog remains bounded. Never report its job
	# kill as normal close; retain logs and the primary failure for diagnosis.
	JsonFile $summaryPath $summary
}
