<#
.SYNOPSIS
Runs audio continuity and real managed gameplay from an installed tree.
.DESCRIPTION
Prerequisites: parent configure/build/install, current correctness-artifacts.json,
Windows with initialized audio provider and supported graphics device. Example:
pwsh -NoProfile -File ./Build/Tests/Tools/Audio/RunInstalledAudioControlAcceptance.ps1 -InstallRoot ./Build -WorkRoot ./out/validation/audio-fresh
WorkRoot must not exist. Managed is the independent story lane; All additionally
requires native count/retention/settings and real allocation-failure evidence.
The runner changes only its disposable project/preferences, never exports or
manufactures approval tokens. Missing capabilities remain failed acceptance.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory=$true)][string]$InstallRoot,
	[Parameter(Mandatory=$true)][string]$WorkRoot,
	[ValidateRange(1024,65535)][int]$McpPort=43207,
	[ValidateRange(1,10000)][int]$Repetitions=100,
	[ValidateSet('All','Managed')][string]$Lane='All',
	[ValidateRange(30,600)][int]$StageTimeoutSeconds=180,
	[ValidateRange(120,3600)][int]$EditorWatchdogSeconds=1200
)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) { throw 'Installed playback acceptance requires Windows.' }
if ($Lane -eq 'All' -and $Repetitions -lt 100) { throw 'All acceptance requires at least 100 repetitions.' }
$install=[IO.Path]::GetFullPath($InstallRoot); $work=[IO.Path]::GetFullPath($WorkRoot)
if (Test-Path -LiteralPath $work) { throw 'WorkRoot must be fresh; previous evidence is never overwritten.' }
function Below([string]$Root,[string]$Child) {
	$prefix=[IO.Path]::GetFullPath($Root).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
	if (![IO.Path]::GetFullPath($Child).StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) { throw "Path escapes owned root: $Child" }
}
$manifestPath=Join-Path $install 'correctness-artifacts.json'
$manifest=Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.schemaVersion -ne 1 -or !$manifest.builtAtUtc -or !$manifest.files -or !$manifest.minimumCases) { throw 'Unsupported/incomplete installed artifact manifest.' }
$inventory=@{}
foreach ($entry in $manifest.files) {
	if ([IO.Path]::IsPathRooted($entry.relativePath) -or $inventory.ContainsKey($entry.relativePath)) { throw 'Invalid/duplicate manifest path.' }
	$path=[IO.Path]::GetFullPath((Join-Path $install $entry.relativePath)); Below $install $path
	if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-Item -LiteralPath $path).Length -ne [long]$entry.length -or
		(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Missing/hash-mismatched artifact: $path" }
	$inventory[$entry.relativePath]=$path
}
function Installed([string]$Relative) {
	if (!$inventory.ContainsKey($Relative)) { throw "Required installed manifest entry absent: $Relative" }
	return $inventory[$Relative]
}
$editor=Installed 'Editor/TerraEngine-Editor.exe'
$watchdog=Installed 'Tests/Tools/Correctness/RunWithWatchdog.ps1'
$runner=Installed 'Tests/Tools/Audio/RunInstalledAudioControlAcceptance.ps1'
if ([IO.Path]::GetFullPath($PSCommandPath) -ne $runner) { throw 'Invoke this installed runner, not its source/build copy.' }
. (Installed 'Tests/Tools/Audio/AudioControlAcceptanceContract.ps1')
$manifest.builtAtUtc=ConvertTo-AudioManifestTimestamp $manifest.builtAtUtc
foreach ($relative in @('Editor/TerraEngine.dll','Editor/Managed/TerraEngine.CSharp.dll','Editor/Managed/Analyzers/TerraEngine.Export.Generator.dll','Editor/DotNet/dotnet.exe','Editor/NativeAotNuGet/global.json')) { $null=Installed $relative }
$sdkVersion=(Get-Content -LiteralPath (Installed 'Editor/NativeAotNuGet/global.json') -Raw | ConvertFrom-Json).sdk.version
$null=Installed ('Editor/DotNet/sdk/'+$sdkVersion+'/dotnet.dll')
$fixturePrefix='Tests/Tools/Audio/Fixtures/ControlContinuityProject/'
$fixtureFiles=@('Assembly-CSharp.csproj','game.json','ProjectSettings/ProjectSettings.terra','ProjectSettings/RenderSettings.json','Assets/Scenes/Startup.tescene','Assets/Scripts/AudioControlObserver.cs','Assets/Audio/Continuity.wav','Assets/Audio/Continuity.wav.meta')
foreach ($name in $fixtureFiles) { $null=Installed ($fixturePrefix+$name) }
New-Item -ItemType Directory -Path $work | Out-Null
$rpcLog=Join-Path $work 'mcp.jsonl'; $summaryPath=Join-Path $work 'summary.json'
$summary=[ordered]@{schemaVersion=1;passed=$false;lane=$Lane;finalAcceptance=($Lane -eq 'All');builtAtUtc=$manifest.builtAtUtc;installRoot=$install;workRoot=$work;manifestSha256=(Get-FileHash $manifestPath).Hash;native=@();live=@();failure=$null}
function JsonFile([string]$Path,$Value) {
	$pending=$Path+'.pending'; $Value | ConvertTo-Json -Depth 80 | Set-Content -LiteralPath $pending -Encoding UTF8
	[IO.File]::Move($pending,$Path,$true)
}
function Record([string]$Name,$Data) { $summary.live+=@{name=$Name;data=$Data} }
function PsQuote([string]$Value) { return "'"+$Value.Replace("'","''")+"'" }
$shell=(Get-Process -Id $PID).Path
$wrapper=Get-Content -LiteralPath $watchdog -Raw
$hiddenStartupAnchor='flags=visibleWindow ? 0x101 : 0x100'
if (!$wrapper.Contains($hiddenStartupAnchor)) { throw 'Installed watchdog hidden-startup schema changed.' }
# STARTF_USESHOWWINDOW must accompany SW_HIDE: CREATE_NO_WINDOW alone only
# suppresses consoles, and GLFW otherwise explicitly shows its native window.
$wrapper=$wrapper.Replace($hiddenStartupAnchor,'flags=visibleWindow ? 0x101 : 0x101')
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

