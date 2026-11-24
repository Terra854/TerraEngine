<#
Requires PowerShell 7, an installed BUILD_TESTING tree, its bundled managed SDK,
and an unlocked Windows Vulkan desktop. Run from the installed copy:
pwsh -File <install>/Tests/Tools/PostProcessing/TestInstalledGraphicsSettings.ps1 `
  -InstallRoot <install> -OutputDirectory <fresh-evidence-directory>
Add -SpatialAA to run the AA fixture's editor/export runtime lanes instead of
the seven ordinary settings/startup lanes. AA settings receipts do not certify
shader image quality, GPU execution, or performance; those have separate gates.
Creates a disposable three-object project from the installed canonical scene
fixture. No personal project or global editor preference is opened or changed.
Every result comes from the managed fixture's native receipts and snapshots.
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory)][string]$InstallRoot,
	[Parameter(Mandatory)][string]$OutputDirectory,
	[ValidateRange(1024,65535)][int]$McpPort = 43207,
	[ValidateRange(60,1800)][int]$TimeoutSeconds = 300,
	[switch]$SpatialAA
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$install = [IO.Path]::GetFullPath($InstallRoot)
$work = [IO.Path]::GetFullPath($OutputDirectory)
function Require([bool]$Condition, [string]$Message) { if (!$Condition) { throw $Message } }
function Below([string]$Child, [string]$Parent) {
	return $Child.StartsWith($Parent.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)
}
Require (!(Test-Path -LiteralPath $work)) 'OutputDirectory must be fresh so old observations cannot satisfy this run.'
Require ($work -ine $install -and !(Below $work $install) -and !(Below $install $work)) 'Evidence and installed trees must be disjoint.'
$editor = Join-Path $install 'Editor/TerraEngine-Editor.exe'
$exporter = Join-Path $install 'Editor/TerraEngine-Export.exe'
$template = Join-Path $install 'Tests/Tools/Correctness/Fixtures/ManagedExportProject'
$fixture = Join-Path $install 'Tests/Fixtures/PostProcessing'
$fixtureClass = if ($SpatialAA) { 'AASettingsFixture' } else { 'GraphicsSettingsFixture' }
$fixtureScript = Join-Path $fixture ('Scripts/' + $fixtureClass + '.cs')
foreach ($file in @($editor,$exporter,(Join-Path $template 'Assets/Scenes/Startup.tescene'),
	(Join-Path $fixture 'Assembly-CSharp.csproj'),$fixtureScript)) {
	Require (Test-Path -LiteralPath $file -PathType Leaf) "Missing installed prerequisite: $file"
}
$null = [IO.Directory]::CreateDirectory($work)
$runId = 'graphics-' + [Guid]::NewGuid().ToString('N')
$summary = [ordered]@{schemaVersion=1;runId=$runId;status='running';failure='';installRoot=$install
	executables=@{};lanes=@();export=$null}
if ($SpatialAA) { $summary.fixture = 'spatial-aa'; $summary.settingsOnly = $true; $summary.imageryVerified = $false; $summary.gpuExecutionVerified = $false }
foreach ($path in @($editor,$exporter,(Join-Path $install 'Editor/TerraEngine.dll'))) {
	$summary.executables[$path] = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
}
function WriteJson([string]$Path, $Value) {
	[IO.File]::WriteAllText($Path,($Value | ConvertTo-Json -Depth 60),[Text.UTF8Encoding]::new($false))
}
function ReadPublishedObservation([string]$Path) {
	# Read one complete publication with shared access. The spatial fixture's
	# terminal observation has a fresh destination, avoiding Windows replace races.
	$share = [IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete
	$stream = [IO.File]::Open($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,$share)
	$reader = [IO.StreamReader]::new($stream)
	try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}

# Clone only authored inputs from a maintained schema fixture. The two camera
# objects are separate identities; destroying CameraB cannot destroy the script.
$project = Join-Path $work 'GraphicsSettingsContract'
foreach ($directory in @('Assets/Scenes','Assets/Scripts','ProjectSettings','BuildProfiles','Build/Assembly-CSharp/References')) {
	$null = [IO.Directory]::CreateDirectory((Join-Path $project $directory))
}
$scene = Get-Content -LiteralPath (Join-Path $template 'Assets/Scenes/Startup.tescene') -Raw | ConvertFrom-Json -AsHashtable
$cameraA = $scene.objects[0]
$cameraA.name = 'CameraA'
$cameraB = ($cameraA | ConvertTo-Json -Depth 60 | ConvertFrom-Json -AsHashtable)
$cameraB.name = 'CameraB'; $cameraB.uuid = '71000000000000000000000000000002'
$controller = $scene.objects[1]
$controller.name = $fixtureClass; $controller.uuid = '71000000000000000000000000000003'
$bindings = @($controller.components.Script.BindingsJson | ConvertFrom-Json -AsHashtable)
Require ($bindings.Count -eq 1) 'Canonical template must provide exactly one controller binding.'
$bindings[0].className = $fixtureClass
$bindings[0].bindingId = '72000000000000000000000000000001'
$controller.components.Script.BindingsJson = ConvertTo-Json -InputObject $bindings -Depth 30 -Compress
$scene.name = 'Graphics Settings Contract'; $scene.objects = @($cameraA,$cameraB,$controller)
WriteJson (Join-Path $project 'Assets/Scenes/Startup.tescene') $scene
WriteJson (Join-Path $project 'game.json') @{name='GraphicsSettingsContract';gameId=$runId;version='1.0.0';startupScene='Scenes/Startup.tescene'}
WriteJson (Join-Path $project 'ProjectSettings/ProjectSettings.terra') @{projectDisplayName='GraphicsSettingsContract'}
Copy-Item -LiteralPath (Join-Path $template 'ProjectSettings/RenderSettings.json') -Destination (Join-Path $project 'ProjectSettings/RenderSettings.json')
Copy-Item -LiteralPath (Join-Path $template 'BuildProfiles/correctness.json') -Destination (Join-Path $project 'BuildProfiles/graphics.json')
Copy-Item -LiteralPath (Join-Path $fixture 'Assembly-CSharp.csproj') -Destination $project
Copy-Item -LiteralPath $fixtureScript -Destination (Join-Path $project 'Assets/Scripts')
foreach ($reference in @(@('Managed/TerraEngine.CSharp.dll','TerraEngine.CSharp.dll'),
	@('Managed/Analyzers/TerraEngine.Export.Generator.dll','TerraEngine.Export.Generator.dll'))) {
	Copy-Item -LiteralPath (Join-Path (Split-Path $editor) $reference[0]) -Destination (Join-Path $project ('Build/Assembly-CSharp/References/' + $reference[1]))
}
$authoredScene = Join-Path $project 'Assets/Scenes/Startup.tescene'
$sceneHash = (Get-FileHash -LiteralPath $authoredScene -Algorithm SHA256).Hash
$rpcId = 0
function Rpc([string]$Method, $Parameters) {
	$request = @{jsonrpc='2.0';id=(++$script:rpcId);method=$Method;params=$Parameters}
	$response = Invoke-RestMethod -Uri "http://127.0.0.1:$McpPort/mcp" -Method Post -ContentType 'application/json' -Body ($request | ConvertTo-Json -Depth 30 -Compress) -TimeoutSec 10
	[IO.File]::AppendAllText((Join-Path $work 'rpc.jsonl'),((@{request=$request;response=$response} | ConvertTo-Json -Depth 60 -Compress) + "`n"))
	Require ($null -eq $response.PSObject.Properties['error']) 'Owned editor returned a JSON-RPC error.'
	return $response
}
function Tool([string]$Name, $Arguments=@{}) {
	$response = Rpc 'tools/call' @{name=$Name;arguments=$Arguments}
	$texts = @($response.result.content | Where-Object type -eq 'text')
	Require ($texts.Count -eq 1) "Tool $Name omitted its unique native envelope."
	$envelope = $texts[0].text | ConvertFrom-Json
	Require ($envelope.ok -eq $true) "Native tool failed: $Name"
	return $envelope.data
}
function AwaitPlay([string]$Expected) {
	$clock = [Diagnostics.Stopwatch]::StartNew()
	do {
		$state = Tool 'play.state'
		if ($state.currentState -ceq $Expected -and $null -eq $state.pendingTransition) { return }
		Start-Sleep -Milliseconds 100
	} while ($clock.Elapsed.TotalSeconds -lt 90)
	throw "Owned editor did not reach Play state $Expected."
}
function StartOwned([string]$Executable, [string[]]$Arguments, [string]$LaneRoot, [hashtable]$Environment) {
	$start = [Diagnostics.ProcessStartInfo]::new($Executable)
	$start.WorkingDirectory = Split-Path -Parent $Executable
	$start.UseShellExecute = $false; $start.CreateNoWindow = $true
	$start.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
	$start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
	foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
	foreach ($key in $Environment.Keys) { $start.Environment[$key] = [string]$Environment[$key] }
	$process = [Diagnostics.Process]::Start($start)
	$out = [IO.File]::Create((Join-Path $LaneRoot 'stdout.log'))
	$err = [IO.File]::Create((Join-Path $LaneRoot 'stderr.log'))
	return @{process=$process;stdout=$out;stderr=$err;outTask=$process.StandardOutput.BaseStream.CopyToAsync($out);errTask=$process.StandardError.BaseStream.CopyToAsync($err)}
}
function StopOwned($Owner) {
	# Only this runner's exact child process is ever closed or terminated.
	$forced = $false
	try {
		if (!$Owner.process.HasExited) {
			$null = $Owner.process.CloseMainWindow()
			if (!$Owner.process.WaitForExit(10000)) { $forced = $true; $Owner.process.Kill($true); $Owner.process.WaitForExit() }
		}
		# Suppress framework VoidTaskResult objects so callers receive exactly
		# one lifecycle record containing the actual process exit status.
		$null = $Owner.outTask.GetAwaiter().GetResult()
		$null = $Owner.errTask.GetAwaiter().GetResult()
		return @{forcedTermination=$forced;exitCode=$Owner.process.ExitCode}
	} finally { $Owner.stdout.Dispose(); $Owner.stderr.Dispose(); $Owner.process.Dispose() }
}
function CheckObservation($Report, [string]$Scenario, [string]$ExpectedRun) {
	if ($SpatialAA) { CheckAAObservation $Report $ExpectedRun; return }
	Require ($Report.schemaVersion -eq 1 -and $Report.runId -ceq $ExpectedRun -and $Report.scenario -ceq $Scenario) 'Fixture observation identity/schema mismatch.'
	Require ($Report.status -ceq 'passed' -and $Report.updates -gt 0 -and $Report.bootId.Length -gt 0) "Fixture failed: $($Report.failure)"
	$required = switch ($Scenario) {
		'runtime' { @('legacy-preset','legacy-settings','atomic-live-batch','rejected-batch-rollback','target-rebuild','two-camera-publications','stale-camera-rejection','preference-save-restore') }
		'startup-stage' { @('mixed-startup-staged-without-live-change') }
		'startup-observe-success' { @('startup-original-receipt-applied') }
		'startup-observe-failure' { @('startup-original-receipt-rejected-with-rollback') }
		'startup-denied' { @('editor-startup-denied-without-live-change') }
	}
	foreach ($check in $required) { Require ($check -cin $Report.checks) "Fixture omitted check $check." }
	Require (@($Report.receipts).Count -gt 0) 'Fixture omitted native request receipts.'
	if ($Scenario -eq 'startup-observe-failure') {
		Require (@($Report.receipts | Where-Object { $_.requestId -ceq $Report.requestId -and $_.state -ceq 'Rejected' -and $_.reason -ceq 'PreparationFailed' }).Count -gt 0) 'Injected failure omitted the original PreparationFailed receipt.'
	}
	if ($Scenario -eq 'startup-denied') {
		Require (@($Report.receipts | Where-Object { $_.requestId -ceq $Report.requestId -and $_.state -ceq 'Rejected' -and $_.reason -ceq 'InvalidScope' }).Count -gt 0) 'Editor staging denial omitted the original InvalidScope receipt.'
	}
}

