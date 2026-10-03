<#
.SYNOPSIS
Runs installed native and live editor correctness checks in a disposable project.
.DESCRIPTION
Prerequisites: parent integrated build/tests/install and an install-root
correctness-artifacts.json (schemaVersion=1, builtAtUtc, files entries with
relativePath/sha256/length, minimumCases map keyed by native filter).
Example: pwsh -NoProfile -File ./Build/Tests/Tools/Correctness/RunInstalledCorrectnessAcceptance.ps1 -InstallRoot ./Build -AllowLiveAcceptance
Export is a high-impact bridge operation. Approve each displayed export request
in the installed editor's normal approval UI; the runner polls the supported
approvals/recent resource and never manufactures an approval token.
All binaries/assets resolve beneath InstallRoot. Fresh work/evidence is retained;
no caller data is deleted. Forced watchdog cleanup is failure, never normal close.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string]$InstallRoot,
	[string]$ArtifactManifest='',
	[string]$WorkRoot='',
	[ValidateRange(1024,65535)][int]$McpPort=43139,
	[ValidateRange(30,600)][int]$StageTimeoutSeconds=180,
	[ValidateRange(30,600)][int]$ApprovalTimeoutSeconds=180,
	[ValidateRange(120,3600)][int]$EditorWatchdogSeconds=1800,
	[switch]$AllowLiveAcceptance
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if (-not $AllowLiveAcceptance) { throw 'Pass -AllowLiveAcceptance after the parent integrated gates pass.' }
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'Installed live acceptance currently requires Windows.' }
$install=[IO.Path]::GetFullPath($InstallRoot)
if (!$ArtifactManifest) { $ArtifactManifest=Join-Path $install 'correctness-artifacts.json' }
if (!$WorkRoot) { $WorkRoot=Join-Path ([IO.Path]::GetTempPath()) ('TerraEngine-installed-correctness-'+[Guid]::NewGuid().ToString('N')) }
$work=[IO.Path]::GetFullPath($WorkRoot)
if (Test-Path -LiteralPath $work) { throw 'WorkRoot must be fresh; existing directories are never overwritten.' }
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
$editorTests=Installed 'Tests/Editor/TerraEngine-EditorTests.exe'
$childFixture=Installed 'Tests/Engine/CorrectnessProcessFixture.exe'
$watchdog=Installed 'Tests/Tools/Correctness/RunWithWatchdog.ps1'
$slice=Installed 'Tests/Tools/Correctness/RunNativeCorrectnessSlice.ps1'
$installedRunner=Installed 'Tests/Tools/Correctness/RunInstalledCorrectnessAcceptance.ps1'
if ([IO.Path]::GetFullPath($PSCommandPath) -ne $installedRunner) { throw 'Invoke the installed runner, not its source/build copy.' }
foreach ($relative in @('Editor/Managed/TerraEngine.CSharp.dll','Editor/Managed/TerraEngine.CSharp.runtimeconfig.json','Editor/Managed/TerraEngine.CSharp.deps.json','Editor/Managed/TerraEngine.Editor.runtimeconfig.json','Editor/Managed/TerraEngine.Editor.deps.json','Editor/Managed/Analyzers/TerraEngine.Export.Generator.dll','Editor/TerraEngine-Export.exe','Editor/TerraEngine.dll','Editor/DotNet/dotnet.exe','Editor/NativeAotNuGet/NuGet.Config','Editor/NativeAotNuGet/global.json')) { $null=Installed $relative }
# The pinned SDK/export runtime is part of the sealed inventory, not merely a
# launcher executable that could resolve a missing or globally registered SDK.
$sdkVersion=(Get-Content -LiteralPath (Installed 'Editor/NativeAotNuGet/global.json') -Raw | ConvertFrom-Json).sdk.version
if (!$sdkVersion -or $sdkVersion -notmatch '^\d+\.\d+\.\d+$') { throw 'Invalid installed pinned SDK version.' }
$null=Installed ('Editor/DotNet/sdk/'+$sdkVersion+'/dotnet.dll')
$null=Installed ('Editor/DotNet/sdk/'+$sdkVersion+'/Sdks/Microsoft.NET.Sdk/Sdk/Sdk.props')
$null=Installed 'Editor/ExportRuntime/TerraEngine-Launcher.exe'
$null=Installed 'Editor/ExportRuntime/TerraEngine.dll'
if (@($inventory.Keys | Where-Object { $_ -like 'Editor/Shaders/*' -and $_ -like '*.tspf' }).Count -eq 0 -or @($inventory.Keys | Where-Object { $_ -like 'Editor/NativeAotNuGet/feed/*' }).Count -eq 0) { throw 'Installed shader packages/offline NativeAOT feed are absent from the manifest.' }
$fixturePrefix='Tests/Tools/Correctness/Fixtures/ManagedExportProject/'
$fixtureFiles=@('game.json','Assembly-CSharp.csproj','Directory.Build.targets','ProjectSettings/ProjectSettings.terra','ProjectSettings/RenderSettings.json','BuildProfiles/correctness.json','Assets/Scenes/Startup.tescene','Assets/Scripts/CorrectnessHealthyScript.cs','Assets/Scripts/CorrectnessControlledFaultScript.cs')
foreach ($relative in $fixtureFiles) { $null=Installed ($fixturePrefix+$relative) }
$layoutFiles=@('EditorLayout.ini','EditorLayout.terra')
foreach ($name in $layoutFiles) { $null=Installed ('Editor/Project/Templates/UserSettings/'+$name) }
New-Item -ItemType Directory -Path $work | Out-Null
$rpcLog=Join-Path $work 'mcp.jsonl'; $summaryPath=Join-Path $work 'summary.json'
$summary=[ordered]@{ schemaVersion=1; passed=$false; builtAtUtc=$manifest.builtAtUtc; manifestSha256=(Get-FileHash -LiteralPath $ArtifactManifest).Hash; installRoot=$install; workRoot=$work; native=@(); live=@(); failure=$null }
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
function Healthy([ulong]$AfterUpdates=0,[string]$ExpectedInstance='') {
	$deadline=[DateTime]::UtcNow.AddSeconds($StageTimeoutSeconds)
	while ([DateTime]::UtcNow -lt $deadline) {
		$reader=$null; $stream=$null
		try {
			# The callback atomically replaces its receipt. Permit delete sharing
			# so observing it cannot cause a managed Windows access-denied fault.
			$stream=[IO.File]::Open((Join-Path $script:observation 'healthy.json'),[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
			$reader=[IO.StreamReader]::new($stream)
			$value=$reader.ReadToEnd() | ConvertFrom-Json
		}
		catch { Start-Sleep -Milliseconds 50; continue }
		finally { if ($reader) { $reader.Dispose() } elseif ($stream) { $stream.Dispose() } }
		if ($ExpectedInstance -and $value.instanceId -ne $ExpectedInstance) { throw 'The installed healthy script instance changed.' }
		if ($value.updates -gt $AfterUpdates) { return $value }
		Start-Sleep -Milliseconds 50
	}
	throw 'No real managed callback receipt arrived.'
}
function GatePids {
	$deadline=[DateTime]::UtcNow.AddSeconds($StageTimeoutSeconds)
	while ([DateTime]::UtcNow -lt $deadline) {
		$files=@(0..2 | ForEach-Object { Join-Path $script:observation "export-pid.$_" })
		if (@($files | Where-Object { !(Test-Path -LiteralPath $_) }).Count -eq 0) {
			$ids=@($files | ForEach-Object { [int](Get-Content -LiteralPath $_ -Raw) })
			# Open and retain each process object while it is held at the gate.
			# Checking a recycled numeric PID after settlement can inspect a stranger.
			foreach ($id in $ids) {
				$process=[Diagnostics.Process]::GetProcessById($id)
				try {
					$null=$process.Handle
					if ($process.HasExited -or [IO.Path]::GetFullPath($process.MainModule.FileName) -ne $childFixture -or
						$process.StartTime.ToUniversalTime() -lt $script:editorProcess.StartTime.ToUniversalTime()) {
						throw 'Export receipt did not identify a live fixture from this editor session.'
					}
					$script:descendants[$id]=$process
				} catch { $process.Dispose(); throw }
			}
			return $ids
		}
		Start-Sleep -Milliseconds 100
	}
	throw 'Export did not reach its real owned-process descendant gate.'
}
function PidsDead([int[]]$Ids) {
	foreach ($id in $Ids) {
		if (!$script:descendants.ContainsKey($id)) { throw 'Export descendant has no retained process identity.' }
		$process=$script:descendants[$id]
		# Completion must already have settled the retained process object. A
		# grace period here would hide a runner returning before its child exits.
		if (!$process.WaitForExit(0)) { throw 'Owned export descendants survived the completion boundary.' }
		$process.Dispose(); $script:descendants.Remove($id)
	}
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
$script:descendants=@{}
try {
	$slices=@(@($engineTests,'Engine.SceneMutationFailureSafety',39),@($engineTests,'Engine.ResourceAsyncLifecycle',28),@($engineTests,'Scene.SaveFailureSafety',10),@($engineTests,'Audio.WorldCellCapture',5),@($engineTests,'Engine.FrameFailureLifecycle',7),@($engineTests,'Engine.OwnedProcess',6),@($editorTests,'Editor.ExportLifecycle',6))
	foreach ($item in $slices) {
		$property=$manifest.minimumCases.PSObject.Properties[$item[1]]
		if (!$property -or [int]$property.Value -lt $item[2]) { throw "Manifest minimum missing/too small for $($item[1])." }
		$logs=Join-Path $work ('native-'+$item[1])
		& $slice -Executable $item[0] -Filter $item[1] -MinimumCases ([int]$property.Value) -TimeoutSeconds 120 -LogDirectory $logs
		if ($LASTEXITCODE -ne 0) { throw "Installed native slice failed: $($item[1])." }
		$summary.native+=@{filter=$item[1];minimumCases=[int]$property.Value;logs=$logs}
	}
	$project=Join-Path $work 'project'; $script:observation=Join-Path $work 'observations'
	New-Item -ItemType Directory -Path $project,$script:observation | Out-Null
	foreach ($relative in $fixtureFiles) { $target=Join-Path $project $relative; New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null; Copy-Item -LiteralPath (Installed ($fixturePrefix+$relative)) -Destination $target }
	# This disposable fixture bypasses CreateProject. Seed its normal installed
	# layout pair so a missing user layout cannot open an unrelated floating UI.
	$userSettings=Join-Path $project 'UserSettings'
	New-Item -ItemType Directory -Path $userSettings | Out-Null
	foreach ($name in $layoutFiles) { Copy-Item -LiteralPath (Installed ('Editor/Project/Templates/UserSettings/'+$name)) -Destination (Join-Path $userSettings $name) }
	$references=Join-Path $project 'Build/Assembly-CSharp/References'; New-Item -ItemType Directory -Force -Path $references | Out-Null
	Copy-Item -LiteralPath (Installed 'Editor/Managed/TerraEngine.CSharp.dll') -Destination $references
	Copy-Item -LiteralPath (Installed 'Editor/Managed/Analyzers/TerraEngine.Export.Generator.dll') -Destination $references
	New-Item -ItemType Directory -Path (Join-Path $project 'Library') | Out-Null
	Set-Content -LiteralPath (Join-Path $project 'Library/LastScene.txt') -Value 'Scenes/Startup.tescene' -Encoding UTF8
	$prefs=Join-Path $work 'editor-preferences.json'
	JsonFile $prefs @{recentProjects=@();autoBuildScripts=$false;editorMcpEnabled=$true;editorMcpWriteModeDefault='writes_enabled';editorMcpListenPort=$McpPort;vulkanValidationLayers=$false;renderDebugDiagnostics=$false;rendererDiagnosticsPreferencesInitialized=$true}
	$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,$McpPort); try { $listener.Start() } finally { $listener.Stop() }
	$script:editorLogs=Join-Path $work 'editor-logs'
	$command='& '+(PsQuote $ownedWrapper)+' -Executable '+(PsQuote $editor)+' -ArgumentList @('+ (PsQuote '--project')+','+(PsQuote $project)+') -WorkingDirectory '+(PsQuote (Split-Path $editor))+' -LogDirectory '+(PsQuote $script:editorLogs)+' -TimeoutSeconds '+$EditorWatchdogSeconds+'; exit $LASTEXITCODE'
	$start=[Diagnostics.ProcessStartInfo]::new(); $start.FileName=$shell; $start.Arguments='-NoProfile -ExecutionPolicy Bypass -EncodedCommand '+[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command)); $start.UseShellExecute=$false; $start.CreateNoWindow=$true; $start.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
	foreach ($pair in @{TERRAENGINE_EDITOR_PREFS_SESSION_PATH=$prefs;TERRAENGINE_SUPPRESS_PROJECT_RECENTS='1';TERRAENGINE_CORRECTNESS_OBSERVATION_ROOT=$script:observation;TERRAENGINE_CORRECTNESS_HOLD_EXPORT='1';TERRAENGINE_CORRECTNESS_PROCESS_FIXTURE=$childFixture}.GetEnumerator()) { $start.EnvironmentVariables[$pair.Key]=$pair.Value }
	$script:wrapperProcess=[Diagnostics.Process]::Start($start)
	$deadline=[DateTime]::UtcNow.AddSeconds($StageTimeoutSeconds); $ready=$false
	while ([DateTime]::UtcNow -lt $deadline) {
		if ($script:wrapperProcess.HasExited) { throw 'Editor watchdog exited before bridge readiness.' }
		if (!$script:editorProcess) { $children=@(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($script:wrapperProcess.Id)" | Where-Object { $_.ExecutablePath -eq $editor }); if ($children.Count -eq 1) { $script:editorProcess=Get-Process -Id $children[0].ProcessId } }
		try { $hello=Rpc 'initialize' @{protocolVersion='2025-03-26';clientInfo=@{name='installed-correctness';version='1'}}; if ($script:editorProcess -and $hello.result.serverInfo.name -eq 'TerraEngine Editor') { $ready=$true; break } } catch { Start-Sleep -Milliseconds 250 }
	}
	if (!$ready) { throw 'Installed editor/bridge did not become ready.' }
	# A free-port precheck alone cannot establish endpoint ownership. Confirm
	# both the listening process and disposable project before any live write.
	$listeners=@(Get-NetTCPConnection -LocalPort $McpPort -State Listen -ErrorAction Stop)
	if (!$listeners.Count -or @($listeners | Where-Object { $_.OwningProcess -ne $script:editorProcess.Id }).Count) { throw 'The loopback endpoint does not belong to the owned installed editor.' }
	$projectState=(Tool 'project.state').data
	if (!$projectState.projectRoot -or [IO.Path]::GetFullPath($projectState.projectRoot) -ne [IO.Path]::GetFullPath($project)) { throw 'Installed editor did not open the disposable acceptance project.' }
	Record 'owned-project-and-endpoint' @{editorPid=$script:editorProcess.Id;projectRoot=$projectState.projectRoot;port=$McpPort}
	$tools=(Rpc 'tools/list').result.tools.name
	foreach ($name in @('project.state','script.status','script.compile','scene.open','scene.list_objects','transaction.execute','scene.save','play.enter','play.pause','play.step','play.exit','export.start','export.cancel','jobs.get')) { if ($name -notin $tools) { throw "Missing live capability $name" } }
	Record 'startup-scripting-ready' (ReadyScripts)
	$compile=Tool 'script.compile' @{profile='RelWithDebInfo';correlationId='correctness-managed-build'}
	Record 'managed-build' (Job $compile.data.jobId 'correctness-managed-build')
	Record 'explicit-scripting-ready' (ReadyScripts)
	# Verify the production startup load first. A repeated scene open would
	# unnecessarily request approval; keep the approved fallback if needed.
	$fixtureState=(Tool 'project.state').data
	if ($fixtureState.activeScene -ne 'Startup') {
		Record 'opened-fixture-scene' (Tool 'scene.open' @{path='Assets/Scenes/Startup.tescene'})
		$fixtureState=(Tool 'project.state').data
	}
	if ($fixtureState.activeScene -ne 'Startup' -or !$fixtureState.derivedScene.sourcePath) { throw 'Canonical fixture is not the active scene.' }
	$fixtureSource=[string]$fixtureState.derivedScene.sourcePath
	if (![IO.Path]::IsPathRooted($fixtureSource)) { $fixtureSource=Join-Path $project $fixtureSource }
	if ([IO.Path]::GetFullPath($fixtureSource) -ne [IO.Path]::GetFullPath((Join-Path $project 'Assets/Scenes/Startup.tescene'))) { throw 'Active scene does not resolve to the authored fixture.' }
	Record 'verified-fixture-scene' $fixtureState
	$before=(Tool 'scene.list_objects' @{limit=256}).data.objects
	if (@($before).Count -ne 3) { throw 'Authored fixture did not publish its three objects.' }
	$bad=Tool 'transaction.execute' @{format='TerraSpatialTransactionV1';correlationId='correctness-mutation-rejected';label='rejected correctness batch';operations=@(@{kind='object.create';localId='duplicate';name='MustNeverPublish'},@{kind='object.create';localId='duplicate';name='MustNeverPublishSibling'})} -ExpectFailure
	if (!(($bad | ConvertTo-Json -Depth 20 -Compress).Contains('local IDs must be unique'))) { throw 'Rejected mutation did not reach duplicate-local-ID validation.' }
	$after=(Tool 'scene.list_objects' @{limit=256}).data.objects
	if (($before | ConvertTo-Json -Depth 20 -Compress) -ne ($after | ConvertTo-Json -Depth 20 -Compress)) { throw 'Rejected transaction left partial visible changes.' }
	Record 'rejected-mutation' $bad
	$good=Tool 'transaction.execute' @{format='TerraSpatialTransactionV1';correlationId='correctness-mutation-accepted';label='accepted correctness batch';operations=@(@{kind='object.create';localId='created';name='CorrectnessPublishedObject'})}
	if (@((Tool 'scene.list_objects' @{search='CorrectnessPublishedObject';limit=256}).data.objects).Count -ne 1) { throw 'Accepted transaction was not visible at the protected boundary.' }
	Record 'accepted-mutation' $good
	$scene=Join-Path $project 'Assets/Scenes/Startup.tescene'; $oldHash=(Get-FileHash -LiteralPath $scene).Hash
	# A deterministic directory blocker exercises real file publication failure.
	$block=Join-Path $project 'Assets/Scenes/blocked.tescene'; New-Item -ItemType Directory -Path $block | Out-Null
	Record 'failed-save' (Tool 'scene.save' @{path=$block;correlationId='correctness-save-failed'} -ExpectFailure)
	if ((Get-FileHash -LiteralPath $scene).Hash -ne $oldHash) { throw 'Failed save changed last-good scene bytes.' }
	Record 'successful-save' (Tool 'scene.save' @{path=$scene;correlationId='correctness-save-succeeded'})
	if (!(Get-Content -LiteralPath $scene -Raw).Contains('CorrectnessPublishedObject')) { throw 'Successful save omitted the accepted mutation.' }
	Tool 'play.enter' @{expectedState='stopped'} | Out-Null
	Record 'play-scripting-ready' (ReadyScripts)
	$healthy=Healthy; $loaded=(Tool 'script.status').data.loadedOutput
	if (!$loaded.published -or $healthy.updates -lt 1) { throw 'Installed gameplay assembly/callback is not usable.' }
	$first=Tool 'export.start' @{profile='correctness';correlationId='correctness-export-cancel'}
	$pids=GatePids
	Record 'duplicate-export' (Tool 'export.start' @{profile='correctness';correlationId='correctness-export-duplicate'} -ExpectFailure)
	$during=Healthy $healthy.updates $healthy.instanceId; $duringLoaded=(Tool 'script.status').data.loadedOutput
	if ($during.instanceId -ne $healthy.instanceId -or $during.updates -le $healthy.updates -or $duringLoaded.fingerprint -ne $loaded.fingerprint -or !$duringLoaded.published) { throw 'Export changed or stopped the live gameplay instance/assembly.' }
	Tool 'play.pause' @{expectedState='playing'} | Out-Null
	[IO.File]::WriteAllText((Join-Path $script:observation 'fault.release'),'release')
	Tool 'play.step' @{expectedState='paused';correlationId='correctness-managed-fault'} | Out-Null
	# The public step request schedules the next frame. Wait for its real
	# callback observation rather than sampling the previous paused frame.
	# The managed bridge catches and logs callback exceptions before returning
	# to native dispatch. failedScripts counts native dispatch failures, so retain
	# that frame as evidence and require the actual receipt plus managed error log.
	$faultDeadline=[DateTime]::UtcNow.AddSeconds($StageTimeoutSeconds)
	$faultToken='Correctness.ControlledManagedFault'
	$faultReceipt=''; $faultLogged=$false
	do {
		$fault=(Tool 'script.status').data.frame
		try {
			$faultReceipt=Get-Content -LiteralPath (Join-Path $script:observation 'fault-observed.txt') -Raw
			$faultLog=Get-Content -LiteralPath (Join-Path $script:editorLogs 'stdout.log') -Raw
			$faultLogged=$faultLog.Contains('Exception in UpdateScript for object') -and
				$faultLog.Contains('System.InvalidOperationException: '+$faultToken)
		} catch {
			# The callback receipt or flushed log may not be readable yet. Retry
			# within the deadline; missing persistent evidence remains a failure.
			$faultReceipt=''; $faultLogged=$false
		}
		if ($faultReceipt -ceq $faultToken -and $faultLogged) { break }
		Start-Sleep -Milliseconds 50
	} while ([DateTime]::UtcNow -lt $faultDeadline)
	if ($faultReceipt -cne $faultToken -or !$faultLogged) { throw 'Controlled managed callback failure was not observed in its receipt and retained editor log.' }
	$healthyAtFault=Healthy; Tool 'play.step' @{expectedState='paused'} | Out-Null; $healthyAfterFault=Healthy $healthyAtFault.updates $healthy.instanceId
	if ($healthyAfterFault.instanceId -ne $healthy.instanceId -or $healthyAfterFault.updates -le $healthyAtFault.updates) { throw 'Healthy sibling did not survive the managed fault.' }
	Record 'managed-continuity-and-fault' @{before=$healthy;during=$during;fault=$fault;faultReceipt=$faultReceipt;faultLogged=$faultLogged;after=$healthyAfterFault;loadedOutput=$loaded}
	Record 'cancel-disposition' (Tool 'export.cancel' @{jobId=$first.data.jobId})
	Record 'canceled-export' (Job $first.data.jobId 'correctness-export-cancel' @('canceled'))
	PidsDead $pids
	if (Test-Path -LiteralPath (Join-Path $project 'Builds/Correctness')) { throw 'Canceled precommit export published output.' }
	if (@(Get-ChildItem -LiteralPath (Join-Path $project 'Builds') -Force | Where-Object { $_.Name -like '.Correctness.export*' }).Count) { throw 'Canceled export retained a staging/backup/lock sibling.' }
	[IO.File]::WriteAllText((Join-Path $script:observation 'export.release'),'release')
	$retry=Tool 'export.start' @{profile='correctness';correlationId='correctness-export-retry'}
	$success=Job $retry.data.jobId 'correctness-export-retry'; Record 'successful-export-retry' $success
	# The launcher reads game.json inside the validated archive. Require the
	# canonical published directory here; package evidence below proves content.
	$publishedOutput=[IO.Path]::GetFullPath((Join-Path $project 'Builds/Correctness'))
	Below $project $publishedOutput
	if (!$success.result.success -or !(Test-Path -LiteralPath $publishedOutput -PathType Container)) { throw 'Retry did not publish a real exported output directory.' }
	$evidence=[IO.Path]::GetFullPath($success.result.evidenceDirectory); Below $project $evidence
	$evidenceHashes=@{}
	foreach ($name in @('export-result.json','resolved-manifest.json','content-inventory.json','validation-report.json','progress.jsonl','package-validation.txt')) {
		$path=Join-Path $evidence $name
		if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -eq 0) { throw "Missing/empty export evidence $name" }
		$evidenceHashes[$name]=(Get-FileHash -LiteralPath $path).Hash
	}
	$exportResult=Get-Content -LiteralPath (Join-Path $evidence 'export-result.json') -Raw | ConvertFrom-Json
	$content=Get-Content -LiteralPath (Join-Path $evidence 'content-inventory.json') -Raw | ConvertFrom-Json
	$validation=Get-Content -LiteralPath (Join-Path $evidence 'validation-report.json') -Raw | ConvertFrom-Json
	$packageValidation=Get-Content -LiteralPath (Join-Path $evidence 'package-validation.txt') -Raw
	$resolvedManifest=Get-Content -LiteralPath (Join-Path $evidence 'resolved-manifest.json') -Raw | ConvertFrom-Json
	foreach ($line in @('Startup scene: PASS','Required runtime entries: PASS','Manifest asset references: PASS','Placeholder scan: PASS','Manifest evidence match: PASS','Final: PASS')) {
		if ($packageValidation -notmatch ('(?m)^'+[regex]::Escape($line)+'\r?$')) { throw "Package validation did not prove $line" }
	}
	if ($resolvedManifest.startupScene -cne 'Scenes/Startup.tescene') { throw 'Resolved export manifest does not name the controlled startup scene.' }
	if (!$exportResult.success -or $exportResult.runId -ne $success.result.runId -or $content.runId -ne $exportResult.runId -or
		$content.schemaVersion -ne 1 -or $content.entryCount -ne @($content.entries).Count -or $content.entryCount -lt 1 -or
		@($content.entries | Where-Object {$_.placeholderDetected}).Count -or !$validation.passed -or $validation.errorCount -ne 0 -or
		$packageValidation -notmatch '(?m)^Final: PASS\r?$') { throw 'Export evidence does not prove this successful run without placeholders and with a validated package.' }
	if (![IO.Path]::GetFullPath($exportResult.outputDirectory).Equals($publishedOutput,[StringComparison]::OrdinalIgnoreCase)) { throw 'Export evidence names a different published directory.' }
	$packages=@(Get-ChildItem -LiteralPath $publishedOutput -File -Filter '*.tepak')
	if ($packages.Count -ne 1 -or $packages[0].Length -eq 0) { throw 'Published export did not contain one nonempty validated package.' }
	if (![IO.Path]::GetFullPath($exportResult.package.archivePath).Equals($packages[0].FullName,[StringComparison]::OrdinalIgnoreCase) -or
		$exportResult.package.byteSize -ne $packages[0].Length -or $exportResult.package.entryCount -lt 1) { throw 'Published package does not match this run''s package evidence.' }
	Record 'export-evidence' @{directory=$evidence;hashes=$evidenceHashes;packageSha256=(Get-FileHash -LiteralPath $packages[0].FullName).Hash;packageBytes=$packages[0].Length}
	Tool 'play.step' @{expectedState='paused'} | Out-Null
	$afterExport=Healthy $healthyAfterFault.updates $healthy.instanceId; $afterLoaded=(Tool 'script.status').data.loadedOutput
	if ($afterExport.instanceId -ne $healthy.instanceId -or $afterExport.updates -le $healthyAfterFault.updates -or $afterLoaded.fingerprint -ne $loaded.fingerprint -or !$afterLoaded.published) { throw 'Successful export changed or invalidated the live gameplay instance.' }
	Record 'managed-after-successful-export' @{receipt=$afterExport;loadedOutput=$afterLoaded}
	Tool 'play.exit' @{expectedState='paused'} | Out-Null
	# Clear only known disposable gates/PID records, after validating containment.
	foreach ($name in @('export.release','export-pid.0','export-pid.1','export-pid.2')) { $path=Join-Path $script:observation $name; Below $work $path; Remove-Item -LiteralPath $path -ErrorAction SilentlyContinue }
	$closing=Tool 'export.start' @{profile='correctness';correlationId='correctness-export-close'}
	$closingPids=GatePids; Record 'close-while-export-active' @{jobId=$closing.data.jobId;pids=$closingPids}
	CloseEditor; PidsDead $closingPids
	$summary.passed=$true
} catch { $summary.failure=$_.Exception.Message; Write-Error -ErrorAction Continue $_ }
finally {
	if ($script:wrapperProcess) {
		try { CloseEditor } catch {
			$forcedReason=$_.Exception.Message; $summary.passed=$false
			# Keep the first failing check actionable even when cleanup also fails.
			if (!$summary.failure) { $summary.failure=$forcedReason }
			try { $script:wrapperProcess.Kill() } catch {}
			try { $null=$script:wrapperProcess.WaitForExit(5000) } catch {}
			$summary.live+=@{name='forced-cleanup';data=$forcedReason}
		}
	}
	foreach ($process in $script:descendants.Values) { $process.Dispose() }
	JsonFile $summaryPath $summary
}
Write-Output "Installed correctness evidence: $summaryPath"
if (!$summary.passed) { exit 1 }
exit 0