# Process.CloseMainWindow selects visible windows. A hidden GLFW main window
# still accepts WM_CLOSE; find only this retained process's unowned GLFW root.
# This is an orderly close request, never a replacement for zero-exit evidence.
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class TerraEngineAudioHiddenClose {
	delegate bool WindowCallback(IntPtr window, IntPtr parameter);
	[DllImport("user32.dll")] static extern bool EnumWindows(WindowCallback callback, IntPtr parameter);
	[DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
	[DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr window, uint command);
	[DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassNameW(IntPtr window, StringBuilder name, int capacity);
	[DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern bool PostMessageW(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
	public static bool Close(uint ownedProcessId) {
		if (ownedProcessId == 0) return false;
		var roots = new List<IntPtr>();
		WindowCallback callback = (window, parameter) => {
			uint processId; GetWindowThreadProcessId(window, out processId);
			if (processId != ownedProcessId || GetWindow(window,4) != IntPtr.Zero) return true;
			var name = new StringBuilder(256);
			if (GetClassNameW(window,name,name.Capacity) > 0 && name.ToString() == "GLFW30") roots.Add(window);
			return true;
		};
		if (!EnumWindows(callback,IntPtr.Zero) || roots.Count != 1) return false;
		uint confirmedProcessId; GetWindowThreadProcessId(roots[0],out confirmedProcessId);
		return confirmedProcessId == ownedProcessId && PostMessageW(roots[0],0x0010,IntPtr.Zero,IntPtr.Zero);
	}
}
'@

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
	if ($response.PSObject.Properties['status'] -and $response.status -eq 'waiting_approval') { throw ('Unexpected approval-required operation: '+$Name) }
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

function Managed([int]$AtLeastStage) {
	$deadline=[DateTime]::UtcNow.AddSeconds($StageTimeoutSeconds)
	while ([DateTime]::UtcNow -lt $deadline) {
		$stream=$null; $reader=$null; $value=$null
		try {
			$stream=[IO.File]::Open((Join-Path $script:observation 'managed.json'),[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
			$reader=[IO.StreamReader]::new($stream); $value=$reader.ReadToEnd() | ConvertFrom-Json
		} catch { } finally { if ($reader) { $reader.Dispose() } elseif ($stream) { $stream.Dispose() } }
		if ($value) {
			if ($value.failure) { throw ('Actual managed gameplay failed: '+$value.failure) }
			if ($value.stage -ge $AtLeastStage -and $value.updates -gt 0) { return $value }
		}
		if ($script:wrapperProcess.HasExited) { throw 'Editor exited before the actual managed stage.' }
		Start-Sleep -Milliseconds 50
	}
	throw "No real managed callback reached stage $AtLeastStage."
}
function CloseEditor {
	if (!$script:wrapperProcess) { return }
	if (!$script:editorProcess) { throw 'Cannot prove normal close without editor process identity.' }
	$script:editorProcess.Refresh()
	if (!$script:editorProcess.HasExited -and !$script:editorProcess.CloseMainWindow()) {
		if (![TerraEngineAudioHiddenClose]::Close([uint32]$script:editorProcess.Id)) { throw 'Owned hidden editor root did not accept an orderly close request.' }
	}
	if (!$script:wrapperProcess.WaitForExit(60000)) { throw 'Normal close did not settle within 60 seconds.' }
	$watch=Get-Content -LiteralPath (Join-Path $script:editorLogs 'watchdog.json') -Raw | ConvertFrom-Json
	Record 'normal-close' $watch
	Assert-AudioOrderlyExit $watch
	$script:editorProcess.Dispose(); $script:editorProcess=$null
	$script:wrapperProcess.Dispose(); $script:wrapperProcess=$null
}
try {
	if ($Lane -eq 'All') {
		$slice=Installed 'Tests/Tools/Correctness/RunNativeCorrectnessSlice.ps1'
		$engineTests=Installed 'Tests/Engine/TerraEngine-EngineTests.exe'
		$editorTests=Installed 'Tests/Editor/TerraEngine-EditorTests.exe'
		$continuity=Join-Path $work 'continuity.json'; $settings=Join-Path $work 'settings.json'
		$oldContinuity=[Environment]::GetEnvironmentVariable('TERRAENGINE_AUDIO_CONTROL_EVIDENCE')
		$oldSettings=[Environment]::GetEnvironmentVariable('TERRAENGINE_AUDIO_SETTINGS_EVIDENCE')
		try {
			$env:TERRAENGINE_AUDIO_CONTROL_EVIDENCE=$continuity; $env:TERRAENGINE_AUDIO_SETTINGS_EVIDENCE=$settings
			foreach ($item in @(@($engineTests,'Audio.ControlContinuity',12),@($engineTests,'Audio.PlaybackBinding',10),@($engineTests,'Audio.SourceSettings',10),@($editorTests,'Editor.AudioControlPersistence',4))) {
				$floor=$manifest.minimumCases.PSObject.Properties[$item[1]]
				if (!$floor -or [int]$floor.Value -lt $item[2]) { throw "Missing/under-floor manifest selection $($item[1])." }
				$logs=Join-Path $work ('native-'+$item[1])
				& $slice -Executable $item[0] -Filter $item[1] -MinimumCases ([int]$floor.Value) -TimeoutSeconds 300 -LogDirectory $logs
				if ($LASTEXITCODE -ne 0) { throw "Installed native selection failed: $($item[1])" }
				$summary.native+=@{filter=$item[1];minimumCases=[int]$floor.Value;logs=$logs}
			}
		} finally {
			[Environment]::SetEnvironmentVariable('TERRAENGINE_AUDIO_CONTROL_EVIDENCE',$oldContinuity)
			[Environment]::SetEnvironmentVariable('TERRAENGINE_AUDIO_SETTINGS_EVIDENCE',$oldSettings)
		}
		Assert-AudioContinuityEvidence (Get-Content $continuity -Raw | ConvertFrom-Json)
		Assert-AudioSettingsEvidence (Get-Content $settings -Raw | ConvertFrom-Json)
		# The watched production-TU child proves actual allocation failures in
		# addition to receipt paths that must perform zero allocation attempts.
		$allocationRunner=Installed 'Tests/Tools/AudioRendererSafety/RunRenderPublicationAllocationSafety.ps1'
		$allocationChild=Installed 'Tests/Tools/AudioRendererSafety/RenderPublicationAllocationFixture.exe'
		$allocationLogs=Join-Path $work 'renderer-allocation'
		& $allocationRunner -Executable $allocationChild -Repetitions $Repetitions -LogDirectory $allocationLogs
		if ($LASTEXITCODE -ne 0) { throw 'Installed allocation-failure child failed.' }
		Assert-AudioAllocationEvidence (Get-Content (Join-Path $allocationLogs 'allocation-safety.json') -Raw | ConvertFrom-Json) $Repetitions
	}
	$project=Join-Path $work 'Project'; $script:observation=Join-Path $work 'managed-observation'
	New-Item -ItemType Directory -Path $project,$script:observation | Out-Null
	foreach ($name in $fixtureFiles) {
		$target=Join-Path $project $name; New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
		Copy-Item -LiteralPath (Installed ($fixturePrefix+$name)) -Destination $target
	}
	$userSettings=Join-Path $project 'UserSettings'; New-Item -ItemType Directory $userSettings | Out-Null
	foreach ($name in @('EditorLayout.ini','EditorLayout.terra')) { Copy-Item -LiteralPath (Installed ('Editor/Project/Templates/UserSettings/'+$name)) -Destination (Join-Path $userSettings $name) }
	$references=Join-Path $project 'Build/Assembly-CSharp/References'; New-Item -ItemType Directory -Force $references | Out-Null
	foreach ($name in @('Editor/Managed/TerraEngine.CSharp.dll','Editor/Managed/Analyzers/TerraEngine.Export.Generator.dll')) { Copy-Item -LiteralPath (Installed $name) -Destination $references }
	New-Item -ItemType Directory (Join-Path $project 'Library') | Out-Null
	Set-Content -LiteralPath (Join-Path $project 'Library/LastScene.txt') -Value 'Scenes/Startup.tescene' -Encoding UTF8
	$prefs=Join-Path $work 'editor-preferences.json'
	JsonFile $prefs @{recentProjects=@();autoBuildScripts=$false;editorMcpEnabled=$true;editorMcpWriteModeDefault='writes_enabled';editorMcpListenPort=$McpPort;vulkanValidationLayers=$false;renderDebugDiagnostics=$false;rendererDiagnosticsPreferencesInitialized=$true}
	$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback,$McpPort); try { $listener.Start() } finally { $listener.Stop() }
	$script:editorLogs=Join-Path $work 'editor-logs'
	$command='& '+(PsQuote $ownedWrapper)+' -Executable '+(PsQuote $editor)+' -ArgumentList @('+(PsQuote '--project')+','+(PsQuote $project)+') -WorkingDirectory '+(PsQuote (Split-Path $editor))+' -LogDirectory '+(PsQuote $script:editorLogs)+' -TimeoutSeconds '+$EditorWatchdogSeconds+'; exit $LASTEXITCODE'
	$start=[Diagnostics.ProcessStartInfo]::new(); $start.FileName=$shell
	$start.Arguments='-NoProfile -ExecutionPolicy Bypass -EncodedCommand '+[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
	$start.UseShellExecute=$false; $start.CreateNoWindow=$true; $start.WindowStyle=[Diagnostics.ProcessWindowStyle]::Hidden
	foreach ($pair in @{TERRAENGINE_EDITOR_PREFS_SESSION_PATH=$prefs;TERRAENGINE_SUPPRESS_PROJECT_RECENTS='1';TERRAENGINE_AUDIO_CONTROL_OBSERVATION_ROOT=$script:observation}.GetEnumerator()) { $start.EnvironmentVariables[$pair.Key]=$pair.Value }
	$script:wrapperProcess=[Diagnostics.Process]::Start($start)
	$deadline=[DateTime]::UtcNow.AddSeconds($StageTimeoutSeconds); $ready=$false
	while ([DateTime]::UtcNow -lt $deadline) {
		if ($script:wrapperProcess.HasExited) { throw 'Editor watchdog exited before MCP readiness.' }
		if (!$script:editorProcess) {
			$children=@(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($script:wrapperProcess.Id)" | Where-Object { $_.ExecutablePath -eq $editor })
			if ($children.Count -eq 1) {
				$script:editorProcess=Get-Process -Id $children[0].ProcessId
				# Retain a real handle until normal close so PID recycling cannot
				# redirect endpoint ownership or hidden-window closure to a stranger.
				$null=$script:editorProcess.Handle
			}
		}
		try { $hello=Rpc 'initialize' @{protocolVersion='2025-03-26';clientInfo=@{name='installed-audio-control';version='1'}}; if ($script:editorProcess -and $hello.result.serverInfo.name -eq 'TerraEngine Editor') { $ready=$true; break } } catch { Start-Sleep -Milliseconds 200 }
	}
	if (!$ready) { throw 'Owned installed editor did not become ready.' }
	$listeners=@(Get-NetTCPConnection -LocalPort $McpPort -State Listen -ErrorAction Stop)
	$projectState=(Tool 'project.state').data
	Assert-AudioEndpoint $listeners $script:editorProcess.Id $projectState.projectRoot $project
	Record 'owned-project-endpoint' @{pid=$script:editorProcess.Id;projectRoot=$project;port=$McpPort}
	$tools=(Rpc 'tools/list').result.tools.name
	foreach ($name in @('project.state','script.status','script.compile','play.enter','play.pause','play.resume','play.step','play.exit','audio.control','component.remove','component.add','component.set_property','jobs.get')) { if ($name -notin $tools) { throw "Missing installed capability $name" } }
	$null=ReadyScripts
	$compile=Tool 'script.compile' @{profile='RelWithDebInfo';correlationId='audio-control-managed-build'}
	Record 'managed-build' (Job $compile.data.jobId 'audio-control-managed-build')
	$loaded=(ReadyScripts).loadedOutput
	$assembly=[IO.Path]::GetFullPath((Join-Path $project $loaded.identity)); Below $project $assembly
	if (!(Test-Path -LiteralPath $assembly) -or !$loaded.published -or !$loaded.fingerprint) { throw 'No current published managed assembly.' }
	# Readiness uses FNV-1a for the exact loaded output. Also retain SHA-256;
	# recording an arbitrary nearby DLL hash would not establish loaded identity.
	Add-Type -TypeDefinition @'
public static class TerraEngineAudioLoadedHash {
	public static string Compute(byte[] bytes) {
		unchecked { ulong hash=14695981039346656037UL; foreach(byte value in bytes) { hash ^= value; hash *=1099511628211UL; } return hash.ToString("x16"); }
	}
}
'@
	if ([TerraEngineAudioLoadedHash]::Compute([IO.File]::ReadAllBytes($assembly)) -ne $loaded.fingerprint) { throw 'Loaded gameplay fingerprint differs from observed assembly bytes.' }
	Record 'loaded-managed-output' @{status=$loaded;sha256=(Get-FileHash $assembly).Hash;length=(Get-Item $assembly).Length}
	Tool 'play.enter' @{expectedState='stopped'} | Out-Null
	$first=Managed 7; Record 'managed-initial-controls' $first
	# Explicit pause/step exercises the editor bridge; timeline progress was
	# already proved by running callback frames, not zero-delta step frames.
	Tool 'play.pause' @{expectedState='playing'} | Out-Null
	Tool 'play.step' @{expectedState='paused'} | Out-Null
	Tool 'play.resume' @{expectedState='paused'} | Out-Null
	Tool 'audio.control' @{objectId='61000000000000000000000000000002';kind='play';producerId=2;correlationId='audio-other-producer';idempotencyKey='audio-other-producer'} | Out-Null
	[IO.File]::WriteAllText((Join-Path $script:observation 'other-producer.ready'),'ready')
	$other=Managed 9; Record 'managed-other-producer' $other
	Tool 'component.remove' @{object='61000000000000000000000000000002';component='AudioSourceComponent'} | Out-Null
	Tool 'component.add' @{object='61000000000000000000000000000002';component='AudioSourceComponent'} | Out-Null
	foreach ($property in @{clipAssetId='67000000000000000000000000000001';playOnStart=$true;loop=$true;spatialBlend=0.0;volume=0.25}.GetEnumerator()) {
		Tool 'component.set_property' @{object='61000000000000000000000000000002';component='AudioSourceComponent';property=$property.Key;value=$property.Value} | Out-Null
	}
	[IO.File]::WriteAllText((Join-Path $script:observation 'replacement.ready'),'ready')
	$managed=Managed 11; Assert-AudioManagedEvidence $managed; Record 'managed-complete' $managed
	$afterLoaded=(Tool 'script.status').data.loadedOutput
	if (!$afterLoaded.published -or $afterLoaded.fingerprint -ne $loaded.fingerprint) { throw 'Managed assembly changed during acceptance.' }
	Tool 'play.exit' @{expectedState='playing'} | Out-Null
	CloseEditor; $summary.passed=$true
} catch { $summary.failure=$_.Exception.Message; Write-Error -ErrorAction Continue $_ }
finally {
	if ($script:wrapperProcess) {
		try { CloseEditor } catch {
			$summary.passed=$false; if (!$summary.failure) { $summary.failure=$_.Exception.Message }
			Record 'forced-cleanup' $_.Exception.Message
			try { $script:wrapperProcess.Kill(); $null=$script:wrapperProcess.WaitForExit(5000) } catch { }
		}
	}
	JsonFile $summaryPath $summary
}
Write-Output "Installed audio evidence: $summaryPath"
if (!$summary.passed) { exit 1 }
exit 0