# Audit the actual native evidence, including rejected/unavailable modes. A
# fixture's optimistic status/check string alone cannot satisfy the AA lane.
function PositiveAAInteger($Value, [string]$Name) {
	[UInt64]$parsed = 0
	Require ([UInt64]::TryParse([string]$Value,[Globalization.NumberStyles]::None,[Globalization.CultureInfo]::InvariantCulture,[ref]$parsed) -and $parsed -gt 0) "AA evidence has an invalid positive $Name."
	return $parsed
}
function CheckAAObservation($Report, [string]$ExpectedRun) {
	Require ($Report.schemaVersion -eq 1 -and $Report.runId -ceq $ExpectedRun -and $Report.bootId.Length -gt 0) 'AA fixture observation identity/schema mismatch.'
	Require ($Report.status -ceq 'passed' -and $Report.updates -gt 0) "AA fixture did not pass: status=$($Report.status), failure=$($Report.failure), blocked=$($Report.blocked -join '; ')"
	Require ($Report.settingsOnly -eq $true -and $Report.imageryVerified -eq $false -and $Report.gpuExecutionVerified -eq $false) 'AA settings report must preserve the separate image/GPU verification boundary.'
	Require (@($Report.blocked).Count -eq 0) 'Unavailable required AA modes cannot pass the installed lane.'
	foreach ($scope in @('game','cameraA','cameraB')) {
		$descriptor = $Report.discovery.$scope
		Require ($null -ne $descriptor -and $descriptor.id -eq 64 -and $descriptor.kind -ceq 'Unsigned32' -and $descriptor.available -eq $true -and $descriptor.activation -ceq 'Rebuild') "AA discovery is missing its available typed rebuild descriptor for $scope."
		Require ($(if ($scope -ceq 'game') { $descriptor.gameScope } else { $descriptor.cameraScope })) "AA discovery does not advertise the required $scope scope."
		foreach ($mode in @(0,1,2,3)) { Require ($mode -in $descriptor.options) "AA mode $mode is unavailable for required $scope coverage." }
	}
	foreach ($check in @('native-mode-0-published','native-mode-3-published','native-mode-1-published','native-mode-2-published',
		'invalid-enum-atomic-rejection','camera-a-aa-isolated-from-b','camera-aa-reset-inherits-game',
		'explicit-aa-preference-saved','explicit-aa-preference-restored','camera-aa-override-held-for-preferences',
		'camera-aa-override-survives-preference-restore')) {
		Require ($check -cin $Report.checks) "AA fixture omitted required check $check."
	}
	Require (@($Report.checks | Where-Object { $_ -ceq 'native-mode-0-published' }).Count -eq 2) 'AA fixture must actually publish Off before and after CMAA2.'
	$publications = @($Report.publications)
	Require (@($publications | Group-Object requestId | Where-Object Count -gt 1).Count -eq 0) 'Each AA publication phase must retain its own native request receipt.'
	$modeRows = @($publications | Where-Object { $_.phase -ceq 'Modes' })
	Require ($modeRows.Count -eq 5) 'AA report must contain all five ordered native mode publications.'
	$expectedModes = @(0,3,0,1,2)
	[UInt64]$previousRequest = 0; [UInt64]$previousRevision = 0; [UInt64]$previousFrame = 0
	for ($index = 0; $index -lt $expectedModes.Count; ++$index) {
		$row = $modeRows[$index]
		Require ($row.effective.mode -eq $expectedModes[$index]) 'AA mode publication order must be Off/CMAA2/Off/FXAA/SMAA.'
		$id = PositiveAAInteger $row.requestId 'request ID'
		$revision = PositiveAAInteger $row.effective.revision 'effective revision'
		$frame = PositiveAAInteger $row.effective.frameId 'published frame'
		Require ($id -gt $previousRequest -and $revision -gt $previousRevision -and $frame -ge $previousFrame) 'AA switches must have distinct ordered requests and increasing Game revisions in this session.'
		$previousRequest = $id; $previousRevision = $revision; $previousFrame = $frame
	}
	$session = $modeRows[0].effective
	foreach ($row in $publications) {
		$id = PositiveAAInteger $row.requestId 'publication request ID'
		$applied = @($Report.receipts | Where-Object { $_.requestId -ceq [string]$row.requestId -and $_.state -ceq 'Applied' -and $_.reason -ceq 'None' })
		Require ($applied.Count -eq 1) "AA publication $id lacks one unambiguous Applied native receipt."
		$receipt = $applied[0]
		$receiptRevision = PositiveAAInteger $receipt.effectiveRevision 'receipt revision'
		$receiptFrame = PositiveAAInteger $receipt.appliedFrameId 'receipt frame'
		Require ([string]$row.receiptEffectiveRevision -ceq [string]$receipt.effectiveRevision -and [string]$row.receiptAppliedFrameId -ceq [string]$receipt.appliedFrameId) 'AA publication receipt identity differs from the native terminal evidence.'
		Require ((PositiveAAInteger $row.effective.revision 'snapshot revision') -ge $receiptRevision -and (PositiveAAInteger $row.effective.frameId 'snapshot frame') -ge $receiptFrame) 'AA publication did not observe its terminal receipt becoming effective.'
		CheckAASnapshot $row.effective $session
	}
	Require (@($Report.receipts | Where-Object { $_.state -cin @('RestartRequired','ScheduledRestart','Cancelled') }).Count -eq 0) 'AA runtime requests must not restart or silently cancel.'
	Require (@($Report.receipts | Where-Object { $_.state -ceq 'Rejected' -and $_.reason -ceq 'InvalidValue' }).Count -ge 1) 'AA fixture omitted an actual unknown-mode InvalidValue rejection.'
	foreach ($phase in @('Cameras','CameraReset','CameraPreferenceOverride','Change','Restore')) {
		Require (@($publications | Where-Object { $_.phase -ceq $phase }).Count -eq 1) "AA fixture omitted native publication phase $phase."
	}
	CheckAASnapshot $Report.effective $session
	CheckAASnapshot $Report.cameraA $session
	CheckAASnapshot $Report.cameraB $session
	CheckAASnapshot $Report.preferenceCameraBeforeA $session
	CheckAASnapshot $Report.preferenceCameraBeforeB $session
	Require ($Report.cameraA.cameraId -cne $Report.cameraB.cameraId -and [string]$Report.cameraA.cameraId -cne [string]$session.cameraId -and [string]$Report.cameraB.cameraId -cne [string]$session.cameraId) 'AA camera survival evidence must retain two distinct camera owners.'
	$null = PositiveAAInteger $Report.cameraA.viewLifetime 'CameraA view lifetime'
	$null = PositiveAAInteger $Report.cameraB.viewLifetime 'CameraB view lifetime'
	$isolated = @($publications | Where-Object { $_.phase -ceq 'Cameras' })[0].effective
	$reset = @($publications | Where-Object { $_.phase -ceq 'CameraReset' })[0].effective
	Require ($isolated.cameraId -ceq $Report.cameraA.cameraId -and $reset.cameraId -ceq $Report.cameraA.cameraId) 'Camera reset/override publications must belong to the surviving CameraA owner.'
	Require ($reset.mode -eq $modeRows[-1].effective.mode -and $Report.savedMode -eq $modeRows[-1].effective.mode -and $Report.effective.mode -eq $Report.savedMode) 'Camera reset and preference restore must return to the saved Game AA mode.'
	Require ($Report.cameraA.mode -ne $Report.effective.mode -and $Report.cameraB.mode -eq $Report.effective.mode) 'Final native camera reads must retain A override while B inherits restored Game AA.'
	foreach ($name in @('A','B')) {
		$before = $Report.('preferenceCameraBefore' + $name)
		$after = $Report.('camera' + $name)
		Require ($before.cameraId -ceq $after.cameraId -and $before.mode -eq $after.mode -and
			(PositiveAAInteger $after.revision 'surviving camera revision') -ge (PositiveAAInteger $before.revision 'preference camera revision')) 'Preference roundtrip must preserve each real camera owner and its prior AA values.'
	}
	$saveId = PositiveAAInteger $Report.preferenceSaveRequestId 'preference save request'
	$restoreId = PositiveAAInteger $Report.preferenceRestoreRequestId 'preference restore request'
	Require ($restoreId -gt $saveId) 'Preference restore must follow the durable save request.'
	foreach ($id in @($saveId,$restoreId)) {
		Require (@($Report.receipts | Where-Object { $_.requestId -ceq [string]$id -and $_.state -ceq 'Applied' -and $_.reason -ceq 'None' }).Count -eq 1) "AA preference request $id lacks its actual Applied receipt."
	}
	Require (@($publications | Where-Object { $_.phase -ceq 'Restore' })[0].requestId -ceq [string]$restoreId) 'Preference restore publication belongs to another request.'
}
function CheckAASnapshot($Snapshot, $Session) {
	Require ($null -ne $Snapshot) 'AA fixture omitted a required native snapshot.'
	foreach ($field in @('project','play','renderer','backend')) {
		Require ([string]$Snapshot.$field -ceq [string]$Session.$field) "AA publication crossed the $field lifetime during the same-session lane."
	}
	$null = PositiveAAInteger $Snapshot.project 'project generation'
	$null = PositiveAAInteger $Snapshot.renderer 'renderer generation'
	$null = PositiveAAInteger $Snapshot.backend 'backend generation'
	$null = PositiveAAInteger $Snapshot.revision 'snapshot revision'
	$null = PositiveAAInteger $Snapshot.frameId 'snapshot frame'
	Require ([int]$Snapshot.mode -cin @(0,1,2,3)) 'AA snapshot contains an unknown native mode.'
	Require ($Snapshot.sampleCount -eq 1) 'Spatial AA snapshots must retain native single-sample rendering.'
	if ($Snapshot.mode -ne 0) {
		Require ((PositiveAAInteger $Snapshot.postProcessResourceGeneration 'post-processing resource generation') -eq
			(PositiveAAInteger $Snapshot.backend 'backend resource generation')) 'Active AA snapshot lacks its exact native preparation certificate.'
	}
	Require (@($Snapshot.internalExtent).Count -eq 2 -and @($Snapshot.outputExtent).Count -eq 2 -and $Snapshot.internalExtent[0] -gt 0 -and $Snapshot.internalExtent[1] -gt 0 -and
		$Snapshot.internalExtent[0] -eq $Snapshot.outputExtent[0] -and $Snapshot.internalExtent[1] -eq $Snapshot.outputExtent[1]) 'AA publication must retain positive native internal/output extents.'
}
function RunLane([string]$Mode, [string]$Scenario, [string]$ObservationRoot, [string]$LaneRunId, [string]$Launcher, [string]$DataRoot) {
	$null = [IO.Directory]::CreateDirectory($ObservationRoot)
	$laneRoot = Join-Path $work ($Mode + '-' + $Scenario + '-' + [Guid]::NewGuid().ToString('N'))
	$null = [IO.Directory]::CreateDirectory($laneRoot)
	$environment = @{
		TERRAENGINE_GRAPHICS_FIXTURE_ROOT=$ObservationRoot;TERRAENGINE_GRAPHICS_FIXTURE_RUN_ID=$LaneRunId
		TERRAENGINE_GRAPHICS_FIXTURE_SCENARIO=$Scenario;TERRAENGINE_GRAPHICS_FIXTURE_DATA_ROOT=$DataRoot
		TERRAENGINE_GRAPHICS_FIXTURE_FAIL_STARTUP=$(if ($Scenario -eq 'startup-observe-failure') {'1'} else {'0'})
		TERRAENGINE_SUPPRESS_PROJECT_RECENTS='1'
	}
	$observation = Join-Path $ObservationRoot 'graphics-observation.json'
	if ($SpatialAA) {
		Require ($Scenario -ceq 'runtime') 'Spatial AA only supports the runtime installed fixture lanes.'
		$environment = @{TERRAENGINE_AA_FIXTURE_ROOT=$ObservationRoot;TERRAENGINE_AA_FIXTURE_RUN_ID=$LaneRunId
			TERRAENGINE_GRAPHICS_FIXTURE_DATA_ROOT=$DataRoot;TERRAENGINE_GRAPHICS_FIXTURE_FAIL_STARTUP='0';TERRAENGINE_SUPPRESS_PROJECT_RECENTS='1'}
		$observation = Join-Path $ObservationRoot 'aa-settings-observation.json'
	}
	$previousBoot = if (Test-Path -LiteralPath $observation) { (ReadPublishedObservation $observation | ConvertFrom-Json).bootId } else { '' }
	$executable = $Launcher; $arguments = @()
	if ($Mode -eq 'editor') {
		Require (@(Get-NetTCPConnection -LocalPort $McpPort -State Listen -ErrorAction SilentlyContinue).Count -eq 0) 'MCP port is already occupied.'
		$prefs = Join-Path $laneRoot 'editor-preferences.json'
		WriteJson $prefs @{recentProjects=@();autoBuildScripts=$true;editorMcpEnabled=$true;editorMcpWriteModeDefault='writes_enabled';editorMcpListenPort=$McpPort;rendererDiagnosticsPreferencesInitialized=$true}
		$environment.TERRAENGINE_EDITOR_PREFS_SESSION_PATH = $prefs
		$executable = $editor; $arguments = @('--project',$project)
	}
	$owner = StartOwned $executable $arguments $laneRoot $environment
	$proof = @{pid=$owner.process.Id;executable=$executable;scenario=$Scenario;mode=$Mode;runId=$LaneRunId}
	try {
		$clock = [Diagnostics.Stopwatch]::StartNew()
		if ($Mode -eq 'editor') {
			$ready = $false; $lastError = ''
			do {
				try {
					Require (!$owner.process.HasExited) 'Owned editor exited before readiness.'
					$listeners = @(Get-NetTCPConnection -LocalPort $McpPort -State Listen -ErrorAction SilentlyContinue)
					Require ($listeners.Count -eq 1 -and $listeners[0].OwningProcess -eq $owner.process.Id) 'MCP listener does not belong to the launched editor.'
					$hello = Rpc 'initialize' @{protocolVersion='2025-03-26';clientInfo=@{name='graphics-settings-installed';version='1'}}
					Require ($hello.result.serverInfo.name -ceq 'TerraEngine Editor') 'Unexpected loopback server.'
					$state = Tool 'project.state'
					Require ($state.hasProject -and [IO.Path]::GetFullPath($state.projectRoot) -ieq $project) 'Owned editor project mismatch.'
					if (!$state.assetImportsActive -and $state.readiness.projectSettingsReady -and $state.readiness.simulationEssentialReady -and $state.activeScenePath) { $ready = $true; break }
				} catch { $lastError = $_.Exception.Message }
				Start-Sleep -Milliseconds 250
			} while ($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds)
			Require $ready "Editor readiness failed: $lastError"
			AwaitPlay 'stopped'; $null = Tool 'play.enter' @{expectedState='stopped'}; AwaitPlay 'playing'
			$clock.Restart()
		}
		$report = $null; $reportText = ''
		do {
			if (Test-Path -LiteralPath $observation) {
				$candidateText = ReadPublishedObservation $observation
				$candidate = $candidateText | ConvertFrom-Json
				$scenarioMatches = $SpatialAA -or $candidate.scenario -ceq $Scenario
				if ($candidate.bootId -cne $previousBoot -and $scenarioMatches -and $candidate.status -cne 'pending') { $report = $candidate; $reportText = $candidateText; break }
			}
			Require (!$owner.process.HasExited) 'Owned application exited before reporting the requested scenario.'
			Start-Sleep -Milliseconds 100
		} while ($clock.Elapsed.TotalSeconds -lt $TimeoutSeconds)
		Require ($null -ne $report) 'Native managed observation deadline expired.'
		CheckObservation $report $Scenario $LaneRunId
		if ($Scenario -eq 'startup-denied') {
			# The scope rejection must occur before durable staging, including a
			# cancelled or rejected transaction being written into an empty store.
			$documents = if (Test-Path -LiteralPath $DataRoot) { @(Get-ChildItem -LiteralPath $DataRoot -Filter startup.json -Recurse -File) } else { @() }
			Require (@($documents).Count -eq 0) 'Editor Play Mode created an exported startup document despite rejecting the request.'
			$proof.startupDocumentAbsent = $true
		}
		[IO.File]::WriteAllText((Join-Path $laneRoot 'observation.json'),$reportText,[Text.UTF8Encoding]::new($false))
		if ($Mode -eq 'editor') { $null = Tool 'play.exit' @{}; AwaitPlay 'stopped' }
		$proof.observation = $report; $proof.status = 'passed'
		return $proof
	} finally {
		$proof.lifecycle = StopOwned $owner
		WriteJson (Join-Path $laneRoot 'process.json') $proof
		if ($proof.ContainsKey('status') -and $proof.status -ceq 'passed') {
			Require (!$proof.lifecycle.forcedTermination -and $proof.lifecycle.exitCode -eq 0) 'Application did not shut down cleanly after completing its observation.'
		}
	}
}

try {
	# Export through the installed production packager. It compiles the same
	# authored script against the staged facade and generator used by the editor.
	$exportRoot = Join-Path $work 'export'; $null = [IO.Directory]::CreateDirectory($exportRoot)
	$package = Join-Path $work 'Package'
	$owner = StartOwned $exporter @('--project',$project,'--profile',(Join-Path $project 'BuildProfiles/graphics.json'),'--output',$package,'--evidence',$exportRoot,'--run-id',$runId) $exportRoot @{}
	try {
		Require ($owner.process.WaitForExit(900000)) 'Installed exporter exceeded its deadline.'
		Require ($owner.process.ExitCode -eq 0) "Installed exporter failed with $($owner.process.ExitCode)."
		$summary.export = @{exitCode=$owner.process.ExitCode;evidence=$exportRoot}
	} finally { $null = StopOwned $owner }
	$launcher = Join-Path $package 'GraphicsSettingsContract.exe'
	Require ((Test-Path -LiteralPath $launcher -PathType Leaf) -and (Test-Path -LiteralPath ([IO.Path]::ChangeExtension($launcher,'.tepak')) -PathType Leaf)) 'Export omitted the native launcher/archive pair.'
	foreach ($mode in @('editor','export')) {
		$runtimeRoot = Join-Path $work ($mode + '-runtime-observations')
		$summary.lanes += RunLane $mode 'runtime' $runtimeRoot ($runId + '-' + $mode + '-runtime') $launcher (Join-Path $work ($mode + '-runtime-data'))
		if ($SpatialAA) { continue }
		if ($mode -eq 'editor') {
			$summary.lanes += RunLane 'editor' 'startup-denied' (Join-Path $work 'editor-startup-denied') ($runId + '-editor-denied') $launcher (Join-Path $work 'editor-startup-denied-data')
			continue
		}
		# Only an exported game owns durable next-launch transactions. Editor
		# Play Mode is covered above by the explicit rejected-scope lane.
		foreach ($outcome in @('success','failure')) {
			$root = Join-Path $work ($mode + '-startup-' + $outcome)
			$id = $runId + '-' + $mode + '-' + $outcome
			$data = Join-Path $work ($mode + '-startup-' + $outcome + '-data')
			$staged = RunLane $mode 'startup-stage' $root $id $launcher $data
			$summary.lanes += $staged
			$observed = RunLane $mode ('startup-observe-' + $outcome) $root $id $launcher $data
			Require ($staged.observation.bootId -cne $observed.observation.bootId -and $staged.observation.requestId -ceq $observed.observation.requestId) 'Startup result was not produced by a fresh process for the original receipt.'
			$summary.lanes += $observed
		}
	}
	if ($SpatialAA) {
		Require (@($summary.lanes).Count -eq 2 -and @($summary.lanes | Where-Object { $_.scenario -cne 'runtime' -or $_.status -cne 'passed' }).Count -eq 0) 'AA acceptance requires both actual editor and exported runtime lanes.'
		Require ($summary.lanes[0].mode -ceq 'editor' -and $summary.lanes[1].mode -ceq 'export' -and $summary.lanes[0].observation.bootId -cne $summary.lanes[1].observation.bootId) 'AA editor/export evidence must come from distinct launched processes.'
	}
	Require ((Get-FileHash -LiteralPath $authoredScene -Algorithm SHA256).Hash -ceq $sceneHash) 'Application altered the authored fixture scene.'
	$summary.status = 'passed'
} catch { $summary.status = 'failed'; $summary.failure = $_.Exception.Message }
finally { WriteJson (Join-Path $work $(if ($SpatialAA) {'aa-settings-acceptance.json'} else {'graphics-settings-acceptance.json'})) $summary }
if ($summary.status -cne 'passed') { throw $summary.failure }
Write-Output (Join-Path $work $(if ($SpatialAA) {'aa-settings-acceptance.json'} else {'graphics-settings-acceptance.json'}))
